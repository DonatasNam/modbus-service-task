# modbus-poller

A small service for an embedded Linux device. `modbus-pollerd` (C11) reads the provided `modbus_config_tcp.json`
unchanged, polls every enabled Modbus TCP device on its own cycle, and keeps the latest engineering values in RAM.
`modbus-poller-sdk` (Python, stdlib only) reads them over a Unix socket, from any number of client processes.

```
config/modbus_config_tcp.json   the provided config (installed as /etc/modbus-poller/config.json)
config/demo_local.json          the same meters, pointed at the simulator on 127.0.0.1:15020
modbus-poller/                  the C module (prefix Mbp): include/mbp.h is its only public header
daemon/modbus_pollerd.c         argument parsing, signals, Mbp_Init / Mbp_Deinit
sdk/                            the Python package, pip install -e sdk/
sim/modbus_sim.py               Modbus TCP simulator with fault injection (stdlib asyncio)
examples/monitor.py, alarm.py   two example applications
tests/                          run.sh (the one test entry), behaviour suite, config fixtures
packaging/                      build-deb.sh, systemd unit, maintainer scripts
```

## Architecture and concurrency

- **One worker thread and one TCP connection per station** (meter). A worker connects, picks its earliest due
  datapoint, waits until it is due, reads it (FC3 or FC4), decodes it and stores it. A slow or dead meter blocks
  only its own thread, so it never delays another device's polling. The cost is about 50 KB per meter, and one TCP
  connection per meter at the gateway.
- **One IPC thread** serves the Unix socket with `poll()`: non-blocking sockets, at most 32 clients, one read per client
  per wake-up, so no client can stall the others.
- **The store** holds the latest value of every point and the published health of every station behind one mutex.
  A worker holds it for one slot update, the IPC thread for one slot copy (≤ 2.5 KB); replies are formatted from the
  copy outside the lock. Reading through the SDK therefore cannot slow down device communication.
- The config is parsed once at start and is read-only afterwards. No heap is used after init: fixed tables
  (16 connections, 32 stations, 64 points per station) and a static arena for request parsing.
- Decoding: bytes A..D are the value's big-endian bytes and the byte order is their order on the wire
  (`CDAB`: reg0 = CD, reg1 = AB). A 16-bit value only takes the byte swap (`BADC`/`DCBA` swap, `ABCD`/`CDAB` do not).
  Engineering value = raw × scale. A NaN or infinite result gives quality `bad`.

## Communication with the library

A Unix stream socket (`/run/modbus-poller/poller.sock`, mode 0660, group `modbus-poller`) with one JSON object per
line each way. Why: local only, no port to secure, file permissions are the access control, any language can speak
it, and request/response keeps the service stateless per client. Wire protocol v1:

```
{"cmd":"list"}                                   -> {"ok":true,"result":[{"id","name","connection","host","port","unit","enabled","points"}]}
{"cmd":"status","device":"1"}                    -> {"ok":true,"result":{"id","state","since","last_ok","last_error","reads_ok","reads_failed","reconnects"}}
{"cmd":"values","device":"1"}                    -> {"ok":true,"result":{"id","points":[point, ...]}}
{"cmd":"value","device":"1","point":"12"}        -> {"ok":true,"result":point}
point = {"device","id","descr","units","value","ts","quality","error"}   quality: none | good | stale | bad
errors: {"ok":false,"error":{"code","message"}}  codes: bad_request unknown_command unknown_device unknown_point too_long busy internal
```

Times are epoch milliseconds. A device id is the station `id` (unique across the file), a point id the datapoint
`id`. Requests are limited to 512 bytes and 4 levels of nesting; idle connections are closed after 60 s (the SDK
reconnects transparently); a client that stops reading is dropped. Every close the service starts ends in a clean
EOF, never a reset.

## Slow, failing and disconnected devices

| Situation | Handling |
|---|---|
| Slow reply | Each attempt is bounded by `request_timeout` for the whole reply frame |
| No reply, bad data, I/O error | Retried `request_retries` times, each retry over a **fresh connection**, so a late reply is never taken for the retry's reply |
| Exception 01-04 (or unknown code) | A point fault: that point goes `stale` with the exception named, no retry, the station stays `online` |
| Exception 06, 0A, 0B | A link fault (busy, gateway): retried on the same connection |
| All attempts failed | Station `error`, all its points `stale` (last value and timestamp kept), link dropped, then a backoff of 1 s doubling to 30 s before the next connect |
| Connect fails | Counted; more than `connect_retries` in a row makes the station `offline`; reconnects go on forever with the backoff |
| Device returns | The next connect succeeds, the next good read makes it `online` again and resets the backoff |

