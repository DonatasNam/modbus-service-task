"""Behaviour suite: the service binary ($MBP_BINARY) against simulators and real client processes.

The gate runs it against the release and the sanitizer build with short timeouts and outages; the bench runs the
full durations of acceptance.md.
"""

from __future__ import annotations

import json
import socket
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path
from typing import List, Optional, Tuple

from harness import FIXTURES, PROVIDED_CONFIG, Service, Simulator, free_port, raw_exchange, read_line, service_binary, wait_until, write_config
from modbus_poller import Client, ServiceBusy, ServiceUnavailable, UnknownDevice, UnknownPoint

FAST_LINK = {"request_timeout": "300", "request_retries": "1", "connect_timeout": "300", "connect_retries": "1"}
RULES = {
    "float32": lambda unit, point: unit * 100 + point + 0.25,
    "int16": lambda unit, point: -(unit * 100 + point),
    "uint16": lambda unit, point: 60000 + point,
    "int32": lambda unit, point: -(unit * 1000000 + point * 1000),
    "uint32": lambda unit, point: 3000000000 + point,
}
INVALID_CONFIGS = {
    "data_type_float64.json": "modbus_tcp[0].stations[0].datapoints[0].data_type",
    "byte_order_abdc.json": "modbus_tcp[0].stations[0].datapoints[0].byte_order",
    "func_code_5.json": "modbus_tcp[0].stations[0].datapoints[0].func_code",
    "reg_size_float32_1.json": "modbus_tcp[0].stations[0].datapoints[0].reg_size",
    "tcp_port_abc.json": "modbus_tcp[0].tcp_port",
    "tcp_port_number.json": "modbus_tcp[0].tcp_port",
    "duplicate_station_id.json": "modbus_tcp[0].stations[1].id",
    "duplicate_point_id.json": "modbus_tcp[0].stations[0].datapoints[1].id",
    "tcp_host_missing.json": "modbus_tcp[0].tcp_host",
    "tcp_host_name.json": "modbus_tcp[0].tcp_host",
    "enable_yes.json": "modbus_tcp[0].enable",
    "start_addr_70000.json": "modbus_tcp[0].stations[0].datapoints[0].start_addr",
    "start_addr_past_end.json": "modbus_tcp[0].stations[0].datapoints[0].start_addr",
    "start_addr_space.json": "modbus_tcp[0].stations[0].datapoints[0].start_addr",
    "start_addr_plus.json": "modbus_tcp[0].stations[0].datapoints[0].start_addr",
    "station_addr_250.json": "modbus_tcp[0].stations[0].station_addr",
    "station_addr_0.json": "modbus_tcp[0].stations[0].station_addr",
    "name_control_char.json": "modbus_tcp[0].stations[0].name",
    "scale_overflow.json": "modbus_tcp[0].stations[0].datapoints[0].meta.scale",
    "truncated.json": "config is not valid JSON",
}
CONFIG_EXIT_LIMIT_S = 2.0
CYCLE_TOLERANCE_S = 0.25
NEIGHBOUR_MAX_INTERVAL_S = 1.3


def timestamps_ms(client: Client, device_id: str, point_id: str, duration_s: float) -> List[int]:
    """The distinct timestamps one point shows while sampled for duration_s."""
    seen: List[int] = []
    deadline = time.perf_counter() + duration_s
    while time.perf_counter() < deadline:
        point = client.value(device_id, point_id)
        if point.timestamp is not None:
            stamp = int(round(point.timestamp.timestamp() * 1000))
            if not seen or stamp != seen[-1]:
                seen.append(stamp)
        time.sleep(0.05)
    return seen


def intervals_s(stamps: List[int]) -> List[float]:
    """Gaps between consecutive timestamps, in seconds."""
    return [(later - earlier) / 1000.0 for earlier, later in zip(stamps, stamps[1:])]


