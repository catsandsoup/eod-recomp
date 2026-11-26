"""Portable creator map v1. Members are ordinary files in a deterministic USTAR archive."""
from copy import copy
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import tarfile
import tempfile

import numpy as np


def json_bytes(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def snapshot_for_export(world):
    """Freeze authored transforms and markers, sharing immutable model buffers with the editor."""
    snapshot = copy(world)
    snapshot.objects = [copy(obj) for obj in world.objects if not obj.deleted and obj.source != "Retail"]
    snapshot.markers = [copy(m) for m in world.markers if m.source != "Retail"]
    snapshot.models = {obj.model_key: world.models[obj.model_key] for obj in snapshot.objects}
    snapshot.textures = {k: copy(world.textures[k]) for model in snapshot.models.values()
                         for k in model.textures if k in world.textures}
    return snapshot


def geometry(world):
    positions, colors, indices = [], [], []
    vertex_count = 0
    for obj in world.objects:
        if obj.deleted or obj.source == "Retail":
            continue
        model = world.models[obj.model_key]
        matrix = obj.world_matrix()
        for part in model.parts:
            if not part.indices.size:
                continue
            points = part.vertices[:, :3] @ matrix[:3, :3].T + matrix[:3, 3]
            faces = np.asarray(part.indices).reshape(-1)
            if (faces.size % 3 or faces.min() < 0 or faces.max() >= len(points)
                    or not np.isfinite(points).all() or vertex_count + len(points) > 0xffffffff):
                raise ValueError(f"Invalid mesh in {obj.name}")
            positions.append(points)
            asset = world.textures.get(part.texture)
            rgb = np.array(asset.average_rgb if asset else (176, 181, 184)) / 255.
            colors.append(np.broadcast_to(rgb, points.shape))
            indices.append(faces.astype("<u4") + vertex_count)
            vertex_count += len(points)

    def joined(parts, dtype):
        return np.concatenate(parts).astype(dtype, copy=False) if parts else np.empty(0, dtype)

    return joined(positions, "<f4"), joined(colors, "<f4"), joined(indices, "<u4")


def scene_glb(points, colors, faces):
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{}]}
    chunks = []
    if points.size:
        raw = points.tobytes() + colors.tobytes() + faces.tobytes()
        doc.update({
            "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1}, "indices": 2,
                                        "material": 0}]}],
            "materials": [{"doubleSided": True, "pbrMetallicRoughness": {"metallicFactor": 0}}],
            "buffers": [{"byteLength": len(raw)}],
            "bufferViews": [{"buffer": 0, "byteOffset": pos, "byteLength": size}
                            for pos, size in ((0, points.nbytes), (points.nbytes, colors.nbytes),
                                              (points.nbytes + colors.nbytes, faces.nbytes))],
            "accessors": [{"bufferView": 0, "componentType": 5126, "count": len(points),
                           "type": "VEC3", "min": points.min(0).tolist(), "max": points.max(0).tolist()},
                          {"bufferView": 1, "componentType": 5126, "count": len(colors), "type": "VEC3"},
                          {"bufferView": 2, "componentType": 5125, "count": len(faces), "type": "SCALAR"}],
        })
        chunks.append(struct.pack("<I4s", len(raw), b"BIN\0") + raw)
    meta = json_bytes(doc)
    meta += b" " * (-len(meta) % 4)
    chunks.insert(0, struct.pack("<I4s", len(meta), b"JSON") + meta)
    return struct.pack("<4sII", b"glTF", 2, 12 + sum(map(len, chunks))) + b"".join(chunks)


def spawn_data(world, preview_start=None):
    roles = {"Practice start": "grid_starts", "Start Grid": "grid_starts", "Spawn": "battle_respawns",
             "Checkpoint": "checkpoints", "Finish": "finish_gates", "Waypoint": "ai_waypoints"}
    groups = {key: [] for key in roles.values()}
    for m in sorted(world.markers, key=lambda m: m.marker_id != world.default_practice_start):
        if m.source == "Retail" or m.kind not in roles:
            continue
        groups[roles[m.kind]].append({"position": list(m.position), "normal": list(m.normal),
                                      "heading_deg": m.heading_deg})
    if preview_start is not None:
        x, y, z, yaw = map(float, preview_start)
        groups["grid_starts"].insert(0, {"position": [x, y, z], "normal": [0, 1, 0], "heading_deg": yaw})
    return groups


def export_eodmap(world, title, output, *, author="", replace_retail_spawns=True,
                  preview_start=None, token=None, progress=None):
    output = Path(output)
    spawns = spawn_data(world, preview_start)
    if replace_retail_spawns and not (spawns["grid_starts"] or spawns["battle_respawns"]):
        raise ValueError("Place a start gate or a respawn before exporting.")
    points, colors, faces = geometry(world)
    members = {
        "spawns.json": json_bytes(spawns),
        "scene.glb": scene_glb(points, colors, faces),
        "collision.eodcol": struct.pack("<8sII", b"EODCOL1\0", len(points), len(faces))
                            + points.tobytes() + faces.tobytes(),
    }
    manifest = {"version": 1, "title": title, "author": author, "base_arena": world.track_name,
                "replace_retail_spawns": replace_retail_spawns, "validated": False}
    # Hash length-framed data, including metadata, before adding the digest itself.
    digest = hashlib.sha256()
    for data in (json_bytes(manifest), *members.values()):
        digest.update(struct.pack("<Q", len(data)))
        digest.update(data)
    manifest["sha256"] = digest.hexdigest()
    members = {"manifest.json": json_bytes(manifest), **members}
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as temp:
        tmp = Path(temp.name)
    try:
        with tarfile.open(tmp, "w", format=tarfile.USTAR_FORMAT) as archive:
            for name, data in members.items():
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
        if token is None or not token.is_cancelled:
            if tmp.stat().st_size > 256 * 1024 * 1024:
                raise ValueError("Map exceeds the 256 MiB package limit.")
            os.replace(tmp, output)
    finally:
        tmp.unlink(missing_ok=True)
    return {"eodmap": str(output), "sha256": manifest["sha256"]}
