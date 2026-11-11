#!/usr/bin/env python3
"""Watches one datapoint against limits and reports alarms and loss of communication, through the modbus-poller SDK."""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import List, Optional

from modbus_poller import Client, PollerError, RequestError

TIMEOUT_S = 2.0


@dataclass(frozen=True)
class Reading:
    """One look at the point: its value when communication is fine, else why it is not."""

    value: Optional[float]
    comm_problem: Optional[str]


def now_text() -> str:
    """The current time for a report line, aware UTC."""
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def read(client: Client, device_id: str, point_id: str) -> Reading:
    """The point's value, or the communication problem that keeps it from being fresh. A RequestError propagates."""
    try:
        status = client.status(device_id)
        point = client.value(device_id, point_id)
    except RequestError:
        raise
    except PollerError as err:
        return Reading(value=None, comm_problem=f"service: {err}")
    if not status.online:
        return Reading(value=None, comm_problem=f"device {status.state}: {status.last_error or 'no reply yet'}")
    if point.quality != "good" or point.value is None:
        return Reading(value=None, comm_problem=f"point {point.quality}: {point.error or 'no value yet'}")
    return Reading(value=point.value, comm_problem=None)


def is_outside(value: float, low: Optional[float], high: Optional[float]) -> bool:
    """True when value breaks a configured limit."""
    return (low is not None and value < low) or (high is not None and value > high)


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="device id")
    parser.add_argument("--point", required=True, help="datapoint id")
    parser.add_argument("--min", type=float, default=None, help="alarm below this value")
    parser.add_argument("--max", type=float, default=None, help="alarm above this value")
    parser.add_argument("--socket", default=None, help="service socket (default: $MODBUS_POLLER_SOCKET or /run/modbus-poller/poller.sock)")
    parser.add_argument("--interval", type=float, default=0.5, help="seconds between checks")
    return parser.parse_args(argv)


def watch(client: Client, args: argparse.Namespace) -> None:
    """Prints a line on every change: ALARM / CLEAR for the limits, COMM LOSS / COMM OK for communication."""
    is_alarm = False
    comm_problem: Optional[str] = None
    is_first = True
    target = f"{args.device}/{args.point}"
    while True:
        reading = read(client, args.device, args.point)
        if reading.comm_problem is not None and (comm_problem is None or is_first):
            print(f"{now_text()} COMM LOSS {target}: {reading.comm_problem}", flush=True)
        elif reading.comm_problem is None and comm_problem is not None:
            print(f"{now_text()} COMM OK {target}", flush=True)
        comm_problem = reading.comm_problem
        is_first = False
        if reading.value is not None:
            is_outside_now = is_outside(reading.value, args.min, args.max)
            if is_outside_now and not is_alarm:
                low = "-inf" if args.min is None else f"{args.min:g}"
                high = "inf" if args.max is None else f"{args.max:g}"
                print(f"{now_text()} ALARM {target} = {reading.value:.6g} outside [{low}, {high}]", flush=True)
            elif not is_outside_now and is_alarm:
                print(f"{now_text()} CLEAR {target} = {reading.value:.6g}", flush=True)
            is_alarm = is_outside_now
        time.sleep(args.interval)


def main(argv: List[str]) -> int:
    """Watches until SIGINT (exit 0); an unknown device or point exits 2."""
    args = parse_args(argv)
    client = Client(args.socket, timeout=TIMEOUT_S)
    try:
        watch(client, args)
    except RequestError as err:
        print(f"{now_text()} cannot watch {args.device}/{args.point}: {err}", file=sys.stderr, flush=True)
        return 2
    except KeyboardInterrupt:
        return 0
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
