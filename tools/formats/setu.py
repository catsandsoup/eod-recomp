"""SETU: car setup sheets (`*.gar`, 196 bytes), from the game's NASCAR-derived engine.

Contract: docs/FORMAT_VIEW_CONTRACT.md. Evidence and per-mode file list:
docs/SETU_CAR_SETUP.md.

The payload is 49 little-endian float32 values with no header: the type code SETU
exists only in the archive directory entry, never in the 196 bytes. Most values
are 0-1 slider positions across the setup ranges every stock car declares in its
physics.val ([Front]/[Rear] rate, bump, rebound, camber, toe, sway; [Chassis]
wheel_lock, rspoiler_angle). Tyre pressures, gear ratios, final drive and fuel
are stored directly.

The retail executable (SLUS_209.10) never loads these files: no code requests
type SETU (ResourceGet 0x00132638 / 0x00130D90 and ResourceGetStream 0x001323E8
are always passed a literal or table type, none of them SETU), no ".gar" name or
name hash exists in the executable, and its car physics (0x00185470) reads a
reduced physics.val key set without any of the setup range keys. Field meanings
below therefore come from data patterns, not from a loader; confidence says so.
"""
import math
import struct

FOURCCS = ("SETU",)

SIZE = 196
SLOTS = SIZE // 4

_NOT_READ = ("No reader in SLUS_209.10: the SETU type, '.gar' names and their CRCs are absent from the "
             "executable, and car physics 0x00185470 never reads the setup range keys.")

# Stock ranges from every retail car's physics.val (37 cars, identical).
_RANGE = {
    "spring": (200.0, 600.0, "lb/in"),
    "shock": (60.0, 840.0, ""),
    "camber_front": (-10.0, 10.0, "deg"),
    "camber_rear": (-1.5, 1.5, "deg"),
    "toe_front": (-4.0, 4.0, ""),
    "toe_rear": (-0.5, 0.5, ""),
    "sway_front": (0.0, 800.0, ""),
    "sway_rear": (0.0, 50.0, ""),
    "lock": (0.0, 35.0, "deg"),
    "spoiler": (40.0, 70.0, "deg"),
}

_WHEELS = (("front_left", "Front left wheel", "front"), ("front_right", "Front right wheel", "front"),
           ("rear_left", "Rear left wheel", "rear"), ("rear_right", "Rear right wheel", "rear"))

ROLES = {
    "default.gar": "Default sheet for this track.",
    "advanced.gar": "Sheet for the advanced handling mode (name from the source engine).",
    "arcade.gar": "Sheet for the arcade handling mode (name from the source engine).",
    "ai.gar": "Sheet for computer-driven opponents: rounder, hand-typed values, equal tyre pressures and no fuel.",
    "dirtac.gar": "Dirt-car copy of Advanced.gar with its own final drive.",
    "dirtc.gar": "Dirt-car variant of the default sheet with its own final drive.",
    "modifiedac.gar": "Modified-car copy of Advanced.gar with its own final drive.",
    "modifiedc.gar": "Modified-car variant of the default sheet with its own final drive.",
    "fig8.gar": "Figure-8 layout sheet.",
}


def _slot(index, fid, group, label, unit, lo, hi, confidence, explanation, evidence, kind="raw", scale=None):
    return dict(index=index, id=fid, group=group, label=label, unit=unit, min=lo, max=hi,
                confidence=confidence, explanation=explanation, evidence=evidence, kind=kind, scale=scale)


