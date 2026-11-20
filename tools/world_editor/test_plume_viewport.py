"""Tests for editor texture parity and the native Plume data bridge."""
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

import numpy as np

from plume_viewport import VERTEX, PlumeViewport, clip_matrix, pack_world
from project_state import build_eodhtx_from_rgba
from viewport_gl import GL, ViewportGL, lh_perspective


class PlumeViewportData(unittest.TestCase):
    def test_replacement_upload_matches_exported_hosttex_levels(self):
        rgba = bytes((sample * 13) % 256 for sample in range(5 * 3 * 4))
        texture = SimpleNamespace(width=5, height=3, orig_width=4, orig_height=2,
                                  rgba=rgba, is_replaced=True, alpha_kind="cutout")
        calls = []

        def upload(*args):
            calls.append(args)
            return True

        view = SimpleNamespace(handle=1, _resident={}, _slots={"road.tex": 2},
                               lib=SimpleNamespace(eod_plume_view_texture=upload))
        PlumeViewport._upload_texture(view, "road.tex", texture)
        PlumeViewport._upload_texture(view, "road.tex", texture)

        encoded, width, height = build_eodhtx_from_rgba(rgba, 5, 3, 4, 2)
        self.assertEqual((width, height), (8, 4))
        pixels = encoded[16:]
        self.assertEqual(calls, [(1, 2, width, height, 1, pixels, len(pixels))])

    def test_ps2_fast_uploads_exported_levels_and_restores_retail_mips(self):
        rgba = bytes((sample * 17) % 256 for sample in range(5 * 3 * 4))
        texture = SimpleNamespace(width=5, height=3, orig_width=4, orig_height=2,
                                  rgba=rgba, is_replaced=True, alpha_kind="opaque")
        view = SimpleNamespace(_gl_textures={"road.tex": (1, False)})
        with patch.multiple(GL, glPixelStorei=Mock(), glBindTexture=Mock(), glTexParameteri=Mock(),
                            glTexImage2D=Mock(), glGenerateMipmap=Mock()):
            ViewportGL._upload_one_texture(view, "road.tex", texture)
            uploads = [call.args for call in GL.glTexImage2D.call_args_list]
            self.assertEqual([args[1] for args in uploads], [0, 1, 2, 3])
            self.assertEqual([args[3:5] for args in uploads], [(8, 4), (4, 2), (2, 1), (1, 1)])
            encoded, _, _ = build_eodhtx_from_rgba(rgba, 5, 3, 4, 2)
            self.assertEqual(b"".join(args[8] for args in uploads), encoded[16:])
            GL.glGenerateMipmap.assert_not_called()

            texture.is_replaced = False
            texture.width, texture.height = 4, 2
            texture.rgba = bytes(4 * 2 * 4)
            ViewportGL._upload_one_texture(view, "road.tex", texture)
            self.assertEqual(GL.glTexImage2D.call_args.args[3:5], (4, 2))
            GL.glGenerateMipmap.assert_called_once_with(GL.GL_TEXTURE_2D)
            GL.glTexParameteri.assert_called_with(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAX_LEVEL, 2)

    def test_pack_applies_placements_visibility_and_index_offsets(self):
        part = SimpleNamespace(
            texture="surface",
            vertices=np.array([[0, 0, 0, 0, 1, 0, 0, 0],
                               [1, 0, 0, 0, 1, 0, 1, 0],
                               [0, 0, 1, 0, 1, 0, 0, 1]], dtype=np.float32),
            indices=np.array([0, 1, 2], dtype=np.uint32),
        )

        def placement(x, *, visible=True, category="static"):
            transform = np.eye(4, dtype=np.float32)
            transform[0, 3] = x
            return SimpleNamespace(category=category, model_key="model", visible=visible,
                                   world_matrix=lambda: transform)

        world = SimpleNamespace(
            objects=[placement(1), placement(3), placement(4, visible=False),
                     placement(5, category="event-prop")],
            models={"model": SimpleNamespace(parts=[part])},
            textures={"surface": SimpleNamespace(alpha_kind="opaque")},
            object_visible_in_event_template=lambda obj: obj.visible,
        )
        slots = {}
        vertices, indices, batches = pack_world(world, {"event-prop": False}, slots)
        self.assertEqual(VERTEX.itemsize, 28)
        np.testing.assert_array_equal(vertices["xyz"][:, 0], [1, 2, 1, 3, 4, 3])
        np.testing.assert_array_equal(vertices["uv"][:3], [[0, 0], [1, 0], [0, 1]])
        np.testing.assert_array_equal(indices, [0, 1, 2, 3, 4, 5])
        self.assertEqual(list(zip(batches["first"], batches["count"], batches["slot"])), [(0, 6, 1)])
        self.assertEqual(slots, {"surface": 1})
        world.textures["surface"].alpha_kind = "blend"
        _, _, batches = pack_world(world, {"event-prop": False}, slots)
        self.assertEqual(list(zip(batches["first"], batches["count"], batches["slot"])), [(0, 3, 1), (3, 3, 1)])
        self.assertGreater(float(batches["center"][1, 0]), float(batches["center"][0, 0]))
        world.objects[1].visible = False
        _, indices, _ = pack_world(world, {"event-prop": False}, slots)
        np.testing.assert_array_equal(indices, [0, 1, 2])

    def test_clip_matrix_reverses_depth_and_maps_window_y(self):
        clip = clip_matrix(lh_perspective(60, 1.5, 1, 500))
        for camera_z, depth in [(1, 1.0), (500, 0.0)]:
            projection = clip @ np.array([0, 1, camera_z, 1])
            self.assertAlmostEqual(projection[2] / projection[3], depth, places=6)
            self.assertLess(projection[1], 0)


if __name__ == "__main__":
    unittest.main()
