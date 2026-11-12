#!/usr/bin/env python3
"""Backend lane process: runs modding_backend actions off the service process.

    python3 -I tools/kit_service/lane.py <lane-name>

The service keeps a few of these per lane (kit_service/lanes.py): "model" for 3D
model decode and mesh build, "write" for project saves, package builds and profile
installs. Interactive reads stay in the service's thread pool, so a long package
build or a heavy model never holds the interpreter that answers the next inspect.

Protocol on stdin/stdout, one JSON line each way per request:
    -> {"id": n, "request": {...}}
    <- {"id": n, "ok": bool, "images": [...]}   header
    <- <envelope>                              exactly the bytes the helper prints
The process exits when stdin closes (its service has gone).
"""
import json
import os
import sys
from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parents[1]))

from kit_service import backend as B  # noqa: E402


def main(argv):
    lane = argv[1] if len(argv) > 1 else "lane"
    out = sys.stdout.buffer
    for line in sys.stdin.buffer:
        if not line.strip():
            continue
        msg = json.loads(line)
        if msg.get("clear"):  # cache.clear on the service reaches its lane processes too
            B.cache.clear()
            out.write(json.dumps(dict(id=msg.get("id"), ok=True, images=[])).encode() + b"\n{}\n")
            out.flush()
            continue
        try:
            result = B.mb.dispatch(msg["request"])
            envelope = json.dumps(dict(ok=True, result=result), allow_nan=False).encode()
            header = dict(id=msg.get("id"), ok=True, images=B._images(result))
        except Exception as error:  # noqa: BLE001 - every failure becomes the helper's error envelope
            envelope = json.dumps(B.error_envelope(error)).encode()
            header = dict(id=msg.get("id"), ok=False, images=[])
        header.update(lane=lane, pid=os.getpid())
        out.write(json.dumps(header).encode() + b"\n" + envelope + b"\n")
        out.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
