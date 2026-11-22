"""Car Studio interactive telemetry graphs: Dyno, Gearbox Sawtooth & Pacejka Tyres.

Pure physics evaluation helpers (headless-safe for CLI & tests) paired with crisp
dark-theme PySide6 QPainter graph widgets for the World Editor Car Studio.
"""

from __future__ import annotations

import math
from typing import Any

import numpy as np
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import (
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QVBoxLayout,
    QWidget,
)

# ---------------------------------------------------------------------------
# Defaults & Palette
# ---------------------------------------------------------------------------

_PACEJKA_DEFAULTS: dict[str, dict[str, float]] = {
    "Front": {"magic_b": 10.0, "magic_c": 1.9, "magic_d": 1.05, "magic_e": 0.97, "grip": 1.00},
    "Rear": {"magic_b": 10.5, "magic_c": 1.9, "magic_d": 1.10, "magic_e": 0.97, "grip": 1.00},
    "FrontDirt": {"magic_b": 6.5, "magic_c": 1.75, "magic_d": 0.80, "magic_e": 0.85, "grip": 0.78},
    "RearDirt": {"magic_b": 6.8, "magic_c": 1.75, "magic_d": 0.84, "magic_e": 0.85, "grip": 0.80},
}

_SCALAR_DEFAULTS: dict[str, float] = {
    "idle_rpm": 850.0,
    "redline": 6500.0,
    "torque_max": 310.0,
    "torque_rpm": 4000.0,
    "power_max": 320.0,
    "power_rpm": 5600.0,
    "rear_end_ratio": 3.73,
    "g1": 3.20,
    "g2": 2.10,
    "g3": 1.45,
    "g4": 1.08,
    "g5": 0.84,
    "g6": 0.0,
    "tire_diameter": 0.66,
    "mass": 1550.0,
    "cm_height": 0.52,
    "wheelbase": 2.70,
    "brake_factor": 1.0,
    "spring_factor": 1.0,
    "damage_tweak": 1.0,
}

_BG_CARD = QColor("#151922")
_BG_PLOT = QColor("#0f131a")
_BORDER = QColor("#273042")
_GRID = QColor("#1f2736")
_TEXT_DIM = QColor("#8c9bb3")
_TEXT_BRIGHT = QColor("#e6edf7")
_CYAN = QColor("#38bdf8")
_BLUE = QColor("#60a5fa")
_AMBER = QColor("#f59e0b")
_ORANGE = QColor("#fb923c")
_RED = QColor("#ef4444")
_GREEN = QColor("#34d399")


# ---------------------------------------------------------------------------
# Pure Physics Evaluation Helpers
# ---------------------------------------------------------------------------

def _to_float(val: Any, default: float) -> float:
    try:
        v = float(val)
        return v if math.isfinite(v) else default
    except (TypeError, ValueError):
        return default


_KEY_ALIASES: dict[str, tuple[str, ...]] = {
    "idle_rpm": ("idle_rpm", "idle_speed", "idle"),
    "redline": ("redline", "rpm_limit", "max_rpm", "redline_rpm"),
    "torque_max": ("torque_max", "max_torque", "peak_torque"),
    "torque_rpm": ("torque_rpm", "max_torque_rpm", "peak_torque_rpm"),
    "power_max": ("power_max", "max_power", "horsepower", "peak_hp", "hp_max"),
    "power_rpm": ("power_rpm", "max_power_rpm", "peak_hp_rpm"),
    "rear_end_ratio": ("rear_end_ratio", "final_drive", "ratio"),
    "damage_tweak": ("damage_tweak", "damage_scale"),
}