def _layout():
    s = []
    s.append(_slot(0, "chassis.front_sway_bar", "Chassis", "Front anti-roll bar", "", 0.0, 1.0, "medium",
                   "Stiffer bar: less body roll and more understeer at the limit.",
                   "Every retail value is (k/16)^4 or (k/32)^4, the fourth power of a bar diameter as a torsion "
                   "bar's stiffness is; scales the car's physics.val [Front] sway range. " + _NOT_READ,
                   kind="sway", scale="sway_front"))
    s.append(_slot(1, "chassis.front_toe", "Chassis", "Front toe (unconfirmed)", "", 0.0, 1.0, "low",
                   "Probably front toe; 0.5 is straight ahead. Toe-out sharpens turn-in, toe-in steadies the car.",
                   "Values 0.5-0.55 land on 0.1 steps of the physics.val [Front] toe range (-4 to 4). " + _NOT_READ,
                   kind="slider", scale="toe_front"))
    s.append(_slot(2, "chassis.unknown_08", "Chassis", "Unknown chassis setting at 0x08", "", 0.0, 1.0, "low",
                   "Meaning not established. 0 in almost every sheet; 0.75 and 0.125 only in two default sheets.",
                   "Data pattern only. " + _NOT_READ))
    s.append(_slot(3, "chassis.unknown_0c", "Chassis", "Unknown chassis setting at 0x0C", "", 0.0, 1.0, "low",
                   "Meaning not established. A 0-1 slider in 0.05 steps that differs between handling modes.",
                   "Data pattern only. " + _NOT_READ))
    s.append(_slot(4, "chassis.rear_sway_bar", "Chassis", "Rear anti-roll bar", "", 0.0, 1.0, "medium",
                   "Stiffer bar: less body roll, the rear steps out more easily (more oversteer).",
                   "Retail values 0, 0.5^4, 0.6^4 and 1, the same diameter^4 pattern as the front bar; scales the "
                   "physics.val [Rear] sway range. " + _NOT_READ, kind="sway", scale="sway_rear"))
    s.append(_slot(5, "chassis.rear_toe", "Chassis", "Rear toe (unconfirmed)", "", 0.0, 1.0, "low",
                   "Probably rear toe; 0.5 is straight ahead. Toe-in steadies the rear under power.",
                   "Values 0.5-0.7 land on 0.1 steps of the physics.val [Rear] toe range (-0.5 to 0.5). " + _NOT_READ,
                   kind="slider", scale="toe_rear"))
    s.append(_slot(6, "chassis.unknown_18", "Chassis", "Unknown chassis setting at 0x18", "", 0.0, 1.0, "low",
                   "Meaning not established. 0 in most sheets; 0.125, 0.3 and 1 in a few.",
                   "Data pattern only. " + _NOT_READ))
    s.append(_slot(7, "chassis.unknown_1c", "Chassis", "Unknown chassis setting at 0x1C", "", 0.0, 1.0, "low",
                   "Meaning not established. 1 in every player sheet, 0.5 in the opponent sheets.",
                   "Data pattern only. " + _NOT_READ))
    for w, (key, group, axle) in enumerate(_WHEELS):
        base = 8 + 5 * w
        s.append(_slot(base, f"{key}.spring", group, "Spring rate", "", 0.0, 1.0, "medium",
                       "Stiffer spring: quicker response and less dive and squat, less grip over bumps.",
                       "0-1 slider in 1/80 steps (5 lb/in of the physics.val rate range 200-600). " + _NOT_READ,
                       kind="slider", scale="spring"))
        s.append(_slot(base + 1, f"{key}.bump", group, "Shock bump", "", 0.0, 1.0, "medium",
                       "Firmer bump: the wheel resists being pushed up; steadier on smooth tracks, skittish on rough ones.",
                       "Every non-opponent value is a multiple of 1/78, 10-unit steps of the physics.val bump range "
                       "60-840; usually below rebound. " + _NOT_READ, kind="slider", scale="shock"))
        s.append(_slot(base + 2, f"{key}.rebound", group, "Shock rebound", "", 0.0, 1.0, "medium",
                       "Firmer rebound: the wheel returns more slowly; holds the body flatter, can lose contact on bumps.",
                       "Multiples of 1/78 of the physics.val rebound range 60-840. " + _NOT_READ,
                       kind="slider", scale="shock"))
        camber = "camber_front" if axle == "front" else "camber_rear"
        s.append(_slot(base + 3, f"{key}.camber", group, "Camber", "", 0.0, 1.0, "medium",
                       "0.5 is upright. Tilting the top of the tyre toward the car adds cornering grip and costs braking grip.",
                       ("Lands on 0.1 degree steps of the physics.val [Front] camber range -10 to 10; left side "
                        "positive and right side negative on ovals. " if axle == "front" else
                        "Lands on 0.05 degree steps of the physics.val [Rear] camber range -1.5 to 1.5; left side "
                        "above right as on ovals. ") + _NOT_READ, kind="slider", scale=camber))
        s.append(_slot(base + 4, f"{key}.pressure", group, "Tyre pressure", "psi", 10.0, 60.0, "high",
                       "Higher pressure: sharper turn-in and less rolling drag, less grip, especially on dirt.",
                       "Stored directly as psi: 24-32.5 in every sheet, left side lower than right on oval sheets. "
                       + _NOT_READ, kind="psi"))
    s.append(_slot(28, "steering.lock", "Steering", "Steering lock", "", 0.0, 1.0, "medium",
                   "More lock: the front wheels turn further for tighter corners and spins; less makes steering calmer.",
                   "Every value is a whole number of degrees of the physics.val [Chassis] wheel_lock range 0-35 "
                   "(9-16 on default sheets, 25-30 on arcade sheets). " + _NOT_READ, kind="slider", scale="lock"))
    s.append(_slot(29, "aero.rear_spoiler", "Aerodynamics", "Rear spoiler angle (unconfirmed)", "", 0.0, 1.0, "low",
                   "Probably the rear spoiler: steeper means more rear grip and more drag.",
                   "Values 0.55-0.85 land on half degrees of the physics.val [Chassis] rspoiler_angle range 40-70. "
                   + _NOT_READ, kind="slider", scale="spoiler"))
    for g in range(4):
        s.append(_slot(30 + g, f"gear.{g + 1}", "Gearbox", f"{('1st', '2nd', '3rd', '4th')[g]} gear ratio", "ratio",
                       0.2, 10.0, "high",
                       "Higher ratio: faster acceleration in this gear, lower top speed before the next shift.",
                       "Decreasing ratios in every sheet (1st 2.4-3.5 down to 4th 0.9-1.6); stock cars have "
                       "num_gears = 4 in physics.val. " + _NOT_READ, kind="ratio"))
    for g, value in ((34, "0.6 or 1.0"), (35, "0.2 or 0.75")):
        s.append(_slot(g, f"gear.{g - 29}", "Gearbox", f"Gear slot {g - 29} (unused by 4-speed cars)", "ratio",
                       0.0, 10.0, "low",
                       f"Follows the four gear ratios; {value} in every sheet. Stock cars have four gears, so this "
                       "slot is probably never used.", "Data pattern only. " + _NOT_READ, kind="ratio"))
    s.append(_slot(36, "gearbox.unknown_90", "Gearbox", "Unknown drivetrain value at 0x90", "", 0.0, 1.0, "low",
                   "Meaning not established. 0.5 in every retail sheet.", "Data pattern only. " + _NOT_READ))
    s.append(_slot(37, "final_drive", "Final drive", "Final drive ratio", "ratio", 1.0, 10.0, "medium",
                   "Higher ratio: stronger acceleration in every gear, lower top speed. Dirt and modified variants "
                   "differ from their base sheet only here.",
                   "3.07-6.5 across sheets; the NASCAR import (tools/nascar_roster.py) maps this offset to "
                   "rear_end_ratio from a trace of the source game. " + _NOT_READ, kind="ratio"))
    s.append(_slot(38, "brakes.balance", "Brakes", "Brake balance (unconfirmed)", "", 0.0, 1.0, "low",
                   "Probably the front share of braking force: higher locks the fronts first (stable, more "
                   "understeer); lower lets the rear lock and rotate the car.",
                   "0.475-0.51 in 0.005 steps, the usual granularity of a brake bias setting. " + _NOT_READ))
    s.append(_slot(39, "weight.unknown_9c", "Weight", "Unknown weight setting at 0x9C", "", 0.0, 1.0, "low",
                   "Meaning not established: 0.5 on default sheets, 0.542 on advanced, arcade, dirt and opponent "
                   "sheets. Possibly left-side weight share.", "Data pattern only. " + _NOT_READ))
    s.append(_slot(40, "weight.unknown_a0", "Weight", "Unknown weight setting at 0xA0", "", 0.0, 1.0, "low",
                   "Meaning not established: 0.5-0.52. Possibly cross weight (wedge).", "Data pattern only. " + _NOT_READ))
    s.append(_slot(41, "fuel.load", "Fuel", "Fuel load", "gal", 0.0, 22.0, "medium",
                   "Fuel at the start. More fuel is heavier: slower acceleration and braking.",
                   "22 in every player sheet, equal to fuel_capacity = 22 in every stock physics.val; 0 in "
                   "opponent sheets. " + _NOT_READ, kind="gal"))
    s.append(_slot(42, "chassis.unknown_a8", "Chassis", "Unknown setting at 0xA8", "", 0.0, 1.0, "low",
                   "Meaning not established: 0, 0.125 or 0.2. Possibly grille tape (more tape: more downforce, "
                   "hotter engine).", "Data pattern only. " + _NOT_READ))
    s.append(_slot(43, "chassis.unknown_ac", "Chassis", "Unknown setting at 0xAC", "", 0.0, 30.0, "low",
                   "Meaning not established: a whole number (0, 9 or 11) equal to the next value. Possibly a rear "
                   "track bar height in inches; 0 in most sheets.", "Data pattern only. " + _NOT_READ))
    s.append(_slot(44, "chassis.unknown_b0", "Chassis", "Unknown setting at 0xB0", "", 0.0, 30.0, "low",
                   "Meaning not established: always equal to the value before it.", "Data pattern only. " + _NOT_READ))
    return s


