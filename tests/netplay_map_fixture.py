"""Build a small portable map for the lobby transfer test with only the standard library."""
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile

def encoded(item):
    return json.dumps(item, sort_keys=True, separators=(",", ":")).encode()

with tempfile.TemporaryDirectory() as tmp:
    folder = Path(tmp) / "host/mods/maps"
    folder.mkdir(parents=True)
    manifest = dict(version=1, title="Banked test", author="", base_arena="AFTON",
                    replace_retail_spawns=False, validated=False)
    roles = ("grid_starts", "battle_respawns", "checkpoints", "finish_gates", "ai_waypoints")
    glb = encoded({"asset": {"version": "2.0"}})
    glb += b" " * (-len(glb) % 4)
    members = {"spawns.json": encoded(dict.fromkeys(roles, [])),
               "scene.glb": struct.pack("<4sIII4s", b"glTF", 2, 20+len(glb), len(glb), b"JSON") + glb,
               "collision.eodcol": struct.pack("<8sII", b"EODCOL1", 0, 0)}
    digest = hashlib.sha256()
    for data in (encoded(manifest), *members.values()):
        digest.update(struct.pack("<Q", len(data)) + data)
    manifest["sha256"] = digest.hexdigest()
    path = folder / "track.eodmap"
    with tarfile.open(path, "w", format=tarfile.USTAR_FORMAT) as package:
        for name, data in {"manifest.json": encoded(manifest), **members}.items():
            entry = tarfile.TarInfo(name)
            entry.size = len(data)
            package.addfile(entry, io.BytesIO(data))
    # Exercise at least two selective retransmission windows.
    with path.open("ab") as stream:
        stream.write(bytes(65536))
    subprocess.run([sys.argv[1], tmp], check=True)