_KEY_SECTION_PRIORITY: dict[str, tuple[str, ...]] = {
    "idle_rpm": ("engine", "enginenormal", "enginepro", "enginearcade"),
    "redline": ("engine", "enginenormal", "enginepro", "enginearcade"),
    "torque_max": ("engine", "enginenormal", "enginepro", "enginearcade"),
    "torque_rpm": ("engine", "enginenormal", "enginepro", "enginearcade"),
    "power_max": ("engine", "enginenormal", "enginepro", "enginearcade"),
    "power_rpm": ("engine", "enginenormal", "enginepro", "enginearcade"),
    "rear_end_ratio": ("transmission", "gearbox", "drivetrain"),
    "g1": ("transmission", "gearbox", "drivetrain"),
    "g2": ("transmission", "gearbox", "drivetrain"),
    "g3": ("transmission", "gearbox", "drivetrain"),
    "g4": ("transmission", "gearbox", "drivetrain"),
    "g5": ("transmission", "gearbox", "drivetrain"),
    "g6": ("transmission", "gearbox", "drivetrain"),
    "mass": ("chassis", "body", "car"),
    "cm_height": ("chassis", "body", "car"),
    "wheelbase": ("chassis", "body", "car"),
    "brake_factor": ("chassis", "brakes", "front", "rear"),
    "spring_factor": ("chassis", "suspension", "front", "rear"),
    "damage_tweak": ("chassis", "damage", "body"),
    "tire_diameter": ("front", "rear", "tires", "wheels", "chassis"),
}


def parse_physics_dict(raw_fields: dict[str, Any] | None) -> dict[str, Any]:
    """Normalize flat, dotted (`Section.key`), or nested `physics.val` dicts."""
    raw = raw_fields or {}
    top_flat: dict[str, Any] = {}
    any_flat: dict[str, Any] = {}
    section_maps: dict[str, dict[str, Any]] = {}

    for k, v in raw.items():
        if isinstance(v, dict):
            sec = str(k).strip().lower()
            sec_dict = section_maps.setdefault(sec, {})
            for sk, sv in v.items():
                sk_low = str(sk).strip().lower()
                sec_dict[sk_low] = sv
                any_flat.setdefault(sk_low, sv)
        else:
            k_str = str(k).strip()
            k_low = k_str.lower()
            matched_sep = False
            for sep in (".", "/"):
                if sep in k_low:
                    sec, sub = k_low.split(sep, 1)
                    section_maps.setdefault(sec, {})[sub] = v
                    any_flat[sub] = v
                    matched_sep = True
                    break
            if not matched_sep:
                top_flat[k_low] = v
            any_flat[k_low] = v

    def _lookup_scalar(key: str, default: float) -> float:
        aliases = _KEY_ALIASES.get(key, (key,))
        # 1. Explicit flat top-level overrides win first
        for alias in aliases:
            if alias in top_flat:
                return _to_float(top_flat[alias], default)
        # 2. Section-priority lookup (e.g. Engine override > EngineNormal; Chassis.mass > EngineNormal.mass)
        for sec in _KEY_SECTION_PRIORITY.get(key, ()):
            sec_dict = section_maps.get(sec)
            if not sec_dict:
                continue
            for alias in aliases:
                if alias in sec_dict:
                    return _to_float(sec_dict[alias], default)
        # 3. Fallback to any section or dotted key
        for alias in aliases:
            if alias in any_flat:
                return _to_float(any_flat[alias], default)
        return default

    parsed: dict[str, Any] = {}
    for key, default in _SCALAR_DEFAULTS.items():
        parsed[key] = _lookup_scalar(key, default)

    # Optional multiplicative torque_scale support when present on Engine/EngineNormal
    tq_scale = _to_float(
        top_flat.get(
            "torque_scale",
            section_maps.get("engine", {}).get(
                "torque_scale",
                section_maps.get("enginenormal", {}).get("torque_scale", 1.0),
            ),
        ),
        1.0,
    )
    if tq_scale > 0.0 and abs(tq_scale - 1.0) > 1e-6:
        parsed["torque_max"] *= tq_scale
        parsed["power_max"] *= tq_scale

    # Sanity clamps on core physical bounds
    parsed["idle_rpm"] = max(300.0, parsed["idle_rpm"])
    parsed["redline"] = max(parsed["idle_rpm"] + 500.0, parsed["redline"])
    parsed["torque_max"] = max(10.0, parsed["torque_max"])
    parsed["power_max"] = max(10.0, parsed["power_max"])
    parsed["torque_rpm"] = float(np.clip(parsed["torque_rpm"], parsed["idle_rpm"] + 100.0, parsed["redline"]))
    parsed["power_rpm"] = float(np.clip(parsed["power_rpm"], parsed["idle_rpm"] + 200.0, parsed["redline"]))
    parsed["rear_end_ratio"] = max(0.5, parsed["rear_end_ratio"])
    parsed["tire_diameter"] = max(0.2, parsed["tire_diameter"])
    parsed["mass"] = max(200.0, parsed["mass"])
    for i in range(1, 7):
        gkey = f"g{i}"
        parsed[gkey] = max(0.0, parsed[gkey])

    for sec_name, defaults in _PACEJKA_DEFAULTS.items():
        sec_low = sec_name.lower()
        sec_src = section_maps.get(sec_low, {})
        tyre: dict[str, float] = {}
        for pkey, pdef in defaults.items():
            val = sec_src.get(pkey)
            if val is None:
                val = any_flat.get(f"{sec_low}_{pkey}", any_flat.get(f"{sec_low}.{pkey}", pdef))
            tyre[pkey] = _to_float(val, pdef)
        tyre["grip"] = max(0.05, tyre["grip"])
        parsed[sec_name] = tyre

    return parsed