LAYOUT = _layout()
_BY_ID = {s["id"]: s for s in LAYOUT}
TAIL = (45 * 4, SIZE - 45 * 4)


def _physical(slot, value):
    """Stock-car equivalent of a slider position, or None."""
    if slot["scale"] is None or not math.isfinite(value):
        return None
    lo, hi, unit = _RANGE[slot["scale"]]
    return lo + value * (hi - lo), lo, hi, unit


def _describe_value(slot, value):
    phys = _physical(slot, value)
    if phys is None:
        return slot["explanation"]
    amount, lo, hi, unit = phys
    unit = f" {unit}" if unit else ""
    if slot["kind"] == "sway":
        return (f"{slot['explanation']} Bar size {value ** 0.25 if value > 0 else 0:.4g} of the largest; "
                f"stock-car stiffness about {amount:.4g} on the {lo:g}-{hi:g} scale.")
    return f"{slot['explanation']} On stock cars about {amount:.4g}{unit} (range {lo:g} to {hi:g}{unit})."


def _check(data):
    if len(data) != SIZE:
        raise ValueError(f"a car setup sheet is {SIZE} bytes; this one is {len(data)}")
    return struct.unpack("<49f", data)


def role(member):
    return ROLES.get(str(member or "").rsplit("/", 1)[-1].lower())


