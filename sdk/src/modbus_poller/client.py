"""Client for the modbus-pollerd Unix socket: one JSON request per line, one JSON reply per line."""

from __future__ import annotations

import json
import os
import socket
import time
from types import TracebackType
from typing import Any, Callable, Dict, List, Optional, Type, TypeVar

from .errors import PollerError, ProtocolError, RequestError, ServiceBusy, ServiceUnavailable, UnknownDevice, UnknownPoint
from .models import Device, DeviceStatus, PointValue, as_mapping

DEFAULT_SOCKET_PATH = "/run/modbus-poller/poller.sock"
SOCKET_PATH_ENV = "MODBUS_POLLER_SOCKET"
RECV_BYTES = 65536
REPLY_LIMIT_BYTES = 1 << 20  # the service's largest reply is about 36 KB

T = TypeVar("T")


class _ConnectionLost(Exception):
    """The service closed or reset the connection: worth one reconnect."""


def default_socket_path() -> str:
    """$MODBUS_POLLER_SOCKET, else the packaged service's socket."""
    return os.environ.get(SOCKET_PATH_ENV, DEFAULT_SOCKET_PATH)


class Client:
    """A connection to the service, opened on the first call. Not thread-safe: use one Client per thread.

    A broken connection (EOF, reset, broken pipe) is reconnected once and the request resent. Any call that ends
    without a whole reply (a timeout, a partial or bad reply, a KeyboardInterrupt) closes the connection, so a late
    reply can never answer a later call.
    """

    def __init__(self, socket_path: Optional[str] = None, timeout: float = 2.0) -> None:
        self._socket_path = socket_path if socket_path is not None else default_socket_path()
        self._timeout = timeout
        self._sock: Optional[socket.socket] = None
        self._buffer = b""

    def __enter__(self) -> Client:
        return self

    def __exit__(self, exc_type: Optional[Type[BaseException]], exc: Optional[BaseException], traceback: Optional[TracebackType]) -> None:
        self.close()

    def list_devices(self) -> List[Device]:
        """Every configured device, in config order."""
        return self._call({"cmd": "list"}, _parse_devices)

    def status(self, device_id: str) -> DeviceStatus:
        """The communication status of one device."""
        return self._call({"cmd": "status", "device": device_id}, lambda result: _parse_status(result, device_id))

    def values(self, device_id: str) -> Dict[str, PointValue]:
        """The latest value of every datapoint of one device, by datapoint id."""
        return self._call({"cmd": "values", "device": device_id}, lambda result: _parse_values(result, device_id))

    def value(self, device_id: str, point_id: str) -> PointValue:
        """The latest value of one datapoint."""
        return self._call({"cmd": "value", "device": device_id, "point": point_id}, lambda result: _parse_value(result, device_id, point_id))

    def close(self) -> None:
        """Closes the connection; the next call opens a new one. Safe to call twice."""
        if self._sock is not None:
            self._sock.close()
            self._sock = None
        self._buffer = b""

    def _call(self, payload: Dict[str, str], parse: Callable[[Any], T]) -> T:
        line = json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n"
        try:
            return parse(self._result(self._exchange_once_more(line)))
        except RequestError:
            raise  # a whole error reply was read: the connection is still in step
        except BaseException:
            self.close()  # a timeout, a bad or partial reply, or an interrupt: a late reply must never answer a later call
            raise

    def _exchange_once_more(self, line: bytes) -> bytes:
        try:
            return self._exchange(line)
        except _ConnectionLost:
            self.close()
        try:
            return self._exchange(line)
        except _ConnectionLost as err:
            raise ServiceUnavailable(f"connection to {self._socket_path} lost: {err}") from err

    def _exchange(self, line: bytes) -> bytes:
        deadline = time.perf_counter() + self._timeout
        sock = self._connect()
        try:
            sock.sendall(line)
        except (BrokenPipeError, ConnectionResetError):
            pass  # the service closed this connection; a reply it queued first (busy) is still read below
        except socket.timeout as err:
            raise ServiceUnavailable(f"request to {self._socket_path} timed out after {self._timeout} s") from err
        except OSError as err:
            raise ServiceUnavailable(f"request to {self._socket_path} failed: {err}") from err
        return self._read_line(sock, deadline)

    def _connect(self) -> socket.socket:
        if self._sock is not None:
            return self._sock
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(self._timeout)
        try:
            sock.connect(self._socket_path)
        except OSError as err:
            sock.close()
            raise ServiceUnavailable(f"cannot connect to {self._socket_path}: {err}") from err
        self._sock = sock
        self._buffer = b""
        return sock

    def _read_line(self, sock: socket.socket, deadline: float) -> bytes:
        while b"\n" not in self._buffer:
            remaining = deadline - time.perf_counter()
            if remaining <= 0.0:
                raise ServiceUnavailable(f"no reply from {self._socket_path} within {self._timeout} s")
            sock.settimeout(remaining)
            try:
                chunk = sock.recv(RECV_BYTES)
            except socket.timeout as err:
                raise ServiceUnavailable(f"no reply from {self._socket_path} within {self._timeout} s") from err
            except (ConnectionResetError, BrokenPipeError) as err:
                raise _ConnectionLost(str(err)) from err
            except OSError as err:
                raise ServiceUnavailable(f"reading from {self._socket_path} failed: {err}") from err
            if not chunk:
                raise _ConnectionLost("the service closed the connection")
            self._buffer += chunk
            if len(self._buffer) > REPLY_LIMIT_BYTES:
                raise ProtocolError(f"reply longer than {REPLY_LIMIT_BYTES} bytes")
        line, _, rest = self._buffer.partition(b"\n")
        if rest:
            raise ProtocolError("data after the reply line")
        self._buffer = b""
        return line

    def _result(self, line: bytes) -> Any:
        try:
            reply = as_mapping(json.loads(line.decode("utf-8")), "reply")
        except (UnicodeDecodeError, ValueError) as err:
            raise ProtocolError(f"reply is not JSON: {err}") from err
        ok_field = reply.get("ok")
        if ok_field is True and "result" in reply:
            return reply["result"]
        if ok_field is not False:
            raise ProtocolError("reply has no ok/result envelope")
        error = as_mapping(reply.get("error"), "error")
        code = error.get("code")
        message = error.get("message")
        if not isinstance(code, str) or not isinstance(message, str):
            raise ProtocolError("error reply without code and message")
        raise _request_error(code, message)