def _hermite_eval(xs: np.ndarray, knots_x: list[float], knots_y: list[float]) -> np.ndarray:
    """Evaluate a smooth monotone-guarded cubic Hermite curve across sorted knots."""
    kx = np.asarray(knots_x, dtype=np.float64)
    ky = np.asarray(knots_y, dtype=np.float64)
    n = len(kx)
    if n < 2:
        return np.full_like(xs, ky[0] if n else 0.0)

    dx = np.diff(kx)
    secants = np.diff(ky) / np.maximum(dx, 1e-6)
    slopes = np.zeros(n, dtype=np.float64)
    slopes[0] = secants[0]
    slopes[-1] = secants[-1]
    for i in range(1, n - 1):
        if secants[i - 1] * secants[i] <= 0.0:
            slopes[i] = 0.0
        else:
            slopes[i] = 2.0 * secants[i - 1] * secants[i] / (secants[i - 1] + secants[i])

    out = np.empty_like(xs, dtype=np.float64)
    idx = np.clip(np.searchsorted(kx, xs, side="right") - 1, 0, n - 2)
    h = np.maximum(kx[idx + 1] - kx[idx], 1e-6)
    t = np.clip((xs - kx[idx]) / h, 0.0, 1.0)
    t2 = t * t
    t3 = t2 * t
    h00 = 2.0 * t3 - 3.0 * t2 + 1.0
    h10 = t3 - 2.0 * t2 + t
    h01 = -2.0 * t3 + 3.0 * t2
    h11 = t3 - t2
    out[:] = h00 * ky[idx] + h10 * h * slopes[idx] + h01 * ky[idx + 1] + h11 * h * slopes[idx + 1]
    return out


