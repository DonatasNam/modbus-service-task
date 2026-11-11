"""Process harness for the behaviour suite: the service binary, simulators, config copies and raw socket clients."""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

from modbus_poller import Client

REPO = Path(__file__).resolve().parents[2]
SIMULATOR = REPO / "sim" / "modbus_sim.py"
FIXTURES = REPO / "tests" / "fixtures"
PROVIDED_CONFIG = REPO / "config" / "modbus_config_tcp.json"
DEFAULT_BINARY = REPO / "build" / "release" / "modbus-pollerd"
START_TIMEOUT_S = 10.0
STOP_TIMEOUT_S = 15.0
POLL_INTERVAL_S = 0.05
SANITIZER_MARKERS = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:")


def service_binary() -> Path:
    """The binary under test: $MBP_BINARY, else the release build."""
    return Path(os.environ.get("MBP_BINARY", str(DEFAULT_BINARY)))


def free_port() -> int:
    """A TCP port on 127.0.0.1 that nothing listens on right now."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def wait_until(predicate: Callable[[], bool], timeout_s: float) -> bool:
    """Polls predicate until it is true or timeout_s passes; returns its last answer."""
    deadline = time.perf_counter() + timeout_s
    while time.perf_counter() < deadline:
        if predicate():
            return True
        time.sleep(POLL_INTERVAL_S)
    return predicate()


def write_config(source: Path, target: Path, ports: Dict[int, int], overrides: Optional[Dict[int, Dict[str, str]]] = None) -> Path:
    """A copy of source with connection i on ports[i] and overrides[i] applied to its fields; returns target."""
    config = json.loads(source.read_text(encoding="utf-8"))
    for index, connection in enumerate(config["modbus_tcp"]):
        if index in ports:
            connection["tcp_port"] = str(ports[index])
        connection.update((overrides or {}).get(index, {}))
    target.write_text(json.dumps(config, indent=1), encoding="utf-8")
    return target


def raw_exchange(socket_path: Path, payload: bytes, timeout_s: float = 2.0) -> bytes:
    """Sends payload on a fresh connection and returns the first reply line, newline included."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout_s)
        sock.connect(str(socket_path))
        sock.sendall(payload)
        return read_line(sock)


def read_line(sock: socket.socket) -> bytes:
    """One line from sock, newline included; what came before EOF if the peer closed first."""
    data = b""
    while not data.endswith(b"\n"):
        chunk = sock.recv(65536)
        if not chunk:
            break
        data += chunk
    return data


class Simulator:
    """One sim/modbus_sim.py process serving one connection entry."""

    def __init__(self, workdir: Path, config: Path, connection: int, port: int, serial_bus_ms: Optional[int] = None) -> None:
        self.config = config
        self.connection = connection
        self.port = port
        self.serial_bus_ms = serial_bus_ms
        self.control = workdir / f"sim{connection}.ctl"
        self.log = workdir / f"sim{connection}.log"
        self.process: Optional[subprocess.Popen[bytes]] = None
        self.start()

    def start(self) -> None:
        """Starts the process and waits for its control socket."""
        command = [sys.executable, str(SIMULATOR), "--config", str(self.config), "--connection", str(self.connection), "--port", str(self.port), "--control", str(self.control)]
        if self.serial_bus_ms is not None:
            command += ["--serial-bus", str(self.serial_bus_ms)]
        with self.log.open("ab") as log:
            self.process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        if not wait_until(self.control.exists, START_TIMEOUT_S):
            raise RuntimeError(f"simulator on port {self.port} did not start")

    def command(self, line: str) -> List[str]:
        """Runs one control command; returns its reply lines, the closing ok or err included."""
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(5.0)
            sock.connect(str(self.control))
            sock.sendall(line.encode("ascii") + b"\n")
            lines: List[str] = []
            with sock.makefile("rb") as stream:
                for raw in stream:
                    lines.append(raw.decode("ascii").strip())
                    if lines[-1] == "ok" or lines[-1].startswith("err"):
                        break
        if lines[-1] != "ok":
            raise RuntimeError(f"simulator command {line!r} failed: {lines}")
        return lines

    def requests(self) -> Dict[int, int]:
        """Requests per unit since start."""
        counts: Dict[int, int] = {}
        for line in self.command("stats"):
            words = line.split()
            if words[0] == "unit":
                counts[int(words[1])] = int(words[3])
        return counts

    def connections(self) -> int:
        """TCP connections accepted since start."""
        for line in self.command("stats"):
            words = line.split()
            if words[0] == "connections":
                return int(words[1])
        raise RuntimeError("stats without a connections line")

    def kill(self) -> None:
        """SIGKILL, as a device that vanishes."""
        if self.process is not None:
            self.process.kill()
            self.process.wait(timeout=STOP_TIMEOUT_S)
            self.process = None
        if self.control.exists():
            self.control.unlink()

    def stop(self) -> None:
        """SIGTERM and wait."""
        if self.process is not None:
            self.process.terminate()
            self.process.wait(timeout=STOP_TIMEOUT_S)
            self.process = None


class Service:
    """One modbus-pollerd process; stderr goes to a file in the work folder."""

    def __init__(self, workdir: Path, config: Path, name: str = "service", socket_path: Optional[Path] = None, wait_ready: bool = True) -> None:
        self.socket_path = socket_path if socket_path is not None else workdir / f"{name}.sock"
        self.stderr_path = workdir / f"{name}.err"
        self.config = config
        with self.stderr_path.open("ab") as stderr:
            self.process = subprocess.Popen([str(service_binary()), "-c", str(config), "-s", str(self.socket_path)], stdout=subprocess.DEVNULL, stderr=stderr)
        if wait_ready and not wait_until(self.is_answering, START_TIMEOUT_S):
            self.process.kill()
            raise RuntimeError(f"service did not answer: {self.stderr()}")

    @property
    def pid(self) -> int:
        return self.process.pid

    def is_answering(self) -> bool:
        """True when a list request gets a reply."""
        try:
            return raw_exchange(self.socket_path, b'{"cmd":"list"}\n', 1.0).startswith(b'{"ok":true')
        except OSError:
            return False

    def client(self, timeout_s: float = 2.0) -> Client:
        """An SDK client on this service's socket."""
        return Client(str(self.socket_path), timeout=timeout_s)

    def request(self, payload: Dict[str, Any]) -> Any:
        """One raw request; the decoded reply."""
        return json.loads(raw_exchange(self.socket_path, json.dumps(payload).encode("utf-8") + b"\n"))

    def stderr(self) -> str:
        """Everything the service wrote to stderr so far."""
        return self.stderr_path.read_text(encoding="utf-8", errors="replace")

    def stop(self) -> int:
        """SIGTERM; returns the exit status."""
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        return self.process.wait(timeout=STOP_TIMEOUT_S)

    def sanitizer_reports(self) -> List[str]:
        """Sanitizer report lines in stderr; empty on a clean run."""
        return [line for line in self.stderr().splitlines() if any(marker in line for marker in SANITIZER_MARKERS)]
