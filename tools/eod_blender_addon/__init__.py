"""EOD World Studio Bridge — Blender 4.2+ Extension Add-On.

Cleanly guarded so ``import eod_blender_addon`` works outside Blender (for CI,
unittest, and World Studio integration). Inside Blender 4.2+, registers:
  - File > Import > EOD Track (.TRK / .eodworld)
  - File > Import > EOD Vehicle (.CAR)
  - File > Export > EOD World Studio Bridge (.OBJ)
  - 3D Viewport N-Panel ("EOD Studio") with Scene Lights vs PS2 2x Prelight
    shader toggle and one-click Export Back to World Studio.
"""

from __future__ import annotations

import math
from pathlib import Path
from typing import Any

from .eod_bridge import (
    blender_rh_to_eod_lh,
    build_world_model_from_obj,
    eod_lh_to_blender_rh,
    load_car_assembly_payload,
    load_trk_scene_payload,
    parse_obj_to_mesh_parts,
)

bl_info = {
    "name": "EOD World Studio Bridge",
    "author": "EOD Recomp Project",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "File > Import/Export & View3D > Sidebar > EOD Studio",
    "description": "Two-way Eve of Destruction (.TRK / .eodworld / .CAR) bridge for Blender 4.2+",
    "category": "Import-Export",
}

ADDON_ID = "eod_world_studio"

try:
    import bpy  # type: ignore[import-not-found]
    from bpy.props import EnumProperty, StringProperty  # type: ignore[import-not-found]
    from bpy.types import Operator, Panel  # type: ignore[import-not-found]
    from bpy_extras.io_utils import ExportHelper, ImportHelper  # type: ignore[import-not-found]

    HAS_BPY = True
except ImportError:
    bpy = None  # type: ignore[assignment]
    HAS_BPY = False

__all__ = [
    "ADDON_ID",
    "HAS_BPY",
    "bl_info",
    "blender_rh_to_eod_lh",
    "build_world_model_from_obj",
    "eod_lh_to_blender_rh",
    "load_car_assembly_payload",
    "load_trk_scene_payload",
    "parse_obj_to_mesh_parts",
    "register",
    "unregister",
]


