#!/usr/bin/env python3
"""Modbus TCP simulator for one connection entry of a modbus-poller config, with fault injection.

It serves every station of the chosen connection on 127.0.0.1, unit id = station_addr. Each configured datapoint
holds the value rule below, encoded with its data_type and byte_order; unconfigured registers read 0. A control
socket takes one command per line (delay, silent, exception, drop, stats) and answers "ok" or "err <why>".
"""

from __future__ import annotations

import argparse
import asyncio
import json
import signal
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

MBAP_FORMAT = ">HHHB"
MBAP_BYTES = 7
FUNC_READ_HOLDING = 3
FUNC_READ_INPUT = 4
MAX_READ_REGISTERS = 125
EXCEPTION_ILLEGAL_FUNCTION = 0x01
EXCEPTION_ILLEGAL_VALUE = 0x03
EXCEPTION_GATEWAY_TARGET = 0x0B
EXCEPTION_FLAG = 0x80

PACK_FORMATS: Dict[str, str] = {"int16": ">h", "uint16": ">H", "int32": ">i", "uint32": ">I", "float32": ">f"}
# Byte positions of A..D on the wire, per byte order; a 16-bit value only takes the byte swap.
WIRE_ORDERS_32: Dict[str, Tuple[int, ...]] = {"ABCD": (0, 1, 2, 3), "BADC": (1, 0, 3, 2), "CDAB": (2, 3, 0, 1), "DCBA": (3, 2, 1, 0)}
WIRE_ORDERS_16: Dict[str, Tuple[int, ...]] = {"ABCD": (0, 1), "CDAB": (0, 1), "BADC": (1, 0), "DCBA": (1, 0)}

RegisterMap = Dict[Tuple[int, int], Dict[int, int]]


def rule_value(data_type: str, unit: int, point_id: int) -> float:
    """The value a datapoint holds: the acceptance rule, from the station unit and the numeric datapoint id."""
    if data_type == "float32":
        return unit * 100 + point_id + 0.25
    if data_type == "int16":
        return -(unit * 100 + point_id)
    if data_type == "uint16":
        return 60000 + point_id
    if data_type == "int32":
        return -(unit * 1000000 + point_id * 1000)
    if data_type == "uint32":
        return 3000000000 + point_id
    raise ValueError(f"unknown data_type {data_type!r}")


def encode_registers(value: float, data_type: str, byte_order: str) -> List[int]:
    """The registers that carry value on the wire, reg0 first."""
    pack_format = PACK_FORMATS[data_type]
    number = value if data_type == "float32" else int(value)
    big_endian = struct.pack(pack_format, number)
    positions = WIRE_ORDERS_16[byte_order] if len(big_endian) == 2 else WIRE_ORDERS_32[byte_order]
    wire = bytes(big_endian[position] for position in positions)
    return list(struct.unpack(f">{len(wire) // 2}H", wire))


def build_registers(connection: Dict[str, Any]) -> Tuple[RegisterMap, List[int]]:
    """Every configured register of the connection by (unit, function code), and the units it serves."""
    registers: RegisterMap = {}
    units: List[int] = []
    for station in connection["stations"]:
        unit = int(station["station_addr"], 10)
        units.append(unit)
        for point in station["datapoints"]:
            function = int(point["func_code"], 10)
            start = int(point["start_addr"], 10)
            value = rule_value(point["data_type"], unit, int(point["id"], 10))
            space = registers.setdefault((unit, function), {})
            for offset, register in enumerate(encode_registers(value, point["data_type"], point["byte_order"])):
                space[start + offset] = register
    return registers, units


@dataclass
class UnitFaults:
    """The faults injected into one unit."""

    delay_ms: int = 0
    is_silent: bool = False
    exception: Optional[int] = None
    address_exceptions: Dict[int, int] = field(default_factory=dict)

    def exception_for(self, start: int, count: int) -> Optional[int]:
        """The exception code a read of start..start+count-1 gets, or None."""
        if self.exception is not None:
            return self.exception
        for address, code in self.address_exceptions.items():
            if start <= address < start + count:
                return code
        return None