def compute_dyno_curve(
    phys: dict[str, Any], samples: int = 64
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Return `(rpm, torque_ftlb, hp)` smooth curve satisfying `HP = Torque * RPM / 5252`."""
    p = parse_physics_dict(phys)
    n = max(8, int(samples))
    idle = p["idle_rpm"]
    redline = p["redline"]
    t_max = p["torque_max"]
    p_max = p["power_max"]
    t_rpm = p["torque_rpm"]
    p_rpm = p["power_rpm"]

    rpm = np.linspace(idle, redline, n, dtype=np.float64)

    # Target torque at peak-HP RPM required to match power_max exactly
    t_at_prpm = p_max * 5252.0 / max(p_rpm, 1.0)
    t_idle = max(15.0, min(t_max, t_at_prpm) * 0.60)
    t_mid_low = t_idle + (t_max - t_idle) * 0.65

    knots: list[tuple[float, float]] = [(idle, t_idle)]
    if t_rpm - idle > 350.0:
        knots.append((idle + 0.45 * (t_rpm - idle), t_mid_low))
    knots.append((t_rpm, t_max))
    if abs(p_rpm - t_rpm) >= 150.0:
        knots.append((p_rpm, t_at_prpm))
    else:
        knots[-1] = (t_rpm, max(t_max, t_at_prpm))

    knots.sort(key=lambda kv: kv[0])
    last_rpm, last_tq = knots[-1]
    if redline - last_rpm > 50.0:
        # Taper power smoothly past power_rpm so peak HP occurs accurately at power_rpm
        redline_hp_drop = 0.10 * min(1.0, (redline - last_rpm) / max(500.0, 0.20 * redline))
        t_redline = max(15.0, (p_max * (1.0 - redline_hp_drop)) * 5252.0 / redline)
        knots.append((redline, min(last_tq * 0.96, t_redline)))
    elif knots[-1][0] < redline:
        knots[-1] = (redline, last_tq)

    # Deduplicate strictly increasing knot x coordinates
    clean_x: list[float] = []
    clean_y: list[float] = []
    for kx, ky in knots:
        if not clean_x or kx > clean_x[-1] + 1.0:
            clean_x.append(kx)
            clean_y.append(ky)
        else:
            clean_y[-1] = max(clean_y[-1], ky)

    torque = np.maximum(5.0, _hermite_eval(rpm, clean_x, clean_y))
    hp = torque * rpm / 5252.0
    return rpm, torque, hp


def compute_gearbox_sawtooth(phys: dict[str, Any]) -> list[dict[str, float | int]]:
    """Compute sawtooth speed/RPM envelope for each active gear `g1..g6` (> 0)."""
    p = parse_physics_dict(phys)
    idle = p["idle_rpm"]
    redline = p["redline"]
    final_drive = p["rear_end_ratio"]
    tire_diam = p["tire_diameter"]

    def _mph(rpm_val: float, gear_ratio: float) -> float:
        return rpm_val * math.pi * tire_diam * 60.0 / (gear_ratio * final_drive * 1609.344)

    active = [(i, p[f"g{i}"]) for i in range(1, 7) if p[f"g{i}"] > 0.0]
    if not active:
        return []

    gears: list[dict[str, float | int]] = []
    prev_ratio: float | None = None
    for gear_num, ratio in active:
        max_mph = _mph(redline, ratio)
        if prev_ratio is None:
            drop_rpm = idle
            min_mph = _mph(idle, ratio)
        else:
            drop_rpm = float(np.clip(redline * (ratio / prev_ratio), idle, redline))
            min_mph = _mph(drop_rpm, ratio)
        gears.append(
            {
                "gear_num": gear_num,
                "ratio": float(ratio),
                "min_mph": float(min_mph),
                "max_mph": float(max_mph),
                "shift_drop_rpm": float(drop_rpm),
                "top_speed_mph": 0.0,
            }
        )
        prev_ratio = ratio

    top_speed = max(float(g["max_mph"]) for g in gears)
    for g in gears:
        g["top_speed_mph"] = top_speed
    return gears


def compute_pacejka_curve(
    b: float, c: float, d: float, e: float, grip: float = 1.0, samples: int = 64
) -> tuple[np.ndarray, np.ndarray]:
    """Evaluate Pacejka Magic Formula over slip `x in [0, 1.0]`."""
    n = max(8, int(samples))
    x = np.linspace(0.0, 1.0, n, dtype=np.float64)
    bx = float(b) * x
    y = float(grip) * float(d) * np.sin(float(c) * np.arctan(bx - float(e) * (bx - np.arctan(bx))))
    return x, y


def estimate_performance_summary(phys: dict[str, Any]) -> dict[str, float]:
    """Estimate peak dyno outputs, top speed, power-to-weight, 0–60 and 1/4-mile metrics."""
    p = parse_physics_dict(phys)
    _, torque, hp = compute_dyno_curve(p)
    gears = compute_gearbox_sawtooth(p)

    peak_hp = float(np.max(hp))
    peak_tq = float(np.max(torque))
    top_speed = float(gears[-1]["top_speed_mph"]) if gears else 0.0

    mass_kg = p["mass"]
    mass_lb = mass_kg * 2.20462262185
    p2w_hp_ton = peak_hp / (mass_kg / 1000.0)

    # Effective asphalt traction from Front & Rear Pacejka peak (grip * D)
    f_grip = p["Front"]["grip"] * abs(p["Front"]["magic_d"])
    r_grip = p["Rear"]["grip"] * abs(p["Rear"]["magic_d"])
    asphalt_grip = float(np.clip(0.5 * (f_grip + r_grip), 0.45, 2.2))

    # Hale / Fox empirical scaling by weight-to-power ratio and asphalt grip
    wt_per_hp = mass_lb / max(peak_hp, 1.0)
    est_quarter_s = 5.825 * (wt_per_hp ** (1.0 / 3.0)) * ((1.0 / asphalt_grip) ** 0.16)
    est_trap_mph = 234.0 * ((max(peak_hp, 1.0) / mass_lb) ** (1.0 / 3.0))
    est_0_60_s = 0.88 * (wt_per_hp ** 0.75) * ((1.0 / asphalt_grip) ** 0.24)

    return {
        "peak_hp": peak_hp,
        "peak_torque_ftlb": peak_tq,
        "top_speed_mph": top_speed,
        "power_to_weight_hp_ton": p2w_hp_ton,
        "est_0_60_s": float(est_0_60_s),
        "est_quarter_mile_s": float(est_quarter_s),
        "est_quarter_trap_mph": float(est_trap_mph),
    }


# ---------------------------------------------------------------------------
# Dark-Theme Qt6 QPainter Graph Widgets
# ---------------------------------------------------------------------------

class _BaseGraphWidget(QWidget):
    """Shared dark-theme canvas, grid, axis labels and legend drawing helpers."""

    def __init__(self, title: str, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._title = title
        self.setMinimumSize(260, 175)

    def _begin_frame(self, painter: QPainter) -> QRectF:
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        outer = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        painter.setPen(QPen(_BORDER, 1.0))
        painter.setBrush(_BG_CARD)
        painter.drawRoundedRect(outer, 8.0, 8.0)

        font_title = QFont("Inter", 9, QFont.Weight.DemiBold)
        painter.setFont(font_title)
        painter.setPen(_TEXT_BRIGHT)
        painter.drawText(QRectF(12.0, 6.0, outer.width() - 24.0, 20.0), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, self._title)

        plot = QRectF(44.0, 30.0, max(40.0, outer.width() - 58.0), max(40.0, outer.height() - 54.0))
        painter.setPen(QPen(_BORDER, 1.0))
        painter.setBrush(_BG_PLOT)
        painter.drawRoundedRect(plot, 4.0, 4.0)
        return plot

    def _draw_grid(
        self,
        painter: QPainter,
        plot: QRectF,
        x_min: float,
        x_max: float,
        y_min: float,
        y_max: float,
        x_ticks: int = 5,
        y_ticks: int = 4,
        x_fmt: str = "{:.0f}",
        y_fmt: str = "{:.0f}",
    ) -> None:
        font_axis = QFont("Inter", 8)
        painter.setFont(font_axis)
        grid_pen = QPen(_GRID, 1.0)

        for i in range(y_ticks + 1):
            frac = i / y_ticks
            y = plot.bottom() - frac * plot.height()
            val = y_min + frac * (y_max - y_min)
            if 0 < i < y_ticks:
                painter.setPen(grid_pen)
                painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y))
            painter.setPen(_TEXT_DIM)
            painter.drawText(QRectF(4.0, y - 8.0, plot.left() - 8.0, 16.0), Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, y_fmt.format(val))

        for i in range(x_ticks + 1):
            frac = i / x_ticks
            x = plot.left() + frac * plot.width()
            val = x_min + frac * (x_max - x_min)
            if 0 < i < x_ticks:
                painter.setPen(grid_pen)
                painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()))
            painter.setPen(_TEXT_DIM)
            painter.drawText(QRectF(x - 24.0, plot.bottom() + 3.0, 48.0, 16.0), Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop, x_fmt.format(val))

    @staticmethod
    def _map_pt(plot: QRectF, x: float, y: float, x_min: float, x_max: float, y_min: float, y_max: float) -> QPointF:
        nx = (x - x_min) / max(x_max - x_min, 1e-6)
        ny = (y - y_min) / max(y_max - y_min, 1e-6)
        return QPointF(plot.left() + nx * plot.width(), plot.bottom() - ny * plot.height())

    @staticmethod
    def _draw_polyline(painter: QPainter, pts: list[QPointF], color: QColor, width: float = 2.0, dashed: bool = False) -> None:
        if len(pts) < 2:
            return
        pen = QPen(color, width, Qt.PenStyle.DashLine if dashed else Qt.PenStyle.SolidLine)
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        path = QPainterPath(pts[0])
        for pt in pts[1:]:
            path.lineTo(pt)
        painter.drawPath(path)

    @staticmethod
    def _draw_legend(painter: QPainter, plot: QRectF, items: list[tuple[str, QColor, bool]]) -> None:
        font = QFont("Inter", 8)
        painter.setFont(font)
        x = plot.right() - 6.0
        y = plot.top() - 12.0
        for label, color, dashed in reversed(items):
            tw = painter.fontMetrics().horizontalAdvance(label)
            x -= tw
            painter.setPen(_TEXT_DIM)
            painter.drawText(QPointF(x, y + 4.0), label)
            x -= 16.0
            pen = QPen(color, 2.0, Qt.PenStyle.DashLine if dashed else Qt.PenStyle.SolidLine)
            painter.setPen(pen)
            painter.drawLine(QPointF(x, y), QPointF(x + 12.0, y))
            x -= 10.0


class DynoGraphWidget(_BaseGraphWidget):
    """Engine dyno graph rendering Torque (amber), HP (cyan), peaks & redline bar."""

    def __init__(self, phys: dict[str, Any] | None = None, parent: QWidget | None = None) -> None:
        super().__init__("Engine Dyno (HP & Torque)", parent)
        self._phys = parse_physics_dict(phys)
        self._rpm, self._torque, self._hp = compute_dyno_curve(self._phys)

    def set_physics(self, phys: dict[str, Any]) -> None:
        self._phys = parse_physics_dict(phys)
        self._rpm, self._torque, self._hp = compute_dyno_curve(self._phys)
        self.update()

    def paintEvent(self, _event: Any) -> None:
        painter = QPainter(self)
        plot = self._begin_frame(painter)
        x_min = 0.0
        x_max = max(self._phys["redline"] * 1.06, 1000.0)
        y_max = max(float(np.max(self._torque)), float(np.max(self._hp)), 50.0) * 1.15

        self._draw_grid(painter, plot, x_min, x_max, 0.0, y_max, x_ticks=5, y_ticks=4, x_fmt="{:.0f}", y_fmt="{:.0f}")
        self._draw_legend(painter, plot, [("Torque (ft-lb)", _AMBER, False), ("Power (HP)", _CYAN, False)])

        # Redline vertical indicator
        rl_x = self._map_pt(plot, self._phys["redline"], 0.0, x_min, x_max, 0.0, y_max).x()
        painter.setPen(QPen(_RED, 1.5, Qt.PenStyle.DashLine))
        painter.drawLine(QPointF(rl_x, plot.top()), QPointF(rl_x, plot.bottom()))

        tq_pts = [self._map_pt(plot, float(r), float(t), x_min, x_max, 0.0, y_max) for r, t in zip(self._rpm, self._torque)]
        hp_pts = [self._map_pt(plot, float(r), float(h), x_min, x_max, 0.0, y_max) for r, h in zip(self._rpm, self._hp)]
        self._draw_polyline(painter, tq_pts, _AMBER, 2.2)
        self._draw_polyline(painter, hp_pts, _CYAN, 2.2)

        # Peak callout markers
        tq_i = int(np.argmax(self._torque))
        hp_i = int(np.argmax(self._hp))
        painter.setFont(QFont("Inter", 8, QFont.Weight.DemiBold))
        for pt, color, txt, dy in (
            (tq_pts[tq_i], _AMBER, f"{self._torque[tq_i]:.0f} lb-ft", -8.0),
            (hp_pts[hp_i], _CYAN, f"{self._hp[hp_i]:.0f} HP", -18.0 if abs(tq_pts[tq_i].x() - hp_pts[hp_i].x()) < 55 else -8.0),
        ):
            painter.setBrush(color)
            painter.setPen(QPen(_BG_PLOT, 1.2))
            painter.drawEllipse(pt, 3.8, 3.8)
            painter.setPen(color)
            tx = min(max(pt.x() - 28.0, plot.left() + 4.0), plot.right() - 62.0)
            ty = max(pt.y() + dy, plot.top() + 12.0)
            painter.drawText(QPointF(tx, ty), txt)
        painter.end()


class GearboxSawtoothWidget(_BaseGraphWidget):
    """Speed-vs-RPM gearbox sawtooth with dashed shift drops and top-speed callout."""

    def __init__(self, phys: dict[str, Any] | None = None, parent: QWidget | None = None) -> None:
        super().__init__("Gearbox Sawtooth (MPH vs RPM)", parent)
        self._phys = parse_physics_dict(phys)
        self._gears = compute_gearbox_sawtooth(self._phys)

    def set_physics(self, phys: dict[str, Any]) -> None:
        self._phys = parse_physics_dict(phys)
        self._gears = compute_gearbox_sawtooth(self._phys)
        self.update()

    def paintEvent(self, _event: Any) -> None:
        painter = QPainter(self)
        plot = self._begin_frame(painter)
        redline = self._phys["redline"]
        top_mph = float(self._gears[-1]["top_speed_mph"]) if self._gears else 120.0
        x_max = max(top_mph * 1.08, 40.0)
        y_max = redline * 1.12

        self._draw_grid(painter, plot, 0.0, x_max, 0.0, y_max, x_ticks=5, y_ticks=4, x_fmt="{:.0f} mph", y_fmt="{:.0f}")

        # Redline horizontal reference
        rl_y = self._map_pt(plot, 0.0, redline, 0.0, x_max, 0.0, y_max).y()
        painter.setPen(QPen(_RED, 1.2, Qt.PenStyle.DashLine))
        painter.drawLine(QPointF(plot.left(), rl_y), QPointF(plot.right(), rl_y))

        painter.setFont(QFont("Inter", 8, QFont.Weight.DemiBold))
        prev_end: QPointF | None = None
        for g in self._gears:
            p0 = self._map_pt(plot, float(g["min_mph"]), float(g["shift_drop_rpm"]), 0.0, x_max, 0.0, y_max)
            p1 = self._map_pt(plot, float(g["max_mph"]), redline, 0.0, x_max, 0.0, y_max)
            if prev_end is not None:
                self._draw_polyline(painter, [prev_end, p0], _AMBER, 1.4, dashed=True)
            self._draw_polyline(painter, [p0, p1], _CYAN, 2.2)
            mid = QPointF(0.5 * (p0.x() + p1.x()), 0.5 * (p0.y() + p1.y()) - 6.0)
            painter.setPen(_TEXT_BRIGHT)
            painter.drawText(QPointF(mid.x() - 8.0, mid.y()), f"G{g['gear_num']}")
            prev_end = p1

        # Top speed callout badge
        badge = f"Top: {top_mph:.0f} mph"
        painter.setPen(_GREEN)
        painter.drawText(QRectF(plot.right() - 105.0, plot.top() - 22.0, 102.0, 18.0), Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, badge)
        painter.end()


class PacejkaGraphWidget(_BaseGraphWidget):
    """Pacejka Magic Formula tyre curves: Asphalt Front/Rear vs Dirt Front/Rear."""

    _SERIES = (
        ("Front", "Asphalt F", _CYAN, False),
        ("Rear", "Asphalt R", _BLUE, True),
        ("FrontDirt", "Dirt F", _AMBER, False),
        ("RearDirt", "Dirt R", _ORANGE, True),
    )

    def __init__(self, phys: dict[str, Any] | None = None, parent: QWidget | None = None) -> None:
        super().__init__("Pacejka Tyre Grip (Asphalt vs Dirt)", parent)
        self._phys = parse_physics_dict(phys)
        self._curves = self._eval_curves()

    def _eval_curves(self) -> dict[str, tuple[np.ndarray, np.ndarray]]:
        out: dict[str, tuple[np.ndarray, np.ndarray]] = {}
        for sec, _, _, _ in self._SERIES:
            t = self._phys[sec]
            out[sec] = compute_pacejka_curve(t["magic_b"], t["magic_c"], t["magic_d"], t["magic_e"], t["grip"])
        return out

    def set_physics(self, phys: dict[str, Any]) -> None:
        self._phys = parse_physics_dict(phys)
        self._curves = self._eval_curves()
        self.update()

    def paintEvent(self, _event: Any) -> None:
        painter = QPainter(self)
        plot = self._begin_frame(painter)
        y_max = max((float(np.max(np.abs(y))) for _, y in self._curves.values()), default=1.2)
        y_max = max(y_max * 1.15, 0.5)

        self._draw_grid(painter, plot, 0.0, 1.0, 0.0, y_max, x_ticks=5, y_ticks=4, x_fmt="{:.2f}", y_fmt="{:.2f}")
        self._draw_legend(painter, plot, [(lbl, col, dash) for _, lbl, col, dash in self._SERIES])

        for sec, _, color, dashed in self._SERIES:
            xs, ys = self._curves[sec]
            pts = [self._map_pt(plot, float(x), float(y), 0.0, 1.0, 0.0, y_max) for x, y in zip(xs, ys)]
            self._draw_polyline(painter, pts, color, 2.0, dashed=dashed)
        painter.end()


class CarTelemetryGraphsPanel(QWidget):
    """Unified dark-theme telemetry card with KPI pills and 3 live physics graphs."""

    def __init__(self, phys: dict[str, Any] | None = None, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._phys = parse_physics_dict(phys)
        self._kpi_labels: dict[str, QLabel] = {}

        root = QVBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(8)

        # KPI pill bar
        kpi_row = QHBoxLayout()
        kpi_row.setSpacing(6)
        for key, title, accent in (
            ("peak_hp", "PEAK HP", "#38bdf8"),
            ("peak_torque_ftlb", "PEAK TORQUE", "#f59e0b"),
            ("top_speed_mph", "TOP SPEED", "#34d399"),
            ("est_0_60_s", "0–60 MPH", "#a78bfa"),
            ("est_quarter_mile_s", "1/4 MILE", "#fb923c"),
        ):
            pill = QFrame()
            pill.setStyleSheet(
                f"QFrame {{ background: #151922; border: 1px solid #273042; border-radius: 7px; }}"
            )
            pl = QVBoxLayout(pill)
            pl.setContentsMargins(10, 5, 10, 5)
            pl.setSpacing(1)
            lbl_t = QLabel(title)
            lbl_t.setStyleSheet("color: #8c9bb3; font-size: 9px; font-weight: 600; border: none;")
            lbl_v = QLabel("—")
            lbl_v.setStyleSheet(f"color: {accent}; font-size: 13px; font-weight: 700; border: none;")
            pl.addWidget(lbl_t)
            pl.addWidget(lbl_v)
            kpi_row.addWidget(pill, 1)
            self._kpi_labels[key] = lbl_v
        root.addLayout(kpi_row)

        # 3 live graph widgets
        self.dyno_widget = DynoGraphWidget(self._phys, self)
        self.gearbox_widget = GearboxSawtoothWidget(self._phys, self)
        self.pacejka_widget = PacejkaGraphWidget(self._phys, self)

        grid = QGridLayout()
        grid.setSpacing(8)
        grid.addWidget(self.dyno_widget, 0, 0)
        grid.addWidget(self.gearbox_widget, 0, 1)
        grid.addWidget(self.pacejka_widget, 1, 0, 1, 2)
        root.addLayout(grid, 1)

        self.set_physics(self._phys)

    def set_physics(self, phys: dict[str, Any]) -> None:
        self._phys = parse_physics_dict(phys)
        summary = estimate_performance_summary(self._phys)
        self._kpi_labels["peak_hp"].setText(f"{summary['peak_hp']:.0f} HP")
        self._kpi_labels["peak_torque_ftlb"].setText(f"{summary['peak_torque_ftlb']:.0f} lb-ft")
        self._kpi_labels["top_speed_mph"].setText(f"{summary['top_speed_mph']:.0f} mph")
        self._kpi_labels["est_0_60_s"].setText(f"{summary['est_0_60_s']:.2f} s")
        self._kpi_labels["est_quarter_mile_s"].setText(
            f"{summary['est_quarter_mile_s']:.2f} s @ {summary['est_quarter_trap_mph']:.0f}"
        )
        self.dyno_widget.set_physics(self._phys)
        self.gearbox_widget.set_physics(self._phys)
        self.pacejka_widget.set_physics(self._phys)