if HAS_BPY:
    def _build_image_from_rgba(name: str, width: int, height: int, rgba: bytes) -> Any:
        img = bpy.data.images.get(name) or bpy.data.images.new(name, width=width, height=height, alpha=True)
        if img.size[0] != width or img.size[1] != height:
            img.scale(width, height)
        # Flip vertical rows for Blender bottom-left image origin and normalize to [0, 1]
        floats = [b / 255.0 for b in rgba]
        row_len = width * 4
        flipped: list[float] = []
        for row in range(height - 1, -1, -1):
            start = row * row_len
            flipped.extend(floats[start : start + row_len])
        img.pixels.foreach_set(flipped)
        img.pack()
        return img

    def _configure_eod_material(mat: Any, tex_img: Any | None, mode: str) -> None:
        mat.use_nodes = True
        mat["eod_material"] = True
        nt = mat.node_tree
        nt.nodes.clear()

        out = nt.nodes.new("ShaderNodeOutputMaterial")
        out.location = (520, 0)
        tex_node = None
        if tex_img is not None:
            tex_node = nt.nodes.new("ShaderNodeTexImage")
            tex_node.image = tex_img
            tex_node.location = (-320, 80)

        if mode == "PS2_PRELIGHT":
            # PS2 GS 2x prelight modulation (unlit 2x RGB matching hardware TFUNC)
            emit = nt.nodes.new("ShaderNodeEmission")
            emit.location = (240, 80)
            emit.inputs["Strength"].default_value = 1.0
            if tex_node is not None:
                mul = nt.nodes.new("ShaderNodeVectorMath")
                mul.operation = "SCALE"
                mul.inputs["Scale"].default_value = 2.0
                mul.location = (-20, 80)
                nt.links.new(tex_node.outputs["Color"], mul.inputs[0])
                nt.links.new(mul.outputs["Vector"], emit.inputs["Color"])
            nt.links.new(emit.outputs["Emission"], out.inputs["Surface"])
        else:
            bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
            bsdf.location = (180, 80)
            bsdf.inputs["Roughness"].default_value = 0.78
            if tex_node is not None:
                nt.links.new(tex_node.outputs["Color"], bsdf.inputs["Base Color"])
                nt.links.new(tex_node.outputs["Alpha"], bsdf.inputs["Alpha"])
            nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])

    def _mesh_from_parts(name: str, parts: list[dict[str, Any]], mats: dict[str, Any]) -> Any:
        verts: list[tuple[float, float, float]] = []
        faces: list[tuple[int, int, int]] = []
        uvs: list[tuple[float, float]] = []
        mat_indices: list[int] = []
        mat_slots: list[Any] = []
        slot_lookup: dict[str, int] = {}

        for p in parts:
            tname = p.get("texture") or "untextured"
            if tname not in slot_lookup:
                slot_lookup[tname] = len(mat_slots)
                mat_slots.append(mats.get(tname))
            base = len(verts)
            verts.extend(p["verts"])
            uvs.extend(p["uvs"])
            for a, b, c in p["tris"]:
                faces.append((base + a, base + b, base + c))
                mat_indices.append(slot_lookup[tname])

        mesh = bpy.data.meshes.new(name)
        mesh.from_pydata(verts, [], faces)
        for m in mat_slots:
            if m is not None:
                mesh.materials.append(m)
        if faces:
            uv_layer = mesh.uv_layers.new(name="UVMap")
            for poly, midx in zip(mesh.polygons, mat_indices):
                poly.material_index = midx
                for loop_idx in poly.loop_indices:
                    v_idx = mesh.loops[loop_idx].vertex_index
                    uv_layer.data[loop_idx].uv = uvs[v_idx]
        mesh.update()
        return mesh

    class EOD_OT_import_track(Operator, ImportHelper):
        """Import an EOD .TRK or .eodworld scene into Blender."""

        bl_idname = "eod_studio.import_track"
        bl_label = "Import EOD Track (.TRK / .eodworld)"
        bl_options = {"REGISTER", "UNDO"}
        filename_ext = ".TRK"
        filter_glob: StringProperty(default="*.TRK;*.trk;*.eodworld", options={"HIDDEN"})  # type: ignore[valid-type]

        def execute(self, context: Any) -> set[str]:
            payload = load_trk_scene_payload(Path(self.filepath))
            mode = getattr(context.scene, "eod_lighting_mode", "SCENE_LIGHTS")
            col = bpy.data.collections.new(f"EOD_{payload['track_name']}")
            context.scene.collection.children.link(col)

            mats: dict[str, Any] = {}
            for tname, tinfo in payload["textures"].items():
                img = _build_image_from_rgba(tname, tinfo["width"], tinfo["height"], tinfo["rgba"])
                mat = bpy.data.materials.new(name=Path(tname).stem or tname)
                _configure_eod_material(mat, img, mode)
                mats[tname] = mat

            mesh_cache: dict[str, Any] = {}
            for mkey, minfo in payload["models"].items():
                if minfo["parts"]:
                    mesh_cache[mkey] = _mesh_from_parts(minfo["name"], minfo["parts"], mats)

            for oinfo in payload["objects"]:
                mesh = mesh_cache.get(oinfo["model_key"])
                if mesh is None:
                    continue
                obj = bpy.data.objects.new(oinfo["name"], mesh)
                obj.location = oinfo["position_blender"]
                h, p, r = (math.radians(a) for a in oinfo["rotation_hpr_deg"])
                obj.rotation_euler = (p, r, -h)
                obj.scale = oinfo["scale_blender"]
                col.objects.link(obj)
            self.report({"INFO"}, f"Imported EOD track {payload['track_name']} ({len(payload['objects'])} objects)")
            return {"FINISHED"}

    class EOD_OT_import_car(Operator, ImportHelper):
        """Import an EOD .CAR vehicle with wheels and Bone.mvl damage bones."""

        bl_idname = "eod_studio.import_car"
        bl_label = "Import EOD Vehicle (.CAR)"
        bl_options = {"REGISTER", "UNDO"}
        filename_ext = ".CAR"
        filter_glob: StringProperty(default="*.CAR;*.car", options={"HIDDEN"})  # type: ignore[valid-type]

        def execute(self, context: Any) -> set[str]:
            payload = load_car_assembly_payload(Path(self.filepath))
            mode = getattr(context.scene, "eod_lighting_mode", "SCENE_LIGHTS")
            col = bpy.data.collections.new(f"CAR_{payload['car_name']}")
            context.scene.collection.children.link(col)

            mats: dict[str, Any] = {}
            for tname, tinfo in payload["textures"].items():
                img = _build_image_from_rgba(tname, tinfo["width"], tinfo["height"], tinfo["rgba"])
                mat = bpy.data.materials.new(name=Path(tname).stem or tname)
                _configure_eod_material(mat, img, mode)
                mats[tname] = mat

            body_mesh = _mesh_from_parts(f"{payload['car_name']}_Body", payload["body_parts"], mats)
            body_obj = bpy.data.objects.new(f"{payload['car_name']}_Body", body_mesh)
            col.objects.link(body_obj)

            wheel_mesh = (
                _mesh_from_parts(f"{payload['car_name']}_Wheel", payload["wheel_parts"], mats)
                if payload["wheel_parts"]
                else None
            )
            for w in payload["wheels"]:
                wobj = bpy.data.objects.new(w["name"], wheel_mesh)
                wobj.location = w["position_blender"]
                if w["flip_side"]:
                    wobj.rotation_euler = (0.0, 0.0, math.pi)
                wobj.parent = body_obj
                col.objects.link(wobj)

            for b in payload["bones"]:
                empty = bpy.data.objects.new(b["name"], None)
                empty.empty_display_type = "SPHERE"
                empty.empty_display_size = 0.08
                empty.location = b["position_blender"]
                empty.parent = body_obj
                col.objects.link(empty)

            self.report(
                {"INFO"},
                f"Imported {payload['car_name']} ({len(payload['wheels'])} wheels, {len(payload['bones'])} damage bones)",
            )
            return {"FINISHED"}

    class EOD_OT_export_studio_obj(Operator, ExportHelper):
        """Export selected mesh back to EOD World Studio as Blender RH Z-up OBJ+MTL."""

        bl_idname = "eod_studio.export_studio_obj"
        bl_label = "Export Back to World Studio (.OBJ)"
        filename_ext = ".obj"
        filter_glob: StringProperty(default="*.obj", options={"HIDDEN"})  # type: ignore[valid-type]

        def execute(self, context: Any) -> set[str]:
            out = Path(self.filepath)
            out.parent.mkdir(parents=True, exist_ok=True)
            bpy.ops.wm.obj_export(
                filepath=str(out),
                export_selected_objects=True,
                export_materials=True,
                export_uv=True,
                export_normals=True,
                forward_axis="Y",
                up_axis="Z",
            )
            self.report({"INFO"}, f"Exported for World Studio Sync -> {out}")
            return {"FINISHED"}

    class EOD_OT_set_lighting_mode(Operator):
        """Switch EOD material shaders between Scene Lights and PS2 2x Prelight."""

        bl_idname = "eod_studio.set_lighting_mode"
        bl_label = "Apply EOD Lighting Mode"
        mode: EnumProperty(  # type: ignore[valid-type]
            items=[
                ("SCENE_LIGHTS", "Scene Lights", "Principled BSDF using Blender scene lights"),
                ("PS2_PRELIGHT", "PS2 2x Prelight", "Unlit PS2 GS 2x prelight shader preview"),
            ],
            default="SCENE_LIGHTS",
        )

        def execute(self, context: Any) -> set[str]:
            context.scene.eod_lighting_mode = self.mode
            count = 0
            for mat in bpy.data.materials:
                if not mat.get("eod_material"):
                    continue
                tex_img = None
                if mat.use_nodes and mat.node_tree:
                    for node in mat.node_tree.nodes:
                        if node.type == "TEX_IMAGE" and node.image is not None:
                            tex_img = node.image
                            break
                _configure_eod_material(mat, tex_img, self.mode)
                count += 1
            self.report({"INFO"}, f"Switched {count} EOD materials to {self.mode}")
            return {"FINISHED"}

    class EOD_PT_world_studio(Panel):
        """3D Viewport sidebar panel for EOD World Studio two-way workflow."""

        bl_label = "EOD World Studio"
        bl_idname = "EOD_PT_world_studio"
        bl_space_type = "VIEW_3D"
        bl_region_type = "UI"
        bl_category = "EOD Studio"

        def draw(self, context: Any) -> None:
            layout = self.layout
            col = layout.column(align=True)
            col.label(text="Viewport Shader Mode:")
            row = col.row(align=True)
            op_lit = row.operator(EOD_OT_set_lighting_mode.bl_idname, text="Scene Lights")
            op_lit.mode = "SCENE_LIGHTS"
            op_ps2 = row.operator(EOD_OT_set_lighting_mode.bl_idname, text="PS2 2x Prelight")
            op_ps2.mode = "PS2_PRELIGHT"
            layout.separator()
            col2 = layout.column(align=True)
            col2.operator(EOD_OT_import_track.bl_idname, text="Import Track (.TRK / .eodworld)")
            col2.operator(EOD_OT_import_car.bl_idname, text="Import Vehicle (.CAR)")
            layout.separator()
            layout.operator(EOD_OT_export_studio_obj.bl_idname, text="Export Back to World Studio")

    _CLASSES = (
        EOD_OT_import_track,
        EOD_OT_import_car,
        EOD_OT_export_studio_obj,
        EOD_OT_set_lighting_mode,
        EOD_PT_world_studio,
    )

    def _menu_import(self: Any, _context: Any) -> None:
        self.layout.operator(EOD_OT_import_track.bl_idname, text="EOD Track (.TRK / .eodworld)")
        self.layout.operator(EOD_OT_import_car.bl_idname, text="EOD Vehicle (.CAR)")

    def _menu_export(self: Any, _context: Any) -> None:
        self.layout.operator(EOD_OT_export_studio_obj.bl_idname, text="EOD World Studio Bridge (.OBJ)")


def register() -> None:
    if not HAS_BPY:
        return
    bpy.types.Scene.eod_lighting_mode = EnumProperty(
        name="EOD Lighting",
        items=[
            ("SCENE_LIGHTS", "Scene Lights", "Principled BSDF using Blender scene lights"),
            ("PS2_PRELIGHT", "PS2 2x Prelight", "PS2 GS 2x prelight shader preview"),
        ],
        default="SCENE_LIGHTS",
    )
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_import.append(_menu_import)
    bpy.types.TOPBAR_MT_file_export.append(_menu_export)


def unregister() -> None:
    if not HAS_BPY:
        return
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)
    bpy.types.TOPBAR_MT_file_export.remove(_menu_export)
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
    if hasattr(bpy.types.Scene, "eod_lighting_mode"):
        del bpy.types.Scene.eod_lighting_mode
