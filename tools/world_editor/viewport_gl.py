"""Hardware-Accelerated OpenGL 3.3 Core 3D Viewport (QOpenGLWidget) for EOD World Editor.

Features:
  - OpenGL 3.3 Core shaders: sky gradient dome, lit/textured world with fog & alpha cutout,
    bright translucent green object selection highlight, unlit line/ribbon/gizmo shader.
  - Texture-batched static VBOs for >60 FPS on 2,000+ retail instances.
  - Native EOD Left-Handed camera (+X right, +Y up, +Z forward): right-drag look,
    WASD move, Q/E down/up, two-finger pan, pinch to dolly, Shift boost.
  - Interactive 3D Move (R/G/B arrows), Rotate (R/G/B rings), and Scale (R/G/B cubes) gizmos.
  - 3D raycast picking of objects, road nodes, road segments, markers, and terrain.
  - Road Tool (ILIN) 3D ribbon, yellow/magenta diamond handles, Green/Cyan/White hover feedback.
  - Floating Qt overlays: top-left Object/Edit/Shield pill bar + top-center Road Tool HUD card.
"""

from __future__ import annotations

import ctypes
import math
import os
import random
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np
import OpenGL
OpenGL.ERROR_CHECKING = False
OpenGL.ERROR_LOGGING = False
from OpenGL import GL
from PySide6.QtCore import QEvent, QPoint, QPointF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QFont, QMouseEvent, QNativeGestureEvent, QPainter, QWheelEvent
from PySide6.QtOpenGLWidgets import QOpenGLWidget
from PySide6.QtWidgets import (
    QCheckBox,
    QDoubleSpinBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)
from plume_viewport import PlumeViewport

from perf_profiler import GpuPassTimer, PerfHudOverlay, get_profiler
from project_state import build_eodhtx_from_rgba
from theme import (
    ACCENT_BLUE,
    ACCENT_BLUE_HOVER,
    BG_ELEVATED,
    BG_HOVER,
    BORDER_LIGHT,
    TEXT_PRIMARY,
    TEXT_SECONDARY,
    svg_icon,
)
from world_loader import (
    TrackWorld,
    WorldMarker,
    WorldObject,
    compose_transform,
    normal_to_pitch_roll,
)
import world_collision as wc

MIME_MODEL_KEY = "application/x-eod-model-key"


# ---------------------------------------------------------------------------
# GLSL 3.30 Core Shaders
# ---------------------------------------------------------------------------
_SKY_VERT = """
#version 330 core
out vec2 vNdc;
void main() {
    vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vNdc = pos[gl_VertexID];
    gl_Position = vec4(pos[gl_VertexID], 0.9999, 1.0);
}
"""

_SKY_FRAG = """
#version 330 core
in vec2 vNdc;
out vec4 FragColor;
uniform vec3 uCamFwd;
uniform vec3 uCamRight;
uniform vec3 uCamUp;
uniform float uTanHalfFov;
uniform float uAspect;
uniform vec3 uSkyZenith;
uniform vec3 uSkyMid;
uniform vec3 uSkyHorizon;
uniform vec3 uSkyGround;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
void main() {
    vec3 dir = normalize(
        uCamFwd
        + uCamRight * (vNdc.x * uAspect * uTanHalfFov)
        + uCamUp * (vNdc.y * uTanHalfFov)
    );
    float y = clamp(dir.y, -1.0, 1.0);
    vec3 col;
    if (y >= 0.0) {
        float t1 = smoothstep(0.0, 0.25, y);
        float t2 = smoothstep(0.25, 0.85, y);
        col = mix(uSkyHorizon, uSkyMid, t1);
        col = mix(col, uSkyZenith, t2);
        float sunDot = max(0.0, dot(dir, normalize(uSunDir)));
        col += uSunColor * pow(sunDot, 28.0) * 0.35;
        col += uSunColor * smoothstep(0.9982, 0.9994, sunDot) * 1.15;
    } else {
        col = mix(uSkyHorizon, uSkyGround, smoothstep(0.0, 0.18, -y));
    }
    FragColor = vec4(col, 1.0);
}
"""

_WORLD_VERT = """
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

uniform mat4 uViewProj;
uniform mat4 uModel;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;

void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vec3 wn = mat3(uModel) * aNormal;
    float nl = length(wn);
    vNormal = (nl > 1e-4) ? (wn / nl) : vec3(0.0, 1.0, 0.0);
    vUv = aUv;
    gl_Position = uViewProj * wp;
}
"""

_WORLD_FRAG = """
#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;

uniform sampler2D uTex;
uniform int uHasTex;
uniform int uCutout;
uniform int uLighting;
uniform int uRenderMode; // 0 = PS2 Performance, 1 = Plume High-Quality
uniform vec3 uCamPos;
uniform vec4 uTint;
uniform float uHighlightMix;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform float uAmbientIntensity;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogDensity;

out vec4 FragColor;

void main() {
    vec4 baseCol = vec4(0.66, 0.69, 0.74, 1.0);
    if (uHasTex != 0) {
        baseCol = texture(uTex, vUv);
        if (uCutout != 0 && baseCol.a < 0.45) {
            discard;
        }
    }
    vec3 rgb = baseCol.rgb * uTint.rgb;
    if (uLighting != 0) {
        float nl = length(vNormal);
        vec3 n = (nl > 1e-4) ? (vNormal / nl) : vec3(0.0, 1.0, 0.0);
        float sl = length(uSunDir);
        vec3 sunDir = (sl > 1e-4) ? (uSunDir / sl) : vec3(0.45, 0.82, 0.36);

        // Two-sided soft wrap lighting for PS2 meshes with mixed strip winding
        float ndlTwoSided = abs(dot(n, sunDir));
        float halfLambert = clamp(ndlTwoSided * 0.65 + 0.35, 0.0, 1.0);
        float hemi = clamp(abs(n.y) * 0.5 + 0.5, 0.0, 1.0);

        if (uRenderMode == 0) {
            // PS2 Performance Mode: bright, clean PS2-era outdoor lighting
            vec3 amb = mix(vec3(0.46, 0.48, 0.52), vec3(0.64, 0.68, 0.74), hemi) * uAmbientIntensity;
            vec3 diff = uSunColor * (0.44 * halfLambert);
            rgb = rgb * clamp(amb + diff, vec3(0.35), vec3(1.25));
        } else {
            // Plume High-Quality Mode: rich sun + warm ground bounce + subtle Fresnel sheen
            vec3 viewDir = normalize(uCamPos - vWorldPos);
            vec3 halfVec = normalize(sunDir + viewDir);
            float spec = pow(abs(dot(n, halfVec)), 28.0) * 0.14 * ndlTwoSided;
            float fresnel = pow(1.0 - abs(dot(n, viewDir)), 3.0) * 0.07;
            vec3 skyAmb = mix(vec3(0.38, 0.38, 0.40), vec3(0.58, 0.65, 0.76), hemi) * uAmbientIntensity;
            vec3 sunLight = uSunColor * (0.26 * halfLambert + 0.42 * ndlTwoSided);
            rgb = rgb * clamp(skyAmb + sunLight, vec3(0.30), vec3(1.35)) + uSunColor * spec + uFogColor * fresnel;
        }
    }
    if (uHighlightMix > 0.001) {
        vec3 selGreen = vec3(0.16, 0.94, 0.44);
        rgb = mix(rgb, selGreen, uHighlightMix);
    }
    float dist = length(vWorldPos - uCamPos);
    float fogT = clamp((dist - uFogStart) / max(1.0, (uFogEnd - uFogStart)), 0.0, 1.0);
    if (uRenderMode == 1) {
        fogT = fogT * fogT * (3.0 - 2.0 * fogT);
    }
    rgb = mix(rgb, uFogColor, fogT * uFogDensity);
    FragColor = vec4(rgb, uTint.a);
}
"""

_FLAT_VERT = """
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uViewProj;
uniform mat4 uModel;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = uViewProj * uModel * vec4(aPos, 1.0);
}
"""

_ID_VERT = """
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUv;
layout(location = 3) in float aObjId;
uniform mat4 uViewProj;
uniform mat4 uModel;
out vec2 vUv;
flat out float vObjId;
void main() {
    vUv = aUv;
    vObjId = aObjId;
    gl_Position = uViewProj * uModel * vec4(aPos, 1.0);
}
"""

_ID_FRAG = """
#version 330 core
in vec2 vUv;
flat in float vObjId;
uniform sampler2D uTex;
uniform int uHasTex;
uniform int uCutout;
out vec4 FragColor;
void main() {
    if (uHasTex != 0 && uCutout != 0) {
        float thresh = (uCutout == 2) ? 0.15 : 0.45;
        if (texture(uTex, vUv).a < thresh) {
            discard;
        }
    }
    int id = int(vObjId + 0.5);
    float r = float(id & 255) / 255.0;
    float g = float((id >> 8) & 255) / 255.0;
    float b = float((id >> 16) & 255) / 255.0;
    FragColor = vec4(r, g, b, 1.0);
}
"""

_FLAT_FRAG = """
#version 330 core
in vec4 vColor;
uniform vec4 uOverrideColor;
uniform int uUseOverride;
out vec4 FragColor;
void main() {
    FragColor = (uUseOverride != 0) ? uOverrideColor : vColor;
}
"""


def _compile_program(vert_src: str, frag_src: str) -> int:
    vs = GL.glCreateShader(GL.GL_VERTEX_SHADER)
    GL.glShaderSource(vs, vert_src)
    GL.glCompileShader(vs)
    if not GL.glGetShaderiv(vs, GL.GL_COMPILE_STATUS):
        raise RuntimeError(f"Vertex shader error: {GL.glGetShaderInfoLog(vs).decode()}")

    fs = GL.glCreateShader(GL.GL_FRAGMENT_SHADER)
    GL.glShaderSource(fs, frag_src)
    GL.glCompileShader(fs)
    if not GL.glGetShaderiv(fs, GL.GL_COMPILE_STATUS):
        raise RuntimeError(f"Fragment shader error: {GL.glGetShaderInfoLog(fs).decode()}")

    prog = GL.glCreateProgram()
    GL.glAttachShader(prog, vs)
    GL.glAttachShader(prog, fs)
    GL.glLinkProgram(prog)
    if not GL.glGetProgramiv(prog, GL.GL_LINK_STATUS):
        raise RuntimeError(f"Shader link error: {GL.glGetProgramInfoLog(prog).decode()}")
    GL.glDeleteShader(vs)
    GL.glDeleteShader(fs)
    return int(prog)


# ---------------------------------------------------------------------------
# Left-Handed Camera Math (+X right, +Y up, +Z forward)
# ---------------------------------------------------------------------------
def _normalize(v: np.ndarray) -> np.ndarray:
    n = float(np.linalg.norm(v))
    return v / n if n > 1e-7 else v


def lh_look_matrix(eye: np.ndarray, fwd: np.ndarray, right: np.ndarray, up: np.ndarray) -> np.ndarray:
    """Row-major 4x4 LH view matrix transforming world -> camera (+X right, +Y up, +Z forward)."""
    m = np.eye(4, dtype=np.float32)
    m[0, 0:3] = right
    m[1, 0:3] = up
    m[2, 0:3] = fwd
    m[0, 3] = -float(np.dot(right, eye))
    m[1, 3] = -float(np.dot(up, eye))
    m[2, 3] = -float(np.dot(fwd, eye))
    return m


def lh_perspective(fov_y_deg: float, aspect: float, near: float, far: float) -> np.ndarray:
    """Row-major 4x4 LH perspective projection for OpenGL NDC z in [-1, 1], w_clip = +z_cam."""
    f = 1.0 / math.tan(math.radians(fov_y_deg) * 0.5)
    m = np.zeros((4, 4), dtype=np.float32)
    m[0, 0] = f / max(aspect, 0.01)
    m[1, 1] = f
    m[2, 2] = (far + near) / (far - near)
    m[2, 3] = -(2.0 * far * near) / (far - near)
    m[3, 2] = 1.0
    return m


# ---------------------------------------------------------------------------
# Raycasting Helpers
# ---------------------------------------------------------------------------
def ray_aabb_hit(origin: np.ndarray, inv_dir: np.ndarray, lo: np.ndarray, hi: np.ndarray) -> float | None:
    t1 = (lo - origin) * inv_dir
    t2 = (hi - origin) * inv_dir
    tmin = float(np.max(np.minimum(t1, t2)))
    tmax = float(np.min(np.maximum(t1, t2)))
    if tmax >= max(0.0, tmin):
        return tmin if tmin >= 0.0 else 0.0
    return None


def ray_mesh_hit(
    origin: np.ndarray,
    direction: np.ndarray,
    verts: np.ndarray,
    indices: np.ndarray,
) -> float | None:
    """Vectorized Möller–Trumbore ray vs triangle array intersection."""
    if len(indices) == 0:
        return None
    tris = indices.reshape(-1, 3)
    v0 = verts[tris[:, 0]]
    v1 = verts[tris[:, 1]]
    v2 = verts[tris[:, 2]]
    e1 = v1 - v0
    e2 = v2 - v0
    pvec = np.cross(direction, e2)
    det = np.einsum("ij,ij->i", e1, pvec)
    mask = np.abs(det) > 1e-6
    if not np.any(mask):
        return None
    inv_det = 1.0 / det[mask]
    tvec = origin - v0[mask]
    u = np.einsum("ij,ij->i", tvec, pvec[mask]) * inv_det
    m2 = (u >= 0.0) & (u <= 1.0)
    if not np.any(m2):
        return None
    qvec = np.cross(tvec[m2], e1[mask][m2])
    v = np.einsum("j,ij->i", direction, qvec) * inv_det[m2]
    m3 = (v >= 0.0) & (u[m2] + v <= 1.0)
    if not np.any(m3):
        return None
    t = np.einsum("ij,ij->i", e2[mask][m2][m3], qvec[m3]) * inv_det[m2][m3]
    valid = t[t > 0.05]
    if len(valid) == 0:
        return None
    return float(valid.min())


def closest_point_ray_to_line(
    ray_o: np.ndarray, ray_d: np.ndarray, line_p: np.ndarray, line_d: np.ndarray
) -> tuple[float, float]:
    """Return (param_on_line, dist_between_lines)."""
    w0 = ray_o - line_p
    a = float(np.dot(ray_d, ray_d))
    b = float(np.dot(ray_d, line_d))
    c = float(np.dot(line_d, line_d))
    d = float(np.dot(ray_d, w0))
    e = float(np.dot(line_d, w0))
    denom = a * c - b * b
    if abs(denom) < 1e-7:
        return 0.0, 1e9
    sc = (b * e - c * d) / denom
    tc = (a * e - b * d) / denom
    sc = max(0.0, sc)
    p1 = ray_o + sc * ray_d
    p2 = line_p + tc * line_d
    return tc, float(np.linalg.norm(p1 - p2))


def ray_plane_intersect(
    ray_o: np.ndarray, ray_d: np.ndarray, plane_p: np.ndarray, plane_n: np.ndarray
) -> np.ndarray | None:
    denom = float(np.dot(ray_d, plane_n))
    if abs(denom) < 1e-6:
        return None
    t = float(np.dot(plane_p - ray_o, plane_n)) / denom
    if t < 0.0:
        return None
    return ray_o + t * ray_d


def _point_segment_distance(pt: np.ndarray, a: np.ndarray, b: np.ndarray) -> tuple[float, np.ndarray]:
    ab = b - a
    ab2 = float(np.dot(ab, ab))
    if ab2 < 1e-8:
        return float(np.linalg.norm(pt - a)), a
    t = max(0.0, min(1.0, float(np.dot(pt - a, ab)) / ab2))
    proj = a + t * ab
    return float(np.linalg.norm(pt - proj)), proj


# ---------------------------------------------------------------------------
# Floating Qt Overlays (Top-Left Mode Pill Bar & Top-Center Road Tool HUD)
# ---------------------------------------------------------------------------
class ModePillOverlay(QFrame):
    modeChanged = Signal(str)  # "object" | "edit"
    shieldToggled = Signal(bool)

    def __init__(self, parent: QWidget):
        super().__init__(parent)
        self.setProperty("hud", True)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(5, 4, 5, 4)
        layout.setSpacing(4)

        self.btn_object = QPushButton(" Object Mode")
        self.btn_object.setIcon(svg_icon("object_mode"))
        self.btn_object.setCheckable(True)
        self.btn_object.setChecked(True)
        self.btn_object.setCursor(Qt.CursorShape.PointingHandCursor)

        self.btn_edit = QPushButton(" Edit Mode")
        self.btn_edit.setIcon(svg_icon("edit_mode"))
        self.btn_edit.setCheckable(True)
        self.btn_edit.setCursor(Qt.CursorShape.PointingHandCursor)

        self.btn_shield = QPushButton(" Collision")
        self.btn_shield.setIcon(svg_icon("shield"))
        self.btn_shield.setCheckable(True)
        self.btn_shield.setToolTip("Toggle Terrain & Barrier Collision Overlay")
        self.btn_shield.setCursor(Qt.CursorShape.PointingHandCursor)

        pill_css = (
            "QPushButton { background: transparent; border: 1px solid transparent; "
            "border-radius: 5px; padding: 4px 10px; color: #c7ccd8; font-weight: 600; font-size: 8.5pt; }"
            "QPushButton:hover { background: rgba(255,255,255,0.08); color: #ffffff; }"
            f"QPushButton:checked {{ background: {ACCENT_BLUE}; border-color: {ACCENT_BLUE_HOVER}; color: #ffffff; }}"
        )
        for b in (self.btn_object, self.btn_edit, self.btn_shield):
            b.setStyleSheet(pill_css)

        # Hide legacy Potemkin Object/Edit mode toggles until real vertex editing is wired
        self.btn_object.hide()
        self.btn_edit.hide()
        layout.addWidget(self.btn_shield)

        self.btn_object.clicked.connect(lambda: self.set_mode("object", emit=True))
        self.btn_edit.clicked.connect(lambda: self.set_mode("edit", emit=True))
        self.btn_shield.toggled.connect(self.shieldToggled.emit)
        self.adjustSize()

    def set_mode(self, mode: str, emit: bool = False) -> None:
        self.btn_object.setChecked(mode == "object")
        self.btn_edit.setChecked(mode == "edit")
        if emit:
            self.modeChanged.emit(mode)


class RoadToolHudOverlay(QFrame):
    endRoadClicked = Signal()
    closeToolClicked = Signal()
    lanesChanged = Signal(int)

    def __init__(self, parent: QWidget):
        super().__init__(parent)
        self.setProperty("hud", True)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.lanes = 2
        self.width_m = 14.0

        outer = QVBoxLayout(self)
        outer.setContentsMargins(12, 8, 12, 8)
        outer.setSpacing(6)

        row1 = QHBoxLayout()
        row1.setSpacing(10)
        self.title_lbl = QLabel("<b>Draw racing line (ILIN)</b>")
        self.title_lbl.setStyleSheet("color: #ffffff; background: transparent; font-size: 9.5pt;")
        self.spec_lbl = QLabel("14.0 m · 2 lanes")
        self.spec_lbl.setStyleSheet(
            "color: #93c5fd; background: rgba(45, 127, 249, 0.18); "
            "border: 1px solid rgba(45, 127, 249, 0.45); border-radius: 4px; "
            "padding: 2px 7px; font-weight: 600; font-size: 8.5pt;"
        )
        btn_minus = QPushButton("- Lane")
        btn_plus = QPushButton("+ Lane")
        for b in (btn_minus, btn_plus):
            b.setFixedHeight(22)
            b.setStyleSheet(
                "QPushButton { background: rgba(255,255,255,0.08); color: #e6e9f0; "
                "border: 1px solid #3f434e; border-radius: 4px; padding: 1px 7px; font-size: 8pt; }"
                "QPushButton:hover { background: rgba(255,255,255,0.16); }"
            )
        btn_minus.clicked.connect(lambda: self._step_lanes(-1))
        btn_plus.clicked.connect(lambda: self._step_lanes(1))

        row1.addWidget(self.title_lbl)
        row1.addWidget(self.spec_lbl)
        row1.addStretch(1)
        row1.addWidget(btn_minus)
        row1.addWidget(btn_plus)
        outer.addLayout(row1)

        legend = QLabel(
            '<span style="color:#2ecc71; font-weight:600;">● Green:</span> join node &nbsp;·&nbsp; '
            '<span style="color:#22d3ee; font-weight:600;">● Cyan:</span> split line &nbsp;·&nbsp; '
            '<span style="color:#f8fafc; font-weight:600;">● White:</span> new node'
        )
        legend.setStyleSheet("color: #cbd5e1; background: transparent; font-size: 8.5pt;")
        outer.addWidget(legend)

        row3 = QHBoxLayout()
        row3.setSpacing(6)
        self.btn_end = QPushButton("End this road (Enter)")
        self.btn_end.setProperty("primary", True)
        self.btn_end.setFixedHeight(26)
        self.btn_close = QPushButton("Close the tool (Esc)")
        self.btn_close.setFixedHeight(26)
        row3.addWidget(self.btn_end)
        row3.addWidget(self.btn_close)
        outer.addLayout(row3)

        self.btn_end.clicked.connect(self.endRoadClicked.emit)
        self.btn_close.clicked.connect(self.closeToolClicked.emit)
        self.setMinimumWidth(370)
        self.adjustSize()
        self.hide()

    def _step_lanes(self, delta: int) -> None:
        self.lanes = max(1, min(6, self.lanes + delta))
        self.width_m = max(6.0, self.lanes * 6.5)
        self.spec_lbl.setText(f"{self.width_m:.1f} m · {self.lanes} lane{'s' if self.lanes != 1 else ''}")
        self.lanesChanged.emit(self.lanes)


class ScatterHudOverlay(QFrame):
    """Lumion / Twinmotion floating Scatter & Paint Brush HUD."""

    closeToolClicked = Signal()

    def __init__(self, parent: QWidget):
        super().__init__(parent)
        self.setProperty("hud", True)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(12, 8, 12, 8)
        outer.setSpacing(6)

        row1 = QHBoxLayout()
        row1.setSpacing(8)
        title = QLabel("<b>Scatter / Paint Brush</b>")
        title.setStyleSheet("color: #ffffff; background: transparent; font-size: 9.5pt;")
        self.model_lbl = QLabel("Select a model or car in Explorer")
        self.model_lbl.setStyleSheet(
            "color: #6ee7b7; background: rgba(46, 204, 113, 0.16); "
            "border: 1px solid rgba(46, 204, 113, 0.42); border-radius: 4px; "
            "padding: 2px 7px; font-weight: 600; font-size: 8pt;"
        )
        btn_close = QPushButton("Close (Esc)")
        btn_close.setFixedHeight(22)
        btn_close.clicked.connect(self.closeToolClicked.emit)
        row1.addWidget(title)
        row1.addWidget(self.model_lbl, 1)
        row1.addWidget(btn_close)
        outer.addLayout(row1)

        row2 = QHBoxLayout()
        row2.setSpacing(8)

        lbl_r = QLabel("Radius")
        lbl_r.setStyleSheet("color: #cbd5e1; background: transparent; font-size: 8pt;")
        self.spn_radius = QDoubleSpinBox()
        self.spn_radius.setRange(2.0, 60.0)
        self.spn_radius.setSingleStep(2.0)
        self.spn_radius.setValue(14.0)
        self.spn_radius.setSuffix(" m")
        self.spn_radius.setFixedWidth(68)

        lbl_d = QLabel("Density")
        lbl_d.setStyleSheet("color: #cbd5e1; background: transparent; font-size: 8pt;")
        self.spn_density = QSpinBox()
        self.spn_density.setRange(1, 20)
        self.spn_density.setValue(4)
        self.spn_density.setFixedWidth(48)

        lbl_s = QLabel("Scale ±")
        lbl_s.setStyleSheet("color: #cbd5e1; background: transparent; font-size: 8pt;")
        self.spn_jitter = QSpinBox()
        self.spn_jitter.setRange(0, 50)
        self.spn_jitter.setValue(15)
        self.spn_jitter.setSuffix("%")
        self.spn_jitter.setFixedWidth(56)

        row2.addWidget(lbl_r)
        row2.addWidget(self.spn_radius)
        row2.addWidget(lbl_d)
        row2.addWidget(self.spn_density)
        row2.addWidget(lbl_s)
        row2.addWidget(self.spn_jitter)
        outer.addLayout(row2)

        row3 = QHBoxLayout()
        row3.setSpacing(12)
        self.chk_rand_yaw = QCheckBox("Random Yaw")
        self.chk_rand_yaw.setChecked(True)
        self.chk_align_normal = QCheckBox("Align to Normal")
        self.chk_align_normal.setChecked(True)
        hint = QLabel("LMB click/drag to paint")
        hint.setStyleSheet("color: #94a3b8; background: transparent; font-size: 8pt;")
        row3.addWidget(self.chk_rand_yaw)
        row3.addWidget(self.chk_align_normal)
        row3.addStretch(1)
        row3.addWidget(hint)
        outer.addLayout(row3)

        self.setMinimumWidth(420)
        self.adjustSize()
        self.hide()

    def set_model_label(self, text: str) -> None:
        self.model_lbl.setText(text or "Select a model or car in Explorer")
        self.adjustSize()


