"""Creator archive regressions independent of game data, the GUI, or a game process."""
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import struct
import tarfile
import tempfile
from types import SimpleNamespace
import unittest

import numpy as np

from eodmap import export_eodmap, snapshot_for_export
from world_loader import MeshPart, WorldMarker, WorldModel, WorldObject


class PortableMapTest(unittest.TestCase):
    def test_world_space_indices_hash_and_spawn_order(self):
        # One vertex above the 16-bit index limit after appending the second part.
        points = np.zeros((65536, 8), np.float32)
        points[1, 0], points[2, 2] = 1, 1
        part = MeshPart("", points, np.array([0, 1, 2], np.uint32), 1)
        tile = replace(part, vertices=points[:3])
        model = WorldModel("deck", "Deck", "Roads", "", "", [part, tile], 2, [],
                           points[:, :3].min(0), points[:, :3].max(0), (0, 0, 0))
        obj = WorldObject("obj", "Deck", "deck", "Placed by the editor", "Roads", "", "", "", "",
                          2, [], (2, 20, 8), rotation_hpr=(90, 12, 0))
        first = WorldMarker("first", "S1", "Practice start", (2, 20, 8), normal=(0., .8, .6))
        markers = [WorldMarker(str(i), "Respawn", "Spawn", (0, i, 0)) for i in range(40)]
        markers += [replace(first, source="Retail"), replace(first, marker_id="second"), first]
        for kind in ("Checkpoint", "Finish", "Waypoint"):
            markers.append(replace(first, marker_id=kind, kind=kind))
        world = SimpleNamespace(track_name="TEST", objects=[obj, replace(obj, deleted=True),
                                replace(obj, source="Retail")], models={"deck": model}, markers=markers,
                                default_practice_start="first", textures={})
        snapshot = snapshot_for_export(world)
        obj.position = (0, 0, 0)
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "map.eodmap"
            export_eodmap(snapshot, "Test", dest, author="Créateur")
            first_bytes = dest.read_bytes()
            export_eodmap(snapshot, "Test", dest, author="Créateur")
            self.assertEqual(first_bytes, dest.read_bytes())
            with tarfile.open(dest) as archive:
                self.assertEqual(archive.getnames(), ["manifest.json", "spawns.json", "scene.glb", "collision.eodcol"])
                manifest, spawns, scene, collision = [archive.extractfile(m).read() for m in archive]
            doc = json.loads(manifest)
            expected_hash = doc.pop("sha256")
            digest = hashlib.sha256()
            for payload in (json.dumps(doc, sort_keys=True, separators=(",", ":")).encode(), spawns, scene, collision):
                digest.update(struct.pack("<Q", len(payload)))
                digest.update(payload)
            self.assertEqual(expected_hash, digest.hexdigest())
            roles = json.loads(spawns)
            self.assertEqual(len(roles["battle_respawns"]), 40)
            self.assertEqual(len(roles["grid_starts"]), 2)
            self.assertEqual(roles["grid_starts"][0]["normal"], [0, .8, .6])
            self.assertTrue(all(len(roles[k]) == 1 for k in ("checkpoints", "finish_gates", "ai_waypoints")))
            magic, count, length = struct.unpack_from("<8sII", collision)
            self.assertEqual((magic, count, length), (b"EODCOL1\0", 65539, 6))
            xyz = np.frombuffer(collision, "<f4", count * 3, 16).reshape(-1, 3)
            np.testing.assert_allclose(xyz[0], [2, 20, 8])
            np.testing.assert_allclose(xyz[1], snapshot.objects[0].world_matrix()[:3, 0] + xyz[0], atol=1e-5)
            self.assertEqual(struct.unpack_from("<3I", collision, len(collision)-12), (65536, 65537, 65538))
            self.assertEqual(struct.unpack_from("<4sII", scene), (b"glTF", 2, len(scene)))
            snapshot.markers.clear()
            with self.assertRaises(ValueError):
                export_eodmap(snapshot, "Empty starts", dest)
            self.assertEqual(dest.read_bytes(), first_bytes)
            export_eodmap(snapshot, "Camera test", dest, preview_start=(1, 2, 3, 90))
            self.assertFalse(snapshot.markers)


if __name__ == "__main__":
    unittest.main()
