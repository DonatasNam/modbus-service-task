"""SDK tests against a fake service on a Unix socket: no device and no real service needed."""

from __future__ import annotations

import json
import os
import socket
import tempfile
import threading
import time
import unittest
from datetime import timezone
from pathlib import Path
from typing import Any, Callable, Dict, Optional
from unittest import mock

from modbus_poller import Client, ProtocolError, RequestError, ServiceBusy, ServiceUnavailable, UnknownDevice, UnknownPoint, default_socket_path

SILENT = object()
CLOSE = object()

POINT_1 = {"device": "1", "id": "1", "descr": "Volts", "units": "V", "value": 230.5, "ts": 1700000000000, "quality": "good", "error": None}
POINT_2 = {"device": "1", "id": "2", "descr": "Amps", "units": "A", "value": None, "ts": None, "quality": "stale", "error": "modbus response timeout"}
STATUS_1 = {"id": "1", "state": "online", "since": 1700000000001, "last_ok": 1700000000002, "last_error": None, "reads_ok": 5, "reads_failed": 0, "reconnects": 1}
DEVICE_1 = {"id": "1", "name": "Meter", "connection": "Gateway", "host": "127.0.0.1", "port": 502, "unit": 1, "enabled": True, "points": 2}


def ok(result: Any) -> str:
    return json.dumps({"ok": True, "result": result})


def failure(code: str, message: str) -> str:
    return json.dumps({"ok": False, "error": {"code": code, "message": message}})


def standard_reply(request: Dict[str, Any]) -> Any:
    command = request.get("cmd")
    device = request.get("device")
    if command == "list":
        return ok([DEVICE_1])
    if device != "1":
        return failure("unknown_device", "unknown device")
    if command == "status":
        return ok(STATUS_1)
    if command == "values":
        return ok({"id": "1", "points": [POINT_1, POINT_2]})
    if command == "value":
        points = {"1": POINT_1, "2": POINT_2}
        if request.get("point") not in points:
            return failure("unknown_point", "unknown point")
        return ok(points[request["point"]])
    return failure("unknown_command", "unknown command")


class FakeService:
    """Answers each request line with handler(request): a reply line, SILENT (no reply) or CLOSE."""

    def __init__(self, path: Path, handler: Callable[[Dict[str, Any]], Any], close_after_reply: bool = False) -> None:
        self.path = path
        self.handler = handler
        self.close_after_reply = close_after_reply
        self.connections = 0
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(str(path))
        self.listener.listen(8)
        self.thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.thread.start()

    def _accept_loop(self) -> None:
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            self.connections += 1
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn: socket.socket) -> None:
        with conn, conn.makefile("rwb") as stream:
            for line in stream:
                reply = self.handler(json.loads(line))
                if reply is SILENT:
                    continue
                if reply is CLOSE:
                    return
                stream.write(reply.encode("utf-8") + b"\n")
                stream.flush()
                if self.close_after_reply:
                    return

    def close(self) -> None:
        self.listener.shutdown(socket.SHUT_RDWR)
        self.listener.close()
        self.path.unlink()


class ClientTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "poller.sock"
        self.service: Optional[FakeService] = None

    def tearDown(self) -> None:
        if self.service is not None:
            self.service.close()
        self.tmp.cleanup()

    def start(self, handler: Callable[[Dict[str, Any]], Any] = standard_reply, close_after_reply: bool = False) -> Client:
        if self.service is not None:
            self.service.close()
        self.service = FakeService(self.path, handler, close_after_reply)
        return Client(str(self.path), timeout=1.0)

    def test_list_devices(self) -> None:
        with self.start() as client:
            devices = client.list_devices()
        self.assertEqual(len(devices), 1)
        self.assertEqual(devices[0].id, "1")
        self.assertEqual(devices[0].point_count, 2)
        self.assertTrue(devices[0].enabled)

    def test_status_times_are_aware_utc(self) -> None:
        with self.start() as client:
            status = client.status("1")
        self.assertTrue(status.online)
        self.assertEqual(status.reconnects, 1)
        self.assertIsNone(status.last_error)
        self.assertIsNotNone(status.since)
        assert status.since is not None
        self.assertEqual(status.since.tzinfo, timezone.utc)
        self.assertEqual(int(status.since.timestamp() * 1000), 1700000000001)

    def test_values_by_point_id(self) -> None:
        with self.start() as client:
            values = client.values("1")
        self.assertEqual(sorted(values), ["1", "2"])
        self.assertEqual(values["1"].value, 230.5)
        self.assertEqual(values["1"].quality, "good")
        self.assertIsNone(values["2"].value)
        self.assertIsNone(values["2"].timestamp)
        self.assertEqual(values["2"].error, "modbus response timeout")

    def test_value(self) -> None:
        with self.start() as client:
            point = client.value("1", "1")
        self.assertEqual(point.units, "V")
        self.assertEqual(point.description, "Volts")

    def test_service_errors_map_to_exceptions(self) -> None:
        with self.start() as client:
            with self.assertRaises(UnknownDevice):
                client.status("9")
            with self.assertRaises(UnknownPoint):
                client.value("1", "9")
            self.assertEqual(client.status("1").device_id, "1")
        with self.start(lambda request: failure("bad_request", "request is not valid JSON")) as client:
            with self.assertRaises(RequestError) as caught:
                client.list_devices()
        self.assertEqual(caught.exception.code, "bad_request")

    def test_mismatched_device_closes_the_connection(self) -> None:
        def other_device(request: Dict[str, Any]) -> Any:
            if request.get("cmd") == "status":
                return ok(dict(STATUS_1, id="2"))
            return standard_reply(request)

        with self.start(other_device) as client:
            with self.assertRaises(ProtocolError):
                client.status("1")
            client.list_devices()
        assert self.service is not None
        self.assertEqual(self.service.connections, 2)

    def test_timeout_raises_and_reconnects_for_the_next_call(self) -> None:
        silent = {"on": True}

        def maybe_silent(request: Dict[str, Any]) -> Any:
            return SILENT if silent["on"] else standard_reply(request)

        with self.start(maybe_silent) as client:
            started = time.perf_counter()
            with self.assertRaises(ServiceUnavailable):
                client.values("1")
            self.assertLess(time.perf_counter() - started, 1.5)
            silent["on"] = False
            self.assertEqual(client.status("1").device_id, "1")
        assert self.service is not None
        self.assertEqual(self.service.connections, 2)

    def test_closed_connection_is_reconnected_once(self) -> None:
        with self.start(close_after_reply=True) as client:
            for _ in range(3):
                self.assertEqual(client.status("1").device_id, "1")
        assert self.service is not None
        self.assertEqual(self.service.connections, 3)

    def test_connection_closed_twice_raises(self) -> None:
        with self.start(lambda request: CLOSE) as client:
            with self.assertRaises(ServiceUnavailable):
                client.list_devices()

    def test_no_service_raises_unavailable(self) -> None:
        client = Client(str(self.path), timeout=1.0)
        with self.assertRaises(ServiceUnavailable):
            client.list_devices()
        client.close()
        client.close()

    def test_busy_raises_service_busy(self) -> None:
        with self.start(lambda request: failure("busy", "client limit reached"), close_after_reply=True) as client:
            with self.assertRaises(ServiceBusy) as caught:
                client.list_devices()
        self.assertIsInstance(caught.exception, ServiceUnavailable)
        self.assertEqual(caught.exception.code, "busy")

    def test_not_json_raises_protocol_error(self) -> None:
        with self.start(lambda request: "garbage") as client:
            with self.assertRaises(ProtocolError):
                client.list_devices()

    def test_default_path_from_environment(self) -> None:
        with mock.patch.dict(os.environ, {"MODBUS_POLLER_SOCKET": "/tmp/elsewhere.sock"}):
            self.assertEqual(default_socket_path(), "/tmp/elsewhere.sock")
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(default_socket_path(), "/run/modbus-poller/poller.sock")


if __name__ == "__main__":
    unittest.main()