Why: the configured timeouts and retries are honoured as written, a retry can never be confused by a late reply, and
the backoff bounds how often a dead meter's retries occupy a shared serial bus behind a gateway. The price is one TCP
handshake per failed attempt and up to 30 s until a returning device is read again. With the provided config a
silent meter is detected in at most 4 × 2 s = 8 s.

## Library API

```python
from modbus_poller import Client, ServiceUnavailable, UnknownDevice

with Client() as client:                      # socket: argument, $MODBUS_POLLER_SOCKET, or /run/modbus-poller/poller.sock
    for device in client.list_devices():      # Device(id, name, connection, host, port, unit, enabled, point_count)
        status = client.status(device.id)     # DeviceStatus(device_id, state, since, last_ok, last_error, reads_ok, reads_failed, reconnects), .online
        values = client.values(device.id)     # {point_id: PointValue(device_id, point_id, description, units, value, timestamp, quality, error)}
    volts = client.value("1", "1")
```

Frozen dataclasses with timezone-aware UTC datetimes; one exception family, each with a stable `code`:
`PollerError` > `ServiceUnavailable` (> `ServiceBusy`), `ProtocolError`, `RequestError` (> `UnknownDevice`,
`UnknownPoint`). The client connects lazily, reconnects once after a broken connection, closes its socket after a
timeout or a bad reply, and checks that every reply answers the device and point it asked for. One `Client` per
thread.

## Dependencies

- Service: libmodbus ≥ 3.1.6 (Modbus TCP client), cJSON ≥ 1.7.13 (config and request parsing), pthreads. Build:
  CMake ≥ 3.16 and a C11 compiler.
- SDK, simulator, examples and tests: Python ≥ 3.8 standard library only.

## Install and run on Debian / Ubuntu

```sh
sudo apt install build-essential cmake pkg-config libmodbus-dev libcjson-dev python3
packaging/build-deb.sh                                   # no root needed -> dist/modbus-poller_0.1.0_<arch>.deb
sudo dpkg -i dist/modbus-poller_0.1.0_amd64.deb          # installs, creates user modbus-poller, starts the unit
sudo usermod -aG modbus-poller $USER                      # clients need the group to open the socket (log in again)
pip install -e sdk/
python3 examples/monitor.py --once
```

The unit runs `/usr/sbin/modbus-pollerd -c /etc/modbus-poller/config.json -s /run/modbus-poller/poller.sock` as user
`modbus-poller`, restarts on failure and logs to the journal (`journalctl -u modbus-poller`). Without the package:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release && cmake --build build/release
python3 sim/modbus_sim.py --config config/demo_local.json --port 15020 --control /tmp/sim.ctl &
build/release/modbus-pollerd -c config/demo_local.json -s /tmp/poller.sock &
PYTHONPATH=sdk/src python3 examples/monitor.py --socket /tmp/poller.sock
PYTHONPATH=sdk/src python3 examples/alarm.py --socket /tmp/poller.sock --device 1 --point 1 --max 230
echo "silent 2 on" | nc -U /tmp/sim.ctl                    # inject a fault: delay, silent, exception, drop, stats
```

`tests/run.sh` builds the release and the AddressSanitizer/UBSan builds, runs the C unit tests, the SDK tests and
the behaviour suite (service + simulators + client processes) against both binaries, and ends with `TESTS PASS`.

## What was simplified, what next, what for production

Simplified or left out on purpose: one thread per station instead of an event loop; one register read per datapoint
(no coalescing of neighbouring registers, no pipelining); read-only FC3/FC4, no writes, coils, RTU, 64-bit or string
types; no config reload, history or persistence; request/response only, no push or subscribe; no authentication
beyond socket permissions; `tcp_host` must be an IPv4 literal; a single `dpkg-deb` script instead of full debhelper
packaging; no cross-build; no metrics or runtime log levels.

Known limits: a gateway that accepts fewer TCP connections than it has meters refuses the extra workers (they show
`offline`); the fallback is one shared connection per gateway. Behind an RS485 gateway a dead meter still holds the
bus during its retry bursts; the backoff bounds how often.

Next: coalesce neighbouring registers into one read per station cycle, share one connection per gateway, push
updates to subscribers, reload the config on SIGHUP.

For a production device: a cross-build for the target (ARM) with a pinned toolchain, a hardware watchdog fed by the
workers' progress, metrics (reads, latency, errors per station), log rate limiting, the SDK packaged as a `.deb`,
and a signed package with debhelper and lintian clean.