class Simulator:
    """The Modbus TCP server of one connection entry and its control commands."""

    def __init__(self, registers: RegisterMap, units: List[int], serial_bus_ms: Optional[int]) -> None:
        self.registers = registers
        self.faults: Dict[int, UnitFaults] = {unit: UnitFaults() for unit in units}
        self.requests: Dict[int, int] = {unit: 0 for unit in units}
        self.connections = 0
        self.writers: Set[asyncio.StreamWriter] = set()
        self.serial_bus_ms = serial_bus_ms
        self.bus = asyncio.Lock() if serial_bus_ms is not None else None

    async def handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        """Serves one TCP client: requests in order, each answered, delayed or swallowed."""
        self.connections += 1
        self.writers.add(writer)
        try:
            while True:
                header = await reader.readexactly(MBAP_BYTES)
                transaction, _protocol, length, unit = struct.unpack(MBAP_FORMAT, header)
                if length < 2:
                    break
                pdu = await reader.readexactly(length - 1)
                reply = await self.answer(unit, pdu)
                if reply is not None:
                    writer.write(struct.pack(MBAP_FORMAT, transaction, 0, len(reply) + 1, unit) + reply)
                    await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError):
            pass  # the client went away, or drop closed it
        finally:
            self.writers.discard(writer)
            writer.close()

    async def answer(self, unit: int, pdu: bytes) -> Optional[bytes]:
        """The reply PDU for one request, or None for a silent unit."""
        faults = self.faults.get(unit)
        if faults is None:
            return bytes([pdu[0] | EXCEPTION_FLAG, EXCEPTION_GATEWAY_TARGET])
        self.requests[unit] += 1
        if self.bus is None:
            return await self.answer_unit(unit, pdu, faults)
        async with self.bus:
            return await self.answer_unit(unit, pdu, faults)

    async def answer_unit(self, unit: int, pdu: bytes, faults: UnitFaults) -> Optional[bytes]:
        """Applies the unit's faults; on a serial bus a silent unit holds the bus for the configured time."""
        if faults.is_silent:
            if self.serial_bus_ms is not None:
                await asyncio.sleep(self.serial_bus_ms / 1000.0)
            return None
        if faults.delay_ms > 0:
            await asyncio.sleep(faults.delay_ms / 1000.0)
        return self.read_reply(unit, pdu, faults)

    def read_reply(self, unit: int, pdu: bytes, faults: UnitFaults) -> bytes:
        """The FC3/FC4 reply or the exception reply for one request."""
        function = pdu[0]
        if function not in (FUNC_READ_HOLDING, FUNC_READ_INPUT) or len(pdu) != 5:
            return bytes([function | EXCEPTION_FLAG, EXCEPTION_ILLEGAL_FUNCTION])
        start, count = struct.unpack(">HH", pdu[1:5])
        if count < 1 or count > MAX_READ_REGISTERS or start + count > 0x10000:
            return bytes([function | EXCEPTION_FLAG, EXCEPTION_ILLEGAL_VALUE])
        code = faults.exception_for(start, count)
        if code is not None:
            return bytes([function | EXCEPTION_FLAG, code])
        space = self.registers.get((unit, function), {})
        values = [space.get(address, 0) for address in range(start, start + count)]
        return bytes([function, 2 * count]) + struct.pack(f">{count}H", *values)

    def control(self, words: List[str]) -> str:
        """Runs one control command and returns its reply lines."""
        try:
            return self.run_command(words)
        except (ValueError, KeyError, IndexError) as err:
            return f"err {err}\n"

    def run_command(self, words: List[str]) -> str:
        """Dispatches one parsed control command; a bad argument raises."""
        command = words[0] if words else ""
        if command == "delay":
            self.unit_faults(words[1]).delay_ms = int(words[2], 10)
        elif command == "silent":
            self.unit_faults(words[1]).is_silent = parse_switch(words[2])
        elif command == "exception":
            self.set_exception(self.unit_faults(words[1]), words[2], words[3] if len(words) > 3 else None)
        elif command == "drop":
            for writer in list(self.writers):
                writer.close()
        elif command == "stats":
            lines = [f"unit {unit} requests {count}" for unit, count in sorted(self.requests.items())]
            lines.append(f"connections {self.connections}")
            return "\n".join(lines) + "\nok\n"
        else:
            raise ValueError(f"unknown command {command!r}")
        return "ok\n"

    def unit_faults(self, text: str) -> UnitFaults:
        """The faults of a configured unit; an unknown unit raises."""
        unit = int(text, 10)
        if unit not in self.faults:
            raise ValueError(f"unit {unit} is not served")
        return self.faults[unit]

    @staticmethod
    def set_exception(faults: UnitFaults, code_text: str, address_text: Optional[str]) -> None:
        """exception <unit> <code>|off [<addr>]: for every request, or only those covering one register."""
        code = None if code_text == "off" else parse_code(code_text)
        if address_text is None:
            faults.exception = code
            if code is None:
                faults.address_exceptions.clear()
            return
        address = int(address_text, 10)
        if code is None:
            faults.address_exceptions.pop(address, None)
        else:
            faults.address_exceptions[address] = code


