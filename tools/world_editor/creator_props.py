"""Small palette props available on every creator canvas."""
from collections import defaultdict

import numpy as np

from world_loader import MeshPart, TextureAsset, WorldModel


def ensure_creator_prop(world, key):
    if key in world.models or key not in ("creator:cone", "creator:barrier"):
        return
    if key == "creator:cone":
        name = "Traffic cone"
        angles = np.arange(12) * (2 * np.pi / 12)
        perimeter = np.column_stack((np.cos(angles), np.sin(angles)))
        bands = [(0., .55, "rubber"), (.09, .55, "rubber"), (.10, .42, "orange"),
                 (.50, .28, "orange"), (.68, .22, "white"), (1.12, .06, "orange")]
    else:
        name = "Course barrier"
        perimeter = np.array([[-1.6, -.42], [1.6, -.42], [1.6, .42], [-1.6, .42]])
        bands = [(0., 1., "rubber"), (.12, 1., "rubber"), (.46, .92, "orange"),
                 (.64, .88, "white"), (.94, .84, "orange")]

    rings = [np.column_stack((perimeter[:, 0] * scale, np.full(len(perimeter), height),
                              perimeter[:, 1] * scale)) for height, scale, _ in bands]
    faces = defaultdict(list)
    for j in range(1, len(rings)):
        lower, upper = rings[j - 1], rings[j]
        for i in range(len(perimeter)):
            k = (i + 1) % len(perimeter)
            faces[bands[j][2]].extend([(lower[i], upper[i], upper[k]), (lower[i], upper[k], lower[k])])
    for i in range(1, len(perimeter) - 1):
        faces[bands[0][2]].append((rings[0][0], rings[0][i], rings[0][i + 1]))
        faces[bands[-1][2]].append((rings[-1][0], rings[-1][i + 1], rings[-1][i]))

    palette = {"rubber": (54, 60, 64), "white": (242, 240, 224), "orange": (241, 104, 34)}
    parts = []
    for color, triangles in faces.items():
        texture = "creator_" + color
        rgb = palette[color]
        if texture not in world.textures:
            world.textures[texture] = TextureAsset(texture, 1, 1, bytes((*rgb, 255)), "opaque", rgb, texture)
        xyz = np.asarray(triangles, np.float32)
        normals = np.cross(xyz[:, 1] - xyz[:, 0], xyz[:, 2] - xyz[:, 0])
        normals /= np.linalg.norm(normals, axis=1, keepdims=True)
        verts = np.zeros((len(triangles) * 3, 8), np.float32)
        verts[:, :3] = xyz.reshape(-1, 3)
        verts[:, 3:6] = np.repeat(normals, 3, axis=0)
        parts.append(MeshPart(texture, verts, np.arange(len(verts), dtype=np.uint32), len(triangles)))

    bounds = np.concatenate(rings)
    world.models[key] = WorldModel(key, name, "Props", "procedural:prop", "", parts,
                                   sum(p.tri_count for p in parts), [p.texture for p in parts],
                                   bounds.min(0), bounds.max(0), (0., 0., 0.), placed_by_map=False)
