"""The SDK's one exception family: every error is a PollerError with a stable code and a message."""

from __future__ import annotations


class PollerError(Exception):
    """Base of every SDK error. code is stable across versions; message says what happened."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class ServiceUnavailable(PollerError):
    """The service cannot be reached: no socket, connect refused, connection lost twice, or no reply in time."""

    def __init__(self, message: str, code: str = "unavailable") -> None:
        super().__init__(code, message)


class ServiceBusy(ServiceUnavailable):
    """The service refused the connection: its client limit is reached."""

    def __init__(self, message: str) -> None:
        super().__init__(message, code="busy")


class ProtocolError(PollerError):
    """A reply that does not parse, or that answers another request than the one sent."""

    def __init__(self, message: str) -> None:
        super().__init__("protocol", message)


class RequestError(PollerError):
    """The service refused the request; code is the service's error code (bad_request, unknown_command, ...)."""


class UnknownDevice(RequestError):
    """No configured device has the requested id."""

    def __init__(self, message: str) -> None:
        super().__init__("unknown_device", message)


class UnknownPoint(RequestError):
    """The device has no datapoint with the requested id."""

    def __init__(self, message: str) -> None:
        super().__init__("unknown_point", message)
