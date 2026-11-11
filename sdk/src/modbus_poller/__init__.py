"""Python client for the modbus-pollerd service: list devices, read their status and their latest values."""

from .client import Client, default_socket_path
from .errors import PollerError, ProtocolError, RequestError, ServiceBusy, ServiceUnavailable, UnknownDevice, UnknownPoint
from .models import Device, DeviceStatus, PointValue

__version__ = "0.1.0"

__all__ = [
    "Client",
    "Device",
    "DeviceStatus",
    "PointValue",
    "PollerError",
    "ProtocolError",
    "RequestError",
    "ServiceBusy",
    "ServiceUnavailable",
    "UnknownDevice",
    "UnknownPoint",
    "default_socket_path",
]
