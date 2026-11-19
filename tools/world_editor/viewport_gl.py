"""Hardware-Accelerated OpenGL 3.3 Core 3D Viewport (QOpenGLWidget) for EOD World Editor.

Features:
  - OpenGL 3.3 Core shaders: sky gradient dome, lit/textured world with fog & alpha cutout,
    bright translucent green object selection highlight, unlit line/ribbon/gizmo shader.
  - Texture-batched static VBOs for >60 FPS on 2,000+ retail instances.
  - Native EOD Left-Handed camera (+X right, +Y up, +Z forward): right-drag look,
    WASD move, Q/E down/up, wheel adjusts fly speed, Shift boost.
  - Interactive 3D Move (R/G/B arrows), Rotate (R/G/B rings), and Scale (R/G/B cubes) gizmos.
  - 3D raycast picking of objects, road nodes, road segments, markers, and terrain.
  - Road Tool (ILIN) 3D ribbon, yellow/magenta diamond handles, Green/Cyan/White hover feedback.
  - Floating Qt overlays: top-left Object/Edit/Shield pill bar + top-center Road Tool HUD card.
"""

from __future__ import annotations

import ctypes
import math
import random
import time
from typing import Any

import numpy as np
import OpenGL
OpenGL.ERROR_CHECKING = False
OpenGL.ERROR_LOGGING = False
from OpenGL import GL
from PySide6.QtCore import QPoint, QPointF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QFont, QMouseEvent, QPainter, QWheelEvent
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
        return tmin if tmin >= 0.0 else tmax
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

        self.btn_shield = QPushButton()
        self.btn_shield.setIcon(svg_icon("shield"))
        self.btn_shield.setCheckable(True)
        self.btn_shield.setToolTip("Toggle Collision / Shield Overlay")
        self.btn_shield.setFixedWidth(30)
        self.btn_shield.setCursor(Qt.CursorShape.PointingHandCursor)

        pill_css = (
            "QPushButton { background: transparent; border: 1px solid transparent; "
            "border-radius: 5px; padding: 4px 10px; color: #c7ccd8; font-weight: 600; font-size: 8.5pt; }"
            "QPushButton:hover { background: rgba(255,255,255,0.08); color: #ffffff; }"
            f"QPushButton:checked {{ background: {ACCENT_BLUE}; border-color: {ACCENT_BLUE_HOVER}; color: #ffffff; }}"
        )
        for b in (self.btn_object, self.btn_edit, self.btn_shield):
            b.setStyleSheet(pill_css)

        layout.addWidget(self.btn_object)
        layout.addWidget(self.btn_edit)
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
            "display-object": False,
            "prop-catalog": True,
            "car-wreck": True,
        }

        # Selection
        self.selected_obj: WorldObject | None = None
        self.selected_road_node_id: int | None = None
        self.selected_marker: WorldMarker | None = None
        self.selected_barrier_idx: int = -1

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
        self._gizmo_drag_orig_hdg: float = 0.0
        self._dragging_road_node: bool = False
        self._drag_road_orig_pos: tuple[float, float, float] = (0.0, 0.0, 0.0)

        # GL resources & cached uniform locations
        self._gl_ready = False
        self._prog_sky = 0
        self._prog_world = 0
        self._prog_flat = 0
        self._u_sky: dict[str, int] = {}
        self._u_world: dict[str, int] = {}
        self._u_flat: dict[str, int] = {}
        self._vao_empty = 0
        self._gl_textures: dict[str, tuple[int, bool]] = {}  # name -> (tex_id, cutout)
        self._static_batches: list[dict[str, Any]] = []
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

        # Profiler metrics
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

        # Demand-driven flight timer: ONLY runs while WASD/QE keys are held (0% idle CPU/GPU!)
        self._timer = QTimer(self)
        self._timer.setInterval(16)
        self._timer.timeout.connect(self._on_tick)

    def set_render_mode(self, mode: str) -> None:
        """Switch viewport shading mode: 'ps2' (fast power-sipping) or 'plume' (high quality)."""
        self.render_mode = "plume" if mode == "plume" else "ps2"
        self.update()

    # -----------------------------------------------------------------------
    # Public API
    # -----------------------------------------------------------------------
    def set_world(self, world: TrackWorld) -> None:
        self.world = world
        self.selected_obj = None
        self.selected_road_node_id = None
        self.selected_marker = None
        self.road_chain_last_node = None

        cx, cy, cz = world.world_center
        r = min(max(world.world_radius * 0.65, 65.0), 260.0)
        self.cam_pos = np.array([cx - r * 0.55, cy + r * 0.42, cz - r * 0.65], dtype=np.float32)
        self.look_at(np.array([cx, cy, cz], dtype=np.float32))

        self._static_dirty = True
        self._sel_dirty = True
        self._col_dirty = True
        self._roads_dirty = True
        self._markers_dirty = True
        if self._gl_ready:
            self.makeCurrent()
            self._upload_textures()
            self._rebuild_collision_vbo()
            self._rebuild_static_batches()
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

    def select_object(self, obj: WorldObject | None, emit: bool = True) -> None:
        prev_road = self.selected_road_node_id
        prev_marker = self.selected_marker
        self.selected_obj = obj
        if obj is not None:
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
            self.objectSelected.emit(obj)

    def select_road_node(self, node_id: int | None, emit: bool = True) -> None:
        self.selected_road_node_id = node_id
        if node_id is not None:
            self.selected_obj = None
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
            self.selected_road_node_id = None
            self.selected_marker = None
            self._sel_dirty = True
            self._roads_dirty = True
            self._markers_dirty = True
        self.update()
        if emit:
            self.barrierSelected.emit(self.selected_barrier_idx)

    def mark_scene_dirty(self) -> None:
        self._static_dirty = True
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
        GL.glTexImage2D(
            GL.GL_TEXTURE_2D, 0, GL.GL_RGBA8, ta.width, ta.height, 0,
            GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, ta.rgba,
        )
        GL.glGenerateMipmap(GL.GL_TEXTURE_2D)
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
        vao = int(GL.glGenVertexArrays(1))
        vbo = int(GL.glGenBuffers(1))
        ebo = int(GL.glGenBuffers(1))
        GL.glBindVertexArray(vao)
        GL.glBindBuffer(GL.GL_ARRAY_BUFFER, vbo)
        GL.glBufferData(GL.GL_ARRAY_BUFFER, verts.nbytes, verts, GL.GL_STATIC_DRAW)
        GL.glBindBuffer(GL.GL_ELEMENT_ARRAY_BUFFER, ebo)
        GL.glBufferData(GL.GL_ELEMENT_ARRAY_BUFFER, indices.nbytes, indices, GL.GL_STATIC_DRAW)
        stride = 8 * 4
        GL.glEnableVertexAttribArray(0)
        GL.glVertexAttribPointer(0, 3, GL.GL_FLOAT, GL.GL_FALSE, stride, ctypes.c_void_p(0))
        GL.glEnableVertexAttribArray(1)
        GL.glVertexAttribPointer(1, 3, GL.GL_FLOAT, GL.GL_FALSE, stride, ctypes.c_void_p(12))
        GL.glEnableVertexAttribArray(2)
        GL.glVertexAttribPointer(2, 2, GL.GL_FLOAT, GL.GL_FALSE, stride, ctypes.c_void_p(24))
        GL.glBindVertexArray(0)
        return {"vao": vao, "vbo": vbo, "ebo": ebo, "count": len(indices), "tris": len(indices) // 3, "tex": tex}

    def _rebuild_static_batches(self) -> None:
        self._static_dirty = False
        self._delete_batches(self._static_batches)
        if self.world is None:
            return

        grouped_v: dict[str, list[np.ndarray]] = {}
        grouped_i: dict[str, list[np.ndarray]] = {}
        grouped_base: dict[str, int] = {}

        for obj in self.world.objects:
            if obj.deleted or not obj.visible:
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
            for part in model.parts:
                v = part.vertices.copy()
                if is_ident_rot:
                    v[:, 0:3] += t3
                else:
                    v[:, 0:3] = v[:, 0:3] @ rot3.T + t3
                    v[:, 3:6] = v[:, 3:6] @ rot3.T
                tex = part.texture
                base = grouped_base.get(tex, 0)
                grouped_v.setdefault(tex, []).append(v)
                grouped_i.setdefault(tex, []).append(part.indices + base)
                grouped_base[tex] = base + len(v)

        for tex, vlist in grouped_v.items():
            verts = np.ascontiguousarray(np.concatenate(vlist, axis=0), dtype=np.float32)
            inds = np.ascontiguousarray(np.concatenate(grouped_i[tex], axis=0), dtype=np.uint32)
            self._static_batches.append(self._upload_mesh_batch(verts, inds, tex))

    def _rebuild_selection_batches(self) -> None:
        self._sel_dirty = False
        self._delete_batches(self._sel_batches)
        if self.world is None or self.selected_obj is None or self.selected_obj.deleted:
            return
        model = self.world.models.get(self.selected_obj.model_key)
        if not model:
            return
        for part in model.parts:
            verts = np.ascontiguousarray(part.vertices, dtype=np.float32)
            inds = np.ascontiguousarray(part.indices, dtype=np.uint32)
            self._sel_batches.append(self._upload_mesh_batch(verts, inds, part.texture))

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
            lxz = math.hypot(float(seg[0]), float(seg[2]))
            if lxz < 1e-4:
                continue
            perp = np.array([-seg[2] / lxz, 0.0, seg[0] / lxz], dtype=np.float32)
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

    # -----------------------------------------------------------------------
    # Paint Frame
    # -----------------------------------------------------------------------
    def paintGL(self) -> None:
        t0 = time.perf_counter()
        if self._pending_tex_updates:
            self._flush_pending_textures()
        if self._static_dirty:
            self._rebuild_static_batches()
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
        for b in self._static_batches:
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

        # 4. Selected object with bright translucent green highlight (polygon-offset so static VBOs never rebuild on click!)
        if self.selected_obj is not None and not self.selected_obj.deleted and self._sel_batches:
            m4 = self.selected_obj.world_matrix()
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

        # 5. Collision overlay (SCOL terrain + height-coded SOBL barriers + custom boxes)
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

        # 8. Interactive 3D Transform Gizmo (for selected Object OR selected Marker!)
        gizmo_origin = None
        if self.active_tool in ("move", "rotate", "scale", "select"):
            if self.selected_obj is not None and not self.selected_obj.deleted:
                gizmo_origin = np.array(self.selected_obj.position, dtype=np.float32)
            elif self.selected_marker is not None:
                gizmo_origin = np.array(self.selected_marker.position, dtype=np.float32)
        if gizmo_origin is not None:
            glines, gtris = self._build_gizmo_geometry(gizmo_origin)
            GL.glClear(GL.GL_DEPTH_BUFFER_BIT)
            draws += self._draw_flat_verts(glines, GL.GL_LINES, view_proj)
            draws += self._draw_flat_verts(gtris, GL.GL_TRIANGLES, view_proj)

        self._last_draw_calls = draws
        self._last_tris_drawn = tris_drawn
        self._last_frame_ms = (time.perf_counter() - t0) * 1000.0
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
        """Pick the closest SOBL collision box when Collision Overlay is visible."""
        if self.world is None or not self.show_collision:
            return -1
        inv_d = np.where(np.abs(ray_d) > 1e-8, 1.0 / ray_d, 1e12).astype(np.float32)
        best_idx = -1
        best_t = 1e9
        for rec in self.world.collision.sobl_records:
            if rec.get("kind") != "box":
                continue
            corners = np.array(wc.box_corners(rec), dtype=np.float32)
            lo = corners.min(axis=0)
            hi = corners.max(axis=0)
            t = ray_aabb_hit(ray_o, inv_d, lo, hi)
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

    def _pick_road_node(self, ray_o: np.ndarray, ray_d: np.ndarray, max_rad: float = 3.2) -> int | None:
        if self.world is None or not self.show_roads:
            return None
        best_id = None
        best_score = max_rad
        for nid, n in self.world.road_graph.nodes.items():
            p = np.array([n.x, n.y + 0.65, n.z], dtype=np.float32)
            v = p - ray_o
            t = float(np.dot(v, ray_d))
            if t < 0.5:
                continue
            perp = float(np.linalg.norm(v - ray_d * t))
            tol = max(max_rad, t * 0.025)
            if perp < tol and perp < best_score + t * 0.025:
                best_score = perp
                best_id = nid
        return best_id

    def _pick_marker(self, ray_o: np.ndarray, ray_d: np.ndarray) -> WorldMarker | None:
        if self.world is None or not self.show_markers:
            return None
        best = None
        best_t = 1e9
        for m in self.world.markers:
            p = np.array(m.position, dtype=np.float32) + np.array([0.0, 1.6, 0.0], dtype=np.float32)
            v = p - ray_o
            t = float(np.dot(v, ray_d))
            if t < 0.5:
                continue
            perp = float(np.linalg.norm(v - ray_d * t))
            if perp < max(2.2, t * 0.025) and t < best_t:
                best_t = t
                best = m
        return best

    def _pick_object(self, ray_o: np.ndarray, ray_d: np.ndarray) -> WorldObject | None:
        if self.world is None:
            return None
        inv_d = np.where(np.abs(ray_d) > 1e-8, 1.0 / ray_d, 1e12).astype(np.float32)
        candidates: list[tuple[float, WorldObject]] = []
        for obj in self.world.objects:
            if obj.deleted or not obj.visible:
                continue
            if not self.layer_visibility.get(obj.category, True):
                continue
            model = self.world.models.get(obj.model_key)
            if not model:
                continue
            lo, hi = obj.world_aabb(model)
            hit_t = ray_aabb_hit(ray_o, inv_d, lo, hi)
            if hit_t is not None:
                candidates.append((hit_t, obj))
        candidates.sort(key=lambda item: item[0])

        best_obj: WorldObject | None = None
        best_t = 1e9
        for aabb_t, obj in candidates[:48]:
            if aabb_t > best_t:
                break
            model = self.world.models[obj.model_key]
            m4 = obj.world_matrix()
            inv_m = np.linalg.inv(m4)
            local_o = (inv_m @ np.array([*ray_o, 1.0], dtype=np.float32))[:3]
            local_d = (inv_m[:3, :3] @ ray_d)
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
        return best_obj

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

        if event.button() == Qt.MouseButton.RightButton:
            self._rmb_down = True
            self._rmb_press_pos = self._last_mouse
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
                    if gkind == "obj" and self.selected_obj is not None:
                        self._gizmo_drag_orig_pos = self.selected_obj.position
                        self._gizmo_drag_orig_rot = self.selected_obj.rotation_hpr
                        self._gizmo_drag_orig_scale = self.selected_obj.scale
                    elif gkind == "marker" and self.selected_marker is not None:
                        self._gizmo_drag_orig_pos = self.selected_marker.position
                        self._gizmo_drag_orig_hdg = self.selected_marker.heading_deg
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

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        pos = event.position().toPoint()
        dx = pos.x() - self._last_mouse.x()
        dy = pos.y() - self._last_mouse.y()
        self._last_mouse = pos

        if self._rmb_down:
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
            ax_idx = self._gizmo_drag_axis
            ax = np.eye(3, dtype=np.float32)[ax_idx]
            origin = np.array(self._gizmo_drag_orig_pos, dtype=np.float32)

            if self._gizmo_drag_target == "obj" and self.selected_obj is not None:
                if self.active_tool in ("move", "select"):
                    t_now, _ = closest_point_ray_to_line(ray_o, ray_d, origin, ax)
                    delta = self._snap_val(float(t_now - self._gizmo_drag_start_t), self.grid_snap_step, self.grid_snap_enabled)
                    new_p = list(self._gizmo_drag_orig_pos)
                    new_p[ax_idx] = self._snap_val(new_p[ax_idx] + delta, self.grid_snap_step, self.grid_snap_enabled)
                    self.selected_obj.position = (new_p[0], new_p[1], new_p[2])
                elif self.active_tool == "rotate":
                    hit = ray_plane_intersect(ray_o, ray_d, origin, ax)
                    if hit is not None:
                        u = _normalize(np.cross(ax, np.array([0, 1, 0], dtype=np.float32) if ax_idx != 1 else np.array([1, 0, 0], dtype=np.float32)))
                        v = _normalize(np.cross(ax, u))
                        rel = hit - origin
                        ang = math.atan2(float(np.dot(rel, v)), float(np.dot(rel, u)))
                        da_deg = self._snap_val(math.degrees(ang - self._gizmo_drag_start_angle), self.angle_snap_step, self.angle_snap_enabled)
                        hpr_idx = {1: 0, 0: 1, 2: 2}[ax_idx]
                        new_r = list(self._gizmo_drag_orig_rot)
                        new_r[hpr_idx] = (new_r[hpr_idx] + da_deg) % 360.0
                        self.selected_obj.rotation_hpr = (new_r[0], new_r[1], new_r[2])
                elif self.active_tool == "scale":
                    t_now, _ = closest_point_ray_to_line(ray_o, ray_d, origin, ax)
                    s_ref = max(1.0, self._gizmo_scale(origin))
                    factor = max(0.05, 1.0 + (t_now - self._gizmo_drag_start_t) / s_ref)
                    new_s = list(self._gizmo_drag_orig_scale)
                    if event.modifiers() & Qt.KeyboardModifier.ShiftModifier:
                        new_s = [v * factor for v in self._gizmo_drag_orig_scale]
                    else:
                        new_s[ax_idx] = self._gizmo_drag_orig_scale[ax_idx] * factor
                    self.selected_obj.scale = (new_s[0], new_s[1], new_s[2])
                self.objectSelected.emit(self.selected_obj)
                self.update()
                return

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
                return

        # Dragging a selected road node
        if self._dragging_road_node and self.selected_road_node_id is not None and self.world is not None:
            tp = self._raycast_terrain_point(ray_o, ray_d)
            if tp is not None:
                n = self.world.road_graph.nodes.get(self.selected_road_node_id)
                if n is not None:
                    n.x, n.y, n.z = float(tp[0]), float(tp[1]) + 0.35, float(tp[2])
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
        if event.button() == Qt.MouseButton.RightButton:
            self._rmb_down = False
            self.unsetCursor()
            moved = (event.position().toPoint() - self._rmb_press_pos).manhattanLength()
            if moved <= 4 and self.selected_obj is not None and not self.selected_obj.deleted:
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
                if target == "obj" and self.selected_obj is not None:
                    obj = self.selected_obj
                    op, orot, oscl = self._gizmo_drag_orig_pos, self._gizmo_drag_orig_rot, self._gizmo_drag_orig_scale
                    np_, nrot, nscl = obj.position, obj.rotation_hpr, obj.scale
                    if op != np_ or orot != nrot or oscl != nscl:
                        obj.position, obj.rotation_hpr, obj.scale = op, orot, oscl
                        label = {"move": "Move object", "rotate": "Rotate object", "scale": "Scale object"}.get(
                            self.active_tool, "Transform object"
                        )
                        self.objectTransformed.emit(obj, op, orot, oscl, np_, nrot, nscl, label)
                elif target == "marker" and self.selected_marker is not None:
                    mk = self.selected_marker
                    op, oh = self._gizmo_drag_orig_pos, self._gizmo_drag_orig_hdg
                    np_, nh = mk.position, mk.heading_deg
                    if op != np_ or abs(oh - nh) > 1e-4:
                        mk.position, mk.heading_deg = op, oh
                        label = "Rotate marker" if self.active_tool == "rotate" else "Move marker"
                        self.markerTransformed.emit(mk, op, oh, np_, nh, label)
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

            # Normal picking: Road Node -> Marker -> SOBL Barrier (if Collision ON) -> World Object
            if self.show_roads:
                hit_nid = self._pick_road_node(ray_o, ray_d, max_rad=2.8)
                if hit_nid is not None:
                    self.select_road_node(hit_nid)
                    return

            hit_marker = self._pick_marker(ray_o, ray_d)
            if hit_marker is not None:
                self.select_marker(hit_marker)
                return

            if self.show_collision:
                hit_b = self._pick_sobl_barrier(ray_o, ray_d)
                if hit_b >= 0:
                    self.select_barrier(hit_b)
                    return

            hit_obj = self._pick_object(ray_o, ray_d)
            if hit_obj is not None:
                self.select_object(hit_obj)
            else:
                self.select_object(None)
                self.select_road_node(None)
                self.select_marker(None)
                self.select_barrier(-1, emit=False)

    def wheelEvent(self, event: QWheelEvent) -> None:
        steps = event.angleDelta().y() / 120.0
        if steps > 0:
            self.fly_speed = min(400.0, self.fly_speed * (1.18 ** steps))
        elif steps < 0:
            self.fly_speed = max(2.0, self.fly_speed / (1.18 ** (-steps)))
        self._emit_stats()

    def keyPressEvent(self, event) -> None:
        key = event.key()
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
            step = -self.angle_snap_step if (event.modifiers() & Qt.KeyboardModifier.ShiftModifier) else self.angle_snap_step
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
        if self._timer.isActive():
            self._timer.stop()
        super().focusOutEvent(event)

    def hideEvent(self, event) -> None:
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
                "mode": self.render_mode,
            }
        )
