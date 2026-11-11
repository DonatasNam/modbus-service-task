"""Typed records of the service's replies, parsed and checked once where the wire data enters."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Literal, Mapping, Optional, Tuple, cast, get_args

from .errors import ProtocolError

State = Literal["connecting", "online", "error", "offline", "disabled"]
Quality = Literal["none", "good", "stale", "bad"]
STATES: Tuple[str, ...] = get_args(State)
QUALITIES: Tuple[str, ...] = get_args(Quality)
MS_PER_SECOND = 1000.0


def as_mapping(data: Any, what: str) -> Mapping[str, Any]:
    """data as a JSON object, or ProtocolError naming what was expected."""
    if not isinstance(data, Mapping):
        raise ProtocolError(f"{what} is not a JSON object")
    return data


def _text(data: Mapping[str, Any], key: str) -> str:
    value = data.get(key)
    if not isinstance(value, str):
        raise ProtocolError(f"field {key!r} is not a string")
    return value


def _optional_text(data: Mapping[str, Any], key: str) -> Optional[str]:
    value = data.get(key)
    if value is not None and not isinstance(value, str):
        raise ProtocolError(f"field {key!r} is not a string or null")
    return value


def _non_negative_int(data: Mapping[str, Any], key: str) -> int:
    value = data.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ProtocolError(f"field {key!r} is not a non-negative integer")
    return value


def _flag(data: Mapping[str, Any], key: str) -> bool:
    value = data.get(key)
    if not isinstance(value, bool):
        raise ProtocolError(f"field {key!r} is not a boolean")
    return value


def _number(data: Mapping[str, Any], key: str) -> Optional[float]:
    value = data.get(key)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ProtocolError(f"field {key!r} is not a number or null")
    return float(value)


def _time(data: Mapping[str, Any], key: str) -> Optional[datetime]:
    """Epoch milliseconds or null on the wire; an aware UTC datetime here."""
    value = data.get(key)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProtocolError(f"field {key!r} is not epoch milliseconds or null")
    return datetime.fromtimestamp(value / MS_PER_SECOND, tz=timezone.utc)


def _choice(data: Mapping[str, Any], key: str, allowed: Tuple[str, ...]) -> str:
    value = _text(data, key)
    if value not in allowed:
        raise ProtocolError(f"field {key!r} has unknown value {value!r}")
    return value


@dataclass(frozen=True)
class Device:
    """One configured device (a Modbus station) as the service lists it."""

    id: str
    name: str
    connection: str
    host: str
    port: int
    unit: int
    enabled: bool
    point_count: int

    @classmethod
    def from_wire(cls, data: Any) -> Device:
        """Parses one entry of a list reply."""
        item = as_mapping(data, "device entry")
        return cls(
            id=_text(item, "id"),
            name=_text(item, "name"),
            connection=_text(item, "connection"),
            host=_text(item, "host"),
            port=_non_negative_int(item, "port"),
            unit=_non_negative_int(item, "unit"),
            enabled=_flag(item, "enabled"),
            point_count=_non_negative_int(item, "points"),
        )


@dataclass(frozen=True)
class DeviceStatus:
    """A device's communication status. since and last_ok are aware UTC datetimes; last_ok is None before a good read."""

    device_id: str
    state: State
    since: Optional[datetime]
    last_ok: Optional[datetime]
    last_error: Optional[str]
    reads_ok: int
    reads_failed: int
    reconnects: int

    @property
    def online(self) -> bool:
        """True while the last read got a reply."""
        return self.state == "online"

    @classmethod
    def from_wire(cls, data: Any) -> DeviceStatus:
        """Parses the result of a status reply."""
        item = as_mapping(data, "status result")
        return cls(
            device_id=_text(item, "id"),
            state=cast(State, _choice(item, "state", STATES)),
            since=_time(item, "since"),
            last_ok=_time(item, "last_ok"),
            last_error=_optional_text(item, "last_error"),
            reads_ok=_non_negative_int(item, "reads_ok"),
            reads_failed=_non_negative_int(item, "reads_failed"),
            reconnects=_non_negative_int(item, "reconnects"),
        )


@dataclass(frozen=True)
class PointValue:
    """The latest value of one datapoint, already scaled. value is None until a good read, or after a bad one."""

    device_id: str
    point_id: str
    description: str
    units: str
    value: Optional[float]
    timestamp: Optional[datetime]
    quality: Quality
    error: Optional[str]

    @classmethod
    def from_wire(cls, data: Any) -> PointValue:
        """Parses one point of a values or value reply."""
        item = as_mapping(data, "point")
        return cls(
            device_id=_text(item, "device"),
            point_id=_text(item, "id"),
            description=_text(item, "descr"),
            units=_text(item, "units"),
            value=_number(item, "value"),
            timestamp=_time(item, "ts"),
            quality=cast(Quality, _choice(item, "quality", QUALITIES)),
            error=_optional_text(item, "error"),
        )