class PlaceHudOverlay(QFrame):
    """Floating HUD when Ghost Placement Preview mode is active."""

    closeToolClicked = Signal()

    def __init__(self, parent: QWidget):
        super().__init__(parent)
        self.setProperty("hud", True)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(10, 6, 10, 6)
        lay.setSpacing(10)

        self.lbl = QLabel("<b>Place Mode (Ghost)</b> — LMB stamp · R rotate +15° · Esc exit")
        self.lbl.setStyleSheet("color: #e2e8f0; background: transparent; font-size: 8.5pt;")
        self.chk_align = QCheckBox("Align Normal")
        self.chk_align.setChecked(True)
        btn_close = QPushButton("Exit (Esc)")
        btn_close.setFixedHeight(22)
        btn_close.clicked.connect(self.closeToolClicked.emit)

        lay.addWidget(self.lbl)
        lay.addWidget(self.chk_align)
        lay.addWidget(btn_close)
        self.adjustSize()
        self.hide()


# ---------------------------------------------------------------------------
# Main OpenGL 3.3 Core Viewport Widget
# ---------------------------------------------------------------------------
class ViewportGL(QOpenGLWidget):
    """Hardware-accelerated 3D OpenGL 3.3 Core viewport for EOD World Editor."""

    objectSelected = Signal(object)      # WorldObject or None
    roadNodeSelected = Signal(object)    # RoadNode or None
    markerSelected = Signal(object)      # WorldMarker or None
    barrierSelected = Signal(int)        # SOBL record index (or -1)
    objectTransformed = Signal(object, tuple, tuple, tuple, tuple, tuple, tuple, str)
    markerTransformed = Signal(object, tuple, float, tuple, float, str)
    roadNodeMoved = Signal(object, tuple, tuple)
    roadActionRequested = Signal(str, dict)  # "add" | "split" | "join", payload
    modelDropped = Signal(str, float, float, float)
    modelStamped = Signal(str, tuple, tuple, tuple)  # model_key, pos, rot_hpr, scale
    scatterStrokeCompleted = Signal(list)            # list of {"model_key", "pos", "rot_hpr", "scale"}
    dropToGroundRequested = Signal()
    contextMenuRequested = Signal(object)            # QPoint global pos
    multiObjectTransformed = Signal(list, str)       # [(obj, old_p, old_r, old_s, new_p, new_r, new_s), ...], label
    altDuplicateCompleted = Signal(list, str)        # [cloned_objs...], label (Alt+Drag Duplicate & Drag!)
    toolCycled = Signal(str)                         # new active_tool ("move" | "rotate" | "scale")
    selectionChanged = Signal(list)                  # list[WorldObject]
    statsUpdated = Signal(dict)
    statusMessage = Signal(str)

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMouseTracking(True)
        self.setAcceptDrops(True)

        self.world: TrackWorld | None = None

        # Camera state (LH native metres: yaw=0 -> +Z, yaw=90 -> +X)
        self.cam_pos = np.array([0.0, 45.0, -90.0], dtype=np.float32)
        self.cam_yaw = 0.0
        self.cam_pitch = -22.0
        self.cam_fov = 58.0
        self.fly_speed = 45.0
        self._native_camera_anchor: np.ndarray | None = None
        self._scroll_camera_anchor: np.ndarray | None = None
        self._drag_camera_anchor: np.ndarray | None = None
        self._pan_dragging = False

        # Active mode & tool
        self.edit_mode = "object"     # "object" | "edit"
        self.active_tool = "move"     # "select" | "move" | "rotate" | "scale" | "place" | "scatter" | "road"
        self.road_chain_last_node: int | None = None
        self.road_hover_kind: str | None = None   # "join" | "split" | "new"
        self.road_hover_pos: np.ndarray | None = None
        self.road_hover_target: Any = None

        # Lumion / Twinmotion / UE Ghost Placement, Scatter Brush & Snapping state
        self.grid_snap_enabled: bool = False
        self.grid_snap_step: float = 1.0
        self.angle_snap_enabled: bool = False
        self.angle_snap_step: float = 15.0

        self.ghost_model_key: str | None = None
        self.ghost_pos: np.ndarray | None = None
        self.ghost_normal: tuple[float, float, float] = (0.0, 1.0, 0.0)
        self.ghost_yaw: float = 0.0
        self._ghost_batches: list[dict[str, Any]] = []
        self._ghost_dirty: bool = False

        self.scatter_hover_pos: np.ndarray | None = None
        self._scatter_painting: bool = False
        self._scatter_stroke_items: list[dict[str, Any]] = []
        self._last_scatter_pt: np.ndarray | None = None

        # View toggles & renderer mode ("ps2" = fast power-sipping PS2 mode, "plume" = high-quality mode)
        self.render_mode = "ps2"
        self._plume_viewport: PlumeViewport | None = None
        self._plume_module = self._discover_plume_library()
        self._plume_dirty = True
        self._plume_retry = False
        self.show_wireframe = False
        self.show_collision = False
        self.show_markers = True
        self.show_lighting = True
        self.show_labels = False
        self.show_roads = True
        self.show_grid = True
        self.layer_visibility: dict[str, bool] = {
            "static": True,
            "dyno": True,
            "event-prop": True,
            "billboard": True,
            "optional-terrain": True,
            "display-object": True,
            "prop-catalog": True,
            "car-wreck": True,
        }

        # Selection & Compound Group Drill-In
        self.selected_obj: WorldObject | None = None
        self.selected_objects: list[WorldObject] = []
        self.selected_road_node_id: int | None = None
        self.selected_marker: WorldMarker | None = None
        self.selected_barrier_idx: int = -1
        self._drilled_group_id: str = ""
        self._alt_drag_clones: list[WorldObject] = []

        # Interaction state
        self._keys_down: set[int] = set()
        self._rmb_down = False
        self._rmb_press_pos = QPoint()
        self._lmb_down = False
        self._last_mouse = QPoint()
        self._press_mouse = QPoint()
        self._gizmo_hover_axis: int = -1   # 0=X, 1=Y, 2=Z
        self._gizmo_drag_axis: int = -1
        self._gizmo_drag_target: str = ""  # "obj" | "marker"
        self._gizmo_drag_start_t: float = 0.0
        self._gizmo_drag_start_angle: float = 0.0
        self._gizmo_drag_orig_pos: tuple[float, float, float] = (0.0, 0.0, 0.0)
        self._gizmo_drag_orig_rot: tuple[float, float, float] = (0.0, 0.0, 0.0)
        self._gizmo_drag_orig_scale: tuple[float, float, float] = (1.0, 1.0, 1.0)
        self._gizmo_drag_multi: list[tuple[WorldObject, tuple[float, float, float], tuple[float, float, float], tuple[float, float, float], float]] = []
        self._gizmo_drag_orig_hdg: float = 0.0
        self._dragging_road_node: bool = False
        self._drag_road_orig_pos: tuple[float, float, float] = (0.0, 0.0, 0.0)

        # GL resources & cached uniform locations
        self._gl_ready = False
        self._prog_sky = 0
        self._prog_world = 0
        self._prog_flat = 0
        self._prog_id = 0
        self._u_sky: dict[str, int] = {}
        self._u_world: dict[str, int] = {}
        self._u_flat: dict[str, int] = {}
        self._u_id: dict[str, int] = {}
        self._pick_fbo = 0
        self._pick_color_tex = 0
        self._pick_depth_rb = 0
        self._pick_fb_size: tuple[int, int] = (0, 0)
        self._vao_empty = 0
        self._gl_textures: dict[str, tuple[int, bool]] = {}  # name -> (tex_id, cutout)
        self._terrain_batches: list[dict[str, Any]] = []
        self._prop_batches: list[dict[str, Any]] = []
        self._static_batches: list[dict[str, Any]] = []
        self._blend_batches: list[dict[str, Any]] = []
        self._dynamic_batches: list[dict[str, Any]] = []
        self._dynamic_blend_batches: list[dict[str, Any]] = []
        self._dynamic_obj_ids: set[str] = set()
        self._static_obj_spans: dict[str, list[tuple[dict[str, Any], int, int]]] = {}
        self._last_baked_obj_state: dict[str, tuple[Any, ...]] = {}
        self._static_filter_sig: Any = None
        self._force_full_static_rebuild: bool = True
        self._pick_aabb_lo: np.ndarray | None = None
        self._pick_aabb_hi: np.ndarray | None = None
        self._pick_aabb_objs: list[WorldObject] = []
        self._pick_aabb_sig: Any = None
        self._pick_aabb_dirty: bool = True
        self.__static_dirty: bool = False
        self.__dynamic_dirty: bool = False
        self._dynamic_dirty = False
        self._last_batch_rebuild_ms: float = 0.0
        self._active_obj_count: int = 0
        self._sel_batches: list[dict[str, Any]] = []
        self._grid_vao = 0
        self._grid_vbo = 0
        self._grid_count = 0
        self._scol_vao = 0
        self._scol_vbo = 0
        self._scol_count = 0
        self._sobl_vao = 0
        self._sobl_vbo = 0
        self._sobl_count = 0
        self._road_cache_vbos: dict[str, tuple[int, int, int]] = {}
        self._marker_cache_vbos: dict[str, tuple[int, int, int]] = {}
        self._dyn_vao = 0
        self._dyn_vbo = 0
        self._static_dirty = False
        self._sel_dirty = False
        self._col_dirty = False
        self._roads_dirty = True
        self._markers_dirty = True
        self._pending_tex_updates: set[str] = set()

        # Profiler metrics & GL_TIME_ELAPSED GPU pass timer
        self.profiler = get_profiler()
        self.gpu_timer = GpuPassTimer()
        self._last_frame_ms = 0.0
        self._last_draw_calls = 0
        self._last_tris_drawn = 0
        self._last_tick = time.perf_counter()
        self._last_stats_emit = 0.0

        # Floating Qt overlays
        self.mode_pill = ModePillOverlay(self)
        self.mode_pill.modeChanged.connect(self._on_pill_mode)
        self.mode_pill.shieldToggled.connect(self._on_shield_toggled)

        self.road_hud = RoadToolHudOverlay(self)
        self.road_hud.endRoadClicked.connect(self.end_current_road_chain)
        self.road_hud.closeToolClicked.connect(self._close_road_tool)

        self.scatter_hud = ScatterHudOverlay(self)
        self.scatter_hud.closeToolClicked.connect(self._close_road_tool)

        self.place_hud = PlaceHudOverlay(self)
        self.place_hud.closeToolClicked.connect(self._close_road_tool)

        self.perf_hud = PerfHudOverlay(self)
        self.show_perf_hud = False

        # Demand-driven flight timer: ONLY runs while WASD/QE keys are held (0% idle CPU/GPU!)
        self._timer = QTimer(self)
        self._timer.setInterval(16)
        self._timer.timeout.connect(self._on_tick)

    @property
    def _static_dirty(self) -> bool:
        return self.__static_dirty

    @_static_dirty.setter
    def _static_dirty(self, value: bool) -> None:
        self.__static_dirty = bool(value)
        if self.__static_dirty:
            self._invalidate_pick_aabb_cache()

    @property
    def _dynamic_dirty(self) -> bool:
        return self.__dynamic_dirty

    @_dynamic_dirty.setter
    def _dynamic_dirty(self, value: bool) -> None:
        self.__dynamic_dirty = bool(value)
        if self.__dynamic_dirty:
            self._invalidate_pick_aabb_cache()

    def _invalidate_pick_aabb_cache(self) -> None:
        self._pick_aabb_dirty = True
        self._pick_aabb_lo = None
        self._pick_aabb_hi = None
        self._pick_aabb_objs = []
        self._pick_aabb_sig = None

    def set_render_mode(self, mode: str) -> None:
        """Switch viewport shading mode: 'ps2' (fast power-sipping) or 'plume' (high quality)."""
        self.render_mode = "plume" if mode == "plume" else "ps2"
        if self.render_mode == "ps2" and self._plume_viewport:
            self._plume_viewport.close_surface()
            self._plume_dirty = True
        self.update()

    def set_perf_hud_visible(self, visible: bool) -> None:
        """Show or hide the live GPU + CPU Performance Profiler HUD overlay (F3)."""
        self.show_perf_hud = bool(visible)
        self.perf_hud.setVisible(self.show_perf_hud)
        if self.show_perf_hud:
            self.perf_hud.refresh(self.profiler.snapshot())
            self._position_overlays()
        self.update()

    # -----------------------------------------------------------------------
    # Public API
    # -----------------------------------------------------------------------
    def set_world(self, world: TrackWorld) -> None:
        self.world = world
        self._plume_dirty = True
        self.selected_obj = None
        self.selected_objects = []
        self.selected_road_node_id = None
        self.selected_marker = None
        self.road_chain_last_node = None

        cx, cy, cz = world.world_center
        r = min(max(world.world_radius * 0.65, 65.0), 260.0)
        self.cam_pos = np.array([cx - r * 0.55, cy + r * 0.42, cz - r * 0.65], dtype=np.float32)
        self.look_at(np.array([cx, cy, cz], dtype=np.float32))

        self._drilled_group_id = ""
        self._alt_drag_clones.clear()
        self._dynamic_obj_ids.clear()
        self._static_obj_spans.clear()
        self._last_baked_obj_state.clear()
        self._static_filter_sig = None
        self._force_full_static_rebuild = True
        self._active_obj_count = sum(1 for o in world.objects if not o.deleted)
        self._static_dirty = True
        self._dynamic_dirty = True
        self._sel_dirty = True
        self._col_dirty = True
        self._roads_dirty = True
        self._markers_dirty = True
        if self._gl_ready:
            self.makeCurrent()
            self._upload_textures()
            self._rebuild_collision_vbo()
            self._rebuild_static_batches()
            self._rebuild_dynamic_batches()
            self._rebuild_selection_batches()
            self.doneCurrent()
        self.update()
        self._emit_stats(force=True)

    def look_at(self, target: np.ndarray) -> None:
        d = target - self.cam_pos
        dist_xz = math.hypot(float(d[0]), float(d[2]))
        if dist_xz < 1e-5 and abs(float(d[1])) < 1e-5:
            return
        self.cam_yaw = math.degrees(math.atan2(float(d[0]), float(d[2])))
        self.cam_pitch = max(-88.0, min(88.0, math.degrees(math.atan2(float(d[1]), max(1e-5, dist_xz)))))

    def frame_selection(self) -> None:
        if self.selected_obj is not None and self.world is not None:
            m = self.world.models.get(self.selected_obj.model_key)
            if m:
                lo, hi = self.selected_obj.world_aabb(m)
                c = (lo + hi) * 0.5
                r = max(8.0, float(np.linalg.norm(hi - lo)) * 0.9)
                self.cam_pos = c + np.array([-r, r * 0.65, -r], dtype=np.float32)
                self.look_at(c)
                self.update()
                return
        if self.selected_road_node_id is not None and self.world is not None:
            n = self.world.road_graph.nodes.get(self.selected_road_node_id)
            if n:
                c = np.array([n.x, n.y, n.z], dtype=np.float32)
                self.cam_pos = c + np.array([-24.0, 18.0, -24.0], dtype=np.float32)
                self.look_at(c)
                self.update()
                return
        if self.world is not None:
            cx, cy, cz = self.world.world_center
            r = min(max(self.world.world_radius * 0.65, 65.0), 260.0)
            self.cam_pos = np.array([cx - r * 0.55, cy + r * 0.42, cz - r * 0.65], dtype=np.float32)
            self.look_at(np.array([cx, cy, cz], dtype=np.float32))
            self.update()

    def set_tool(self, tool: str) -> None:
        self.active_tool = tool
        self.road_hud.setVisible(tool == "road")
        self.scatter_hud.setVisible(tool == "scatter")
        self.place_hud.setVisible(tool == "place")
        if tool == "road":
            self.show_roads = True
            self.mode_pill.set_mode("edit", emit=False)
            self.edit_mode = "edit"
            self.statusMessage.emit("Road Tool active — Click terrain to place node, Cyan to split, Green to join")
        elif tool == "place":
            self.mode_pill.set_mode("object", emit=False)
            self.edit_mode = "object"
            mname = (
                self.world.models[self.ghost_model_key].name
                if (self.world and self.ghost_model_key in self.world.models)
                else "select a model in Explorer"
            )
            self.place_hud.lbl.setText(f"<b>Place Mode (Ghost):</b> {mname} — LMB stamp · R rotate +15° · Esc exit")
            self.statusMessage.emit(f"Ghost Place Mode active ({mname}) — Hover terrain, R rotate 15°, LMB stamp")
        elif tool == "scatter":
            self.mode_pill.set_mode("object", emit=False)
            self.edit_mode = "object"
            mname = (
                self.world.models[self.ghost_model_key].name
                if (self.world and self.ghost_model_key in self.world.models)
                else "Select a model or car in Explorer"
            )
            self.scatter_hud.set_model_label(mname)
            self.statusMessage.emit(f"Scatter Brush active ({mname}) — Click/drag on terrain to paint")
        else:
            self.road_chain_last_node = None
            self.road_hover_kind = None
            self.road_hover_pos = None
            self.ghost_pos = None
            self.scatter_hover_pos = None
        self._position_overlays()
        self.update()

    def set_ghost_model(self, model_key: str | None) -> None:
        self.ghost_model_key = model_key
        self._ghost_dirty = True
        if self.world and model_key and model_key in self.world.models:
            mname = self.world.models[model_key].name
            self.scatter_hud.set_model_label(mname)
            self.place_hud.lbl.setText(f"<b>Place Mode (Ghost):</b> {mname} — LMB stamp · R rotate +15° · Esc exit")
            self._position_overlays()
        self.update()

    def end_current_road_chain(self) -> None:
        self.road_chain_last_node = None
        self.statusMessage.emit("Ended current road chain. Click anywhere to start a new road.")
        self.update()

    def _close_road_tool(self) -> None:
        self.set_tool("move")
        self.mode_pill.set_mode("object", emit=False)
        self.edit_mode = "object"
        self.statusMessage.emit("Returned to Move tool")

    def _on_pill_mode(self, mode: str) -> None:
        self.edit_mode = mode
        if mode == "object" and self.active_tool == "road":
            self.set_tool("move")
        self.statusMessage.emit(f"Switched to {'Edit Mode' if mode == 'edit' else 'Object Mode'}")
        self.update()

    def _on_shield_toggled(self, checked: bool) -> None:
        self.show_collision = checked
        self.statusMessage.emit(f"Collision Overlay {'ON' if checked else 'OFF'}")
        self.update()

    def set_event_template(self, template_name: str) -> None:
        """Live-switch the active Event Template filter in <1 ms (rebuilds static batches cleanly)."""
        if self.world is None:
            return
        if self.world.active_event_template == template_name:
            return
        self.world.active_event_template = template_name
        self._plume_dirty = True
        # Drop any selected objects no longer visible in this template
        vis = [o for o in self.selected_objects if self.world.object_visible_in_event_template(o)]
        if len(vis) != len(self.selected_objects):
            self.select_objects(vis, emit=True)
        self._force_full_static_rebuild = True
        self._static_dirty = True
        self._sel_dirty = True
        self.update()

    def select_object(self, obj: WorldObject | None, emit: bool = True) -> None:
        self.select_objects([obj] if obj is not None else [], emit=emit)

    def select_objects(self, objs: list[WorldObject], emit: bool = True) -> None:
        prev_road = self.selected_road_node_id
        prev_marker = self.selected_marker
        clean: list[WorldObject] = []
        seen: set[str] = set()
        for o in objs:
            if o is not None and not o.deleted and o.obj_id not in seen:
                seen.add(o.obj_id)
                clean.append(o)
        self.selected_objects = clean
        self.selected_obj = clean[-1] if clean else None
        if clean:
            self.selected_road_node_id = None
            self.selected_marker = None
            self.selected_barrier_idx = -1
        if prev_road != self.selected_road_node_id:
            self._roads_dirty = True
        if prev_marker is not self.selected_marker:
            self._markers_dirty = True
        self._sel_dirty = True
        self.update()
        if emit:
            self.objectSelected.emit(self.selected_obj)
            self.selectionChanged.emit(list(self.selected_objects))

    def toggle_object_selection(self, obj: WorldObject, emit: bool = True) -> None:
        """Shift+Click toggle: add/remove `obj` in multi-selection."""
        if obj is None or obj.deleted:
            return
        cur = [o for o in self.selected_objects if not o.deleted]
        if any(o.obj_id == obj.obj_id for o in cur):
            cur = [o for o in cur if o.obj_id != obj.obj_id]
        else:
            cur.append(obj)
        self.select_objects(cur, emit=emit)

    def select_road_node(self, node_id: int | None, emit: bool = True) -> None:
        self.selected_road_node_id = node_id
        if node_id is not None:
            self.selected_obj = None
            self.selected_objects = []
            self.selected_marker = None
            self.selected_barrier_idx = -1
            self._sel_dirty = True
            self._markers_dirty = True
        self._roads_dirty = True
        self.update()
        if emit:
            node = self.world.road_graph.nodes.get(node_id) if (self.world and node_id is not None) else None
            self.roadNodeSelected.emit(node)

    def select_marker(self, marker: WorldMarker | None, emit: bool = True) -> None:
        self.selected_marker = marker
        if marker is not None:
            self.selected_obj = None
            self.selected_objects = []
            self.selected_road_node_id = None
            self.selected_barrier_idx = -1
            self._sel_dirty = True
            self._roads_dirty = True
        self._markers_dirty = True
        self.update()
        if emit:
            self.markerSelected.emit(marker)

    def select_barrier(self, rec_idx: int, emit: bool = True) -> None:
        self.selected_barrier_idx = int(rec_idx)
        if rec_idx >= 0:
            self.selected_obj = None
            self.selected_objects = []
            self.selected_road_node_id = None
            self.selected_marker = None
            self._sel_dirty = True
            self._roads_dirty = True
            self._markers_dirty = True
        self.update()
        if emit:
            self.barrierSelected.emit(self.selected_barrier_idx)

    def mark_objects_dirty(self, objs: list[WorldObject] | tuple[WorldObject, ...]) -> None:
        """Fast O(k) dirty-mark for edited objects (avoids rebuilding 6,000-object static VBOs!)."""
        if self.world is None:
            return
        self._plume_dirty = True
        for o in objs:
            if o is not None:
                self._dynamic_obj_ids.add(o.obj_id)
        self._dynamic_dirty = True
        self._sel_dirty = True
        self.update()

    def mark_scene_dirty(self, *, force_full: bool = False) -> None:
        self._plume_dirty = True
        if force_full:
            self._force_full_static_rebuild = True
            self._static_dirty = True
        else:
            # Incremental dirty flag: paintGL diffs changed/new objects into dynamic VBO (<4 ms)
            # and only falls back to full static rebuild when layers/template changed or >256 objects changed!
            self._dynamic_dirty = True
        self._sel_dirty = True
        self._roads_dirty = True
        self._markers_dirty = True
        self.update()

    def mark_collision_dirty(self) -> None:
        self._col_dirty = True
        self.update()

    def update_gl_texture(self, tex_name: str) -> None:
        """Schedule immediate single-texture GPU upload on next paintGL (for live PNG swap)."""
        self._pending_tex_updates.add(tex_name)
        self.update()

    def ensure_gl_textures(self) -> None:
        """Queue any newly added textures (e.g. background-loaded DATA/CAR models) for GPU upload."""
        if self.world is None:
            return
        for name in self.world.textures:
            if name not in self._gl_textures:
                self._pending_tex_updates.add(name)
        if self._pending_tex_updates:
            self.update()

    def scatter_at_point(
        self,
        cx: float,
        cz: float,
        model_key: str | None = None,
        rng: random.Random | None = None,
    ) -> list[dict[str, Any]]:
        """Generate terrain-snapped scatter placements around (cx, cz) honoring Scatter HUD params."""
        if self.world is None:
            return []
        mkey = model_key or self.ghost_model_key
        if not mkey or mkey not in self.world.models:
            return []
        wm = self.world.models[mkey]
        gen = rng or random
        radius = float(self.scatter_hud.spn_radius.value())
        count = int(self.scatter_hud.spn_density.value())
        rand_yaw = self.scatter_hud.chk_rand_yaw.isChecked()
        align_nrm = self.scatter_hud.chk_align_normal.isChecked()
        jitter = float(self.scatter_hud.spn_jitter.value()) / 100.0

        ext = wm.bbox_max - wm.bbox_min
        min_sep = max(1.2, float(max(ext[0], ext[2])) * 0.75)
        placed_xz: list[tuple[float, float]] = []
        items: list[dict[str, Any]] = []

        attempts = max(count * 6, 12)
        for _ in range(attempts):
            if len(items) >= count:
                break
            ang = gen.uniform(0.0, 2.0 * math.pi)
            dist = radius * math.sqrt(gen.uniform(0.0, 1.0))
            px = cx + math.cos(ang) * dist
            pz = cz + math.sin(ang) * dist
            if any(math.hypot(px - ox, pz - oz) < min_sep for ox, oz in placed_xz):
                continue
            placed_xz.append((px, pz))
            gy, nrm = self.world.collision.ground_sample(px, pz)
            yaw = gen.uniform(0.0, 360.0) if rand_yaw else self.ghost_yaw
            rot = normal_to_pitch_roll(nrm, yaw) if align_nrm else (yaw, 0.0, 0.0)
            s = max(0.35, 1.0 + gen.uniform(-jitter, jitter)) if jitter > 0.0 else 1.0
            items.append(
                {
                    "model_key": mkey,
                    "pos": (float(px), float(gy), float(pz)),
                    "rot_hpr": rot,
                    "scale": (s, s, s),
                }
            )
        return items

    def camera_vectors(self) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        yaw_r = math.radians(self.cam_yaw)
        pitch_r = math.radians(self.cam_pitch)
        cp = math.cos(pitch_r)
        fwd = np.array(
            [math.sin(yaw_r) * cp, math.sin(pitch_r), math.cos(yaw_r) * cp],
            dtype=np.float32,
        )
        fwd = _normalize(fwd)
        world_up = np.array([0.0, 1.0, 0.0], dtype=np.float32)
        right = _normalize(np.cross(world_up, fwd))
        up = _normalize(np.cross(fwd, right))
        return fwd, right, up

    def camera_state(self) -> dict[str, Any]:
        return {
            "pos": [float(v) for v in self.cam_pos],
            "yaw": float(self.cam_yaw),
            "pitch": float(self.cam_pitch),
            "fly_speed": float(self.fly_speed),
        }

    def apply_camera_state(self, st: dict[str, Any]) -> None:
        if "pos" in st and len(st["pos"]) == 3:
            self.cam_pos = np.array(st["pos"], dtype=np.float32)
        if "yaw" in st:
            self.cam_yaw = float(st["yaw"])
        if "pitch" in st:
            self.cam_pitch = float(st["pitch"])
        if "fly_speed" in st:
            self.fly_speed = float(st["fly_speed"])
        self.update()

    def screen_ray(self, sx: float, sy: float) -> tuple[np.ndarray, np.ndarray]:
        w = max(1, self.width())
        h = max(1, self.height())
        nx = (2.0 * sx / w) - 1.0
        ny = 1.0 - (2.0 * sy / h)
        aspect = w / h
        tan_half = math.tan(math.radians(self.cam_fov) * 0.5)
        fwd, right, up = self.camera_vectors()
        rd = _normalize(fwd + right * (nx * aspect * tan_half) + up * (ny * tan_half))
        return self.cam_pos.copy(), rd

    # -----------------------------------------------------------------------
    # OpenGL 3.3 Core lifecycle
    # -----------------------------------------------------------------------
    def initializeGL(self) -> None:
        self._prog_sky = _compile_program(_SKY_VERT, _SKY_FRAG)
        self._prog_world = _compile_program(_WORLD_VERT, _WORLD_FRAG)
        self._prog_flat = _compile_program(_FLAT_VERT, _FLAT_FRAG)
        self._prog_id = _compile_program(_ID_VERT, _ID_FRAG)

        # Cache all uniform locations once (zero glGetUniformLocation calls per frame!)
        self._u_sky = {
            name: GL.glGetUniformLocation(self._prog_sky, name)
            for name in (
                "uCamFwd", "uCamRight", "uCamUp", "uTanHalfFov", "uAspect",
                "uSkyZenith", "uSkyMid", "uSkyHorizon", "uSkyGround", "uSunDir", "uSunColor",
            )
        }
        self._u_world = {
            name: GL.glGetUniformLocation(self._prog_world, name)
            for name in (
                "uViewProj", "uModel", "uTex", "uHasTex", "uCutout", "uLighting", "uRenderMode",
                "uCamPos", "uTint", "uHighlightMix", "uSunDir", "uSunColor",
                "uAmbientIntensity", "uFogColor", "uFogStart", "uFogEnd", "uFogDensity",
            )
        }
        self._u_flat = {
            name: GL.glGetUniformLocation(self._prog_flat, name)
            for name in ("uViewProj", "uModel", "uUseOverride", "uOverrideColor")
        }
        self._u_id = {
            name: GL.glGetUniformLocation(self._prog_id, name)
            for name in ("uViewProj", "uModel", "uTex", "uHasTex", "uCutout")
        }

        self._vao_empty = int(GL.glGenVertexArrays(1))
        self._dyn_vao = int(GL.glGenVertexArrays(1))
        self._dyn_vbo = int(GL.glGenBuffers(1))
        GL.glBindVertexArray(self._dyn_vao)
        GL.glBindBuffer(GL.GL_ARRAY_BUFFER, self._dyn_vbo)
        stride = 7 * 4
        GL.glEnableVertexAttribArray(0)
        GL.glVertexAttribPointer(0, 3, GL.GL_FLOAT, GL.GL_FALSE, stride, ctypes.c_void_p(0))
        GL.glEnableVertexAttribArray(1)
        GL.glVertexAttribPointer(1, 4, GL.GL_FLOAT, GL.GL_FALSE, stride, ctypes.c_void_p(12))
        GL.glBindVertexArray(0)

        self._build_grid_vbo()
        self.gpu_timer.initialize(GL)
        self._gl_ready = True

        if self.world is not None:
            self._upload_textures()
            self._rebuild_collision_vbo()
            self._rebuild_static_batches()
            self._rebuild_selection_batches()

    def resizeGL(self, w: int, h: int) -> None:
        dpr = self.devicePixelRatioF()
        GL.glViewport(0, 0, max(1, int(w * dpr)), max(1, int(h * dpr)))
        self._position_overlays()

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        if self._plume_viewport:
            self._plume_viewport.setGeometry(self.rect())
        self._position_overlays()

    def _position_overlays(self) -> None:
        self.mode_pill.adjustSize()
        self.mode_pill.move(12, 12)
        min_x = self.mode_pill.x() + self.mode_pill.width() + 12
        for hud in (self.road_hud, self.scatter_hud, self.place_hud):
            if hud.isVisible():
                hud.adjustSize()
                centered_x = (self.width() - hud.width()) // 2
                hud.move(max(min_x, centered_x), 12)
        if self.perf_hud.isVisible():
            self.perf_hud.adjustSize()
            px = max(12, self.width() - self.perf_hud.width() - 12)
            self.perf_hud.move(px, 12)

    # -----------------------------------------------------------------------
    # GPU Resource Management
    # -----------------------------------------------------------------------
    def _delete_batches(self, batches: list[dict[str, Any]]) -> None:
        for b in batches:
            if b.get("vao"):
                GL.glDeleteVertexArrays(1, [b["vao"]])
            if b.get("vbo"):
                GL.glDeleteBuffers(1, [b["vbo"]])
            if b.get("ebo"):
                GL.glDeleteBuffers(1, [b["ebo"]])
        batches.clear()

    def _upload_one_texture(self, name: str, ta: Any) -> None:
        width, height = ta.width, ta.height
        encoded = None
        if ta.is_replaced:
            encoded, width, height = build_eodhtx_from_rgba(
                ta.rgba, width, height, ta.orig_width, ta.orig_height,
            )
        old = self._gl_textures.get(name)
        if old is not None:
            tid = old[0]
        else:
            tid = int(GL.glGenTextures(1))
        GL.glPixelStorei(GL.GL_UNPACK_ALIGNMENT, 1)
        GL.glBindTexture(GL.GL_TEXTURE_2D, tid)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_S, GL.GL_REPEAT)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_T, GL.GL_REPEAT)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER, GL.GL_LINEAR_MIPMAP_LINEAR)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAG_FILTER, GL.GL_LINEAR)
        levels = max(width, height).bit_length()
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAX_LEVEL, levels - 1)
        if encoded is None:
            GL.glTexImage2D(
                GL.GL_TEXTURE_2D, 0, GL.GL_RGBA8, width, height, 0,
                GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, ta.rgba,
            )
            GL.glGenerateMipmap(GL.GL_TEXTURE_2D)
        else:
            offset = 16  # EODHTX1 header, followed by tightly packed RGBA levels.
            for level in range(levels):
                w, h = max(1, width >> level), max(1, height >> level)
                end = offset + w * h * 4
                GL.glTexImage2D(
                    GL.GL_TEXTURE_2D, level, GL.GL_RGBA8, w, h, 0,
                    GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, encoded[offset:end],
                )
                offset = end
        GL.glBindTexture(GL.GL_TEXTURE_2D, 0)
        self._gl_textures[name] = (tid, ta.alpha_kind == "cutout")

    def _upload_textures(self) -> None:
        for tex_id, _cut in self._gl_textures.values():
            GL.glDeleteTextures(1, [tex_id])
        self._gl_textures.clear()
        self._pending_tex_updates.clear()
        if self.world is None:
            return
        for name, ta in self.world.textures.items():
            self._upload_one_texture(name, ta)

    def _flush_pending_textures(self) -> None:
        if not self._pending_tex_updates or self.world is None:
            return
        for name in list(self._pending_tex_updates):
            ta = self.world.textures.get(name)
            if ta is not None:
                self._upload_one_texture(name, ta)
        self._pending_tex_updates.clear()

    def _build_grid_vbo(self) -> None:
        pts: list[list[float]] = []
        extent = 600.0
        step = 25.0
        y = -0.15
        n = int(extent / step)
        for i in range(-n, n + 1):
            c = i * step
            col = [0.22, 0.25, 0.31, 0.55] if i % 4 else [0.30, 0.35, 0.44, 0.72]
            if i == 0:
                pts.append([-extent, y, 0.0, 0.75, 0.24, 0.24, 0.85])
                pts.append([extent, y, 0.0, 0.75, 0.24, 0.24, 0.85])
                pts.append([0.0, y, -extent, 0.24, 0.45, 0.85, 0.85])
                pts.append([0.0, y, extent, 0.24, 0.45, 0.85, 0.85])
            else:
                pts.append([c, y, -extent, *col])
                pts.append([c, y, extent, *col])
                pts.append([-extent, y, c, *col])
                pts.append([extent, y, c, *col])
        arr = np.array(pts, dtype=np.float32)
        self._grid_count = len(arr)
        self._grid_vao = int(GL.glGenVertexArrays(1))
        self._grid_vbo = int(GL.glGenBuffers(1))
        GL.glBindVertexArray(self._grid_vao)
        GL.glBindBuffer(GL.GL_ARRAY_BUFFER, self._grid_vbo)
        GL.glBufferData(GL.GL_ARRAY_BUFFER, arr.nbytes, arr, GL.GL_STATIC_DRAW)
        GL.glEnableVertexAttribArray(0)
        GL.glVertexAttribPointer(0, 3, GL.GL_FLOAT, GL.GL_FALSE, 28, ctypes.c_void_p(0))
        GL.glEnableVertexAttribArray(1)
        GL.glVertexAttribPointer(1, 4, GL.GL_FLOAT, GL.GL_FALSE, 28, ctypes.c_void_p(12))
        GL.glBindVertexArray(0)

    def _rebuild_collision_vbo(self) -> None:
        self._col_dirty = False
        for vao, vbo in ((self._scol_vao, self._scol_vbo), (self._sobl_vao, self._sobl_vbo)):
            if vao:
                GL.glDeleteVertexArrays(1, [vao])
            if vbo:
                GL.glDeleteBuffers(1, [vbo])
        self._scol_vao = self._scol_vbo = self._scol_count = 0
        self._sobl_vao = self._sobl_vbo = self._sobl_count = 0
        if self.world is None:
            return
        self.world.collision.rebuild_dynamic_ramp_collision(self.world)

        def upload_lines(verts3: np.ndarray, rgba: Any) -> tuple[int, int, int]:
            if len(verts3) == 0:
                return 0, 0, 0
            data = np.empty((len(verts3), 7), dtype=np.float32)
            data[:, 0:3] = verts3
            if isinstance(rgba, np.ndarray) and len(rgba) == len(verts3):
                data[:, 3:7] = rgba
            else:
                data[:, 3:7] = rgba
            vao = int(GL.glGenVertexArrays(1))
            vbo = int(GL.glGenBuffers(1))
            GL.glBindVertexArray(vao)
            GL.glBindBuffer(GL.GL_ARRAY_BUFFER, vbo)
            GL.glBufferData(GL.GL_ARRAY_BUFFER, data.nbytes, data, GL.GL_STATIC_DRAW)
            GL.glEnableVertexAttribArray(0)
            GL.glVertexAttribPointer(0, 3, GL.GL_FLOAT, GL.GL_FALSE, 28, ctypes.c_void_p(0))
            GL.glEnableVertexAttribArray(1)
            GL.glVertexAttribPointer(1, 4, GL.GL_FLOAT, GL.GL_FALSE, 28, ctypes.c_void_p(12))
            GL.glBindVertexArray(0)
            return vao, vbo, len(verts3)

        self._scol_vao, self._scol_vbo, self._scol_count = upload_lines(
            self.world.collision.scol_line_verts, (0.12, 0.85, 0.82, 0.65)
        )
        sobl_col = (
            self.world.collision.sobl_line_colors
            if len(self.world.collision.sobl_line_colors) == len(self.world.collision.sobl_line_verts)
            else (0.98, 0.56, 0.14, 0.90)
        )
        self._sobl_vao, self._sobl_vbo, self._sobl_count = upload_lines(
            self.world.collision.sobl_line_verts, sobl_col
        )

    def _upload_mesh_batch(self, verts: np.ndarray, indices: np.ndarray, tex: str) -> dict[str, Any]:
        # Accept either 8-float (pos3, nrm3, uv2) or 9-float (+obj_id) vertex arrays cleanly
        if verts.ndim == 2 and verts.shape[1] == 8:
            padded = np.empty((verts.shape[0], 9), dtype=np.float32)
            padded[:, :8] = verts
            padded[:, 8] = 0.0
            verts = padded
        verts = np.ascontiguousarray(verts, dtype=np.float32)
        indices = np.ascontiguousarray(indices, dtype=np.uint32)
        vao = int(GL.glGenVertexArrays(1))
        vbo = int(GL.glGenBuffers(1))
        ebo = int(GL.glGenBuffers(1))
        GL.glBindVertexArray(vao)
        GL.glBindBuffer(GL.GL_ARRAY_BUFFER, vbo)
        GL.glBufferData(GL.GL_ARRAY_BUFFER, verts.nbytes, verts, GL.GL_STATIC_DRAW)
        GL.glBindBuffer(GL.GL_ELEMENT_ARRAY_BUFFER, ebo)
        GL.glBufferData(GL.GL_ELEMENT_ARRAY_BUFFER, indices.nbytes, indices, GL.GL_STATIC_DRAW)
        stride = 9 * 4
        GL.glEnableVertexAttribArray(0)
        GL.glVertexAttribPointer(0, 3, GL.GL_FLOAT, GL.GL_FALSE, stride, ctypes.c_void_p(0))
        GL.glEnableVertexAttribArray(1)
        GL.glVertexAttribPointer(1, 3, GL.GL_FLOAT, GL.GL_FALSE, stride, ctypes.c_void_p(12))
        GL.glEnableVertexAttribArray(2)
        GL.glVertexAttribPointer(2, 2, GL.GL_FLOAT, GL.GL_FALSE, stride, ctypes.c_void_p(24))
        GL.glEnableVertexAttribArray(3)
        GL.glVertexAttribPointer(3, 1, GL.GL_FLOAT, GL.GL_FALSE, stride, ctypes.c_void_p(32))
        GL.glBindVertexArray(0)
        return {"vao": vao, "vbo": vbo, "ebo": ebo, "count": len(indices), "tris": len(indices) // 3, "tex": tex}

    def _mask_static_object_in_ebo(self, obj_id: str) -> None:
        """Zero out an object's index slice in static EBOs in-place (<0.05 ms) so it moves cleanly to dynamic VBO."""
        spans = self._static_obj_spans.pop(obj_id, None)
        if not spans:
            return
        for batch, byte_off, idx_count in spans:
            if idx_count <= 0 or not batch.get("ebo"):
                continue
            zeros = np.zeros(idx_count, dtype=np.uint32)
            GL.glBindBuffer(GL.GL_ELEMENT_ARRAY_BUFFER, batch["ebo"])
            GL.glBufferSubData(GL.GL_ELEMENT_ARRAY_BUFFER, byte_off, zeros.nbytes, zeros)
        GL.glBindBuffer(GL.GL_ELEMENT_ARRAY_BUFFER, 0)

    def _sync_dirty_scene_batches(self) -> None:
        """Smart incremental scene sync: splits static vs. dynamic VBO batches so edits stay <4 ms!"""
        if self.world is None:
            self._static_dirty = False
            self._dynamic_dirty = False
            return
        t0 = time.perf_counter_ns()
        cur_sig = (
            self.world.active_event_template,
            tuple(sorted(self.layer_visibility.items())),
        )
        if (
            self._force_full_static_rebuild
            or self._static_dirty
            or cur_sig != self._static_filter_sig
            or not self._last_baked_obj_state
        ):
            self._rebuild_static_batches()
            self._rebuild_dynamic_batches()
            self._last_batch_rebuild_ms = (time.perf_counter_ns() - t0) / 1_000_000.0
            return

        # Fast incremental diff against last baked state (only touches changed/new objects!)
        changed_ids: list[str] = []
        active_cnt = 0
        for obj in self.world.objects:
            if not obj.deleted:
                active_cnt += 1
            oid = obj.obj_id
            st = (obj.position, obj.rotation_hpr, obj.scale, obj.deleted, obj.visible, obj.model_key)
            prev = self._last_baked_obj_state.get(oid)
            if prev != st:
                changed_ids.append(oid)
                self._last_baked_obj_state[oid] = st
        self._active_obj_count = active_cnt

        # If a massive batch edit changed >256 objects at once, rebuild static cleanly
        if len(changed_ids) > 256:
            self._rebuild_static_batches()
            self._rebuild_dynamic_batches()
            self._last_batch_rebuild_ms = (time.perf_counter_ns() - t0) / 1_000_000.0
            return

        for oid in changed_ids:
            self._mask_static_object_in_ebo(oid)
            self._dynamic_obj_ids.add(oid)
        for oid in list(self._dynamic_obj_ids):
            if oid in self._static_obj_spans:
                self._mask_static_object_in_ebo(oid)

        self._rebuild_dynamic_batches()
        self._last_batch_rebuild_ms = (time.perf_counter_ns() - t0) / 1_000_000.0

    def _rebuild_static_batches(self) -> None:
        self._static_dirty = False
        self._force_full_static_rebuild = False
        self._delete_batches(self._terrain_batches)
        self._delete_batches(self._prop_batches)
        self._static_batches.clear()
        self._delete_batches(self._blend_batches)
        self._static_obj_spans.clear()
        self._last_baked_obj_state.clear()
        if self.world is None:
            self._active_obj_count = 0
            return

        self._static_filter_sig = (
            self.world.active_event_template,
            tuple(sorted(self.layer_visibility.items())),
        )

        terr_v: dict[str, list[np.ndarray]] = {}
        terr_i: dict[str, list[np.ndarray]] = {}
        terr_base: dict[str, int] = {}
        terr_idx_off: dict[str, int] = {}
        terr_spans: dict[str, list[tuple[str, int, int]]] = {}

        prop_v: dict[str, list[np.ndarray]] = {}
        prop_i: dict[str, list[np.ndarray]] = {}
        prop_base: dict[str, int] = {}
        prop_idx_off: dict[str, int] = {}
        prop_spans: dict[str, list[tuple[str, int, int]]] = {}

        blend_v: dict[str, list[np.ndarray]] = {}
        blend_i: dict[str, list[np.ndarray]] = {}
        blend_base: dict[str, int] = {}
        blend_idx_off: dict[str, int] = {}
        blend_spans: dict[str, list[tuple[str, int, int]]] = {}

        active_cnt = 0
        for obj_idx, obj in enumerate(self.world.objects):
            if not obj.deleted:
                active_cnt += 1
            self._last_baked_obj_state[obj.obj_id] = (
                obj.position,
                obj.rotation_hpr,
                obj.scale,
                obj.deleted,
                obj.visible,
                obj.model_key,
            )
            if obj.obj_id in self._dynamic_obj_ids:
                continue
            if not self.world.object_visible_in_event_template(obj):
                continue
            if not self.layer_visibility.get(obj.category, True):
                continue
            model = self.world.models.get(obj.model_key)
            if not model:
                continue
            m4 = obj.world_matrix()
            rot3 = m4[:3, :3]
            t3 = m4[:3, 3]
            is_ident_rot = (
                obj.rotation_hpr == (0.0, 0.0, 0.0) and obj.scale == (1.0, 1.0, 1.0)
            )
            is_terrain = obj.category in ("static", "optional-terrain")
            pick_id = float(obj_idx + 1)
            for part in model.parts:
                nv = len(part.vertices)
                v = np.empty((nv, 9), dtype=np.float32)
                if is_ident_rot:
                    v[:, 0:3] = part.vertices[:, 0:3] + t3
                    v[:, 3:8] = part.vertices[:, 3:8]
                else:
                    v[:, 0:3] = part.vertices[:, 0:3] @ rot3.T + t3
                    v[:, 3:6] = part.vertices[:, 3:6] @ rot3.T
                    v[:, 6:8] = part.vertices[:, 6:8]
                v[:, 8] = pick_id
                tex = part.texture
                ta = self.world.textures.get(tex)
                ni = len(part.indices)
                if ta is not None and ta.alpha_kind == "blend":
                    base = blend_base.get(tex, 0)
                    ioff = blend_idx_off.get(tex, 0)
                    blend_v.setdefault(tex, []).append(v)
                    blend_i.setdefault(tex, []).append(part.indices + base)
                    blend_spans.setdefault(tex, []).append((obj.obj_id, ioff * 4, ni))
                    blend_base[tex] = base + nv
                    blend_idx_off[tex] = ioff + ni
                elif is_terrain:
                    base = terr_base.get(tex, 0)
                    ioff = terr_idx_off.get(tex, 0)
                    terr_v.setdefault(tex, []).append(v)
                    terr_i.setdefault(tex, []).append(part.indices + base)
                    terr_spans.setdefault(tex, []).append((obj.obj_id, ioff * 4, ni))
                    terr_base[tex] = base + nv
                    terr_idx_off[tex] = ioff + ni
                else:
                    base = prop_base.get(tex, 0)
                    ioff = prop_idx_off.get(tex, 0)
                    prop_v.setdefault(tex, []).append(v)
                    prop_i.setdefault(tex, []).append(part.indices + base)
                    prop_spans.setdefault(tex, []).append((obj.obj_id, ioff * 4, ni))
                    prop_base[tex] = base + nv
                    prop_idx_off[tex] = ioff + ni

        self._active_obj_count = active_cnt

        for tex, vlist in terr_v.items():
            verts = np.ascontiguousarray(np.concatenate(vlist, axis=0), dtype=np.float32)
            inds = np.ascontiguousarray(np.concatenate(terr_i[tex], axis=0), dtype=np.uint32)
            b = self._upload_mesh_batch(verts, inds, tex)
            self._terrain_batches.append(b)
            for oid, byte_off, ni in terr_spans.get(tex, ()):
                self._static_obj_spans.setdefault(oid, []).append((b, byte_off, ni))

        for tex, vlist in prop_v.items():
            verts = np.ascontiguousarray(np.concatenate(vlist, axis=0), dtype=np.float32)
            inds = np.ascontiguousarray(np.concatenate(prop_i[tex], axis=0), dtype=np.uint32)
            b = self._upload_mesh_batch(verts, inds, tex)
            self._prop_batches.append(b)
            for oid, byte_off, ni in prop_spans.get(tex, ()):
                self._static_obj_spans.setdefault(oid, []).append((b, byte_off, ni))

        self._static_batches = self._terrain_batches + self._prop_batches

        for tex, vlist in blend_v.items():
            verts = np.ascontiguousarray(np.concatenate(vlist, axis=0), dtype=np.float32)
            inds = np.ascontiguousarray(np.concatenate(blend_i[tex], axis=0), dtype=np.uint32)
            b = self._upload_mesh_batch(verts, inds, tex)
            self._blend_batches.append(b)
            for oid, byte_off, ni in blend_spans.get(tex, ()):
                self._static_obj_spans.setdefault(oid, []).append((b, byte_off, ni))

    def _rebuild_dynamic_batches(self) -> None:
        """Rebuild ONLY the small dynamic VBO bucket (edited/moved/placed objects) in <2 ms."""
        self._dynamic_dirty = False
        self._delete_batches(self._dynamic_batches)
        self._delete_batches(self._dynamic_blend_batches)
        if self.world is None or not self._dynamic_obj_ids:
            return

        dyn_v: dict[str, list[np.ndarray]] = {}
        dyn_i: dict[str, list[np.ndarray]] = {}
        dyn_base: dict[str, int] = {}
        dbl_v: dict[str, list[np.ndarray]] = {}
        dbl_i: dict[str, list[np.ndarray]] = {}
        dbl_base: dict[str, int] = {}

        # Build fast id->index lookup only for dynamic IDs
        dyn_set = self._dynamic_obj_ids
        for obj_idx, obj in enumerate(self.world.objects):
            if obj.obj_id not in dyn_set:
                continue
            if not self.world.object_visible_in_event_template(obj):
                continue
            if not self.layer_visibility.get(obj.category, True):
                continue
            model = self.world.models.get(obj.model_key)
            if not model:
                continue
            m4 = obj.world_matrix()
            rot3 = m4[:3, :3]
            t3 = m4[:3, 3]
            pick_id = float(obj_idx + 1)
            for part in model.parts:
                nv = len(part.vertices)
                v = np.empty((nv, 9), dtype=np.float32)
                v[:, 0:3] = part.vertices[:, 0:3] @ rot3.T + t3
                v[:, 3:6] = part.vertices[:, 3:6] @ rot3.T
                v[:, 6:8] = part.vertices[:, 6:8]
                v[:, 8] = pick_id
                tex = part.texture
                ta = self.world.textures.get(tex)
                if ta is not None and ta.alpha_kind == "blend":
                    base = dbl_base.get(tex, 0)
                    dbl_v.setdefault(tex, []).append(v)
                    dbl_i.setdefault(tex, []).append(part.indices + base)
                    dbl_base[tex] = base + nv
                else:
                    base = dyn_base.get(tex, 0)
                    dyn_v.setdefault(tex, []).append(v)
                    dyn_i.setdefault(tex, []).append(part.indices + base)
                    dyn_base[tex] = base + nv

        for tex, vlist in dyn_v.items():
            verts = np.ascontiguousarray(np.concatenate(vlist, axis=0), dtype=np.float32)
            inds = np.ascontiguousarray(np.concatenate(dyn_i[tex], axis=0), dtype=np.uint32)
            self._dynamic_batches.append(self._upload_mesh_batch(verts, inds, tex))
        for tex, vlist in dbl_v.items():
            verts = np.ascontiguousarray(np.concatenate(vlist, axis=0), dtype=np.float32)
            inds = np.ascontiguousarray(np.concatenate(dbl_i[tex], axis=0), dtype=np.uint32)
            self._dynamic_blend_batches.append(self._upload_mesh_batch(verts, inds, tex))

    def _ensure_pick_fbo(self, fb_w: int, fb_h: int) -> bool:
        """Lazily allocate/resize offscreen GPU Object-ID FBO (RGBA8 + DEPTH24) for pixel-exact picking."""
        if not self._gl_ready or fb_w <= 0 or fb_h <= 0:
            return False
        if self._pick_fbo and self._pick_fb_size == (fb_w, fb_h):
            return True
        if self._pick_fbo:
            GL.glDeleteFramebuffers(1, [self._pick_fbo])
        if self._pick_color_tex:
            GL.glDeleteTextures(1, [self._pick_color_tex])
        if self._pick_depth_rb:
            GL.glDeleteRenderbuffers(1, [self._pick_depth_rb])

        self._pick_fbo = int(GL.glGenFramebuffers(1))
        self._pick_color_tex = int(GL.glGenTextures(1))
        self._pick_depth_rb = int(GL.glGenRenderbuffers(1))

        GL.glBindFramebuffer(GL.GL_FRAMEBUFFER, self._pick_fbo)
        GL.glBindTexture(GL.GL_TEXTURE_2D, self._pick_color_tex)
        GL.glTexImage2D(GL.GL_TEXTURE_2D, 0, GL.GL_RGBA8, fb_w, fb_h, 0, GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, None)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER, GL.GL_NEAREST)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAG_FILTER, GL.GL_NEAREST)
        GL.glFramebufferTexture2D(GL.GL_FRAMEBUFFER, GL.GL_COLOR_ATTACHMENT0, GL.GL_TEXTURE_2D, self._pick_color_tex, 0)

        GL.glBindRenderbuffer(GL.GL_RENDERBUFFER, self._pick_depth_rb)
        GL.glRenderbufferStorage(GL.GL_RENDERBUFFER, GL.GL_DEPTH_COMPONENT24, fb_w, fb_h)
        GL.glFramebufferRenderbuffer(GL.GL_FRAMEBUFFER, GL.GL_DEPTH_ATTACHMENT, GL.GL_RENDERBUFFER, self._pick_depth_rb)

        status = GL.glCheckFramebufferStatus(GL.GL_FRAMEBUFFER)
        GL.glBindFramebuffer(GL.GL_FRAMEBUFFER, self.defaultFramebufferObject())
        if status != GL.GL_FRAMEBUFFER_COMPLETE:
            self._pick_fb_size = (0, 0)
            return False
        self._pick_fb_size = (fb_w, fb_h)
        return True

    def _pick_object_gpu(self, sx: float, sy: float) -> WorldObject | None:
        """Pixel-exact GPU Object-ID FBO pick (1x1 / 3x3 readback under cursor in <0.8 ms)."""
        with get_profiler().zone("viewport.pick_gpu", "pick"):
            return self._pick_object_gpu_impl(sx, sy)

    def _pick_object_gpu_impl(self, sx: float, sy: float) -> WorldObject | None:
        if self.world is None or not self._gl_ready or not self._prog_id:
            return None
        w = max(1, self.width())
        h = max(1, self.height())
        dpr = max(1.0, float(self.devicePixelRatioF()))
        fb_w = max(1, int(round(w * dpr)))
        fb_h = max(1, int(round(h * dpr)))
        px = max(0, min(fb_w - 1, int(round(sx * dpr))))
        py = max(0, min(fb_h - 1, fb_h - 1 - int(round(sy * dpr))))

        try:
            self.makeCurrent()
            if self._static_dirty or self._dynamic_dirty:
                self._sync_dirty_scene_batches()
            if not self._ensure_pick_fbo(fb_w, fb_h):
                self.doneCurrent()
                return None

            fwd, right, up = self.camera_vectors()
            view = lh_look_matrix(self.cam_pos, fwd, right, up)
            proj = lh_perspective(self.cam_fov, w / h, 0.4, 4500.0)
            view_proj = proj @ view
            ident = np.eye(4, dtype=np.float32)

            GL.glBindFramebuffer(GL.GL_FRAMEBUFFER, self._pick_fbo)
            GL.glViewport(0, 0, fb_w, fb_h)
            GL.glDisable(GL.GL_BLEND)
            GL.glEnable(GL.GL_DEPTH_TEST)
            GL.glDepthMask(GL.GL_TRUE)
            GL.glDisable(GL.GL_CULL_FACE)
            GL.glPolygonMode(GL.GL_FRONT_AND_BACK, GL.GL_FILL)
            GL.glClearColor(0.0, 0.0, 0.0, 0.0)
            GL.glClear(GL.GL_COLOR_BUFFER_BIT | GL.GL_DEPTH_BUFFER_BIT)

            GL.glUseProgram(self._prog_id)
            uid = self._u_id
            GL.glUniformMatrix4fv(uid["uViewProj"], 1, GL.GL_TRUE, view_proj)
            GL.glUniformMatrix4fv(uid["uModel"], 1, GL.GL_TRUE, ident)
            GL.glUniform1i(uid["uTex"], 0)
            GL.glActiveTexture(GL.GL_TEXTURE0)

            for is_blend_list, batch_list in (
                (False, self._terrain_batches),
                (False, self._prop_batches),
                (False, self._dynamic_batches),
                (True, self._blend_batches),
                (True, self._dynamic_blend_batches),
            ):
                for b in batch_list:
                    tex_entry = self._gl_textures.get(b["tex"])
                    if tex_entry is not None:
                        GL.glBindTexture(GL.GL_TEXTURE_2D, tex_entry[0])
                        GL.glUniform1i(uid["uHasTex"], 1)
                        if is_blend_list:
                            GL.glUniform1i(uid["uCutout"], 2)
                        else:
                            GL.glUniform1i(uid["uCutout"], 1 if tex_entry[1] else 0)
                    else:
                        GL.glUniform1i(uid["uHasTex"], 0)
                        GL.glUniform1i(uid["uCutout"], 0)
                    GL.glBindVertexArray(b["vao"])
                    GL.glDrawElements(GL.GL_TRIANGLES, b["count"], GL.GL_UNSIGNED_INT, None)

            GL.glBindVertexArray(0)
            # Read 3x3 patch centered at (px, py) — exact center pixel first, 1px neighbors fallback for thin fences!
            rx0 = max(0, px - 1)
            ry0 = max(0, py - 1)
            rw = min(3, fb_w - rx0)
            rh = min(3, fb_h - ry0)
            GL.glPixelStorei(GL.GL_PACK_ALIGNMENT, 1)
            raw_px = GL.glReadPixels(rx0, ry0, rw, rh, GL.GL_RGBA, GL.GL_UNSIGNED_BYTE)
            GL.glBindFramebuffer(GL.GL_FRAMEBUFFER, self.defaultFramebufferObject())
            self.doneCurrent()

            arr = np.frombuffer(raw_px, dtype=np.uint8).reshape(rh, rw, 4)
            cx = px - rx0
            cy = py - ry0
            # Check exact pixel first, then immediate neighbors
            order = [(cy, cx)]
            for dy_p in range(rh):
                for dx_p in range(rw):
                    if (dy_p, dx_p) != (cy, cx):
                        order.append((dy_p, dx_p))
            n_objs = len(self.world.objects)
            for yy, xx in order:
                r, g, b_c = int(arr[yy, xx, 0]), int(arr[yy, xx, 1]), int(arr[yy, xx, 2])
                encoded = r | (g << 8) | (b_c << 16)
                if encoded > 0:
                    idx = encoded - 1
                    if 0 <= idx < n_objs:
                        obj = self.world.objects[idx]
                        if not obj.deleted and obj.visible:
                            return obj
            return None
        except Exception:
            try:
                GL.glBindFramebuffer(GL.GL_FRAMEBUFFER, self.defaultFramebufferObject())
                self.doneCurrent()
            except Exception:
                pass
            return None

    def _rebuild_selection_batches(self) -> None:
        self._sel_dirty = False
        self._delete_batches(self._sel_batches)
        if self.world is None:
            return
        active_sel = [o for o in self.selected_objects if not o.deleted]
        if not active_sel and self.selected_obj is not None and not self.selected_obj.deleted:
            active_sel = [self.selected_obj]
        if not active_sel:
            return
        # When single object selected, keep local coords so gizmo drag doesn't rebuild VBOs every frame!
        if len(active_sel) == 1:
            model = self.world.models.get(active_sel[0].model_key)
            if not model:
                return
            for part in model.parts:
                verts = np.ascontiguousarray(part.vertices, dtype=np.float32)
                inds = np.ascontiguousarray(part.indices, dtype=np.uint32)
                b = self._upload_mesh_batch(verts, inds, part.texture)
                b["world_baked"] = False
                self._sel_batches.append(b)
            return
        # Multi-selection: bake world-space batches grouped by texture so all selected objects glow green!
        gv: dict[str, list[np.ndarray]] = {}
        gi: dict[str, list[np.ndarray]] = {}
        gb: dict[str, int] = {}
        for obj in active_sel:
            model = self.world.models.get(obj.model_key)
            if not model:
                continue
            m4 = obj.world_matrix()
            rot3 = m4[:3, :3]
            t3 = m4[:3, 3]
            for part in model.parts:
                v = part.vertices.copy()
                v[:, 0:3] = v[:, 0:3] @ rot3.T + t3
                v[:, 3:6] = v[:, 3:6] @ rot3.T
                tex = part.texture
                base = gb.get(tex, 0)
                gv.setdefault(tex, []).append(v)
                gi.setdefault(tex, []).append(part.indices + base)
                gb[tex] = base + len(v)
        for tex, vlist in gv.items():
            verts = np.ascontiguousarray(np.concatenate(vlist, axis=0), dtype=np.float32)
            inds = np.ascontiguousarray(np.concatenate(gi[tex], axis=0), dtype=np.uint32)
            b = self._upload_mesh_batch(verts, inds, tex)
            b["world_baked"] = True
            self._sel_batches.append(b)

    def _rebuild_ghost_batches(self) -> None:
        self._ghost_dirty = False
        self._delete_batches(self._ghost_batches)
        if self.world is None or not self.ghost_model_key:
            return
        model = self.world.models.get(self.ghost_model_key)
        if not model:
            return
        for part in model.parts:
            verts = np.ascontiguousarray(part.vertices, dtype=np.float32)
            inds = np.ascontiguousarray(part.indices, dtype=np.uint32)
            self._ghost_batches.append(self._upload_mesh_batch(verts, inds, part.texture))

    # -----------------------------------------------------------------------
    # Dynamic geometry drawing helpers
    # -----------------------------------------------------------------------
    def _upload_flat_vbo(self, data: np.ndarray) -> tuple[int, int, int]:
        if len(data) == 0:
            return 0, 0, 0
        arr = np.ascontiguousarray(data, dtype=np.float32)
        vao = int(GL.glGenVertexArrays(1))
        vbo = int(GL.glGenBuffers(1))
        GL.glBindVertexArray(vao)
        GL.glBindBuffer(GL.GL_ARRAY_BUFFER, vbo)
        GL.glBufferData(GL.GL_ARRAY_BUFFER, arr.nbytes, arr, GL.GL_STATIC_DRAW)
        GL.glEnableVertexAttribArray(0)
        GL.glVertexAttribPointer(0, 3, GL.GL_FLOAT, GL.GL_FALSE, 28, ctypes.c_void_p(0))
        GL.glEnableVertexAttribArray(1)
        GL.glVertexAttribPointer(1, 4, GL.GL_FLOAT, GL.GL_FALSE, 28, ctypes.c_void_p(12))
        GL.glBindVertexArray(0)
        return vao, vbo, len(arr)

    def _delete_flat_cache(self, cache: dict[str, tuple[int, int, int]]) -> None:
        for vao, vbo, _cnt in cache.values():
            if vao:
                GL.glDeleteVertexArrays(1, [vao])
            if vbo:
                GL.glDeleteBuffers(1, [vbo])
        cache.clear()

    def _draw_flat_cached(self, entry: tuple[int, int, int] | None, mode: int, view_proj: np.ndarray) -> int:
        if not entry or entry[2] <= 0:
            return 0
        vao, _vbo, cnt = entry
        GL.glUseProgram(self._prog_flat)
        GL.glUniformMatrix4fv(self._u_flat["uViewProj"], 1, GL.GL_TRUE, view_proj)
        GL.glUniformMatrix4fv(self._u_flat["uModel"], 1, GL.GL_TRUE, np.eye(4, dtype=np.float32))
        GL.glUniform1i(self._u_flat["uUseOverride"], 0)
        GL.glBindVertexArray(vao)
        GL.glDrawArrays(mode, 0, cnt)
        GL.glBindVertexArray(0)
        return 1

    def _draw_flat_verts(self, data: np.ndarray, mode: int, view_proj: np.ndarray, model: np.ndarray | None = None) -> int:
        if len(data) == 0:
            return 0
        arr = np.ascontiguousarray(data, dtype=np.float32)
        GL.glUseProgram(self._prog_flat)
        GL.glUniformMatrix4fv(self._u_flat["uViewProj"], 1, GL.GL_TRUE, view_proj)
        m = model if model is not None else np.eye(4, dtype=np.float32)
        GL.glUniformMatrix4fv(self._u_flat["uModel"], 1, GL.GL_TRUE, m)
        GL.glUniform1i(self._u_flat["uUseOverride"], 0)

        GL.glBindVertexArray(self._dyn_vao)
        GL.glBindBuffer(GL.GL_ARRAY_BUFFER, self._dyn_vbo)
        GL.glBufferData(GL.GL_ARRAY_BUFFER, arr.nbytes, arr, GL.GL_STREAM_DRAW)
        GL.glDrawArrays(mode, 0, len(arr))
        GL.glBindVertexArray(0)
        return 1

    def _gizmo_scale(self, origin: np.ndarray) -> float:
        dist = float(np.linalg.norm(self.cam_pos - origin))
        return max(1.5, dist * 0.14)

    def _build_gizmo_geometry(self, origin: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        """Return (line_verts, tri_verts) each shape (N, 7) [x,y,z,r,g,b,a].
        Uses real 3D cylinder shafts / ribbon rings (triangles) so OpenGL 3.3 Core
        never needs glLineWidth > 1.0 and gizmos render bold and crisp.
        """
        s = self._gizmo_scale(origin)
        axes = [
            (np.array([1.0, 0.0, 0.0], dtype=np.float32), [0.95, 0.25, 0.25, 1.0]),
            (np.array([0.0, 1.0, 0.0], dtype=np.float32), [0.22, 0.88, 0.42, 1.0]),
            (np.array([0.0, 0.0, 1.0], dtype=np.float32), [0.26, 0.56, 1.00, 1.0]),
        ]
        lines: list[list[float]] = []
        tris: list[list[float]] = []

        def add_cylinder(p0: np.ndarray, p1: np.ndarray, ax: np.ndarray, r: float, col: list[float], segs: int = 8) -> None:
            u = np.array([0.0, 1.0, 0.0], dtype=np.float32) if abs(ax[1]) < 0.9 else np.array([1.0, 0.0, 0.0], dtype=np.float32)
            u = _normalize(np.cross(ax, u))
            v = _normalize(np.cross(ax, u))
            for k in range(segs):
                a0 = 2.0 * math.pi * k / segs
                a1 = 2.0 * math.pi * (k + 1) / segs
                off0 = r * (math.cos(a0) * u + math.sin(a0) * v)
                off1 = r * (math.cos(a1) * u + math.sin(a1) * v)
                q0, q1, q2, q3 = p0 + off0, p0 + off1, p1 + off1, p1 + off0
                tris.extend([[*q0, *col], [*q1, *col], [*q2, *col], [*q0, *col], [*q2, *col], [*q3, *col]])

        if self.active_tool in ("move", "select"):
            for idx, (ax, col) in enumerate(axes):
                c = [1.0, 0.92, 0.20, 1.0] if idx in (self._gizmo_hover_axis, self._gizmo_drag_axis) else col
                p0 = origin
                p1 = origin + ax * (s * 0.94)
                add_cylinder(p0, p1, ax, s * 0.026, c, segs=6)
                # Arrow cone tip (8-sided)
                u = np.array([0.0, 1.0, 0.0], dtype=np.float32) if abs(ax[1]) < 0.9 else np.array([1.0, 0.0, 0.0], dtype=np.float32)
                u = _normalize(np.cross(ax, u))
                v = _normalize(np.cross(ax, u))
                tip = origin + ax * (s * 1.24)
                base_c = origin + ax * (s * 0.90)
                r = s * 0.095
                segs = 8
                for k in range(segs):
                    a0 = 2.0 * math.pi * k / segs
                    a1 = 2.0 * math.pi * (k + 1) / segs
                    b0 = base_c + r * (math.cos(a0) * u + math.sin(a0) * v)
                    b1 = base_c + r * (math.cos(a1) * u + math.sin(a1) * v)
                    tris.extend([[*tip, *c], [*b0, *c], [*b1, *c]])
        elif self.active_tool == "rotate":
            segs = 44
            r_in = s * 0.96
            r_out = s * 1.04
            for idx, (ax, col) in enumerate(axes):
                c = [1.0, 0.92, 0.20, 1.0] if idx in (self._gizmo_hover_axis, self._gizmo_drag_axis) else col
                u = np.array([0.0, 1.0, 0.0], dtype=np.float32) if abs(ax[1]) < 0.9 else np.array([1.0, 0.0, 0.0], dtype=np.float32)
                u = _normalize(np.cross(ax, u))
                v = _normalize(np.cross(ax, u))
                for k in range(segs):
                    a0 = 2.0 * math.pi * k / segs
                    a1 = 2.0 * math.pi * (k + 1) / segs
                    d0 = math.cos(a0) * u + math.sin(a0) * v
                    d1 = math.cos(a1) * u + math.sin(a1) * v
                    q0, q1, q2, q3 = origin + r_in * d0, origin + r_out * d0, origin + r_out * d1, origin + r_in * d1
                    tris.extend([[*q0, *c], [*q1, *c], [*q2, *c], [*q0, *c], [*q2, *c], [*q3, *c]])
        elif self.active_tool == "scale":
            for idx, (ax, col) in enumerate(axes):
                c = [1.0, 0.92, 0.20, 1.0] if idx in (self._gizmo_hover_axis, self._gizmo_drag_axis) else col
                p0 = origin
                p1 = origin + ax * s
                add_cylinder(p0, p1, ax, s * 0.026, c, segs=6)
                hs = s * 0.085
                self._append_cube_tris(tris, p1, hs, c)

        return (
            np.array(lines, dtype=np.float32) if lines else np.zeros((0, 7), dtype=np.float32),
            np.array(tris, dtype=np.float32) if tris else np.zeros((0, 7), dtype=np.float32),
        )

    @staticmethod
    def _append_cube_tris(tris: list[list[float]], c: np.ndarray, hs: float, col: list[float]) -> None:
        cx, cy, cz = float(c[0]), float(c[1]), float(c[2])
        corners = [
            [cx - hs, cy - hs, cz - hs],
            [cx + hs, cy - hs, cz - hs],
            [cx + hs, cy + hs, cz - hs],
            [cx - hs, cy + hs, cz - hs],
            [cx - hs, cy - hs, cz + hs],
            [cx + hs, cy - hs, cz + hs],
            [cx + hs, cy + hs, cz + hs],
            [cx - hs, cy + hs, cz + hs],
        ]
        faces = [
            (0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4),
            (2, 3, 7, 6), (0, 3, 7, 4), (1, 2, 6, 5),
        ]
        for a, b, d, e in faces:
            tris.extend([[*corners[a], *col], [*corners[b], *col], [*corners[d], *col]])
            tris.extend([[*corners[a], *col], [*corners[d], *col], [*corners[e], *col]])

    @staticmethod
    def _append_diamond_tris(tris: list[list[float]], c: np.ndarray, r: float, col: list[float]) -> None:
        cx, cy, cz = float(c[0]), float(c[1]), float(c[2])
        top = [cx, cy + r * 1.35, cz]
        bot = [cx, cy - r * 0.55, cz]
        ring = [[cx + r, cy, cz], [cx, cy, cz + r], [cx - r, cy, cz], [cx, cy, cz - r]]
        shades = (1.0, 0.84, 0.68, 0.88)
        for k in range(4):
            a = ring[k]
            b = ring[(k + 1) % 4]
            sh = shades[k]
            fc_top = [min(1.0, col[0] * sh), min(1.0, col[1] * sh), min(1.0, col[2] * sh), 1.0]
            fc_bot = [col[0] * sh * 0.72, col[1] * sh * 0.72, col[2] * sh * 0.72, 1.0]
            tris.extend([[*top, *fc_top], [*a, *fc_top], [*b, *fc_top]])
            tris.extend([[*bot, *fc_bot], [*b, *fc_bot], [*a, *fc_bot]])

    def _rebuild_roads_cache(self) -> None:
        self._roads_dirty = False
        self._delete_flat_cache(self._road_cache_vbos)
        if self.world is None:
            return
        g = self.world.road_graph
        ribbon_tris: list[list[float]] = []
        center_lines: list[list[float]] = []
        handle_tris: list[list[float]] = []

        for e in g.edges:
            na = g.nodes.get(e.a)
            nb = g.nodes.get(e.b)
            if not na or not nb:
                continue
            pa = np.array([na.x, na.y + 0.12, na.z], dtype=np.float32)
            pb = np.array([nb.x, nb.y + 0.12, nb.z], dtype=np.float32)
            seg = pb - pa
            seg_3d = float(np.linalg.norm(seg))
            if seg_3d < 1e-4:
                continue
            lxz = math.hypot(float(seg[0]), float(seg[2]))
            if lxz >= 1e-4:
                perp = np.array([-seg[2] / lxz, 0.0, seg[0] / lxz], dtype=np.float32)
            else:
                # Near-vertical climb: keep ribbon visible with a stable lateral normal
                perp = np.array([1.0, 0.0, 0.0], dtype=np.float32)
            hw = e.width * 0.5
            p0 = pa - perp * hw
            p1 = pa + perp * hw
            p2 = pb + perp * hw
            p3 = pb - perp * hw

            rc = [0.20, 0.55, 0.95, 0.32] if not e.two_way else [0.85, 0.45, 0.95, 0.34]
            ribbon_tris.extend([[*p0, *rc], [*p1, *rc], [*p2, *rc], [*p0, *rc], [*p2, *rc], [*p3, *rc]])

            cl_col = [1.0, 0.88, 0.25, 0.95]
            ed_col = [0.70, 0.88, 1.0, 0.85]
            center_lines.extend([[pa[0], pa[1] + 0.08, pa[2], *cl_col], [pb[0], pb[1] + 0.08, pb[2], *cl_col]])
            center_lines.extend([[p0[0], p0[1] + 0.05, p0[2], *ed_col], [p3[0], p3[1] + 0.05, p3[2], *ed_col]])
            center_lines.extend([[p1[0], p1[1] + 0.05, p1[2], *ed_col], [p2[0], p2[1] + 0.05, p2[2], *ed_col]])

        for nid, n in g.nodes.items():
            kind = g.node_kind(nid)
            pos = np.array([n.x, n.y + 0.65, n.z], dtype=np.float32)
            if nid == self.selected_road_node_id:
                col = [0.18, 0.96, 0.46, 1.0]
                rad = 1.45
            elif kind == "junction":
                col = [0.95, 0.25, 0.88, 0.96]
                rad = 1.25
            elif kind == "road end":
                col = [1.0, 0.68, 0.16, 0.96]
                rad = 1.15
            else:
                col = [1.0, 0.92, 0.22, 0.94]
                rad = 0.95
            self._append_diamond_tris(handle_tris, pos, rad, col)

        if ribbon_tris:
            self._road_cache_vbos["ribbon"] = self._upload_flat_vbo(np.array(ribbon_tris, dtype=np.float32))
        if center_lines:
            self._road_cache_vbos["lines"] = self._upload_flat_vbo(np.array(center_lines, dtype=np.float32))
        if handle_tris:
            self._road_cache_vbos["handles"] = self._upload_flat_vbo(np.array(handle_tris, dtype=np.float32))

    def _draw_roads(self, view_proj: np.ndarray) -> int:
        if self.world is None or not self.show_roads:
            return 0
        if self._roads_dirty:
            self._rebuild_roads_cache()

        draws = 0
        GL.glEnable(GL.GL_BLEND)
        GL.glBlendFunc(GL.GL_SRC_ALPHA, GL.GL_ONE_MINUS_SRC_ALPHA)
        if "ribbon" in self._road_cache_vbos:
            GL.glDepthMask(GL.GL_FALSE)
            draws += self._draw_flat_cached(self._road_cache_vbos["ribbon"], GL.GL_TRIANGLES, view_proj)
            GL.glDepthMask(GL.GL_TRUE)
        if "lines" in self._road_cache_vbos:
            draws += self._draw_flat_cached(self._road_cache_vbos["lines"], GL.GL_LINES, view_proj)
        GL.glDisable(GL.GL_BLEND)
        if "handles" in self._road_cache_vbos:
            draws += self._draw_flat_cached(self._road_cache_vbos["handles"], GL.GL_TRIANGLES, view_proj)

        # Hover preview diamond in Road Tool mode (dynamic, tiny)
        if self.active_tool == "road" and self.road_hover_pos is not None:
            g = self.world.road_graph
            hcol = (
                [0.18, 0.96, 0.45, 1.0] if self.road_hover_kind == "join"
                else [0.14, 0.88, 0.98, 1.0] if self.road_hover_kind == "split"
                else [0.98, 0.99, 1.00, 1.0]
            )
            hover_tris: list[list[float]] = []
            self._append_diamond_tris(hover_tris, self.road_hover_pos + np.array([0, 0.9, 0], dtype=np.float32), 1.5, hcol)
            draws += self._draw_flat_verts(np.array(hover_tris, dtype=np.float32), GL.GL_TRIANGLES, view_proj)
            if self.road_chain_last_node is not None and self.road_chain_last_node in g.nodes:
                prev = g.nodes[self.road_chain_last_node]
                hlines = np.array([
                    [prev.x, prev.y + 0.5, prev.z, *hcol],
                    [float(self.road_hover_pos[0]), float(self.road_hover_pos[1]) + 0.5, float(self.road_hover_pos[2]), *hcol],
                ], dtype=np.float32)
                draws += self._draw_flat_verts(hlines, GL.GL_LINES, view_proj)
        return draws

    def _rebuild_markers_cache(self) -> None:
        self._markers_dirty = False
        self._delete_flat_cache(self._marker_cache_vbos)
        if self.world is None or not self.world.markers:
            return
        tris: list[list[float]] = []
        lines: list[list[float]] = []
        for m in self.world.markers:
            pos = np.array(m.position, dtype=np.float32) + np.array([0.0, 1.6, 0.0], dtype=np.float32)
            is_sel = self.selected_marker is m
            if is_sel:
                col = [0.20, 0.98, 0.48, 1.0]
            elif m.kind == "Car":
                col = [0.96, 0.34, 0.28, 0.96]
            elif m.kind in ("Spawn", "Start Grid"):
                col = [0.20, 0.86, 0.96, 0.96]
            elif m.kind == "Chicken":
                col = [1.0, 0.92, 0.25, 0.98]
            elif m.kind == "Trigger":
                col = [0.68, 0.42, 0.98, 0.96]
            elif m.kind == "Breaker":
                col = [0.95, 0.36, 0.85, 0.96]
            else:
                col = [1.0, 0.78, 0.22, 0.96]
            self._append_diamond_tris(tris, pos, 1.45 if is_sel else 1.15, col)
            hdg = math.radians(m.heading_deg)
            fwd_v = np.array([math.sin(hdg), 0.0, math.cos(hdg)], dtype=np.float32)
            right_v = np.array([math.cos(hdg), 0.0, -math.sin(hdg)], dtype=np.float32)
            tip = pos + fwd_v * 5.2
            lines.extend([[*pos, *col], [*tip, *col]])
            base = pos + fwd_v * 3.7
            p_l = base - right_v * 0.85
            p_r = base + right_v * 0.85
            tris.extend([[*tip, *col], [*p_l, *col], [*p_r, *col]])
        if tris:
            self._marker_cache_vbos["tris"] = self._upload_flat_vbo(np.array(tris, dtype=np.float32))
        if lines:
            self._marker_cache_vbos["lines"] = self._upload_flat_vbo(np.array(lines, dtype=np.float32))

    def _draw_markers(self, view_proj: np.ndarray) -> int:
        if self.world is None or not self.show_markers or not self.world.markers:
            return 0
        if self._markers_dirty:
            self._rebuild_markers_cache()
        draws = 0
        if "tris" in self._marker_cache_vbos:
            draws += self._draw_flat_cached(self._marker_cache_vbos["tris"], GL.GL_TRIANGLES, view_proj)
        if "lines" in self._marker_cache_vbos:
            draws += self._draw_flat_cached(self._marker_cache_vbos["lines"], GL.GL_LINES, view_proj)
        return draws

    def _draw_scatter_brush(self, view_proj: np.ndarray) -> int:
        if self.world is None or self.active_tool != "scatter" or self.scatter_hover_pos is None:
            return 0
        cx = float(self.scatter_hover_pos[0])
        cz = float(self.scatter_hover_pos[2])
        rad = float(self.scatter_hud.spn_radius.value())
        segs = 48
        pts: list[list[float]] = []
        col = [0.22, 0.96, 0.65, 0.95]
        prev = None
        for i in range(segs + 1):
            a = 2.0 * math.pi * (i / segs)
            px = cx + math.cos(a) * rad
            pz = cz + math.sin(a) * rad
            py = self.world.collision.ground_height(px, pz) + 0.35
            cur = [px, py, pz, *col]
            if prev is not None:
                pts.append(prev)
                pts.append(cur)
            prev = cur
        # Center cross
        cy = float(self.scatter_hover_pos[1]) + 0.45
        pts.extend([[cx - 1.5, cy, cz, *col], [cx + 1.5, cy, cz, *col]])
        pts.extend([[cx, cy, cz - 1.5, *col], [cx, cy, cz + 1.5, *col]])
        return self._draw_flat_verts(np.array(pts, dtype=np.float32), GL.GL_LINES, view_proj)

    def _draw_selected_barrier(self, view_proj: np.ndarray) -> int:
        if self.world is None or self.selected_barrier_idx < 0:
            return 0
        if self.selected_barrier_idx >= len(self.world.collision.sobl_records):
            return 0
        rec = self.world.collision.sobl_records[self.selected_barrier_idx]
        if rec.get("kind") != "box":
            return 0
        corners = wc.box_corners(rec)
        col = [0.16, 0.95, 1.0, 1.0]
        pts = []
        for a, b in wc.BOX_EDGES:
            pts.append([*corners[a], *col])
            pts.append([*corners[b], *col])
        return self._draw_flat_verts(np.array(pts, dtype=np.float32), GL.GL_LINES, view_proj)

    def _draw_custom_collision_boxes(self, view_proj: np.ndarray) -> int:
        if self.world is None or not self.world.collision.custom_boxes:
            return 0
        edges = [(0, 1), (1, 2), (2, 3), (3, 0), (4, 5), (5, 6), (6, 7), (7, 4), (0, 4), (1, 5), (2, 6), (3, 7)]
        pts: list[list[float]] = []
        col = [0.25, 0.98, 0.45, 0.95]
        for b in self.world.collision.custom_boxes:
            x0, y0, z0 = b["min"]
            x1, y1, z1 = b["max"]
            c = [
                [x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0],
                [x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1],
            ]
            for i0, i1 in edges:
                pts.append([*c[i0], *col])
                pts.append([*c[i1], *col])
        return self._draw_flat_verts(np.array(pts, dtype=np.float32), GL.GL_LINES, view_proj)

    @staticmethod
    def _discover_plume_library() -> str | None:
        override = os.environ.get("EOD_EDITOR_PLUME_LIBRARY")
        if override and Path(override).expanduser().is_file():
            return str(Path(override).expanduser().resolve())
        here = Path(__file__).resolve()
        bundle_root = here.parents[1]
        repo_root = here.parents[2]
        if sys.platform == "darwin":
            name = "eod_editor_plume.dylib"
            candidates = [
                here.parent / name,
                bundle_root / name,
                bundle_root / "Frameworks" / name,
                repo_root / "build-mac-plume" / name,
                repo_root / "build-mac" / name,
                repo_root / "build" / name,
            ]
        elif sys.platform == "win32":
            name = "eod_editor_plume.dll"
            candidates = [
                here.parent / name,
                bundle_root / name,
                repo_root / "build" / name,
                repo_root / "build-win" / name,
            ]
        else:
            name = "eod_editor_plume.so"
            candidates = [here.parent / name, bundle_root / name, repo_root / "build" / name]
        for c in candidates:
            if c.is_file():
                return str(c)
        return override

    @staticmethod
    def _append_ribbon_seg(
        out: list[list[float]],
        p0: np.ndarray,
        p1: np.ndarray,
        cam_pos: np.ndarray,
        half_w: float,
        rgba: tuple[float, float, float, float],
    ) -> None:
        d = p1 - p0
        dlen = float(np.linalg.norm(d))
        if dlen < 1e-5:
            return
        mid = 0.5 * (p0 + p1)
        view_dir = cam_pos - mid
        side = np.cross(d, view_dir)
        slen = float(np.linalg.norm(side))
        if slen < 1e-5:
            side = np.cross(d, np.array([0.0, 1.0, 0.0], dtype=np.float32))
            slen = float(np.linalg.norm(side))
            if slen < 1e-5:
                side = np.array([1.0, 0.0, 0.0], dtype=np.float32)
                slen = 1.0
        off = (side / slen) * float(half_w)
        a0, b0 = p0 - off, p0 + off
        a1, b1 = p1 - off, p1 + off
        r, g, b, al = rgba
        out.append([float(a0[0]), float(a0[1]), float(a0[2]), r, g, b, al])
        out.append([float(b0[0]), float(b0[1]), float(b0[2]), r, g, b, al])
        out.append([float(b1[0]), float(b1[1]), float(b1[2]), r, g, b, al])
        out.append([float(a0[0]), float(a0[1]), float(a0[2]), r, g, b, al])
        out.append([float(b1[0]), float(b1[1]), float(b1[2]), r, g, b, al])
        out.append([float(a1[0]), float(a1[1]), float(a1[2]), r, g, b, al])

    def _build_plume_overlay_verts(self) -> tuple[np.ndarray | None, np.ndarray | None]:
        """Collect 3D triangle overlays for Plume HQ: gizmos (always-on-top) + selection/roads/markers/collision."""
        if self.world is None:
            return None, None
        top_pts: list[list[float]] = []
        depth_pts: list[list[float]] = []
        cam = self.cam_pos

        def _box_edges(target: list[list[float]], lo: np.ndarray, hi: np.ndarray, col: tuple[float, float, float, float], w: float) -> None:
            x0, y0, z0 = float(lo[0]), float(lo[1]), float(lo[2])
            x1, y1, z1 = float(hi[0]), float(hi[1]), float(hi[2])
            corners = [
                np.array([x0, y0, z0], dtype=np.float32), np.array([x1, y0, z0], dtype=np.float32),
                np.array([x1, y1, z0], dtype=np.float32), np.array([x0, y1, z0], dtype=np.float32),
                np.array([x0, y0, z1], dtype=np.float32), np.array([x1, y0, z1], dtype=np.float32),
                np.array([x1, y1, z1], dtype=np.float32), np.array([x0, y1, z1], dtype=np.float32),
            ]
            for i0, i1 in ((0, 1), (1, 2), (2, 3), (3, 0), (4, 5), (5, 6), (6, 7), (7, 4), (0, 4), (1, 5), (2, 6), (3, 7)):
                self._append_ribbon_seg(target, corners[i0], corners[i1], cam, w, col)

        # 1. Selected objects highlight bounding boxes (always visible in Plume HQ!)
        sel_list = [o for o in self.selected_objects if not o.deleted]
        if not sel_list and self.selected_obj is not None and not self.selected_obj.deleted:
            sel_list = [self.selected_obj]
        for obj in sel_list:
            model = self.world.models.get(obj.model_key)
            if model is not None:
                lo, hi = obj.world_aabb(model)
                dist = max(4.0, float(np.linalg.norm(0.5 * (lo + hi) - cam)))
                bw = max(0.05, min(0.45, dist * 0.0035))
                col = (1.0, 0.78, 0.18, 0.95) if obj is self.selected_obj else (0.35, 0.85, 1.0, 0.90)
                _box_edges(top_pts, lo, hi, col, bw)

        # 2. Active 3D Transform Gizmo (Move / Rotate / Scale) on top_pts
        origin, _gkind = self._active_gizmo_origin()
        if origin is not None:
            sc = self._gizmo_scale(origin)
            ax_w = max(0.06, sc * 0.028)
            axes = [
                (0, np.array([1.0, 0.0, 0.0], dtype=np.float32), (1.0, 0.28, 0.28, 1.0)),
                (1, np.array([0.0, 1.0, 0.0], dtype=np.float32), (0.28, 1.0, 0.42, 1.0)),
                (2, np.array([0.0, 0.0, 1.0], dtype=np.float32), (0.32, 0.62, 1.0, 1.0)),
            ]
            for ax_idx, ax_vec, base_col in axes:
                hl = (ax_idx == self._gizmo_hover_axis or ax_idx == self._gizmo_drag_axis)
                col = (1.0, 0.95, 0.25, 1.0) if hl else base_col
                if self.active_tool == "rotate":
                    u = _normalize(np.cross(ax_vec, np.array([0.0, 1.0, 0.0], dtype=np.float32) if ax_idx != 1 else np.array([1.0, 0.0, 0.0], dtype=np.float32)))
                    v = _normalize(np.cross(ax_vec, u))
                    rad = sc * 0.85
                    segs = 28
                    for k in range(segs):
                        a0 = 2.0 * math.pi * k / segs
                        a1 = 2.0 * math.pi * (k + 1) / segs
                        p0 = origin + (u * math.cos(a0) + v * math.sin(a0)) * rad
                        p1 = origin + (u * math.cos(a1) + v * math.sin(a1)) * rad
                        self._append_ribbon_seg(top_pts, p0, p1, cam, ax_w * (1.4 if hl else 1.0), col)
                else:
                    tip = origin + ax_vec * sc
                    self._append_ribbon_seg(top_pts, origin, tip, cam, ax_w * (1.5 if hl else 1.0), col)
                    # Tip cap (arrow/cube indicator)
                    cap = sc * 0.12
                    _box_edges(top_pts, tip - cap, tip + cap, col, ax_w * 0.85)

        # 3. Road spline overlay (depth-tested)
        if self.show_roads and self.world.road_graph.nodes:
            nodes = self.world.road_graph.nodes
            for edge in self.world.road_graph.edges:
                na, nb = nodes.get(edge.a), nodes.get(edge.b)
                if na is None or nb is None:
                    continue
                p0 = np.array([na.x, na.y + 0.55, na.z], dtype=np.float32)
                p1 = np.array([nb.x, nb.y + 0.55, nb.z], dtype=np.float32)
                col = (0.85, 0.45, 1.0, 0.85) if edge.two_way else (0.20, 0.85, 1.0, 0.85)
                self._append_ribbon_seg(depth_pts, p0, p1, cam, 0.35, col)
            if self.selected_road_node_id is not None and self.selected_road_node_id in nodes:
                sn = nodes[self.selected_road_node_id]
                sp = np.array([sn.x, sn.y + 0.65, sn.z], dtype=np.float32)
                _box_edges(top_pts, sp - 1.1, sp + 1.1, (1.0, 0.90, 0.2, 1.0), 0.14)

        # 4. World Markers (spawns, start grids, triggers, car spots)
        if self.show_markers and self.world.markers:
            for mk in self.world.markers:
                mp = np.array(mk.position, dtype=np.float32)
                is_sel = mk is self.selected_marker
                col = (1.0, 0.92, 0.25, 1.0) if is_sel else (0.20, 0.95, 0.48, 0.92)
                self._append_ribbon_seg(depth_pts, mp, mp + np.array([0.0, 3.6, 0.0], dtype=np.float32), cam, 0.22 if is_sel else 0.15, col)
                rad = math.radians(mk.heading_deg)
                fwd = np.array([math.sin(rad), 0.0, math.cos(rad)], dtype=np.float32) * 3.0
                self._append_ribbon_seg(depth_pts, mp + np.array([0.0, 1.8, 0.0], dtype=np.float32), mp + np.array([0.0, 1.8, 0.0], dtype=np.float32) + fwd, cam, 0.14, col)

        # 5. Ghost placement / Scatter brush ring
        if self.active_tool == "place" and self.ghost_pos is not None:
            gp = self.ghost_pos
            _box_edges(top_pts, gp + np.array([-2.2, 0.0, -2.2], dtype=np.float32), gp + np.array([2.2, 2.5, 2.2], dtype=np.float32), (0.2, 1.0, 0.65, 0.95), 0.12)
        elif self.active_tool == "scatter" and self.scatter_hover_pos is not None:
            sp = self.scatter_hover_pos
            rad = float(self.scatter_hud.spn_radius.value())
            segs = 28
            for k in range(segs):
                a0 = 2.0 * math.pi * k / segs
                a1 = 2.0 * math.pi * (k + 1) / segs
                p0 = np.array([sp[0] + math.cos(a0) * rad, sp[1] + 0.5, sp[2] + math.sin(a0) * rad], dtype=np.float32)
                p1 = np.array([sp[0] + math.cos(a1) * rad, sp[1] + 0.5, sp[2] + math.sin(a1) * rad], dtype=np.float32)
                self._append_ribbon_seg(top_pts, p0, p1, cam, 0.25, (0.2, 0.95, 0.55, 0.95))

        # 6. Collision SOBL barriers when Collision overlay is ON
        if self.show_collision and self.world.collision.sobl_records:
            for rec in self.world.collision.sobl_records[:600]:
                if rec.get("kind") != "box":
                    continue
                c = np.array(rec["centre"], dtype=np.float32)
                r = np.array(rec["radii"], dtype=np.float32)
                is_sel = int(rec.get("index", -1)) == self.selected_barrier_idx
                col = (1.0, 0.95, 0.2, 0.95) if is_sel else (1.0, 0.42, 0.15, 0.72)
                _box_edges(top_pts if is_sel else depth_pts, c - r, c + r, col, 0.16 if is_sel else 0.09)

        top_arr = np.array(top_pts, dtype=np.float32) if top_pts else None
        depth_arr = np.array(depth_pts, dtype=np.float32) if depth_pts else None
        return top_arr, depth_arr

    # -----------------------------------------------------------------------
    # Paint Frame
    # -----------------------------------------------------------------------
    def _paint_plume(self) -> bool:
        if self.render_mode != "plume" or self._plume_module is None or self.world is None or not self.isVisible():
            return False
        with get_profiler().zone("viewport.plume_paint", "render"):
            return self._paint_plume_impl()

    def _paint_plume_impl(self) -> bool:
        try:
            if self._plume_viewport is None:
                self._plume_viewport = PlumeViewport(self, self._plume_module)
                self._plume_viewport.setGeometry(self.rect())
            child = self._plume_viewport
            child.open_surface()
            if not self._plume_dirty and self._pending_tex_updates:
                self._plume_dirty = child.update_textures(self.world, self._pending_tex_updates)
            if self._plume_dirty:
                child.upload(self.world, self.layer_visibility)
                self._plume_dirty = False
            top_v, depth_v = self._build_plume_overlay_verts()
            child.upload_overlay(top_v, depth_v)
            fwd, right, up = self.camera_vectors()
            view = lh_look_matrix(self.cam_pos, fwd, right, up)
            proj = lh_perspective(self.cam_fov, max(1, self.width()) / max(1, self.height()), 0.4, 4500.0)
            first = child.presented == 0
            if child.draw(proj @ view, self.world.environment):
                self._plume_retry = False
                if first:
                    self.statusMessage.emit(f"Native Plume viewport presented {child.triangles:,} track triangles")
                return True
            if not self._plume_retry:
                self._plume_retry = True
                QTimer.singleShot(40, self, self.update)
                return True
            raise RuntimeError("Vulkan swapchain did not present")
        except (OSError, RuntimeError, AttributeError) as error:
            if self._plume_viewport:
                self._plume_viewport.close_surface()
            self._plume_module = None
            self.statusMessage.emit(f"Native Plume viewport unavailable ({error}). OpenGL preview active.")
            return False

    def paintGL(self) -> None:
        if self._paint_plume():
            if self._pending_tex_updates:
                self._flush_pending_textures()
            return
        t0 = time.perf_counter_ns()
        self.gpu_timer.begin_frame()
        if self._pending_tex_updates:
            self._flush_pending_textures()
        if self._static_dirty or self._dynamic_dirty:
            self._sync_dirty_scene_batches()
        if self._sel_dirty:
            self._rebuild_selection_batches()
        if self._ghost_dirty:
            self._rebuild_ghost_batches()
        if self._col_dirty:
            self._rebuild_collision_vbo()

        w = max(1, self.width())
        h = max(1, self.height())
        aspect = w / h

        fwd, right, up = self.camera_vectors()
        view = lh_look_matrix(self.cam_pos, fwd, right, up)
        proj = lh_perspective(self.cam_fov, aspect, 0.4, 4500.0)
        view_proj = proj @ view
        ident = np.eye(4, dtype=np.float32)

        env = self.world.environment if self.world is not None else None
        sky_z = env.sky_zenith if env else (0.11, 0.21, 0.36)
        sky_m = env.sky_mid if env else (0.28, 0.45, 0.66)
        sky_h = env.sky_horizon if env else (0.56, 0.68, 0.79)
        sky_g = env.sky_ground if env else (0.14, 0.16, 0.20)
        sun_d = env.sun_direction() if env else (0.45, 0.82, 0.36)
        sun_c = env.sun_color if env else (0.98, 0.95, 0.88)
        amb_i = env.ambient_intensity if env else 1.0
        fog_c = env.fog_color if env else (0.44, 0.55, 0.68)
        fog_s = env.fog_start if env else 350.0
        fog_e = env.fog_end if env else 2200.0
        fog_d = env.fog_density if env else 0.72

        GL.glClearColor(0.11, 0.13, 0.17, 1.0)
        GL.glClear(GL.GL_COLOR_BUFFER_BIT | GL.GL_DEPTH_BUFFER_BIT)

        draws = 0
        tris_drawn = 0

        # 1. Sky gradient dome + live solar disc (cached uniform locations)
        GL.glDisable(GL.GL_BLEND)
        GL.glDisable(GL.GL_DEPTH_TEST)
        GL.glDepthMask(GL.GL_FALSE)
        GL.glUseProgram(self._prog_sky)
        us = self._u_sky
        GL.glUniform3fv(us["uCamFwd"], 1, fwd)
        GL.glUniform3fv(us["uCamRight"], 1, right)
        GL.glUniform3fv(us["uCamUp"], 1, up)
        GL.glUniform1f(us["uTanHalfFov"], math.tan(math.radians(self.cam_fov) * 0.5))
        GL.glUniform1f(us["uAspect"], aspect)
        GL.glUniform3f(us["uSkyZenith"], *sky_z)
        GL.glUniform3f(us["uSkyMid"], *sky_m)
        GL.glUniform3f(us["uSkyHorizon"], *sky_h)
        GL.glUniform3f(us["uSkyGround"], *sky_g)
        GL.glUniform3f(us["uSunDir"], *sun_d)
        GL.glUniform3f(us["uSunColor"], *sun_c)
        GL.glBindVertexArray(self._vao_empty)
        GL.glDrawArrays(GL.GL_TRIANGLES, 0, 3)
        draws += 1
        GL.glDepthMask(GL.GL_TRUE)
        GL.glEnable(GL.GL_DEPTH_TEST)
        GL.glDisable(GL.GL_CULL_FACE)

        # 2. Ground grid
        if self.show_grid and self._grid_count > 0:
            GL.glEnable(GL.GL_BLEND)
            GL.glBlendFunc(GL.GL_SRC_ALPHA, GL.GL_ONE_MINUS_SRC_ALPHA)
            GL.glUseProgram(self._prog_flat)
            GL.glUniformMatrix4fv(self._u_flat["uViewProj"], 1, GL.GL_TRUE, view_proj)
            GL.glUniformMatrix4fv(self._u_flat["uModel"], 1, GL.GL_TRUE, ident)
            GL.glUniform1i(self._u_flat["uUseOverride"], 0)
            GL.glBindVertexArray(self._grid_vao)
            GL.glDrawArrays(GL.GL_LINES, 0, self._grid_count)
            GL.glBindVertexArray(0)
            GL.glDisable(GL.GL_BLEND)
            draws += 1

        # 3. Static world geometry batches (cached uniform locations)
        GL.glUseProgram(self._prog_world)
        uw = self._u_world
        loc_mod = uw["uModel"]
        loc_has = uw["uHasTex"]
        loc_cut = uw["uCutout"]
        loc_lit = uw["uLighting"]
        loc_tint = uw["uTint"]
        loc_hi = uw["uHighlightMix"]

        GL.glUniformMatrix4fv(uw["uViewProj"], 1, GL.GL_TRUE, view_proj)
        GL.glUniformMatrix4fv(loc_mod, 1, GL.GL_TRUE, ident)
        GL.glUniform1i(uw["uTex"], 0)
        GL.glUniform1i(loc_lit, 1 if self.show_lighting else 0)
        GL.glUniform1i(uw["uRenderMode"], 1 if self.render_mode == "plume" else 0)
        GL.glUniform3fv(uw["uCamPos"], 1, self.cam_pos)
        GL.glUniform4f(loc_tint, 1.0, 1.0, 1.0, 1.0)
        GL.glUniform1f(loc_hi, 0.0)
        GL.glUniform3f(uw["uSunDir"], *sun_d)
        GL.glUniform3f(uw["uSunColor"], *sun_c)
        GL.glUniform1f(uw["uAmbientIntensity"], float(amb_i))
        GL.glUniform3f(uw["uFogColor"], *fog_c)
        GL.glUniform1f(uw["uFogStart"], float(fog_s))
        GL.glUniform1f(uw["uFogEnd"], float(fog_e))
        GL.glUniform1f(uw["uFogDensity"], float(fog_d))

        if self.show_wireframe:
            GL.glPolygonMode(GL.GL_FRONT_AND_BACK, GL.GL_LINE)

        GL.glActiveTexture(GL.GL_TEXTURE0)

        # 3a. Terrain pass (static + optional-terrain)
        self.gpu_timer.begin_pass("terrain")
        for b in self._terrain_batches:
            tex_entry = self._gl_textures.get(b["tex"])
            if tex_entry is not None:
                GL.glBindTexture(GL.GL_TEXTURE_2D, tex_entry[0])
                GL.glUniform1i(loc_has, 1)
                GL.glUniform1i(loc_cut, 1 if tex_entry[1] else 0)
            else:
                GL.glUniform1i(loc_has, 0)
                GL.glUniform1i(loc_cut, 0)
            GL.glBindVertexArray(b["vao"])
            GL.glDrawElements(GL.GL_TRIANGLES, b["count"], GL.GL_UNSIGNED_INT, None)
            draws += 1
            tris_drawn += b["tris"]
        self.gpu_timer.end_pass("terrain")

        # 3b. Props + dynamic edited-object pass (dyno, event props, cars/wrecks, billboards, edited meshes)
        self.gpu_timer.begin_pass("props")
        for b in (*self._prop_batches, *self._dynamic_batches):
            tex_entry = self._gl_textures.get(b["tex"])
            if tex_entry is not None:
                GL.glBindTexture(GL.GL_TEXTURE_2D, tex_entry[0])
                GL.glUniform1i(loc_has, 1)
                GL.glUniform1i(loc_cut, 1 if tex_entry[1] else 0)
            else:
                GL.glUniform1i(loc_has, 0)
                GL.glUniform1i(loc_cut, 0)
            GL.glBindVertexArray(b["vao"])
            GL.glDrawElements(GL.GL_TRIANGLES, b["count"], GL.GL_UNSIGNED_INT, None)
            draws += 1
            tris_drawn += b["tris"]
        self.gpu_timer.end_pass("props")

        # 3c. Translucent water / blend surfaces (e.g. Afton pond `sween.tex`)
        self.gpu_timer.begin_pass("water")
        if self._blend_batches or self._dynamic_blend_batches:
            GL.glEnable(GL.GL_BLEND)
            GL.glBlendFunc(GL.GL_SRC_ALPHA, GL.GL_ONE_MINUS_SRC_ALPHA)
            GL.glDepthMask(GL.GL_FALSE)
            for b in (*self._blend_batches, *self._dynamic_blend_batches):
                tex_entry = self._gl_textures.get(b["tex"])
                if tex_entry is not None:
                    GL.glBindTexture(GL.GL_TEXTURE_2D, tex_entry[0])
                    GL.glUniform1i(loc_has, 1)
                    GL.glUniform1i(loc_cut, 0)
                else:
                    GL.glUniform1i(loc_has, 0)
                    GL.glUniform1i(loc_cut, 0)
                GL.glBindVertexArray(b["vao"])
                GL.glDrawElements(GL.GL_TRIANGLES, b["count"], GL.GL_UNSIGNED_INT, None)
                draws += 1
                tris_drawn += b["tris"]
            GL.glDepthMask(GL.GL_TRUE)
            GL.glDisable(GL.GL_BLEND)
        self.gpu_timer.end_pass("water")

        # 4. Selected object(s) with bright translucent green highlight
        if self.selected_obj is not None and not self.selected_obj.deleted and self._sel_batches:
            is_world_baked = bool(self._sel_batches[0].get("world_baked", False))
            m4 = ident if is_world_baked else self.selected_obj.world_matrix()
            GL.glEnable(GL.GL_POLYGON_OFFSET_FILL)
            GL.glPolygonOffset(-1.0, -1.0)
            GL.glUniformMatrix4fv(loc_mod, 1, GL.GL_TRUE, m4)
            GL.glUniform1f(loc_hi, 0.48)
            for b in self._sel_batches:
                tex_entry = self._gl_textures.get(b["tex"])
                if tex_entry is not None:
                    GL.glBindTexture(GL.GL_TEXTURE_2D, tex_entry[0])
                    GL.glUniform1i(loc_has, 1)
                    GL.glUniform1i(loc_cut, 1 if tex_entry[1] else 0)
                else:
                    GL.glUniform1i(loc_has, 0)
                    GL.glUniform1i(loc_cut, 0)
                GL.glBindVertexArray(b["vao"])
                GL.glDrawElements(GL.GL_TRIANGLES, b["count"], GL.GL_UNSIGNED_INT, None)
                draws += 1
                tris_drawn += b["tris"]
            GL.glDisable(GL.GL_POLYGON_OFFSET_FILL)

            # Crisp bright-green translucent rim/wireframe accent
            GL.glPolygonMode(GL.GL_FRONT_AND_BACK, GL.GL_LINE)
            GL.glEnable(GL.GL_BLEND)
            GL.glBlendFunc(GL.GL_SRC_ALPHA, GL.GL_ONE_MINUS_SRC_ALPHA)
            GL.glEnable(GL.GL_POLYGON_OFFSET_LINE)
            GL.glPolygonOffset(-1.0, -1.0)
            GL.glUniform1i(loc_has, 0)
            GL.glUniform1i(loc_lit, 0)
            GL.glUniform1f(loc_hi, 1.0)
            GL.glUniform4f(loc_tint, 0.22, 1.0, 0.48, 0.62)
            for b in self._sel_batches:
                GL.glBindVertexArray(b["vao"])
                GL.glDrawElements(GL.GL_TRIANGLES, b["count"], GL.GL_UNSIGNED_INT, None)
                draws += 1
            GL.glDisable(GL.GL_POLYGON_OFFSET_LINE)
            GL.glDisable(GL.GL_BLEND)
            GL.glUniform1i(loc_lit, 1 if self.show_lighting else 0)

        # 4b. Lumion/Twinmotion Live Ghost Placement Hologram Preview!
        if (
            self.active_tool == "place"
            and self.ghost_pos is not None
            and self.ghost_model_key
            and self._ghost_batches
            and self.world is not None
            and self.ghost_model_key in self.world.models
        ):
            wm = self.world.models[self.ghost_model_key]
            gpos = (
                float(self.ghost_pos[0]),
                float(self.ghost_pos[1]) - float(wm.bbox_min[1]),
                float(self.ghost_pos[2]),
            )
            grot = (
                normal_to_pitch_roll(self.ghost_normal, self.ghost_yaw)
                if self.place_hud.chk_align.isChecked()
                else (self.ghost_yaw, 0.0, 0.0)
            )
            gm4 = compose_transform(gpos, grot, (1.0, 1.0, 1.0))
            GL.glPolygonMode(GL.GL_FRONT_AND_BACK, GL.GL_FILL)
            GL.glEnable(GL.GL_BLEND)
            GL.glBlendFunc(GL.GL_SRC_ALPHA, GL.GL_ONE_MINUS_SRC_ALPHA)
            GL.glUniformMatrix4fv(loc_mod, 1, GL.GL_TRUE, gm4)
            GL.glUniform1f(loc_hi, 0.25)
            GL.glUniform4f(loc_tint, 0.45, 0.95, 1.0, 0.56)
            for b in self._ghost_batches:
                tex_entry = self._gl_textures.get(b["tex"])
                if tex_entry is not None:
                    GL.glBindTexture(GL.GL_TEXTURE_2D, tex_entry[0])
                    GL.glUniform1i(loc_has, 1)
                    GL.glUniform1i(loc_cut, 1 if tex_entry[1] else 0)
                else:
                    GL.glUniform1i(loc_has, 0)
                    GL.glUniform1i(loc_cut, 0)
                GL.glBindVertexArray(b["vao"])
                GL.glDrawElements(GL.GL_TRIANGLES, b["count"], GL.GL_UNSIGNED_INT, None)
                draws += 1
            GL.glDisable(GL.GL_BLEND)

        GL.glPolygonMode(GL.GL_FRONT_AND_BACK, GL.GL_FILL)
        GL.glBindVertexArray(0)

        # 5. Overlays pass (Collision SCOL+SOBL, ILIN roads, markers, scatter brush, 3D gizmo)
        self.gpu_timer.begin_pass("overlays")
        if self.show_collision:
            GL.glEnable(GL.GL_BLEND)
            GL.glBlendFunc(GL.GL_SRC_ALPHA, GL.GL_ONE_MINUS_SRC_ALPHA)
            GL.glUseProgram(self._prog_flat)
            GL.glUniformMatrix4fv(self._u_flat["uViewProj"], 1, GL.GL_TRUE, view_proj)
            GL.glUniformMatrix4fv(self._u_flat["uModel"], 1, GL.GL_TRUE, ident)
            GL.glUniform1i(self._u_flat["uUseOverride"], 0)
            if self._scol_count > 0:
                GL.glBindVertexArray(self._scol_vao)
                GL.glDrawArrays(GL.GL_LINES, 0, self._scol_count)
                draws += 1
            if self._sobl_count > 0:
                GL.glBindVertexArray(self._sobl_vao)
                GL.glDrawArrays(GL.GL_LINES, 0, self._sobl_count)
                draws += 1
            GL.glBindVertexArray(0)
            draws += self._draw_selected_barrier(view_proj)
            draws += self._draw_custom_collision_boxes(view_proj)
            GL.glDisable(GL.GL_BLEND)

        # 6. ILIN Racing Lines / Road Tool overlay
        draws += self._draw_roads(view_proj)

        # 7. Markers / Cars / Spawns / Start Grid
        draws += self._draw_markers(view_proj)

        # 7b. Scatter Brush terrain ring
        draws += self._draw_scatter_brush(view_proj)

        # 8. Interactive 3D Transform Gizmo (for selected Object, Marker, OR Road Node!)
        gizmo_origin, _gizmo_kind = self._active_gizmo_origin()
        if gizmo_origin is not None:
            glines, gtris = self._build_gizmo_geometry(gizmo_origin)
            GL.glClear(GL.GL_DEPTH_BUFFER_BIT)
            draws += self._draw_flat_verts(glines, GL.GL_LINES, view_proj)
            draws += self._draw_flat_verts(gtris, GL.GL_TRIANGLES, view_proj)
        self.gpu_timer.end_pass("overlays")

        gpu_timings = self.gpu_timer.end_frame()
        self._last_draw_calls = draws
        self._last_tris_drawn = tris_drawn
        self._last_frame_ms = (time.perf_counter_ns() - t0) / 1_000_000.0
        self.profiler.record_frame(self._last_frame_ms, gpu_timings, draws, tris_drawn)
        if self.show_perf_hud:
            self.perf_hud.refresh(self.profiler.snapshot())
            self._position_overlays()
        self._emit_stats()

        # Optional 2D screen-space labels overlay (isolate GL state before QPainter!)
        if self.show_labels and self.world is not None:
            GL.glUseProgram(0)
            GL.glBindVertexArray(0)
            GL.glBindBuffer(GL.GL_ARRAY_BUFFER, 0)
            GL.glBindTexture(GL.GL_TEXTURE_2D, 0)
            GL.glDisable(GL.GL_DEPTH_TEST)
            self._paint_labels(view_proj)

    def _paint_labels(self, view_proj: np.ndarray) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.TextAntialiasing, True)
        painter.setFont(QFont("Segoe UI", 8, QFont.Weight.DemiBold))
        w, h = self.width(), self.height()

        def project_pt(p: tuple[float, float, float]) -> tuple[int, int] | None:
            v = np.array([p[0], p[1], p[2], 1.0], dtype=np.float32)
            clip = view_proj @ v
            if clip[3] <= 0.5:
                return None
            ndc = clip[:3] / clip[3]
            if abs(ndc[0]) > 1.05 or abs(ndc[1]) > 1.05 or ndc[2] < -1.0 or ndc[2] > 1.0:
                return None
            return int((ndc[0] * 0.5 + 0.5) * w), int((0.5 - ndc[1] * 0.5) * h)

        # Label nearby objects (up to 40 closest) + markers
        candidates: list[tuple[float, tuple[float, float, float], str, QColor]] = []
        for m in self.world.markers:
            d = float(np.linalg.norm(np.array(m.position) - self.cam_pos))
            if d < 320.0:
                candidates.append((d, (m.position[0], m.position[1] + 3.2, m.position[2]), f"[{m.kind}] {m.name}", QColor("#67e8f9")))
        for o in self.world.objects:
            if o.deleted or not o.visible:
                continue
            d = float(np.linalg.norm(np.array(o.position) - self.cam_pos))
            if d < 120.0 or o is self.selected_obj:
                col = QColor("#4ade80") if o is self.selected_obj else QColor("#e2e8f0")
                candidates.append((d, (o.position[0], o.position[1] + 2.2, o.position[2]), o.name, col))
        candidates.sort(key=lambda t: t[0])
        for _d, pos, text, col in candidates[:40]:
            sp = project_pt(pos)
            if sp is None:
                continue
            sx, sy = sp
            painter.setPen(QColor(0, 0, 0, 190))
            painter.drawText(sx + 1, sy + 1, text)
            painter.setPen(col)
            painter.drawText(sx, sy, text)
        painter.end()

    # -----------------------------------------------------------------------
    # Picking & Gizmo Hit Testing
    # -----------------------------------------------------------------------
    def _active_gizmo_origin(self) -> tuple[np.ndarray | None, str]:
        if self.active_tool not in ("move", "rotate", "scale", "select"):
            return None, ""
        if self.selected_obj is not None and not self.selected_obj.deleted:
            return np.array(self.selected_obj.position, dtype=np.float32), "obj"
        if self.selected_marker is not None:
            return np.array(self.selected_marker.position, dtype=np.float32), "marker"
        if self.selected_road_node_id is not None and self.world is not None:
            n = self.world.road_graph.nodes.get(self.selected_road_node_id)
            if n is not None:
                return np.array([n.x, n.y + 0.65, n.z], dtype=np.float32), "road_node"
        return None, ""

    def _pick_gizmo_axis(self, ray_o: np.ndarray, ray_d: np.ndarray) -> int:
        origin, _kind = self._active_gizmo_origin()
        if origin is None:
            return -1
        s = self._gizmo_scale(origin)
        axes = [
            np.array([1.0, 0.0, 0.0], dtype=np.float32),
            np.array([0.0, 1.0, 0.0], dtype=np.float32),
            np.array([0.0, 0.0, 1.0], dtype=np.float32),
        ]
        best_axis = -1
        best_dist = s * 0.16

        if self.active_tool == "rotate":
            for idx, ax in enumerate(axes):
                hit = ray_plane_intersect(ray_o, ray_d, origin, ax)
                if hit is None:
                    continue
                rad = float(np.linalg.norm(hit - origin))
                err = abs(rad - s)
                if err < best_dist:
                    best_dist = err
                    best_axis = idx
            return best_axis

        for idx, ax in enumerate(axes):
            t_axis, dist = closest_point_ray_to_line(ray_o, ray_d, origin, ax)
            if -0.1 * s <= t_axis <= 1.3 * s and dist < best_dist:
                best_dist = dist
                best_axis = idx
        return best_axis

    def _pick_sobl_barrier(self, ray_o: np.ndarray, ray_d: np.ndarray) -> int:
        """Pick the closest SOBL collision box using exact oriented bounding box (OBB) ray intersection."""
        if self.world is None or not self.show_collision:
            return -1
        best_idx = -1
        best_t = 1e9
        for rec in self.world.collision.sobl_records:
            if rec.get("kind") != "box":
                continue
            centre = np.array(rec["centre"], dtype=np.float32)
            radii = np.array(rec["radii"], dtype=np.float32)
            axes = np.array(rec["axes"], dtype=np.float32)  # 3x3 row-major basis
            # Transform ray into box-local coordinates so rotated SOBL walls pick accurately!
            rel_o = ray_o - centre
            local_o = axes @ rel_o
            local_d = axes @ ray_d
            inv_ld = np.where(np.abs(local_d) > 1e-8, 1.0 / local_d, 1e12).astype(np.float32)
            t = ray_aabb_hit(local_o, inv_ld, -radii, radii)
            if t is not None and t < best_t:
                best_t = t
                best_idx = int(rec["index"])
        return best_idx

    def _raycast_terrain_point(self, ray_o: np.ndarray, ray_d: np.ndarray) -> np.ndarray | None:
        """March ray against SCOL terrain heightfield (fallback y=0 plane)."""
        if self.world is not None and self.world.collision.terrain is not None:
            terr = self.world.collision.terrain
            prev_t = 0.0
            prev_diff = float(ray_o[1]) - terr.ground(float(ray_o[0]), float(ray_o[2]))["y"]
            for step in range(1, 140):
                t = step * 6.0
                p = ray_o + ray_d * t
                gy = terr.ground(float(p[0]), float(p[2]))["y"]
                diff = float(p[1]) - gy
                if diff <= 0.0 and prev_diff > 0.0:
                    # Binary refine
                    lo_t, hi_t = prev_t, t
                    for _ in range(7):
                        mid = 0.5 * (lo_t + hi_t)
                        mp = ray_o + ray_d * mid
                        mgy = terr.ground(float(mp[0]), float(mp[2]))["y"]
                        if float(mp[1]) - mgy > 0.0:
                            lo_t = mid
                        else:
                            hi_t = mid
                    fp = ray_o + ray_d * hi_t
                    fy = terr.ground(float(fp[0]), float(fp[2]))["y"]
                    return np.array([fp[0], fy, fp[2]], dtype=np.float32)
                prev_t, prev_diff = t, diff
        plane_y = self.world.world_center[1] if self.world else 0.0
        return ray_plane_intersect(
            ray_o, ray_d, np.array([0.0, plane_y, 0.0], dtype=np.float32), np.array([0.0, 1.0, 0.0], dtype=np.float32)
        )

    def _pick_road_node_with_t(self, ray_o: np.ndarray, ray_d: np.ndarray, max_rad: float = 3.2) -> tuple[int | None, float]:
        if self.world is None or not self.show_roads:
            return None, 1e9
        best_id = None
        best_t = 1e9
        for nid, n in self.world.road_graph.nodes.items():
            p = np.array([n.x, n.y + 0.65, n.z], dtype=np.float32)
            v = p - ray_o
            t = float(np.dot(v, ray_d))
            if t < 0.5:
                continue
            perp = float(np.linalg.norm(v - ray_d * t))
            tol = max(max_rad, min(14.0, t * 0.022))
            if perp < tol and t < best_t:
                best_t = t
                best_id = nid
        return best_id, best_t

    def _pick_road_node(self, ray_o: np.ndarray, ray_d: np.ndarray, max_rad: float = 3.2) -> int | None:
        nid, _t = self._pick_road_node_with_t(ray_o, ray_d, max_rad=max_rad)
        return nid

    def _pick_marker_with_t(self, ray_o: np.ndarray, ray_d: np.ndarray) -> tuple[WorldMarker | None, float]:
        if self.world is None or not self.show_markers:
            return None, 1e9
        best = None
        best_t = 1e9
        for m in self.world.markers:
            p = np.array(m.position, dtype=np.float32) + np.array([0.0, 1.6, 0.0], dtype=np.float32)
            v = p - ray_o
            t = float(np.dot(v, ray_d))
            if t < 0.5:
                continue
            perp = float(np.linalg.norm(v - ray_d * t))
            if perp < max(2.2, min(12.0, t * 0.022)) and t < best_t:
                best_t = t
                best = m
        return best, best_t

    def _pick_marker(self, ray_o: np.ndarray, ray_d: np.ndarray) -> WorldMarker | None:
        mk, _t = self._pick_marker_with_t(ray_o, ray_d)
        return mk

    def _pick_object_at_screen(self, sx: float, sy: float) -> WorldObject | None:
        """Primary pixel-exact picker: GPU Object-ID FBO (PS2 Fast) or exact CPU ray-triangle (Plume HQ / fallback)."""
        with get_profiler().zone("viewport.pick_screen", "pick"):
            if self.render_mode != "plume" and self._gl_ready and self._prog_id and not self._static_dirty:
                gpu_hit = self._pick_object_gpu(sx, sy)
                if gpu_hit is not None:
                    return gpu_hit
            ray_o, ray_d = self.screen_ray(sx, sy)
            return self._pick_object(ray_o, ray_d)

    def _ensure_pick_aabb_cache(self) -> None:
        """Build or refresh vectorized (N, 3) world AABB arrays for O(1)-NumPy broadphase picking."""
        world = self.world
        if world is None:
            self._invalidate_pick_aabb_cache()
            return
        sig = (
            id(world),
            world.active_event_template,
            tuple(sorted(self.layer_visibility.items())),
            len(world.objects),
        )
        if not self._pick_aabb_dirty and self._pick_aabb_sig == sig and self._pick_aabb_lo is not None:
            return

        models = world.models
        layer_vis = self.layer_visibility
        vis_fn = world.object_visible_in_event_template
        objs: list[WorldObject] = []
        local_lo_list: list[np.ndarray] = []
        local_hi_list: list[np.ndarray] = []
        pos_list: list[tuple[float, float, float]] = []
        rot_list: list[tuple[float, float, float]] = []
        scl_list: list[tuple[float, float, float]] = []

        for obj in world.objects:
            if not vis_fn(obj):
                continue
            if not layer_vis.get(obj.category, True):
                continue
            model = models.get(obj.model_key)
            if model is None:
                continue
            objs.append(obj)
            local_lo_list.append(model.bbox_min)
            local_hi_list.append(model.bbox_max)
            pos_list.append(obj.position)
            rot_list.append(obj.rotation_hpr)
            scl_list.append(obj.scale)

        if not objs:
            self._pick_aabb_lo = np.empty((0, 3), dtype=np.float32)
            self._pick_aabb_hi = np.empty((0, 3), dtype=np.float32)
            self._pick_aabb_objs = []
            self._pick_aabb_sig = sig
            self._pick_aabb_dirty = False
            return

        local_lo = np.asarray(local_lo_list, dtype=np.float32)
        local_hi = np.asarray(local_hi_list, dtype=np.float32)
        pos = np.asarray(pos_list, dtype=np.float32)
        rot_rad = np.deg2rad(np.asarray(rot_list, dtype=np.float32))
        scl = np.asarray(scl_list, dtype=np.float32)

        ch = np.cos(rot_rad[:, 0])
        sh = np.sin(rot_rad[:, 0])
        cp = np.cos(rot_rad[:, 1])
        sp = np.sin(rot_rad[:, 1])
        cr = np.cos(rot_rad[:, 2])
        sr = np.sin(rot_rad[:, 2])

        # Exact Ry(h) @ Rx(p) @ Rz(r) @ diag(sx, sy, sz) matching world_loader.compose_transform
        sx, sy, sz = scl[:, 0], scl[:, 1], scl[:, 2]
        m = np.empty((len(objs), 3, 3), dtype=np.float32)
        m[:, 0, 0] = (ch * cr + sh * sp * sr) * sx
        m[:, 0, 1] = (-ch * sr + sh * sp * cr) * sy
        m[:, 0, 2] = (sh * cp) * sz
        m[:, 1, 0] = (cp * sr) * sx
        m[:, 1, 1] = (cp * cr) * sy
        m[:, 1, 2] = (-sp) * sz
        m[:, 2, 0] = (-sh * cr + ch * sp * sr) * sx
        m[:, 2, 1] = (sh * sr + ch * sp * cr) * sy
        m[:, 2, 2] = (ch * cp) * sz

        # Arvo's exact world AABB of oriented box: center = M @ c + pos, extent = |M| @ e
        local_c = 0.5 * (local_lo + local_hi)
        local_e = 0.5 * (local_hi - local_lo)
        world_c = np.einsum("nij,nj->ni", m, local_c) + pos
        world_e = np.einsum("nij,nj->ni", np.abs(m), local_e)

        self._pick_aabb_lo = (world_c - world_e).astype(np.float32, copy=False)
        self._pick_aabb_hi = (world_c + world_e).astype(np.float32, copy=False)
        self._pick_aabb_objs = objs
        self._pick_aabb_sig = sig
        self._pick_aabb_dirty = False

    def _pick_object_with_t(self, ray_o: np.ndarray, ray_d: np.ndarray) -> tuple[WorldObject | None, float]:
        with get_profiler().zone("viewport.pick_cpu", "pick"):
            if self.world is None:
                return None, 1e9
            self._ensure_pick_aabb_cache()
            lo = self._pick_aabb_lo
            hi = self._pick_aabb_hi
            objs = self._pick_aabb_objs
            if lo is None or hi is None or not objs:
                return None, 1e9

            ray_o32 = np.asarray(ray_o, dtype=np.float32)
            ray_d32 = np.asarray(ray_d, dtype=np.float32)
            safe_d = np.where(np.abs(ray_d32) > 1e-8, ray_d32, 1.0)
            inv_d = np.where(np.abs(ray_d32) > 1e-8, 1.0 / safe_d, 1e12).astype(np.float32)

            # Single vectorized slab intersection across all visible world objects
            t1 = (lo - ray_o32) * inv_d
            t2 = (hi - ray_o32) * inv_d
            tmin = np.max(np.minimum(t1, t2), axis=1)
            tmax = np.min(np.maximum(t1, t2), axis=1)
            hit_mask = tmax >= np.maximum(0.0, tmin)
            hit_indices = np.flatnonzero(hit_mask)
            if hit_indices.size == 0:
                return None, 1e9

            hit_t_vals = np.maximum(0.0, tmin[hit_indices])
            order = np.argsort(hit_t_vals, kind="mergesort")

            best_obj: WorldObject | None = None
            best_t = 1e9
            models = self.world.models
            ray_o_h = np.array([ray_o32[0], ray_o32[1], ray_o32[2], 1.0], dtype=np.float32)
            for idx_pos in order:
                aabb_t = float(hit_t_vals[idx_pos])
                if aabb_t > best_t:
                    break
                obj = objs[int(hit_indices[idx_pos])]
                model = models.get(obj.model_key)
                if model is None:
                    continue
                m4 = obj.world_matrix()
                inv_m = np.linalg.inv(m4)
                local_o = (inv_m @ ray_o_h)[:3]
                local_d = inv_m[:3, :3] @ ray_d32
                dlen = float(np.linalg.norm(local_d))
                if dlen < 1e-6:
                    continue
                local_d /= dlen
                for part in model.parts:
                    t_local = ray_mesh_hit(local_o, local_d, part.vertices[:, 0:3], part.indices)
                    if t_local is not None:
                        t_world = t_local / dlen
                        if t_world < best_t:
                            best_t = t_world
                            best_obj = obj
            return best_obj, best_t

    def _pick_object(self, ray_o: np.ndarray, ray_d: np.ndarray) -> WorldObject | None:
        obj, _t = self._pick_object_with_t(ray_o, ray_d)
        return obj

    def _update_road_hover(self, ray_o: np.ndarray, ray_d: np.ndarray) -> None:
        if self.world is None:
            return
        # 1. Check join existing node (Green)
        hit_nid = self._pick_road_node(ray_o, ray_d, max_rad=3.5)
        if hit_nid is not None:
            n = self.world.road_graph.nodes[hit_nid]
            self.road_hover_kind = "join"
            self.road_hover_pos = np.array([n.x, n.y, n.z], dtype=np.float32)
            self.road_hover_target = hit_nid
            return

        tp = self._raycast_terrain_point(ray_o, ray_d)
        if tp is None:
            self.road_hover_kind = None
            self.road_hover_pos = None
            return

        # 2. Check split existing edge (Cyan)
        g = self.world.road_graph
        best_edge_idx = -1
        best_dist = 4.2
        best_proj = tp
        for idx, e in enumerate(g.edges):
            na, nb = g.nodes.get(e.a), g.nodes.get(e.b)
            if not na or not nb:
                continue
            a = np.array([na.x, na.y, na.z], dtype=np.float32)
            b = np.array([nb.x, nb.y, nb.z], dtype=np.float32)
            d, proj = _point_segment_distance(tp, a, b)
            if d < best_dist:
                best_dist = d
                best_edge_idx = idx
                best_proj = proj

        if best_edge_idx >= 0:
            self.road_hover_kind = "split"
            self.road_hover_pos = best_proj
            self.road_hover_target = best_edge_idx
        else:
            # 3. Open terrain -> new node (White)
            self.road_hover_kind = "new"
            self.road_hover_pos = tp
            self.road_hover_target = None

    # -----------------------------------------------------------------------
    # Input Events (Fly Camera, Gizmo Drag, Ghost Place, Scatter, Road, DnD)
    # -----------------------------------------------------------------------
    def _snap_val(self, val: float, step: float, enabled: bool) -> float:
        if not enabled or step <= 1e-6:
            return val
        return round(val / step) * step

    def mousePressEvent(self, event: QMouseEvent) -> None:
        self.setFocus()
        self._last_mouse = event.position().toPoint()
        self._press_mouse = self._last_mouse

        if event.button() == Qt.MouseButton.MiddleButton:
            self._pan_dragging = True
            self._drag_camera_anchor = self._camera_anchor(event.position())
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            event.accept()
            return

        if event.button() == Qt.MouseButton.RightButton:
            self._rmb_down = True
            self._rmb_press_pos = self._last_mouse
            if event.modifiers() & Qt.KeyboardModifier.AltModifier:
                self._drag_camera_anchor = self._camera_anchor(event.position())
            self.setCursor(Qt.CursorShape.BlankCursor)
            return

        if event.button() == Qt.MouseButton.LeftButton:
            self._lmb_down = True
            ray_o, ray_d = self.screen_ray(event.position().x(), event.position().y())

            # 0. Scatter / Paint Brush stroke start
            if self.active_tool == "scatter" and self.world is not None:
                tp = self._raycast_terrain_point(ray_o, ray_d)
                if tp is not None:
                    self.scatter_hover_pos = tp
                    self._scatter_painting = True
                    self._scatter_stroke_items = self.scatter_at_point(float(tp[0]), float(tp[2]))
                    self._last_scatter_pt = tp.copy()
                    self.update()
                return

            # 1. Check 3D Gizmo grab first (Object OR Marker)
            origin, gkind = self._active_gizmo_origin()
            if origin is not None:
                ax_idx = self._pick_gizmo_axis(ray_o, ray_d)
                if ax_idx >= 0:
                    self._gizmo_drag_axis = ax_idx
                    self._gizmo_drag_target = gkind
                    self._alt_drag_clones = []
                    if gkind == "obj" and self.selected_obj is not None:
                        active_sel = [o for o in self.selected_objects if not o.deleted] or [self.selected_obj]
                        # Alt + Drag on gizmo: UE / 3ds Max / Lumion Duplicate & Drag in ONE motion!
                        if (event.modifiers() & Qt.KeyboardModifier.AltModifier) and self.world is not None:
                            clones = self._spawn_alt_drag_clones(active_sel)
                            if clones:
                                self._alt_drag_clones = clones
                                active_sel = clones
                                self.select_objects(clones, emit=True)
                                self.mark_objects_dirty(clones)
                                self.statusMessage.emit(f"Alt+Drag: duplicating {len(clones)} object(s)...")
                        self._gizmo_drag_orig_pos = self.selected_obj.position
                        self._gizmo_drag_orig_rot = self.selected_obj.rotation_hpr
                        self._gizmo_drag_orig_scale = self.selected_obj.scale
                        self._gizmo_drag_multi = []
                        for so in active_sel:
                            base_min_y = so.position[1]
                            if self.world is not None and so.model_key in self.world.models:
                                lo, _hi = so.world_aabb(self.world.models[so.model_key])
                                base_min_y = float(lo[1])
                            self._gizmo_drag_multi.append(
                                (so, so.position, so.rotation_hpr, so.scale, base_min_y)
                            )
                    elif gkind == "marker" and self.selected_marker is not None:
                        self._gizmo_drag_orig_pos = self.selected_marker.position
                        self._gizmo_drag_orig_hdg = self.selected_marker.heading_deg
                    elif gkind == "road_node" and self.selected_road_node_id is not None and self.world is not None:
                        rn = self.world.road_graph.nodes.get(self.selected_road_node_id)
                        if rn is not None:
                            self._gizmo_drag_orig_pos = (rn.x, rn.y, rn.z)
                    ax = np.eye(3, dtype=np.float32)[ax_idx]
                    if self.active_tool == "rotate":
                        hit = ray_plane_intersect(ray_o, ray_d, origin, ax)
                        if hit is not None:
                            u = _normalize(np.cross(ax, np.array([0, 1, 0], dtype=np.float32) if ax_idx != 1 else np.array([1, 0, 0], dtype=np.float32)))
                            v = _normalize(np.cross(ax, u))
                            rel = hit - origin
                            self._gizmo_drag_start_angle = math.atan2(float(np.dot(rel, v)), float(np.dot(rel, u)))
                    else:
                        t_axis, _ = closest_point_ray_to_line(ray_o, ray_d, origin, ax)
                        self._gizmo_drag_start_t = t_axis
                    return

            # 2. Road node drag if clicking selected road node
            if self.selected_road_node_id is not None and self.world is not None:
                hit_nid = self._pick_road_node(ray_o, ray_d, max_rad=3.2)
                if hit_nid == self.selected_road_node_id and self.active_tool != "road":
                    n = self.world.road_graph.nodes[hit_nid]
                    self._dragging_road_node = True
                    self._drag_road_orig_pos = (n.x, n.y, n.z)
                    return

    def _handle_gizmo_drag_move(self, event: QMouseEvent, ray_o: np.ndarray, ray_d: np.ndarray) -> bool:
        ax_idx = self._gizmo_drag_axis
        ax = np.eye(3, dtype=np.float32)[ax_idx]
        origin = np.array(self._gizmo_drag_orig_pos, dtype=np.float32)

        if self._gizmo_drag_target == "obj" and self.selected_obj is not None:
            drag_list = self._gizmo_drag_multi or [
                (
                    self.selected_obj,
                    self._gizmo_drag_orig_pos,
                    self._gizmo_drag_orig_rot,
                    self._gizmo_drag_orig_scale,
                    self._gizmo_drag_orig_pos[1],
                )
            ]
            if self.active_tool in ("move", "select"):
                t_now, _ = closest_point_ray_to_line(ray_o, ray_d, origin, ax)
                delta = self._snap_val(float(t_now - self._gizmo_drag_start_t), self.grid_snap_step, self.grid_snap_enabled)
                for so, sop, _sor, _sos, _bmy in drag_list:
                    new_p = list(sop)
                    new_p[ax_idx] = self._snap_val(new_p[ax_idx] + delta, self.grid_snap_step, self.grid_snap_enabled)
                    so.position = (new_p[0], new_p[1], new_p[2])
            elif self.active_tool == "rotate":
                hit = ray_plane_intersect(ray_o, ray_d, origin, ax)
                if hit is not None:
                    u = _normalize(np.cross(ax, np.array([0, 1, 0], dtype=np.float32) if ax_idx != 1 else np.array([1, 0, 0], dtype=np.float32)))
                    v = _normalize(np.cross(ax, u))
                    rel = hit - origin
                    ang = math.atan2(float(np.dot(rel, v)), float(np.dot(rel, u)))
                    da_deg = self._snap_val(math.degrees(ang - self._gizmo_drag_start_angle), self.angle_snap_step, self.angle_snap_enabled)
                    hpr_idx = {1: 0, 0: 1, 2: 2}[ax_idx]
                    for so, _sop, sor, _sos, _bmy in drag_list:
                        new_r = list(sor)
                        new_r[hpr_idx] = (new_r[hpr_idx] + da_deg) % 360.0
                        so.rotation_hpr = (new_r[0], new_r[1], new_r[2])
            elif self.active_tool == "scale":
                t_now, _ = closest_point_ray_to_line(ray_o, ray_d, origin, ax)
                s_ref = max(1.0, self._gizmo_scale(origin))
                factor = max(0.05, 1.0 + (t_now - self._gizmo_drag_start_t) / s_ref)
                uniform = bool(event.modifiers() & Qt.KeyboardModifier.ShiftModifier)
                if len(drag_list) > 1:
                    pivot = np.array(self._gizmo_drag_orig_pos, dtype=np.float32)
                    group_base_min_y = min(bmy for _so, _sop, _sor, _sos, bmy in drag_list)
                    sf = np.array([factor, factor, factor], dtype=np.float32) if uniform else np.array([1.0, 1.0, 1.0], dtype=np.float32)
                    if not uniform:
                        sf[ax_idx] = factor
                    new_group_min_y = 1e9
                    for so, sop, _sor, sos, _bmy in drag_list:
                        new_s = (sos[0] * float(sf[0]), sos[1] * float(sf[1]), sos[2] * float(sf[2]))
                        so.scale = new_s
                        rel = np.array(sop, dtype=np.float32) - pivot
                        sp = pivot + rel * sf
                        so.position = (float(sp[0]), float(sp[1]), float(sp[2]))
                        if self.world is not None and so.model_key in self.world.models:
                            lo, _hi = so.world_aabb(self.world.models[so.model_key])
                            new_group_min_y = min(new_group_min_y, float(lo[1]))
                    if (uniform or ax_idx == 1) and new_group_min_y < 1e8:
                        dy_anchor = group_base_min_y - new_group_min_y
                        for so, _sop, _sor, _sos, _bmy in drag_list:
                            px_, py_, pz_ = so.position
                            so.position = (px_, py_ + dy_anchor, pz_)
                else:
                    for so, sop, _sor, sos, base_min_y in drag_list:
                        new_s = [v * factor for v in sos] if uniform else list(sos)
                        if not uniform:
                            new_s[ax_idx] = sos[ax_idx] * factor
                        so.scale = (new_s[0], new_s[1], new_s[2])
                        if (uniform or ax_idx == 1) and self.world is not None and so.model_key in self.world.models:
                            so.position = sop
                            lo, _hi = so.world_aabb(self.world.models[so.model_key])
                            dy_anchor = base_min_y - float(lo[1])
                            so.position = (sop[0], sop[1] + dy_anchor, sop[2])
            if len(drag_list) > 1:
                self._sel_dirty = True
            if self.render_mode == "plume":
                self._plume_dirty = True
            if self.show_collision:
                self._col_dirty = True
            self.objectSelected.emit(self.selected_obj)
            self.update()
            return True

        if self._gizmo_drag_target == "marker" and self.selected_marker is not None:
            if self.active_tool == "rotate":
                hit = ray_plane_intersect(ray_o, ray_d, origin, np.array([0.0, 1.0, 0.0], dtype=np.float32))
                if hit is not None:
                    rel = hit - origin
                    hdg = math.degrees(math.atan2(float(rel[0]), float(rel[2]))) % 360.0
                    hdg = self._snap_val(hdg, self.angle_snap_step, self.angle_snap_enabled)
                    self.selected_marker.heading_deg = hdg
            else:
                t_now, _ = closest_point_ray_to_line(ray_o, ray_d, origin, ax)
                delta = self._snap_val(float(t_now - self._gizmo_drag_start_t), self.grid_snap_step, self.grid_snap_enabled)
                new_p = list(self._gizmo_drag_orig_pos)
                new_p[ax_idx] = self._snap_val(new_p[ax_idx] + delta, self.grid_snap_step, self.grid_snap_enabled)
                self.selected_marker.position = (new_p[0], new_p[1], new_p[2])
            self.markerSelected.emit(self.selected_marker)
            self._markers_dirty = True
            self.update()
            return True

        if self._gizmo_drag_target == "road_node" and self.selected_road_node_id is not None and self.world is not None:
            rn = self.world.road_graph.nodes.get(self.selected_road_node_id)
            if rn is not None:
                t_now, _ = closest_point_ray_to_line(ray_o, ray_d, origin, ax)
                delta = self._snap_val(float(t_now - self._gizmo_drag_start_t), self.grid_snap_step, self.grid_snap_enabled)
                new_p = list(self._gizmo_drag_orig_pos)
                new_p[ax_idx] = self._snap_val(new_p[ax_idx] + delta, self.grid_snap_step, self.grid_snap_enabled)
                rn.x, rn.y, rn.z = float(new_p[0]), float(new_p[1]), float(new_p[2])
                self._roads_dirty = True
                self.roadNodeSelected.emit(rn)
                self.update()
            return True

        return False

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        pos = event.position().toPoint()
        dx = pos.x() - self._last_mouse.x()
        dy = pos.y() - self._last_mouse.y()
        self._last_mouse = pos

        if self._pan_dragging and self._drag_camera_anchor is not None:
            self._pan_camera(dx, dy, self._drag_camera_anchor)
            return

        if self._rmb_down:
            if self._drag_camera_anchor is not None:
                self._orbit_camera(dx * 0.22, -dy * 0.22, self._drag_camera_anchor)
                return
            self.cam_yaw = (self.cam_yaw + dx * 0.22) % 360.0
            self.cam_pitch = max(-88.0, min(88.0, self.cam_pitch - dy * 0.22))
            self.update()
            return

        ray_o, ray_d = self.screen_ray(event.position().x(), event.position().y())

        # Active Scatter Brush dragging
        if self._scatter_painting and self.active_tool == "scatter" and self.world is not None:
            tp = self._raycast_terrain_point(ray_o, ray_d)
            if tp is not None:
                self.scatter_hover_pos = tp
                step_dist = max(3.0, float(self.scatter_hud.spn_radius.value()) * 0.65)
                if self._last_scatter_pt is None or float(np.linalg.norm(tp - self._last_scatter_pt)) >= step_dist:
                    self._scatter_stroke_items.extend(self.scatter_at_point(float(tp[0]), float(tp[2])))
                    self._last_scatter_pt = tp.copy()
                self.update()
            return

        # Active gizmo dragging (Object OR Marker, with Grid & Angle Snap!)
        if self._gizmo_drag_axis >= 0:
            with get_profiler().zone("viewport.gizmo_drag", "gizmo"):
                if self._handle_gizmo_drag_move(event, ray_o, ray_d):
                    return

        # Dragging a selected road node (Shift+Drag = pure 3D vertical Y elevation drag; else preserves custom height offset!)
        if self._dragging_road_node and self.selected_road_node_id is not None and self.world is not None:
            n = self.world.road_graph.nodes.get(self.selected_road_node_id)
            if n is not None:
                if event.modifiers() & Qt.KeyboardModifier.ShiftModifier:
                    origin_r = np.array(self._drag_road_orig_pos, dtype=np.float32)
                    t_y, _ = closest_point_ray_to_line(ray_o, ray_d, origin_r, np.array([0.0, 1.0, 0.0], dtype=np.float32))
                    n.y = self._snap_val(float(self._drag_road_orig_pos[1] + t_y), self.grid_snap_step, self.grid_snap_enabled)
                    self._roads_dirty = True
                    self.roadNodeSelected.emit(n)
                    self.update()
                else:
                    tp = self._raycast_terrain_point(ray_o, ray_d)
                    if tp is not None:
                        orig_gnd = self.world.collision.ground_height(self._drag_road_orig_pos[0], self._drag_road_orig_pos[2])
                        elev_offset = max(0.35, float(self._drag_road_orig_pos[1]) - orig_gnd)
                        n.x, n.y, n.z = float(tp[0]), float(tp[1]) + elev_offset, float(tp[2])
                        self._roads_dirty = True
                        self.roadNodeSelected.emit(n)
                        self.update()
            return

        # Hover updates
        if self.active_tool == "place" and self.world is not None:
            tp = self._raycast_terrain_point(ray_o, ray_d)
            if tp is not None:
                gx = self._snap_val(float(tp[0]), self.grid_snap_step, self.grid_snap_enabled)
                gz = self._snap_val(float(tp[2]), self.grid_snap_step, self.grid_snap_enabled)
                gy, nrm = self.world.collision.ground_sample(gx, gz)
                self.ghost_pos = np.array([gx, gy, gz], dtype=np.float32)
                self.ghost_normal = nrm
            else:
                self.ghost_pos = None
            self.update()
        elif self.active_tool == "scatter" and self.world is not None:
            self.scatter_hover_pos = self._raycast_terrain_point(ray_o, ray_d)
            self.update()
        elif self.active_tool == "road":
            self._update_road_hover(ray_o, ray_d)
            self.update()
        else:
            origin, _ = self._active_gizmo_origin()
            if origin is not None:
                prev = self._gizmo_hover_axis
                self._gizmo_hover_axis = self._pick_gizmo_axis(ray_o, ray_d)
                if prev != self._gizmo_hover_axis:
                    self.update()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.MiddleButton:
            self._pan_dragging = False
            self._drag_camera_anchor = None
            self.unsetCursor()
            event.accept()
            return
        if event.button() == Qt.MouseButton.RightButton:
            self._rmb_down = False
            orbited = self._drag_camera_anchor is not None
            self._drag_camera_anchor = None
            self.unsetCursor()
            moved = (event.position().toPoint() - self._rmb_press_pos).manhattanLength()
            if not orbited and moved <= 4 and self.selected_obj is not None and not self.selected_obj.deleted:
                self.contextMenuRequested.emit(event.globalPosition().toPoint())
            return

        if event.button() == Qt.MouseButton.LeftButton:
            self._lmb_down = False

            # Finish Scatter Brush stroke -> single undoable batch!
            if self._scatter_painting:
                self._scatter_painting = False
                items = list(self._scatter_stroke_items)
                self._scatter_stroke_items.clear()
                self._last_scatter_pt = None
                if items:
                    self.scatterStrokeCompleted.emit(items)
                return

            # Finish gizmo drag -> push QUndoCommand via signal
            if self._gizmo_drag_axis >= 0:
                target = self._gizmo_drag_target
                self._gizmo_drag_axis = -1
                self._gizmo_drag_target = ""
                drag_list = list(self._gizmo_drag_multi)
                self._gizmo_drag_multi = []
                alt_clones = list(self._alt_drag_clones)
                self._alt_drag_clones = []
                if target == "obj" and alt_clones and self.world is not None:
                    # Remove temporary clones so QUndoStack.push() adds them cleanly in one undoable step
                    for c in alt_clones:
                        if c in self.world.objects:
                            self.world.objects.remove(c)
                    self.altDuplicateCompleted.emit(
                        alt_clones, f"Alt+Drag duplicate ({len(alt_clones)} item{'s' if len(alt_clones) != 1 else ''})"
                    )
                    return
                if target == "obj" and self.selected_obj is not None:
                    label = {"move": "Move object", "rotate": "Rotate object", "scale": "Scale object"}.get(
                        self.active_tool, "Transform object"
                    )
                    if len(drag_list) > 1:
                        changes = []
                        for so, op, orot, oscl, _bmy in drag_list:
                            np_, nrot, nscl = so.position, so.rotation_hpr, so.scale
                            if op != np_ or orot != nrot or oscl != nscl:
                                so.position, so.rotation_hpr, so.scale = op, orot, oscl
                                changes.append((so, op, orot, oscl, np_, nrot, nscl))
                        if changes:
                            self.multiObjectTransformed.emit(changes, f"{label} ({len(changes)} items)")
                    else:
                        obj = self.selected_obj
                        op, orot, oscl = self._gizmo_drag_orig_pos, self._gizmo_drag_orig_rot, self._gizmo_drag_orig_scale
                        np_, nrot, nscl = obj.position, obj.rotation_hpr, obj.scale
                        if op != np_ or orot != nrot or oscl != nscl:
                            obj.position, obj.rotation_hpr, obj.scale = op, orot, oscl
                            self.objectTransformed.emit(obj, op, orot, oscl, np_, nrot, nscl, label)
                elif target == "marker" and self.selected_marker is not None:
                    mk = self.selected_marker
                    op, oh = self._gizmo_drag_orig_pos, self._gizmo_drag_orig_hdg
                    np_, nh = mk.position, mk.heading_deg
                    if op != np_ or abs(oh - nh) > 1e-4:
                        mk.position, mk.heading_deg = op, oh
                        label = "Rotate marker" if self.active_tool == "rotate" else "Move marker"
                        self.markerTransformed.emit(mk, op, oh, np_, nh, label)
                elif target == "road_node" and self.selected_road_node_id is not None and self.world is not None:
                    rn = self.world.road_graph.nodes.get(self.selected_road_node_id)
                    if rn is not None:
                        op = self._gizmo_drag_orig_pos
                        np_ = (rn.x, rn.y, rn.z)
                        if op != np_:
                            rn.x, rn.y, rn.z = op
                            self.roadNodeMoved.emit(rn, op, np_)
                return

            # Finish road node drag
            if self._dragging_road_node and self.selected_road_node_id is not None and self.world is not None:
                self._dragging_road_node = False
                n = self.world.road_graph.nodes.get(self.selected_road_node_id)
                if n is not None:
                    new_pos = (n.x, n.y, n.z)
                    if new_pos != self._drag_road_orig_pos:
                        n.x, n.y, n.z = self._drag_road_orig_pos
                        self.roadNodeMoved.emit(n, self._drag_road_orig_pos, new_pos)
                return

            # Click (not a drag)
            moved = (event.position().toPoint() - self._press_mouse).manhattanLength()
            if moved > 5:
                return

            ray_o, ray_d = self.screen_ray(event.position().x(), event.position().y())

            # Ghost Place Mode click -> stamp snapped to SCOL terrain + surface normal!
            if self.active_tool == "place" and self.world is not None and self.ghost_model_key:
                tp = self._raycast_terrain_point(ray_o, ray_d)
                if tp is not None:
                    gx = self._snap_val(float(tp[0]), self.grid_snap_step, self.grid_snap_enabled)
                    gz = self._snap_val(float(tp[2]), self.grid_snap_step, self.grid_snap_enabled)
                    gy, nrm = self.world.collision.ground_sample(gx, gz)
                    rot = (
                        normal_to_pitch_roll(nrm, self.ghost_yaw)
                        if self.place_hud.chk_align.isChecked()
                        else (self.ghost_yaw, 0.0, 0.0)
                    )
                    self.modelStamped.emit(self.ghost_model_key, (gx, gy, gz), rot, (1.0, 1.0, 1.0))
                return

            # Road Tool click -> Join / Split / New node
            if self.active_tool == "road" and self.world is not None:
                self._update_road_hover(ray_o, ray_d)
                if self.road_hover_kind == "join" and self.road_hover_target is not None:
                    target_id = int(self.road_hover_target)
                    if self.road_chain_last_node is not None and self.road_chain_last_node != target_id:
                        self.roadActionRequested.emit(
                            "join",
                            {
                                "a": self.road_chain_last_node,
                                "b": target_id,
                                "lanes": self.road_hud.lanes,
                                "width": self.road_hud.width_m,
                            },
                        )
                    self.road_chain_last_node = target_id
                    self.select_road_node(target_id)
                elif self.road_hover_kind == "split" and self.road_hover_pos is not None:
                    self.roadActionRequested.emit(
                        "split",
                        {
                            "edge_index": int(self.road_hover_target),
                            "pos": [float(v) for v in self.road_hover_pos],
                        },
                    )
                elif self.road_hover_kind == "new" and self.road_hover_pos is not None:
                    self.roadActionRequested.emit(
                        "add",
                        {
                            "pos": [float(self.road_hover_pos[0]), float(self.road_hover_pos[1]) + 0.35, float(self.road_hover_pos[2])],
                            "connect_from": self.road_chain_last_node,
                            "lanes": self.road_hud.lanes,
                            "width": self.road_hud.width_m,
                        },
                    )
                return

            # Depth-aware picking: compute object hit first so occluded road nodes / markers behind
            # foreground ramps or buildings never steal clicks!
            cpu_obj, obj_t = self._pick_object_with_t(ray_o, ray_d)
            hit_obj = self._pick_object_at_screen(event.position().x(), event.position().y())
            if hit_obj is None:
                hit_obj = cpu_obj

            if self.show_roads:
                hit_nid, road_t = self._pick_road_node_with_t(ray_o, ray_d, max_rad=2.4)
                if hit_nid is not None and (hit_obj is None or road_t <= obj_t + 3.0):
                    self.select_road_node(hit_nid)
                    return

            hit_marker, marker_t = self._pick_marker_with_t(ray_o, ray_d)
            if hit_marker is not None and (hit_obj is None or marker_t <= obj_t + 3.0):
                self.select_marker(hit_marker)
                return

            if self.show_collision:
                hit_b = self._pick_sobl_barrier(ray_o, ray_d)
                if hit_b >= 0:
                    self.select_barrier(hit_b)
                    return

            if hit_obj is not None:
                gid = self.world.get_active_group_id(hit_obj) if self.world is not None else ""
                if self._drilled_group_id and gid != self._drilled_group_id:
                    self._drilled_group_id = ""
                if event.modifiers() & Qt.KeyboardModifier.ShiftModifier:
                    self.toggle_object_selection(hit_obj)
                elif gid and gid != self._drilled_group_id and self.world is not None:
                    members = self.world.get_group_members(hit_obj, visible_only=True)
                    # Put clicked object last so it is primary selected_obj while all members move/scale/delete together
                    ordered = [m for m in members if m is not hit_obj] + [hit_obj]
                    self.select_objects(ordered)
                    if len(ordered) > 1:
                        lbl = hit_obj.compound_label or f"Group {gid}"
                        self.statusMessage.emit(
                            f"Selected {lbl} ({len(ordered)} parts) — double-click to drill into sub-part, Ctrl+Shift+G to ungroup"
                        )
                else:
                    self.select_object(hit_obj)
            elif not (event.modifiers() & Qt.KeyboardModifier.ShiftModifier):
                self._drilled_group_id = ""
                self.select_object(None)
                self.select_road_node(None)
                self.select_marker(None)
                self.select_barrier(-1, emit=False)

    def _spawn_alt_drag_clones(self, src_objs: list[WorldObject]) -> list[WorldObject]:
        """Clone `src_objs` in-place for Alt+Drag Duplicate & Drag in one motion."""
        if self.world is None or not src_objs:
            return []
        clones: list[WorldObject] = []
        base_n = len(self.world.objects) + 1
        new_gid = ""
        if len(src_objs) > 1:
            new_gid = f"grp_{self.world._next_user_group_seq:03d}"
            self.world._next_user_group_seq += 1
        for k, src in enumerate(src_objs):
            wm = self.world.models.get(src.model_key)
            if wm is None:
                continue
            oid = f"ed_alt_{base_n + k:04d}_{int(time.time() * 1000) % 100000:05d}"
            c = WorldObject(
                obj_id=oid,
                name=f"{src.name} (Copy)",
                model_key=src.model_key,
                source="Placed by the editor",
                category=src.category if src.category in ("dyno", "prop-catalog", "car-wreck") else "dyno",
                subtype=wm.subtype,
                section_mlist="editor_layer",
                model_store=wm.model_store,
                hash_hex=wm.hash_hex,
                tri_count=wm.tri_count,
                textures=list(wm.textures),
                position=tuple(src.position),
                rotation_hpr=tuple(src.rotation_hpr),
                scale=tuple(src.scale),
                written=False,
                user_group_id=new_gid,
                physics_mode=src.physics_mode,
            )
            self.world.objects.append(c)
            clones.append(c)
        return clones

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        """Double-click a grouped object to drill into its individual sub-part (UE / SketchUp / Blender convention)."""
        if event.button() == Qt.MouseButton.LeftButton and self.world is not None:
            hit_obj = self._pick_object_at_screen(event.position().x(), event.position().y())
            if hit_obj is not None:
                gid = self.world.get_active_group_id(hit_obj)
                if gid:
                    self._drilled_group_id = gid
                    self.select_object(hit_obj)
                    self.statusMessage.emit(
                        f"Drilled into sub-part '{hit_obj.name}' ({gid}) — press Esc or click empty space to exit group"
                    )
                    event.accept()
                    return
        super().mouseDoubleClickEvent(event)

    def cycle_transform_tool(self) -> str:
        """Cycle Move -> Rotate -> Scale -> Move (Tab shortcut)."""
        order = ("move", "rotate", "scale")
        cur = self.active_tool if self.active_tool in order else "move"
        nxt = order[(order.index(cur) + 1) % len(order)]
        self.set_tool(nxt)
        self.toolCycled.emit(nxt)
        self.statusMessage.emit(f"Transform tool: {nxt.capitalize()} (Tab to cycle)")
        return nxt

    def _camera_anchor(self, position: QPointF) -> np.ndarray:
        origin, direction = self.screen_ray(position.x(), position.y())
        target = self._raycast_terrain_point(origin, direction)
        if target is not None and 2.0 <= float(np.linalg.norm(target - origin)) <= 1500.0:
            return target
        return origin + direction * 45.0

    def _pan_camera(self, dx: float, dy: float, anchor: np.ndarray) -> None:
        forward, right, up = self.camera_vectors()
        depth = max(2.0, float(np.dot(anchor - self.cam_pos, forward)))
        metres_per_pixel = 2.0 * depth * math.tan(math.radians(self.cam_fov) / 2) / max(1, self.height())
        self.cam_pos += (up * dy - right * dx) * metres_per_pixel
        self.update()

    def _orbit_camera(self, yaw: float, pitch: float, anchor: np.ndarray) -> None:
        offset = anchor - self.cam_pos
        local = [float(np.dot(offset, axis)) for axis in self.camera_vectors()]
        self.cam_yaw = (self.cam_yaw + yaw) % 360.0
        self.cam_pitch = max(-88.0, min(88.0, self.cam_pitch + pitch))
        self.cam_pos = anchor - sum(component * axis for component, axis in zip(local, self.camera_vectors()))
        self.update()

    def _dolly_camera(self, magnification: float, anchor: np.ndarray) -> None:
        offset = self.cam_pos - anchor
        distance = float(np.linalg.norm(offset))
        if distance == 0.0:
            return
        target_distance = distance / magnification
        nearest = min(distance, 0.75)
        farthest = max(distance, 3200.0)
        target_distance = min(farthest, max(nearest, target_distance))
        self.cam_pos = anchor + offset * (target_distance / distance)
        self.update()

    def event(self, event: QEvent) -> bool:
        if event.type() == QEvent.Type.NativeGesture and self._handle_camera_gesture(event):
            return True
        if event.type() == QEvent.Type.KeyPress:
            if event.key() == Qt.Key.Key_Tab and event.modifiers() == Qt.KeyboardModifier.NoModifier:
                self.cycle_transform_tool()
                event.accept()
                return True
        return super().event(event)

    def _handle_camera_gesture(self, event: QNativeGestureEvent) -> bool:
        kind = event.gestureType()
        if kind in (Qt.NativeGestureType.BeginNativeGesture, Qt.NativeGestureType.EndNativeGesture):
            self._native_camera_anchor = None
        elif kind in (Qt.NativeGestureType.ZoomNativeGesture, Qt.NativeGestureType.RotateNativeGesture):
            if not (self._lmb_down or self._rmb_down or self._pan_dragging):
                if self._native_camera_anchor is None:
                    self._native_camera_anchor = self._camera_anchor(event.position())
                if kind == Qt.NativeGestureType.ZoomNativeGesture:
                    self._dolly_camera(max(0.5, min(2.0, 1.0 + event.value())), self._native_camera_anchor)
                else:
                    self._orbit_camera(event.value(), 0.0, self._native_camera_anchor)
        else:
            return False
        event.accept()
        return True

    def wheelEvent(self, event: QWheelEvent) -> None:
        phase = event.phase()
        if phase in (Qt.ScrollPhase.ScrollBegin, Qt.ScrollPhase.ScrollEnd):
            self._scroll_camera_anchor = None
        event.accept()
        if phase == Qt.ScrollPhase.ScrollMomentum or self._lmb_down or self._pan_dragging:
            return
        pixel, angle = event.pixelDelta(), event.angleDelta()
        dx, dy = (float(pixel.x()), float(pixel.y())) if not pixel.isNull() else (
            angle.x() / 3.0, angle.y() / 3.0)
        if dx == 0.0 and dy == 0.0:
            return
        if self._rmb_down:
            self.fly_speed = min(400.0, max(2.0, self.fly_speed * 1.18 ** (dy / 40)))
            self._emit_stats()
            return
        if self._scroll_camera_anchor is None:
            self._scroll_camera_anchor = self._camera_anchor(event.position())
        anchor = self._scroll_camera_anchor
        if phase == Qt.ScrollPhase.NoScrollPhase:
            self._scroll_camera_anchor = None
        modifiers = event.modifiers()
        if modifiers & Qt.KeyboardModifier.ControlModifier:
            self._dolly_camera(1.15 ** (dy / 40), anchor)
        elif modifiers & Qt.KeyboardModifier.ShiftModifier:
            self._orbit_camera(-dx * 0.22, -dy * 0.22, anchor)
        else:
            self._pan_camera(dx, dy, anchor)

    def keyPressEvent(self, event) -> None:
        key = event.key()
        mods = event.modifiers()
        # Never hijack OS/menu modifier chords (Ctrl+S, Cmd+Shift+S, Ctrl+Shift+S, Alt+...) for camera fly!
        if mods & (
            Qt.KeyboardModifier.ControlModifier
            | Qt.KeyboardModifier.MetaModifier
            | Qt.KeyboardModifier.AltModifier
        ):
            super().keyPressEvent(event)
            return
        if key == Qt.Key.Key_Tab:
            self.cycle_transform_tool()
            event.accept()
            return
        self._keys_down.add(key)
        move_keys = {Qt.Key.Key_W, Qt.Key.Key_S, Qt.Key.Key_A, Qt.Key.Key_D, Qt.Key.Key_Q, Qt.Key.Key_E}
        if key in move_keys and not self._timer.isActive():
            self._last_tick = time.perf_counter()
            self._timer.start()
        if key == Qt.Key.Key_Return or key == Qt.Key.Key_Enter:
            if self.active_tool == "road":
                self.end_current_road_chain()
                return
        if key == Qt.Key.Key_R and self.active_tool == "place":
            step = -self.angle_snap_step if (mods & Qt.KeyboardModifier.ShiftModifier) else self.angle_snap_step
            self.ghost_yaw = (self.ghost_yaw + step) % 360.0
            self.statusMessage.emit(f"Ghost rotation: {self.ghost_yaw:.0f}°")
            self.update()
            return
        if key == Qt.Key.Key_End:
            self.dropToGroundRequested.emit()
            return
        if key == Qt.Key.Key_Escape:
            if self.active_tool in ("road", "place", "scatter"):
                self._close_road_tool()
                return
            self.select_object(None)
            self.select_road_node(None)
            self.select_marker(None)
            self.select_barrier(-1, emit=False)
            return
        if key == Qt.Key.Key_F:
            self.frame_selection()
            return
        super().keyPressEvent(event)

    def keyReleaseEvent(self, event) -> None:
        self._keys_down.discard(event.key())
        move_keys = {Qt.Key.Key_W, Qt.Key.Key_S, Qt.Key.Key_A, Qt.Key.Key_D, Qt.Key.Key_Q, Qt.Key.Key_E}
        if not (self._keys_down & move_keys) and self._timer.isActive():
            self._timer.stop()
        super().keyReleaseEvent(event)

    def focusOutEvent(self, event) -> None:
        self._keys_down.clear()
        self._native_camera_anchor = None
        self._scroll_camera_anchor = None
        self._drag_camera_anchor = None
        if self._pan_dragging or self._rmb_down:
            self._pan_dragging = False
            self._rmb_down = False
            self.unsetCursor()
        if self._timer.isActive():
            self._timer.stop()
        super().focusOutEvent(event)

    def hideEvent(self, event) -> None:
        if self._plume_viewport:
            self._plume_viewport.close_surface()
            self._plume_dirty = True
            self._plume_retry = False
        self._keys_down.clear()
        if self._timer.isActive():
            self._timer.stop()
        super().hideEvent(event)

    # Drag-and-drop model placement from Explorer
    def dragEnterEvent(self, event) -> None:
        if event.mimeData().hasFormat(MIME_MODEL_KEY):
            event.acceptProposedAction()
        else:
            event.ignore()

    def dragMoveEvent(self, event) -> None:
        if event.mimeData().hasFormat(MIME_MODEL_KEY):
            event.acceptProposedAction()

    def dropEvent(self, event) -> None:
        if not event.mimeData().hasFormat(MIME_MODEL_KEY):
            return
        model_key = bytes(event.mimeData().data(MIME_MODEL_KEY)).decode("utf-8")
        pt = event.position()
        ray_o, ray_d = self.screen_ray(pt.x(), pt.y())
        hit = self._raycast_terrain_point(ray_o, ray_d)
        if hit is None:
            fwd, _, _ = self.camera_vectors()
            hit = self.cam_pos + fwd * 30.0
        self.modelDropped.emit(model_key, float(hit[0]), float(hit[1]), float(hit[2]))
        event.acceptProposedAction()

    # -----------------------------------------------------------------------
    # Frame Tick (Smooth WASD / QE Fly Camera + Live Profiler Stats)
    # -----------------------------------------------------------------------
    def _on_tick(self) -> None:
        now = time.perf_counter()
        dt = min(0.08, now - self._last_tick)
        self._last_tick = now

        move_keys = {Qt.Key.Key_W, Qt.Key.Key_S, Qt.Key.Key_A, Qt.Key.Key_D, Qt.Key.Key_Q, Qt.Key.Key_E}
        if not (self._keys_down & move_keys):
            self._timer.stop()
            return
        fwd, right, _up = self.camera_vectors()
        vel = np.zeros(3, dtype=np.float32)
        if Qt.Key.Key_W in self._keys_down:
            vel += fwd
        if Qt.Key.Key_S in self._keys_down:
            vel -= fwd
        if Qt.Key.Key_D in self._keys_down:
            vel += right
        if Qt.Key.Key_A in self._keys_down:
            vel -= right
        if Qt.Key.Key_E in self._keys_down:
            vel[1] += 1.0
        if Qt.Key.Key_Q in self._keys_down:
            vel[1] -= 1.0
        n = float(np.linalg.norm(vel))
        if n > 1e-5:
            boost = 2.8 if Qt.Key.Key_Shift in self._keys_down else 1.0
            self.cam_pos += (vel / n) * (self.fly_speed * boost * dt)
            self.update()

    def _emit_stats(self, force: bool = False) -> None:
        now = time.perf_counter()
        if not force and (now - self._last_stats_emit) < 0.12:
            return
        self._last_stats_emit = now
        mlists = 0
        objs = 0
        inst = 0
        if self.world is not None:
            mlists = sum(1 for m in self.world.mlist_info if m is not None)
            objs = len(self.world.models)
            inst = sum(1 for o in self.world.objects if not o.deleted)
        gt = self.gpu_timer.last_timings
        self.statsUpdated.emit(
            {
                "cam": (float(self.cam_pos[0]), float(self.cam_pos[1]), float(self.cam_pos[2])),
                "speed": self.fly_speed,
                "mlists": mlists,
                "objs": objs,
                "inst": inst,
                "tris": self._last_tris_drawn,
                "draws": self._last_draw_calls,
                "ms": self._last_frame_ms,
                "gpu_ms": gt.total_gpu_ms,
                "terrain_ms": gt.terrain_ms,
                "props_ms": gt.props_ms,
                "water_ms": gt.water_ms,
                "overlays_ms": gt.overlays_ms,
                "mode": self.render_mode,
            }
        )
