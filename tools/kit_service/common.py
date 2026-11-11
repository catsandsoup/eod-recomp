"""Workspace paths, errors and JSON-RPC framing shared by the service and its clients."""
import hashlib
import json
import os
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
PACKAGE = Path(__file__).resolve().parent
DEFAULT_WORKSPACE = "~/Library/Application Support/EODCreationKit"
SOCKET_LIMIT = 100  # sockaddr_un.sun_path is 104 bytes on macOS, keep a margin

# JSON-RPC error codes. Application errors carry a string code in error.data.code.
PARSE_ERROR, INVALID_REQUEST, METHOD_NOT_FOUND, INVALID_PARAMS, INTERNAL = -32700, -32600, -32601, -32602, -32603
APP_ERROR = -32000


def ensure_tools_path():
    """tools/ goes last on sys.path so a module there can never shadow the stdlib."""
    if str(TOOLS) not in sys.path:
        sys.path.append(str(TOOLS))


class KitError(Exception):
    """Plain-language error: what happened (message) and what to do (hint)."""

    def __init__(self, code, message, hint=None, details=None):
        super().__init__(message)
        self.code, self.message, self.hint, self.details = code, message, hint, details

    def payload(self):
        out = dict(code=self.code, message=self.message)
        if self.hint:
            out["hint"] = self.hint
        if self.details is not None:
            out["details"] = self.details
        return out


class ServiceUnavailable(Exception):
    """The service could not be reached or started."""


def workspace(value=None):
    raw = value or os.environ.get("EOD_KIT_WORKSPACE") or os.environ.get("EOD_KIT_HOME") or DEFAULT_WORKSPACE
    return Path(raw).expanduser().resolve()


def service_dir(ws):
    return Path(ws) / "service"


def socket_path(ws):
    """<workspace>/service/kit.sock, or a short per-user path when that is too long for AF_UNIX.

    Clients never derive this: they read it from service.json written after bind."""
    preferred = service_dir(ws) / "kit.sock"
    if len(os.fsencode(str(preferred))) <= SOCKET_LIMIT:
        return preferred
    digest = hashlib.sha256(str(ws).encode()).hexdigest()[:16]
    return Path("/tmp") / f"eodkit-{os.getuid()}" / f"{digest}.sock"


def db_path(ws):
    return service_dir(ws) / "kit.db"


def read_service_info(ws):
    try:
        return json.loads((service_dir(ws) / "service.json").read_text())
    except (OSError, ValueError):
        return None


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), default=str)


def sha256_text(text):
    return hashlib.sha256(text.encode()).hexdigest()


def encode(obj):
    return (json.dumps(obj, separators=(",", ":"), allow_nan=False, default=str) + "\n").encode()


class Raw:
    """A result that is already JSON (cached backend envelopes are sent verbatim)."""

    __slots__ = ("data",)

    def __init__(self, data):
        self.data = data


def response(rid, result):
    if isinstance(result, Raw):
        return b'{"jsonrpc":"2.0","id":' + json.dumps(rid).encode() + b',"result":' + result.data + b"}\n"
    return encode(dict(jsonrpc="2.0", id=rid, result=result))


def error_response(rid, code, message, data=None):
    err = dict(code=code, message=message)
    if data is not None:
        err["data"] = data
    return encode(dict(jsonrpc="2.0", id=rid, error=err))