def _request_error(code: str, message: str) -> PollerError:
    """The SDK exception for a service error code; busy is a ServiceUnavailable, so the caller drops the connection."""
    if code == "busy":
        return ServiceBusy(message)
    if code == "unknown_device":
        return UnknownDevice(message)
    if code == "unknown_point":
        return UnknownPoint(message)
    return RequestError(code, message)


def _parse_devices(result: Any) -> List[Device]:
    if not isinstance(result, list):
        raise ProtocolError("list result is not an array")
    return [Device.from_wire(item) for item in result]


def _parse_status(result: Any, device_id: str) -> DeviceStatus:
    status = DeviceStatus.from_wire(result)
    if status.device_id != device_id:
        raise ProtocolError(f"status reply for device {status.device_id!r}, requested {device_id!r}")
    return status


def _parse_values(result: Any, device_id: str) -> Dict[str, PointValue]:
    item = as_mapping(result, "values result")
    if item.get("id") != device_id or not isinstance(item.get("points"), list):
        raise ProtocolError(f"values reply does not answer device {device_id!r}")
    points = [PointValue.from_wire(point) for point in item["points"]]
    if any(point.device_id != device_id for point in points):
        raise ProtocolError(f"values reply holds points of another device than {device_id!r}")
    return {point.point_id: point for point in points}


def _parse_value(result: Any, device_id: str, point_id: str) -> PointValue:
    point = PointValue.from_wire(result)
    if point.device_id != device_id or point.point_id != point_id:
        raise ProtocolError(f"value reply for {point.device_id!r}/{point.point_id!r}, requested {device_id!r}/{point_id!r}")
    return point