def parse_switch(text: str) -> bool:
    """on|off."""
    if text not in ("on", "off"):
        raise ValueError(f"expected on or off, got {text!r}")
    return text == "on"


def parse_code(text: str) -> int:
    """An exception code, decimal or 0x-prefixed hex, 1..255."""
    code = int(text, 16) if text.lower().startswith("0x") else int(text, 10)
    if not 1 <= code <= 0xFF:
        raise ValueError(f"exception code {code} out of range")
    return code


async def handle_control(simulator: Simulator, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    """Serves one control connection, one command per line."""
    try:
        line = await reader.readline()
        while line:
            writer.write(simulator.control(line.decode("ascii", "replace").split()).encode("ascii"))
            await writer.drain()
            line = await reader.readline()
    except ConnectionError:
        pass  # the controlling script went away
    finally:
        writer.close()


async def run(args: argparse.Namespace, registers: RegisterMap, units: List[int]) -> None:
    """Serves Modbus TCP and the control socket until SIGTERM or SIGINT; the exit cleanup runs after both servers closed."""
    simulator = Simulator(registers, units, args.serial_bus)
    control_path = Path(args.control)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for stop_signal in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(stop_signal, stop.set)
    modbus_server = await asyncio.start_server(simulator.handle_client, "127.0.0.1", args.port, reuse_address=True)
    control_server = await asyncio.start_unix_server(lambda reader, writer: handle_control(simulator, reader, writer), str(control_path))
    print(f"serving units {units} on 127.0.0.1:{args.port}, control {control_path}", flush=True)
    try:
        await stop.wait()
    finally:
        modbus_server.close()
        control_server.close()
        for writer in list(simulator.writers):
            writer.close()
        if control_path.exists():
            control_path.unlink()


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", required=True, help="modbus-poller config file")
    parser.add_argument("--connection", type=int, default=0, help="index of the modbus_tcp entry to serve")
    parser.add_argument("--port", type=int, required=True, help="TCP port on 127.0.0.1")
    parser.add_argument("--control", required=True, help="control Unix socket path")
    parser.add_argument("--serial-bus", type=int, default=None, metavar="MS", help="serve one request at a time; a silent unit holds the bus MS ms")
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    config = json.loads(Path(args.config).read_text(encoding="utf-8"))
    registers, units = build_registers(config["modbus_tcp"][args.connection])
    control_path = Path(args.control)
    if control_path.exists():
        control_path.unlink()
    asyncio.run(run(args, registers, units))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
