#!/usr/bin/env python3
"""Live table of every device's communication state and latest values, read through the modbus-poller SDK."""

from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime, timezone
from typing import List, Optional

from modbus_poller import Client, PollerError

TIMEOUT_S = 2.0


def render(client: Client) -> List[str]:
    """One snapshot: a line per device with its state, then a line per datapoint."""
    lines = [f"modbus-poller {datetime.now(timezone.utc).isoformat(timespec='seconds')}"]
    for device in client.list_devices():
        status = client.status(device.id)
        error = f"  last error: {status.last_error}" if status.last_error else ""
        lines.append(f"{device.id:>6}  {device.name}  ({device.connection} {device.host}:{device.port} unit {device.unit})  [{status.state}]{error}")
        if not device.enabled:
            continue
        for point in client.values(device.id).values():
            value = "-" if point.value is None else f"{point.value:.6g}"
            lines.append(f"        {point.point_id:>6}  {point.description:<32} {value:>14} {point.units:<6} {point.quality}")
    return lines


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", default=None, help="service socket (default: $MODBUS_POLLER_SOCKET or /run/modbus-poller/poller.sock)")
    parser.add_argument("--interval", type=float, default=1.0, help="seconds between snapshots")
    parser.add_argument("--once", action="store_true", help="print one snapshot and exit")
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    """Prints snapshots until SIGINT, or one with --once (exit 1 when the service cannot answer)."""
    args = parse_args(argv)
    client = Client(args.socket, timeout=TIMEOUT_S)
    failure: Optional[PollerError] = None
    try:
        while True:
            try:
                lines = render(client)
                failure = None
            except PollerError as err:
                lines = [f"service unavailable: {err}"]
                failure = err
            print("\n".join(lines), flush=True)
            if args.once:
                return 1 if failure is not None else 0
            print(flush=True)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