def decode(data, context):
    values = _check(bytes(data))
    member = str((context or {}).get("member") or "")
    fields = []
    for slot in LAYOUT:
        value = values[slot["index"]]
        fields.append(dict(
            id=slot["id"], group=slot["group"], label=slot["label"], type="f32", unit=slot["unit"],
            value=value, offset=slot["index"] * 4, size=4, editable=True, min=slot["min"], max=slot["max"],
            confidence=slot["confidence"], explanation=_describe_value(slot, value), evidence=slot["evidence"]))
    tail = data[TAIL[0]:]
    blocks = [dict(name="Unknown block 1", offset=TAIL[0], size=TAIL[1], known=False,
                   explanation=("Four values that are zero in all 64 retail sheets. Saved unchanged."
                                if not any(tail) else "Four values, zero in every retail sheet but not here. Saved unchanged."))]
    rows = []
    for name, part in (("Spring rate", 0), ("Shock bump", 1), ("Shock rebound", 2), ("Camber", 3), ("Tyre pressure (psi)", 4)):
        rows.append([name] + [round(values[8 + 5 * w + part], 4) for w in range(4)])
    warnings = ["Eve of Destruction does not load setup sheets, so edits here do not change how a car drives. "
                "They matter to tools that read sheets, such as the NASCAR import, or to a future loader."]
    bad = [s["label"] for s in LAYOUT if not math.isfinite(values[s["index"]])]
    if bad:
        warnings.append("Not a number in: " + ", ".join(bad) + ". Enter a value to repair it.")
    sheet = role(member)
    summary = ("Car setup sheet: 49 settings covering anti-roll bars, springs, shocks, camber and tyre pressure "
               "for each wheel, steering lock, four gear ratios, final drive and fuel. Most settings are 0-1 "
               "positions on ranges each car declares in its physics.val. These sheets come from the NASCAR engine "
               "the game is built on; the retail executable never reads them, and car handling comes from each "
               "car's physics.val instead. Meanings come from data patterns across all 64 retail sheets.")
    if sheet:
        summary = sheet + " " + summary
    return dict(schema="eod.setu/1", fourcc="SETU", title="Car setup sheet", confidence="medium",
                summary=summary, editable=True, fields=fields, blocks=blocks,
                preview=dict(kind="table", columns=["Setting", "Front left", "Front right", "Rear left", "Rear right"],
                             rows=rows),
                warnings=warnings)


def _number(slot, value):
    if isinstance(value, bool):
        raise ValueError(f"{slot['label']} needs a number, not true or false.")
    try:
        number = float(value)
    except (TypeError, ValueError):
        raise ValueError(f"{slot['label']} needs a number; '{value}' is not one.") from None
    if not math.isfinite(number):
        raise ValueError(f"{slot['label']} needs a finite number.")
    if not slot["min"] <= number <= slot["max"]:
        unit = f" {slot['unit']}" if slot["unit"] and slot["unit"] != "ratio" else ""
        raise ValueError(f"{slot['label']} must be between {slot['min']:g} and {slot['max']:g}{unit}; "
                         f"{number:g} is outside that range. The file was not changed.")
    return number


def encode(data, edits):
    _check(bytes(data))
    out = bytearray(data)
    for field_id, value in (edits or {}).items():
        slot = _BY_ID.get(field_id)
        if slot is None:
            raise ValueError(f"A car setup sheet has no setting '{field_id}'.")
        struct.pack_into("<f", out, slot["index"] * 4, _number(slot, value))
    return bytes(out)