class ServiceTestCase(unittest.TestCase):
    """Starts what a test needs in a temporary folder; every service must stop with exit 0 and no sanitizer report."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.workdir = Path(self.tmp.name)
        self.services: List[Service] = []
        self.simulators: List[Simulator] = []

    def tearDown(self) -> None:
        results = [(service.stop(), service.sanitizer_reports(), service.stderr()) for service in self.services]
        for simulator in self.simulators:
            simulator.stop()
        self.tmp.cleanup()
        for status, reports, stderr in results:
            self.assertEqual(reports, [], stderr)
            self.assertEqual(status, 0, stderr)

    def start_service(self, config: Path, name: str = "service", socket_path: Optional[Path] = None) -> Service:
        service = Service(self.workdir, config, name, socket_path)
        self.services.append(service)
        return service

    def start_all_types(self, disable_second: bool = False) -> Tuple[Service, List[Simulator]]:
        """all_types.json on two simulators with short timeouts; waits until the enabled stations are online."""
        ports = {0: free_port(), 1: free_port()}
        second = dict(FAST_LINK, enable="false") if disable_second else FAST_LINK
        config = write_config(FIXTURES / "all_types.json", self.workdir / "all_types.json", ports, {0: FAST_LINK, 1: second})
        simulators = [Simulator(self.workdir, config, index, ports[index]) for index in (0, 1)]
        self.simulators.extend(simulators)
        service = self.start_service(config)
        online = ["1", "2"] if disable_second else ["1", "2", "3"]
        with service.client() as client:
            self.assertTrue(wait_until(lambda: all(client.status(device).online for device in online), 5.0), service.stderr())
        return service, simulators


class ConfigTest(ServiceTestCase):
    def test_provided_config_lists_both_meters_and_never_goes_online(self) -> None:
        service = self.start_service(PROVIDED_CONFIG)
        with service.client() as client:
            devices = client.list_devices()
            self.assertEqual([device.id for device in devices], ["1", "2"])
            self.assertEqual([device.name for device in devices], ["Electricity Meter Ser. EM1276", "Electricity Meter Ser. EM1277"])
            self.assertEqual([device.point_count for device in devices], [13, 13])
            power = client.value("2", "12")
            self.assertEqual((power.description, power.units, power.quality), ("Total system power", "kW", "none"))
            for _ in range(10):
                self.assertFalse(any(client.status(device.id).online for device in devices))
                time.sleep(0.1)

    def test_invalid_configs_exit_naming_the_field(self) -> None:
        for name, expected in INVALID_CONFIGS.items():
            with self.subTest(config=name):
                started = time.perf_counter()
                result = subprocess.run([str(service_binary()), "-c", str(FIXTURES / "invalid" / name), "-s", str(self.workdir / "x.sock")], capture_output=True, timeout=10, check=False)
                elapsed = time.perf_counter() - started
                stderr = result.stderr.decode("utf-8", "replace")
                self.assertEqual(result.returncode, 1, stderr)
                self.assertLess(elapsed, CONFIG_EXIT_LIMIT_S)
                self.assertIn(expected, stderr)

    def test_missing_config_file_exits_1(self) -> None:
        result = subprocess.run([str(service_binary()), "-c", str(self.workdir / "absent.json"), "-s", str(self.workdir / "x.sock")], capture_output=True, timeout=10, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("absent.json", result.stderr.decode("utf-8", "replace"))


class PollingTest(ServiceTestCase):
    def test_every_type_byte_order_and_function_code_decodes(self) -> None:
        service, _ = self.start_all_types()
        config = json.loads((self.workdir / "all_types.json").read_text(encoding="utf-8"))
        with service.client() as client:
            for connection in config["modbus_tcp"]:
                for station in connection["stations"]:
                    unit = int(station["station_addr"])
                    values = client.values(station["id"])
                    for datapoint in station["datapoints"]:
                        point = values[datapoint["id"]]
                        scale = float(datapoint.get("meta", {}).get("scale", "1"))
                        expected = RULES[datapoint["data_type"]](unit, int(datapoint["id"])) * scale
                        tolerance = 1e-6 if datapoint["data_type"] == "float32" else 1e-9
                        self.assertEqual(point.quality, "good", datapoint)
                        assert point.value is not None
                        self.assertLessEqual(abs(point.value - expected), abs(expected) * tolerance, datapoint)

    def test_one_second_cycle_is_kept(self) -> None:
        service, _ = self.start_all_types()
        with service.client() as client:
            gaps = intervals_s(timestamps_ms(client, "1", "1", 3.6))
        self.assertGreaterEqual(len(gaps), 2)
        for gap in gaps:
            self.assertAlmostEqual(gap, 1.0, delta=CYCLE_TOLERANCE_S)

    def test_silent_meter_does_not_delay_its_neighbour(self) -> None:
        service, simulators = self.start_all_types()
        simulators[0].command("silent 2 on")
        with service.client() as client:
            gaps = intervals_s(timestamps_ms(client, "1", "1", 3.5))
            status = client.status("2")
            self.assertNotEqual(status.state, "online")
            self.assertIn("timeout", status.last_error or "")
            self.assertTrue(all(point.quality == "stale" for point in client.values("2").values()))
            for gap in gaps:
                self.assertLessEqual(gap, NEIGHBOUR_MAX_INTERVAL_S)
            simulators[0].command("silent 2 off")
            self.assertTrue(wait_until(lambda: client.status("2").online, 10.0))
            self.assertTrue(wait_until(lambda: client.value("2", "1").quality == "good", 3.0))

    def test_exception_02_is_a_point_fault(self) -> None:
        service, simulators = self.start_all_types()
        with service.client() as client:
            before = client.status("2")
            connections = simulators[0].connections()
            simulators[0].command("exception 2 2")
            self.assertTrue(wait_until(lambda: all(point.quality == "stale" for point in client.values("2").values()), 12.0))
            after = client.status("2")
            self.assertTrue(after.online)
            self.assertEqual((after.since, after.reconnects), (before.since, before.reconnects))
            self.assertEqual(simulators[0].connections(), connections)
            self.assertIn("illegal data address", client.value("2", "1").error or "")
            self.assertTrue(client.status("1").online)
            simulators[0].command("exception 2 off")
            self.assertTrue(wait_until(lambda: client.value("2", "1").quality == "good", 3.0))
        self.assertNotIn("station 2:", service.stderr().split("station 2: connecting -> online")[-1])

    def test_exception_0b_is_a_link_fault(self) -> None:
        service, simulators = self.start_all_types()
        simulators[0].command("exception 2 11")
        with service.client() as client:
            self.assertTrue(wait_until(lambda: client.status("2").state == "error", 3.0))
            self.assertIn("gateway target", client.status("2").last_error or "")
            simulators[0].command("exception 2 off")
            self.assertTrue(wait_until(lambda: client.status("2").online, 10.0))

    def test_dropped_link_reconnects(self) -> None:
        service, simulators = self.start_all_types()
        with service.client() as client:
            before = client.status("1").reconnects
            simulators[0].command("drop")
            self.assertTrue(wait_until(lambda: client.status("1").reconnects > before, 5.0))
            self.assertTrue(wait_until(lambda: client.status("1").online and client.status("2").online, 5.0))

    def test_vanished_gateway_is_detected_and_recovered(self) -> None:
        service, simulators = self.start_all_types()
        pid = service.pid
        with service.client() as client:
            before = client.status("3").reconnects
            simulators[1].kill()
            self.assertTrue(wait_until(lambda: not client.status("3").online, 3.0))
            self.assertTrue(wait_until(lambda: all(point.quality == "stale" for point in client.values("3").values()), 3.0))
            self.assertTrue(client.status("1").online)
            self.assertTrue(client.status("2").online)
            time.sleep(2.0)
            simulators[1].start()
            self.assertTrue(wait_until(lambda: client.status("3").online, 10.0), service.stderr())
            self.assertTrue(wait_until(lambda: client.value("3", "1").quality == "good", 3.0))
            self.assertGreater(client.status("3").reconnects, before)
        self.assertEqual(service.pid, pid)
        self.assertIsNone(service.process.poll())

    def test_disabled_connection_is_listed_and_never_polled(self) -> None:
        service, simulators = self.start_all_types(disable_second=True)
        time.sleep(1.5)
        with service.client() as client:
            self.assertEqual(client.status("3").state, "disabled")
            self.assertFalse([device for device in client.list_devices() if device.id == "3"][0].enabled)
        self.assertEqual(simulators[1].requests(), {1: 0})
        self.assertEqual(simulators[1].connections(), 0)


class ClientTest(ServiceTestCase):
    def test_hostile_request_shapes_get_bad_request_and_the_connection_survives(self) -> None:
        service, _ = self.start_all_types()
        wide = b'{"cmd":"list","p":[' + b",".join([b"0"] * 240) + b"]}"
        deep = b"[" * 255 + b"]" * 255
        nul = b'{"cmd":"status","device":"' + b"\\u0000" * 70 + b'"}'
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(2.0)
            sock.connect(str(service.socket_path))
            for line, expected in ((wide, b'{"ok":true'), (deep, b'"bad_request"'), (nul, b'"bad_request"'), (b'{"cmd":"list"}', b'{"ok":true')):
                sock.sendall(line + b"\n")
                self.assertIn(expected, read_line(sock))

    def test_long_line_gets_too_long_then_a_clean_eof(self) -> None:
        service, _ = self.start_all_types()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(2.0)
            sock.connect(str(service.socket_path))
            sock.sendall(b"x" * 4096)
            self.assertIn(b'"too_long"', read_line(sock))
            self.assertEqual(sock.recv(100), b"")

    def test_client_limit_answers_busy_then_eof(self) -> None:
        service, _ = self.start_all_types()
        idle: List[socket.socket] = []
        try:
            for _ in range(32):
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(str(service.socket_path))
                idle.append(sock)
            self.assertTrue(wait_until(lambda: len(idle) == 32, 1.0))
            time.sleep(0.2)
            for _ in range(3):
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as extra:
                    extra.settimeout(2.0)
                    extra.connect(str(service.socket_path))
                    extra.sendall(b'{"cmd":"list"}\n')
                    self.assertIn(b'"busy"', read_line(extra))
                    self.assertEqual(extra.recv(100), b"")
            with self.assertRaises(ServiceBusy), service.client() as busy_client:
                busy_client.list_devices()
        finally:
            for sock in idle:
                sock.close()
        with service.client() as client:
            self.assertTrue(wait_until(lambda: len(client.list_devices()) == 3, 2.0))

    def test_crashing_and_flooding_clients_do_not_hurt_the_service(self) -> None:
        service, _ = self.start_all_types()
        pid = service.pid
        half = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        half.connect(str(service.socket_path))
        half.sendall(b'{"cmd":"sta')
        half.close()
        flood = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        flood.connect(str(service.socket_path))
        flood.settimeout(5.0)
        try:
            for _ in range(2000):
                flood.sendall(b'{"cmd":"values","device":"1"}\n')
        except OSError:
            pass  # the service dropped the flooding client, which is the expected outcome
        with service.client() as client:
            started = time.perf_counter()
            for _ in range(50):
                self.assertEqual(client.status("1").device_id, "1")
            self.assertLess(time.perf_counter() - started, 2.0)
            self.assertTrue(client.status("1").online)
        flood.close()
        self.assertEqual(service.pid, pid)
        self.assertIsNone(service.process.poll())

    def test_concurrent_clients_all_succeed(self) -> None:
        service, _ = self.start_all_types()
        failures: List[str] = []

        def work(index: int) -> None:
            with service.client() as client:
                for count in range(150):
                    try:
                        if count % 4 == 0:
                            client.list_devices()
                        elif count % 4 == 1:
                            client.status(str(1 + index % 3))
                        elif count % 4 == 2:
                            client.values(str(1 + index % 3))
                        else:
                            client.value("1", "17")
                    except Exception as err:  # noqa: BLE001 - every failure is collected and reported below
                        failures.append(repr(err))

        threads = [threading.Thread(target=work, args=(index,)) for index in range(6)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(failures, [])

    def test_sdk_errors_and_reconnect_after_a_service_restart(self) -> None:
        service, _ = self.start_all_types()
        client = service.client(timeout_s=1.0)
        try:
            with self.assertRaises(UnknownDevice):
                client.status("99")
            with self.assertRaises(UnknownPoint):
                client.value("1", "99")
            self.assertEqual(client.status("1").device_id, "1")
            self.assertEqual(service.stop(), 0)
            self.services.remove(service)
            self.assertEqual(service.sanitizer_reports(), [])
            started = time.perf_counter()
            with self.assertRaises(ServiceUnavailable):
                client.status("1")
            self.assertLess(time.perf_counter() - started, 1.5)
            self.start_service(service.config, name="restarted", socket_path=service.socket_path)
            self.assertEqual(client.status("1").device_id, "1")
        finally:
            client.close()


class LifecycleTest(ServiceTestCase):
    def config_without_devices(self) -> Path:
        return write_config(FIXTURES / "all_types.json", self.workdir / "unreachable.json", {0: free_port(), 1: free_port()}, {0: FAST_LINK, 1: FAST_LINK})

    def test_sigterm_exits_0_and_removes_the_socket(self) -> None:
        service = Service(self.workdir, self.config_without_devices())
        started = time.perf_counter()
        status = service.stop()
        self.assertEqual(status, 0, service.stderr())
        self.assertLess(time.perf_counter() - started, 6.0)
        self.assertFalse(service.socket_path.exists())
        self.assertEqual(service.sanitizer_reports(), [])

    def test_stale_socket_file_is_replaced(self) -> None:
        stale = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        stale.bind(str(self.workdir / "service.sock"))
        stale.close()
        service = self.start_service(self.config_without_devices())
        self.assertTrue(service.is_answering())

    def test_second_instance_is_refused_and_the_first_keeps_its_socket(self) -> None:
        config = self.config_without_devices()
        first = self.start_service(config)
        second = Service(self.workdir, config, name="second", socket_path=first.socket_path, wait_ready=False)
        self.assertEqual(second.process.wait(timeout=5), 1)
        self.assertIn("in use", second.stderr())
        self.assertTrue(first.is_answering())

    def test_path_that_is_not_a_socket_is_left_alone(self) -> None:
        blocker = self.workdir / "service.sock"
        blocker.write_text("keep me", encoding="utf-8")
        service = Service(self.workdir, self.config_without_devices(), wait_ready=False)
        self.assertEqual(service.process.wait(timeout=5), 1)
        self.assertEqual(blocker.read_text(encoding="utf-8"), "keep me")

    def test_answers_unknown_ids_without_truncation(self) -> None:
        service = self.start_service(self.config_without_devices())
        long_id = raw_exchange(service.socket_path, b'{"cmd":"status","device":"1234567890123456"}\n')
        self.assertIn(b'"unknown_device"', long_id)
        with_nul = raw_exchange(service.socket_path, b'{"cmd":"status","device":"1\\u0000x"}\n')
        self.assertIn(b'"bad_request"', with_nul)
        long_point = raw_exchange(service.socket_path, b'{"cmd":"value","device":"1","point":"1000000000000000"}\n')
        self.assertIn(b'"unknown_point"', long_point)


if __name__ == "__main__":
    unittest.main()
