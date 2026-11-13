"""Rule data for valt_schema.py: family descriptions and one rule per key pattern.

Rule line:  family | section | key | label | group | unit | explanation | options
family, section, key are regular expressions (full match, case-insensitive); `#` matches a
number and `*` matches anything. Options: conf=high|medium|low  used=1|0|?  min=  max=  edit=0
ev=free text (no pipes). First matching rule wins, so specific rules come before general ones.
Confidence: high = the reader was traced and the effect is documented; medium = the reader was
traced or the effect is clear from the value pattern; low = inferred from the name only.
"""
import re
from collections import namedtuple

UNTRACED = "Not yet traced to game code. The value is shown with the name the file uses."
# Key lookup facts (0x135C80): names compare without regard to case, the first matching pair of a
# section wins, and a key missing from a section is looked up in the section it inherits from.
SECTION_GROUP_FALLBACK = "Other values"

_SPLIT = re.compile(r"(?<= )\|(?= )")
Rule = namedtuple("Rule", "fam sec key spec")
RULES = []


def _rx(text):
    text = text.strip()
    if text == "*":
        return re.compile(".*", re.S)
    # Patterns that spell out a capital ("[A-Z]...") are case-sensitive: they pick out named entries
    # (track and car names) from lower-case keyword sections.
    flags = re.S if "[A-Z]" in text else re.I | re.S
    return re.compile(text.replace("#", r"\d+"), flags)


def add(block):
    for raw in block.strip().splitlines():
        line = raw.strip()
        if not line or line.startswith("##"):
            continue
        parts = [p.strip() for p in _SPLIT.split(line)]
        if len(parts) < 7:
            raise ValueError("bad rule line: " + line)
        parts += [""] * (8 - len(parts))
        fam, sec, key, label, group, unit, why, opts = parts[:8]
        spec = dict(label=label, group=group, unit=unit, explanation=why, confidence="medium")
        for token in re.findall(r"(\w+)=((?:\"[^\"]*\")|\S+)", opts):
            name, val = token
            val = val.strip('"')
            if name == "conf":
                spec["confidence"] = {"h": "high", "m": "medium", "l": "low"}.get(val, val)
            elif name == "used":
                spec["used"] = {"1": True, "0": False, "?": None}[val]
            elif name in ("min", "max"):
                spec[name] = float(val)
            elif name == "edit":
                spec["editable"] = val != "0"
            elif name == "ev":
                spec["evidence"] = val
        if spec.get("used") is False and "evidence" not in spec:
            spec["evidence"] = "The key text does not occur in SLUS_209.10; no code reads it"
        RULES.append(Rule(_rx(fam), _rx(sec), _rx(key), spec))


def match(family, section, key):
    sec = re.sub(r"^\$\d+\$", "", section)
    for rule in RULES:
        if rule.fam.fullmatch(family) and rule.sec.fullmatch(sec) and rule.key.fullmatch(key):
            return rule
    if family == "parts":
        return _parts_fallback(sec, key)
    return None


FAMILIES = {}
MEMBER_TITLES = {}
SECTION_RULES = []   # (family regex, section regex, label, explanation)


def family(fid, title, summary):
    FAMILIES[fid] = dict(title=title, summary=summary)


def sections(block):
    for raw in block.strip().splitlines():
        line = raw.strip()
        if not line or line.startswith("##"):
            continue
        fam, sec, label, why = [p.strip() for p in _SPLIT.split(line, 3)]
        SECTION_RULES.append((_rx(fam), _rx(sec), label, why))


def section_info(family_id, section, ordinal, pairs, parent=""):
    """-> (label, explanation) for one section of a table."""
    nested = re.match(r"^\$(\d+)\$(.*)$", section)
    base = nested.group(2) if nested else section
    for fam, sec, label, why in SECTION_RULES:
        if fam.fullmatch(family_id) and sec.fullmatch(base):
            label = label.replace("{n}", str(ordinal)).replace("{name}", _nice(base)).replace("{parent}", parent)
            if "{item}" in label:
                label = label.replace("{item}", _item(family_id, base, pairs) or _nice(base))
            why = why.replace("{parent}", parent)
            if nested:
                why = "Nested table #%d, linked from its parent by the \"@%s\" entry. %s" % (int(nested.group(1)), _unhash(base), why)
            return label, why.strip()
    label = _nice(base)
    if ordinal > 1 or nested:
        label = "%s %d" % (label, ordinal)
    note = "Part of the %s." % FAMILIES.get(family_id, FAMILIES["other"])["title"].lower()
    if nested:
        note = "Nested table #%d of this file. %s" % (int(nested.group(1)), note)
    return label, note + " Not yet traced to game code."


def _unhash(name):
    return name


def _nice(name):
    text = re.sub(r"[_\s]+", " ", name.strip()).strip()
    text = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", text)
    return (text[:1].upper() + text[1:]) if text else "Section"


def _item(family_id, base, pairs):
    d = dict(pairs)
    for k in ("dyno", "name", "friendly_name", "display_name", "event_type", "chassis", "model", "id", "title", "headline"):
        if k in d and d[k]:
            return d[k] if len(d[k]) < 40 else d[k][:37] + "..."
    return None


def value_note(family_id, section, key, value):
    """Per-value text for keys that hold packed data (e.g. 64-symbol position frames)."""
    text = (value or "").strip()
    if len(text) == 64 and " " not in text and "," not in text[:1] + text[-1:] or key in ("unfloat_frame64", "frame64"):
        try:
            import world_export
            f = world_export.decode_frame64(text)
            return "Position x %.1f, y %.1f (up), z %.1f metres" % (f[9], f[10], f[11])
        except Exception:
            return ""
    return ""


family("other", "Game data table", "A key/value table the game reads at start-up or when loading a track or car.")


# ======================================================================= physics.val
family("physics", "Car handling and engine",
       "The car's physics sheet: body dimensions and weight, engine power curve, gearbox, tyre grip model, "
       "cockpit camera and gauge calibration. The game reads it when the car loads.")
MEMBER_TITLES["physics.val"] = ("Car handling and engine", FAMILIES["physics"]["summary"])

sections("""
physics | Chassis | Body and chassis | Weight, size, aerodynamics, cockpit camera and gauge layout of the car.
physics | EngineNormal | Engine | Power and torque curve, idle and redline. The variant used on normal tracks.
physics | EngineRestrictor | Engine (restrictor plate) | Engine variant used on tracks that fit a restrictor plate.
physics | Transmission | Gearbox and drivetrain | Gear ratios, final drive and how drive torque is split between axles.
physics | Front | Front axle and tyres | Front tyre grip model, suspension geometry and wheel class.
physics | Rear | Rear axle and tyres | Rear tyre grip model, suspension geometry and wheel class.
physics | FrontDirt | Front tyres on dirt | Front tyre values that replace the Front section on dirt tracks.
physics | RearDirt | Rear tyres on dirt | Rear tyre values that replace the Rear section on dirt tracks.
physics | speedo_slider | Speedometer needle shift | Moves the speedometer needle between two speeds on the cockpit dial.
""")

add("""
physics | * | // | Comment | Notes | | Free text left by the original authors. The game ignores it. | conf=high used=0 ev=comment line
## ---- Chassis
physics | Chassis | mass | Car weight | Body | lb | Weight of the whole car. Also shown as "Weight" in the part shop. Heavier cars accelerate, brake and turn more slowly. | conf=high used=1 ev=Read by car setup 0x185470; shown in the part shop (0x2DEF98) as lbs
physics | Chassis | mx | Roll inertia (x) | Body | lb in2 | Resistance of the body to rotating about its x axis. Larger values make the car slower to pitch. | conf=medium used=1 ev=Read by car setup 0x185470 together with my and mz; name and magnitude only for the axis assignment
physics | Chassis | my | Spin inertia (y) | Body | lb in2 | Resistance of the body to rotating about its vertical axis. Larger values make the car slower to yaw. | conf=medium used=1 ev=Read by car setup 0x185470
physics | Chassis | mz | Pitch inertia (z) | Body | lb in2 | Resistance of the body to rotating about its z axis. Read together with mx and my. | conf=medium used=1 ev=Read by car setup 0x185470
physics | Chassis | cm_height | Centre of mass height | Body | in | Height of the centre of gravity above the ground. | conf=medium used=1 ev=Read by car setup 0x185470
physics | Chassis | wheelbase | Wheelbase | Body | in | Distance between front and rear axles. | conf=medium used=1 ev=Read by car setup 0x185470 and car preview 0x2B0D20
physics | Chassis | ftrack | Front track width | Body | in | Distance between the front wheels. | conf=medium used=1 ev=Read by car setup 0x185470 and car preview 0x2B0D20
physics | Chassis | rtrack | Rear track width | Body | in | Distance between the rear wheels. | conf=medium used=1 ev=Read by car setup 0x185470 and car preview 0x2B0D20
physics | Chassis | model_offset | 3D model offset | Body | in | Shifts the car's 3D model relative to its physics origin (x, y, z). | conf=medium used=1 ev=Read by car setup 0x185470
physics | Chassis | grip | Tyre grip multiplier | Handling | x | Scales the grip of every tyre on the car. Above 1 sticks better. | conf=high used=1 ev=Read by car setup 0x185470; named in docs/SETU_CAR_SETUP.md as a way to change handling
physics | Chassis | slip | Tyre slip setting | Handling | | Optional per-car tyre slip setting; the car setup reads it with a default when absent. Exact formula not decoded. | conf=low used=1 ev=Read by car setup 0x185470
physics | Chassis | load_factor | Tyre load setting | Handling | | Optional per-car tyre load setting; the car setup reads it with a default when absent. Exact formula not decoded. | conf=low used=1 ev=Read by car setup 0x185470
physics | Chassis | brake_factor | Brake strength factor | Handling | x | Optional multiplier on braking, read with a default when absent. | conf=medium used=1 ev=Read by car setup 0x185470; named in docs/SETU_CAR_SETUP.md
physics | Chassis | spring_factor | Suspension stiffness factor | Handling | x | Scales spring stiffness. Higher is stiffer. | conf=high used=1 ev=Read by car setup 0x185470; named in docs/SETU_CAR_SETUP.md
physics | Chassis | air_drag | Air drag (override) | Aerodynamics | | Optional override of the car's air drag. Only one retail car sets it. | conf=low used=1 ev=Read by car setup 0x185470
physics | Chassis | drag | Air drag coefficient | Aerodynamics | | Drag from air resistance; higher lowers top speed. | conf=medium used=1 ev=Read by car setup 0x185470 (the key is also read for engine drag at 0x185018)
physics | Chassis | damage_tweak | Collision damage multiplier | Damage | x | Scales how much damage this car takes in a crash. | conf=medium used=1 ev=Read by the damage helper lookup 0x18AFE0
physics | Chassis | damage_scale | Damage scale | Damage | x | Not read by the game; crash damage uses damage_tweak and the bone weakness values. | conf=high used=0
physics | Chassis | (cp_height|cp_long) | Aero centre of pressure | Aerodynamics | in | Left over from the physics engine this game is built on. Nothing reads it. | conf=high used=0
physics | Chassis | (front_lift|rear_lift) | Aerodynamic lift | Aerodynamics | | Left over from the source physics engine. Nothing reads it. | conf=high used=0
physics | Chassis | (fspoiler_drag|fspoiler_lift|rspoiler_drag|rspoiler_lift|rspoiler_angle) | Spoiler aerodynamics | Aerodynamics | | Spoiler drag, lift or angle values from the source engine. Nothing reads them. | conf=high used=0
physics | Chassis | (frontal_area|lat_drag|vert_drag) | Aerodynamic area and drag | Aerodynamics | | Frontal area or sideways/vertical drag from the source engine. Nothing reads it. | conf=high used=0
physics | Chassis | fuel_capacity | Fuel tank size | Body | gal | Tank size in the source engine. The game does not read it, so fuel capacity cannot be changed here. | conf=high used=0
physics | Chassis | (min_extents|max_extents) | Body bounding box | Body | in | Box corners from the source engine. Nothing reads them; collision uses the car model. | conf=high used=0
physics | Chassis | wheel_lock | Steering lock | Handling | deg | Steering range limit from the source engine's setup sheets. Nothing reads it. | conf=high used=0
physics | Chassis | height | Body height | Body | in | Overall height. No reader was traced. | conf=low used=?
physics | Chassis | width | Body width | Body | in | Overall width. No reader was traced for this section. | conf=low used=?
physics | Chassis | head_pos | Driver eye position | Cockpit camera | in | Where the driver's-eye camera sits (x, y, z) relative to the car. | conf=medium used=1 ev=Read by car setup 0x185470
physics | Chassis | head_pitch | Driver eye tilt | Cockpit camera | deg | Downward or upward tilt of the driver's-eye camera. | conf=medium used=1 ev=Read by car setup 0x185470
physics | Chassis | lookback_head_pos | Look-back eye position | Cockpit camera | in | Camera position while the player looks backwards. | conf=medium used=1 ev=Read by car setup 0x185470
physics | Chassis | chase_camera_dist | Chase camera distance | Chase camera | | Distance of the behind-the-car camera. Only some cars set it; others use a default. | conf=medium used=1 ev=Read by car setup 0x185470
physics | Chassis | chase_camera_height | Chase camera height | Chase camera | | Height of the behind-the-car camera. Only some cars set it. | conf=medium used=1 ev=Read by car setup 0x185470
physics | Chassis | shifter_offset | Shifter lever position | Cockpit model | | Where the gear lever model sits in the cockpit. | conf=medium used=1 ev=Read by car model loader 0x21ABF8
physics | Chassis | shifter_throw | Shifter lever travel | Cockpit model | | How far the gear lever model moves between gears. | conf=medium used=1 ev=Read by car model loader 0x21ABF8
physics | Chassis | (speedometer|tachometer|fuel_gauge|oil_pressure_gauge|oil_temp_gauge|water_temp_gauge|volt_gauge) | Gauge needle pivot | Cockpit gauges | px | Pixel position where the needle of this dial is pinned on the cockpit art (x, y). | conf=medium used=1 ev=Read by cockpit gauge code 0x21BDF0
physics | Chassis | speedometer# | Speedometer calibration point | Cockpit gauges | mph, px | Needle tip pixel (x, y) at the given speed. The needle is interpolated between points. | conf=medium used=1 ev=Read by cockpit gauge code 0x21BDF0
physics | Chassis | tachometer# | Tachometer calibration point | Cockpit gauges | rpm, px | Needle tip pixel (x, y) at the given engine speed. | conf=medium used=1 ev=Read by cockpit gauge code 0x21BDF0
physics | Chassis | fuel_gauge# | Fuel gauge calibration point | Cockpit gauges | gal, px | Needle tip pixel (x, y) at the given fuel level. | conf=medium used=1 ev=Read by cockpit gauge code 0x21BDF0
physics | Chassis | oil_pressure_gauge# | Oil pressure gauge calibration point | Cockpit gauges | psi, px | Needle tip pixel (x, y) at the given oil pressure. | conf=medium used=1 ev=Read by cockpit gauge code 0x21BDF0
physics | Chassis | oil_temp_gauge# | Oil temperature gauge calibration point | Cockpit gauges | F, px | Needle tip pixel (x, y) at the given oil temperature. | conf=medium used=1 ev=Read by cockpit gauge code 0x21BDF0
physics | Chassis | water_temp_gauge# | Water temperature gauge calibration point | Cockpit gauges | F, px | Needle tip pixel (x, y) at the given coolant temperature. | conf=medium used=1 ev=Read by cockpit gauge code 0x21BDF0
physics | Chassis | volt_gauge# | Voltage gauge calibration point | Cockpit gauges | V, px | Needle tip pixel (x, y) at the given voltage. | conf=medium used=1 ev=Read by cockpit gauge code 0x21ABF8
physics | Chassis | speedo_slider | Speedometer needle shift | Cockpit gauges | | Link to the table that shifts the needle position with speed. | conf=medium used=1 ev=Read by cockpit gauge code 0x21BDF0
physics | speedo_slider | val# | Needle shift at a speed | Cockpit gauges | mph, px | Speed followed by the x and y pixel shift applied to the speedometer needle at that speed. | conf=low used=1 ev=Read by cockpit gauge code 0x21BDF0 (val1, val2); meaning of the shift inferred from the name
## ---- Engine
physics | Engine.* | power_max | Peak power | Engine | hp | Maximum engine power. Shown as "Power" in the part shop. | conf=high used=1 ev=Read by 0x185470 and 0x185018; shown in the part shop (0x2DEF98) as hp
physics | Engine.* | power_rpm | Peak power engine speed | Engine | rpm | Engine speed at which peak power is reached. | conf=medium used=1 ev=Read by 0x185470 and 0x185018
physics | Engine.* | torque_max | Peak torque | Engine | lb-ft | Maximum engine torque. | conf=high used=1 ev=Read by 0x185470 and 0x185018; torque unit lb-ft from the existing inspector
physics | Engine.* | torque_rpm | Peak torque engine speed | Engine | rpm | Engine speed at which peak torque is reached. Shown as "Peak Torque" in the part shop. | conf=high used=1 ev=Read by 0x185470; shown in the part shop (0x2DEF98) as rpm
physics | Engine.* | redline | Redline | Engine | rpm | Highest engine speed before the rev limiter. | conf=high used=1 ev=Read by car setup 0x185470; shown in the part shop as rpm
physics | Engine.* | idle_speed | Idle speed | Engine | rpm | Engine speed with the throttle released in neutral. | conf=medium used=1 ev=Read by 0x185018
physics | Engine.* | drag | Engine drag | Engine | | Engine braking when off the throttle. | conf=medium used=1 ev=Read by 0x185018
physics | Engine.* | engine_sound | Engine sound table | Sound | file | Name of the engine sound table (a .val in the RACE archive). | conf=high used=1 ev=Read by engine sound loader 0x23C400
physics | Engine.* | sound | Engine sound set | Sound | | Name of the engine sound set. | conf=low used=?
physics | Engine.* | idle_sound | Idle sound | Sound | file | Idle sample name from the source engine. Nothing reads it. | conf=high used=0
physics | Engine.* | (cold_idle_speed|displacement|engine_temp|fuel_consumption|inertia|oil_pressure) | Engine data (source engine) | Engine | | Value from the source physics engine. Nothing in the game reads it. | conf=high used=0
physics | Engine.* | mass | Engine mass | Engine | lb | No reader was traced for the engine's own mass. | conf=low used=?
## ---- Transmission
physics | Transmission | g# | Gear ratio | Gearbox | x | Gear ratio for this gear (the key name gives the gear number). Higher ratios accelerate harder and run out of speed sooner. | conf=high used=1 ev=Read as "g%d" up to num_gears at 0x186E54
physics | Transmission | num_gears | Number of gears | Gearbox | | How many forward gears the car has. | conf=high used=1 ev=Read at 0x186DC8
physics | Transmission | rear_end_ratio | Final drive ratio | Gearbox | x | Ratio of the axle gears; multiplies every gear. Defaults to 5.0 when absent. | conf=high used=1 ev=Read by car setup 0x185470 (default 5.0 at 0x1860E8); docs/SETU_CAR_SETUP.md
physics | Transmission | torque_balance | Drive torque to rear axle | Gearbox | 0 to 1 | Share of engine torque sent to the rear wheels: 0 front-drive, 1 rear-drive. Same convention as diff.val (FWD 0, AWD 0.6, standard 1). | conf=medium used=1 ev=Read by 0x185470 and 0x2A76A8
physics | Transmission | (clutch_rpm|clutch_torque|cstiff|fstiff|rstiff|inertia|ratio_factor|ratio|drag) | Drivetrain data (source engine) | Gearbox | | Value from the source physics engine. Nothing in the game reads it. | conf=high used=0
## ---- Tyres and axles
physics | (Front|Rear|FrontDirt|RearDirt) | magic_b | Tyre curve stiffness (B) | Tyre model | | Stiffness factor of the tyre force curve ("magic formula"). | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8 as magic_b to magic_e
physics | (Front|Rear|FrontDirt|RearDirt) | magic_c | Tyre curve shape (C) | Tyre model | | Shape factor of the tyre force curve ("magic formula"). | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8
physics | (Front|Rear|FrontDirt|RearDirt) | magic_d | Tyre curve peak (D) | Tyre model | | Peak factor of the tyre force curve ("magic formula"). | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8
physics | (Front|Rear|FrontDirt|RearDirt) | magic_e | Tyre curve curvature (E) | Tyre model | | Curvature factor of the tyre force curve ("magic formula"). | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8
physics | (Front|Rear|FrontDirt|RearDirt) | frict_load# | Friction curve load point | Tyre model | lb | Tyre load at which the matching friction value applies. Two points define how grip changes with load. | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8 (frict_load1/2 with frict_k1/2)
physics | (Front|Rear|FrontDirt|RearDirt) | frict_k# | Friction at load point | Tyre model | | Grip coefficient at the matching load point. | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8
physics | (Front|Rear|FrontDirt|RearDirt) | stiff_load# | Cornering stiffness load point | Tyre model | lb | Tyre load at which the matching cornering stiffness applies. | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8
physics | (Front|Rear|FrontDirt|RearDirt) | stiff_c# | Cornering stiffness at load point | Tyre model | | Cornering stiffness of the tyre at the matching load point. | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8
physics | (Front|Rear) | camber_stiff | Camber stiffness | Tyre model | | Sideways force gained per degree of camber. | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8
physics | (Front|Rear) | camber_peak | Camber peak angle | Tyre model | deg | Camber angle at which the camber effect peaks. | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8
physics | (Front|Rear) | camber_grip | Camber grip gain | Tyre model | | Grip change caused by camber. | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8
physics | (Front|Rear) | long_stiff | Braking and traction stiffness | Tyre model | | Stiffness of the tyre in the driving direction. | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8
physics | (Front|Rear) | long_frict | Braking and traction friction | Tyre model | | Friction of the tyre in the driving direction. | conf=medium used=1 ev=Read by tyre setup 0x1B1BA8
physics | (Front|Rear) | ref_width | Reference tyre width | Tyre model | mm | Tyre width the grip values were tuned for. | conf=medium used=1 ev=Read at 0x1B2238
physics | (Front|Rear) | tire_diameter | Tyre diameter | Wheels | in | Outer diameter of the tyre. | conf=high used=1 ev=Read by car setup 0x185470, car preview 0x2B0D20 and wheel loader 0x21F780
physics | (Front|Rear) | wheel_class | Wheel model class | Wheels | class | Which wheel model set the car uses: com, mid, mus, bus or busrear (the .val of that name in the RACE archive). | conf=high used=1 ev=Read by 0x2B0D20 and 0x21C3C8; classes match bus.val, com.val, mid.val, mus.val
physics | (Front|Rear) | ride | Ride height range | Suspension | in | Lower and upper ride height values. Read by the car preview. | conf=low used=1 ev=Read by car preview 0x2B0D20
physics | (Front|Rear) | model | Tyre type | Wheels | | Tyre model name. Reader not traced. | conf=low used=?
physics | (Front|Rear) | (width|diameter|friction|mass|moment|resistance) | Tyre or wheel property | Wheels | | Wheel property. No reader was traced for this section. | conf=low used=?
physics | (Front|Rear|FrontDirt|RearDirt) | (anti_pitch|aspect|brake_force|bump|bump_camber|bump_toe|camber|caster|compliance_camber|compliance_steer|damage_impulse|rate|rebound|removal_impulse|rubber_height|rubber_rate|solid_axle|sway|toe|torque_steer|rc_height|trackbar_split) | Suspension and setup value (source engine) | Suspension | | Value from the source engine's setup system. Nothing in the game reads it (see docs/SETU_CAR_SETUP.md). | conf=high used=0
""")


# ======================================================================= shadow.val
family("shadow", "Track shadows",
       "Dark patches painted onto the track surface: soft ellipses (trees, buildings, bleachers), arcs and "
       "parallelograms. Each patch lowers the light level of the road inside its outline. Loaded with the track.")
MEMBER_TITLES["shadow.val"] = ("Track shadows", FAMILIES["shadow"]["summary"])
sections("""
shadow | ellipse | Shadow ellipse {n} | Round or oval shadow. Its light level goes from the centre value to the edge value (nested table). Position, size and direction below are in metres on the track plane.
shadow | parallelogram | Shadow parallelogram {n} | Straight-edged shadow shaped like a slanted rectangle, with one light level.
shadow | arc | Shadow arc {n} | Curved band between an inner and an outer ellipse, cut off by two lines.
shadow | global | Whole-track shadow setting | Setting that applies to the whole shadow table.
shadow | brightness | Ellipse light levels | Light level at the centre and at the edge of the parent ellipse.
shadow | inner | Arc inner ellipse | Radii of the ellipse that forms the inner edge of the arc.
shadow | outer | Arc outer ellipse | Radii of the ellipse that forms the outer edge of the arc.
""")
add("""
shadow | ellipse | brightness | Light level (centre and edge) | Shading | | Link to the table with the centre and edge light levels. Old files that put a single number here are rejected ("unsupported old-school ellipse format"). | conf=high used=1 ev=Ellipse loader 0x32AD98 reads brightness as a table
shadow | brightness | center | Light level at the centre | Shading | 0 to 1+ | How bright the road is at the middle of the shadow. 1 leaves the road unshadowed; lower is darker. | conf=medium used=1 ev=Ellipse loader 0x32AD98 (center); direction of the scale from the values and the debug text "forcing brightness"
shadow | brightness | perimeter | Light level at the edge | Shading | 0 to 1+ | How bright the road is at the rim of the shadow. Set close to 1 for a soft edge. | conf=medium used=1 ev=Ellipse loader 0x32AD98 (perimeter)
shadow | (ellipse|arc|parallelogram) | origin | Position | Placement | m | Centre of the shadow on the track (x, z) in metres in the track's own coordinates. | conf=medium used=1 ev=Read by 0x32AD98, 0x32B030, 0x32B5F0; unit from the world scale used by event props
shadow | (ellipse|arc|parallelogram) | bx | Direction | Placement | | Direction of the shadow's long axis as a unit vector (x, z). 1.00, 0.00 points along x. | conf=medium used=1 ev=Read by 0x32AD98, 0x32B030, 0x32B5F0 (bx); direction meaning from the unit-length pairs in retail files
shadow | ellipse | a | Half-length | Size | m | Radius of the ellipse along its direction. | conf=medium used=1 ev=Ellipse loader 0x32AD98; axis assignment from the name
shadow | ellipse | b | Half-width | Size | m | Radius of the ellipse across its direction. | conf=medium used=1 ev=Ellipse loader 0x32AD98; axis assignment from the name
shadow | (inner|outer) | a | Half-length | Size | m | Radius along the arc's direction. | conf=medium used=1 ev=Arc loader 0x32B030
shadow | (inner|outer) | b | Half-width | Size | m | Radius across the arc's direction. | conf=medium used=1 ev=Arc loader 0x32B030
shadow | arc | (inner|outer) | Arc ellipse | Size | | Link to the nested table with the ellipse radii. An arc needs both ("arcs require two ellipses"). | conf=high used=1 ev=Arc loader 0x32B030
shadow | arc | line0 | First cut line | Shape | | Direction (x, z) of the line that ends one side of the arc. | conf=medium used=1 ev=Arc loader 0x32B030
shadow | arc | line1 | Second cut line | Shape | | Direction (x, z) of the line that ends the other side of the arc. | conf=medium used=1 ev=Arc loader 0x32B030
shadow | (arc|parallelogram) | brightness | Light level | Shading | 0 to 1+ | How bright the road is inside the shadow. 1 leaves the road unshadowed; lower is darker. Arcs report "invalid arc brightness" for bad values. | conf=medium used=1 ev=Read by 0x32B030 and 0x32B5F0
shadow | parallelogram | length | Side lengths | Size | m | Lengths of the two sides (along, across) in metres. | conf=medium used=1 ev=Read by 0x32B5F0; which side is which from the name
shadow | parallelogram | tilt | Slant | Shape | | How far the shape is sheared from a rectangle. 0 is a rectangle. | conf=low used=1 ev=Read by 0x32B5F0; meaning inferred from the name and values
shadow | global | brightness | Baseline light level | Shading | | Brightness read once before the areas load and kept as the table-wide setting. The per-track option trkshad_force_brightness overrides all areas. | conf=low used=1 ev=Read at 0x32AC00 (section global); how it is applied is not decoded
""")
add("""
lod | * | model | 3D model for this level | Models | | Name of the model set used for this detail level (resolved as <car>.car/<name><n>.mod). | conf=medium used=1 ev=Car model loader 0x21ABF8
lod | low | base_tex | Body texture | Textures | file | Texture used on the low-detail body. | conf=medium used=1 ev=Car model loader 0x21ABF8
lod | low | base_texa | Alternate body texture | Textures | file | Second texture for cars with two paint layouts (bus). | conf=low used=1 ev=Car model loader 0x21ABF8
lod | (low|shadow) | lod# | Switch distance for detail level | Detail levels | m, on/off | Distance at which this detail level applies, followed by y or n. The line is parsed as a number and a word ("Lod table: bad value"). | conf=medium used=1 ev=Parsed with "%f %s" by 0x21ABF8; the y/n meaning is not decoded
lod | detail# | use | Model set used | Detail levels | | Which model set this detail setting uses. | conf=medium used=1 ev=Car model loader 0x21ABF8
lod | detail# | min | Lowest detail level | Detail levels | | Lowest detail level allowed at this setting. | conf=low used=1 ev=Car model loader 0x21ABF8; effect inferred from the name
lod | detail# | scale | Distance scale | Detail levels | x | Scales the distances at which detail levels switch. | conf=low used=1 ev=Car model loader 0x21ABF8; effect inferred from the name
lod | detail# | mirror | Mirror detail level | Detail levels | | Detail level used when the car is seen in a mirror. | conf=low used=1 ev=Car model loader 0x21ABF8; effect inferred from the name
lod | detail# | shadow | Shadow detail level | Detail levels | | Detail level used for the car's shadow. | conf=low used=1 ev=Car model loader 0x21ABF8; effect inferred from the name
""")
sections("""
lod | low | Car body detail levels | Distances at which the car switches between its detailed models, and the body texture.
lod | shadow | Car shadow detail levels | Distances at which the car's shadow model switches.
lod | detail# | Detail setting {n} | Chooses how much detail the car keeps under the numbered graphics-detail setting.
""")


# ======================================================================= batch 2: world, event, car support tables
family("event_props", "Event layout",
       "Everything an event needs placed in the world: props with their positions, play-area and out-of-bounds outlines, "
       "checkpoints, start lines and event cameras. One file per event (c_<event>.va0). Positions are in metres.")
family("bones", "Car damage skeleton", "The bones a car's body is built from, how far each may bend and which bones are linked, for crash deformation.")
family("car_efx", "Car effects", "Placement of per-car effects: exhaust and radiator smoke, siren and flag mounts, victory banner.")
family("car_lights", "Car lights", "Flashing and steady lights on emergency and utility vehicles.")
family("camera", "Track cameras", "Cameras used by an event: close-up and TV cameras, named cameras and the regions that switch between them.")
family("track_rules", "Track rules and AI", "Per-track settings read at race start: dirt or tarmac, restrictor plate, tyre wear, qualifying laps and AI behaviour. "
       "Many values here (arcade grip, pit speed, yellow-flag rules) are never looked up by the game.")
family("interlude", "Event intro and outro sequences", "Scripted camera moves and sounds that play before and after each kind of event.")
family("obstacles", "Props and obstacles", "Physical properties of every prop type that can appear in an event: mass, friction, size, surface and damage.")
family("ai_lines", "AI driving lines", "Rules for computer drivers on each track layout: look-ahead, exit paths and where they may leave the racing line.")
family("track_particles", "Track particle effects", "Smoke, sand, gravel and dust that tyres and cars throw up.")
family("default_props", "Default props", "Props a track places in every event of that track.")
family("collision_planes", "Collision shapes", "Flat collision planes making up a simple solid prop.")
family("track_fireworks", "Victory fireworks", "Which firework effect plays where when a race ends.")
family("firework_effects", "Firework effects", "Particle systems, smoke and flipbooks used by fireworks and victory effects.")

MEMBER_TITLES["shadow.val"] = ("Track shadows", FAMILIES["shadow"]["summary"])
sections("""
event_props | objects | Props | One entry per placed prop. Each links to a nested table with the prop type and position.
event_props | object | Prop {n}: {item} | A single placed prop: its type and where it stands.
event_props | ewob_cam | Event camera {n} | A camera position for this event.
event_props | bound(_list|_#)? | Event boundary {n} | Outline of the area where the event takes place, as map points (x, z) in metres.
event_props | (centerline|start_finish|safe_bound|home_endzone|away_endzone|play_area|ringout|malt_bound|home_bound|next_eve_bound|junkyard_bound|paintshop_bound|placer_help) | {name} outline | Outline polygon used by the event rules, as map points (x, z) in metres.
event_props | (checkpoints|points) | Checkpoints | Ordered checkpoints the player must pass, as map points (x, z) in metres.
event_props | home_positions | Starting positions | Where cars and the player start.
bones | bone_# | Bone {n} | One deformable piece of the car body.
bones | link | Bone link {n} | A joint between two bones.
bones | wheel_(lf|rf) | Wheel bone {name} | Which bone carries this wheel.
bones | bonesurf_# | Body surface group {n} | Range of bones belonging to one body surface.
bones | bounds | Body bounds | Bounding box corners for each bone group.
car_lights | info | Light set | Texture and blink style for the lights.
car_lights | seq | Blink pattern {n} | A repeating on/off timing pattern.
car_lights | light | Light {n} | One light on the vehicle.
interlude | default | Default sequence | Used for events with no sequence of their own.
interlude | action | Step {n} | One step of the sequence: camera move, sound or message.
obstacles | dyno.* | Prop type {item} | Physical properties of a prop type.
obstacles | .*_phys | Prop physics {name} | Physical properties shared by a group of props.
ai_lines | oval|fig#|oval-sc|oval-bw|rroval | Track layout {name} | AI settings for this layout.
ai_lines | exceptions? | Line exceptions {n} | Places where AI drivers may leave their line.
ai_lines | lookahead | Look-ahead {n} | How far ahead the AI looks.
""")
add("""
event_props | object | dyno | Prop type | Prop | | Name of the prop to place. Matches a section of obstacle.val, which defines its model, weight and surface. | conf=high used=1 ev=docs/WORLD_FORMAT.md event props; prop table loader 0x22C1E8
event_props | object | unfloat_frame64 | Position and rotation | Prop | | The prop's position and orientation packed into 64 characters (12 numbers). Decoded here to metres. | conf=high used=1 ev=Unpacked at 0x22C1E8 with the alphabet at 0x3CCFC0; docs/WORLD_FORMAT.md
event_props | ewob_cam | frame64 | Camera position and rotation | Camera | | The camera's position and orientation packed into 64 characters. Decoded here to metres. | conf=high used=1 ev=Unpacked at 0x22C1E8
event_props | object | sound | Ambient sound | Prop | file | Sound that plays from this prop (name of a .sf_ file). | conf=medium used=1 ev=Read by the prop loader; name matches sound files
event_props | object | volume | Ambient sound volume | Prop | 0 to 1 | Volume of the prop's sound. | conf=medium used=1 ev=Read by the prop loader
event_props | object | live_radius | Active radius | Prop | m | Distance within which the prop is active. Name only. | conf=low used=?
event_props | objects | autogen | Auto-generated props | Prop | | Marks props the editor generated. Meaning of 0, 1 and 2 not decoded. | conf=low used=?
event_props | (bound.*|centerline|start_finish|safe_bound|home_endzone|away_endzone|play_area|ringout|malt_bound|home_bound|next_eve_bound|junkyard_bound|paintshop_bound|placer_help) | (bound#|point#) | Outline point | Outline | m | One corner (x, z) of the outline, in metres on the map. Points are joined in order. | conf=medium used=1 ev=Event masters read the outline (e.g. 0x13F3F0 football/derby, 0x1523B0 point-to-point); unit from docs/WORLD_FORMAT.md
event_props | .* | winding_ok | Corner order checked | Outline | yes/no | Marks that the outline's corners were verified to run in the right direction. | conf=low used=?
event_props | (checkpoints|points) | point# | Checkpoint | Checkpoints | m | One checkpoint position (x, z) in metres. Checkpoints are passed in order. | conf=medium used=1 ev=Read as "point%d" by 0x1523B0 and 0x1608F8
event_props | home_positions | home_pos# | Start position | Start | m | Starting position (x, z) in metres. | conf=low used=?
event_props | home_positions | home_orient# | Start direction | Start | | Direction the car faces at this start. | conf=low used=?
bones | bone_# | travel_min | Lowest bend limit | Deformation | in | Smallest offset (x, y, z) this bone may move when the body is crushed. | conf=medium used=1 ev=Read by bone loader 0x2DC000
bones | bone_# | travel_max | Highest bend limit | Deformation | in | Largest offset (x, y, z) this bone may move when the body is crushed. | conf=medium used=1 ev=Read by bone loader 0x2DC000
bones | bone_# | link_center | Link centre | Deformation | in | Point (x, y, z) around which this bone's links act. | conf=medium used=1 ev=Read by bone loader 0x2DC000
bones | bone_# | weakness | Weakness | Deformation | x | How easily this bone gives way. Larger bends sooner. | conf=medium used=1 ev=Read by bone loader 0x2DC000; direction inferred from the name
bones | bone_# | dmg_dist | Damage distance | Deformation | | Distance used when spreading crash damage from this bone. | conf=low used=1 ev=Read by bone loader 0x2DC000
bones | bone_# | is_welded | Welded | Deformation | yes/no | A welded bone does not come loose. | conf=medium used=1 ev=Read by bone loader 0x2DC000
bones | link | (i|j) | Linked bone | Links | | Number of one of the two bones this link joins. | conf=medium used=1 ev=Read by bone loader 0x2DC000
bones | link | (linear|planar|pop) | Link stiffness setting | Links | | Setting of the link between two bones (linear, planar, pop). Exact formula not decoded. | conf=low used=1 ev=Read by bone loader 0x2DC000
bones | wheel_(lf|rf) | bone_id | Wheel bone | Wheels | | Bone number that carries this wheel. | conf=medium used=1 ev=Read by bone loader 0x2DC000
bones | wheel_(lf|rf) | (x|y|z)_scale | Wheel scale | Wheels | x | Scale applied to the wheel along this axis. | conf=medium used=1 ev=Read by bone loader 0x2DC000
bones | bounds | (min|max)_# | Body bound corner | Bounds | in | Box corner (x, y, z) of the body region. | conf=medium used=1 ev=Read as "min_%d" and "max_%d" by 0x2DC658
car_lights | info | texture | Light texture | Lights | file | Texture drawn for each light. | conf=high used=1 ev=Read by 0x306978
car_lights | info | type | Light type | Lights | | Blink style (e.g. flashing). | conf=medium used=1 ev=Read by 0x306978
car_lights | seq | name | Pattern name | Blink pattern | | Name that lights refer to. | conf=high used=1 ev=Read by 0x306978
car_lights | seq | t# | Blink time | Blink pattern | s | Time in seconds of one step of the pattern. | conf=medium used=1 ev=Read as "t%d" by 0x306978
car_lights | .*light | loc | Position | Light | m | Where the light sits on the car (x, y, z). | conf=medium used=1 ev=Read by 0x306978
car_lights | .*light | normal | Facing | Light | | Direction the light faces. | conf=medium used=1 ev=Read by 0x306978
car_lights | .*light | radius | Size | Light | m | Size of the light's glow. | conf=medium used=1 ev=Read by 0x306978
car_lights | .*light | color | Colour | Light | rgb 0 to 1 | Red, green and blue of the light. | conf=medium used=1 ev=Read by 0x306978
car_lights | light | seq | Blink pattern | Light | | Name of the pattern this light follows. | conf=high used=1 ev=Read by 0x306978
camera | namedcam|raceday|house | (location_x|location_y|location_z) | Camera position | Placement | m | One coordinate of the camera position. | conf=medium used=1 ev=Read by 0x1D0EE0
camera | namedcam|raceday|house | basis_.* | Camera rotation | Placement | | One element of the 3x3 rotation matrix that points the camera. | conf=medium used=1 ev=Read by 0x1D0EE0
camera | namedcam|raceday|house | fov | Field of view | Lens | rad | Width of the camera's view in radians. | conf=medium used=1 ev=Read by 0x1D0EE0; radian unit from values near 1
camera | namedcam | name | Camera name | Placement | | Name events use to select this camera. | conf=high used=1 ev=Read by 0x1D0EE0
camera | cam | (x|y|z) | Camera position | Placement | m | One coordinate of the camera position. | conf=medium used=1 ev=Read by TV camera loader 0x1CD738
camera | cam | (x|y|z)_a | Alternate camera position | Placement | m | Position used for the alternate shot. | conf=medium used=1 ev=Read by 0x1CD738
camera | cam | new_fov | Field of view | Lens | deg | Camera field of view. | conf=medium used=1 ev=Read by 0x1CD738
camera | cam | new_fov_a | Alternate field of view | Lens | deg | Field of view for the alternate shot. | conf=medium used=1 ev=Read by 0x1CD738
camera | cam | (funky_fov|shakey|rumble) | Camera effect | Lens | yes/no | Turns a camera effect on or off (unusual lens, shake, controller rumble). | conf=medium used=1 ev=Read by 0x1CD738
camera | cam | type | Camera type | Placement | | Style of shot, e.g. CLOSEUP. | conf=medium used=1 ev=Read by 0x1CD738
camera | cam | ex_# | Extra camera setting | Placement | | Extra number that tunes the shot of this camera type. | conf=low used=1 ev=Read by 0x1CD738
camera | region_sphere | (location_x|location_y|location_z) | Region centre | Region | m | One coordinate of the sphere that triggers this camera. | conf=medium used=1 ev=Read by 0x1CE0C0
camera | region_sphere | radius | Region radius | Region | m | Size of the region. | conf=medium used=1 ev=Read by 0x1CE0C0
camera | region_sphere | group | Region group | Region | | Group this region belongs to. | conf=medium used=1 ev=Read by 0x1CE0C0
interlude | .* | duration | Length | Timing | s | How long the sequence runs, in seconds. | conf=medium used=1 ev=Read by interlude loader 0x1E7780; unit from the values (3 to 20)
interlude | action | accel | Accelerating move | Camera move | yes/no | The camera move speeds up instead of moving steadily. | conf=low used=1 ev=Read by 0x1E7780
interlude | action | av | Camera turn rate | Camera move | | Turn rate of the camera move (x, y, z). | conf=low used=1 ev=Read by 0x1E7780
interlude | action | (type|dt|order|msg|fov|origin|dir|duration|sample|speech|sound_type|overlap|immediate|speed|who|where|world_pt|offset|spacing|yaw|pt) | Sequence step setting | Sequence | | Setting of this step. Read by the interlude loader; use of the value not decoded. | conf=low used=1 ev=Read by 0x1E7780
obstacles | .* | mass | Prop weight | Physics | | Weight of the prop. Heavier props are pushed less. | conf=medium used=1 ev=Read by prop loader 0x22F1B0
obstacles | .* | (mx|my|mz) | Rotation inertia | Physics | | Resistance to rotating about one axis. | conf=medium used=1 ev=Read by prop loader 0x22F1B0
obstacles | .* | friction | Surface friction | Physics | | Grip between the prop and the ground. | conf=medium used=1 ev=Read by prop loader 0x22F1B0
obstacles | .* | surf_type | Surface material | Physics | | Material (metal, rubber, plastic, wall) used for hit sounds and sparks. Invalid names are rejected. | conf=high used=1 ev=Read by 0x22F1B0 ("invalid surface type")
obstacles | .* | hit | Hit response | Physics | | Bounce/impact response when something hits the prop. | conf=low used=1 ev=Read by 0x22F1B0
obstacles | .* | (rad|bottom_rad|top_rad|inner_rad|ring_rad) | Prop radius | Size | m | Radius of the prop's collision shape. | conf=medium used=1 ev=Read by 0x22F1B0
obstacles | .* | (wid|len|dx|dy|dz) | Prop size | Size | m | Width, length or half-extent of the collision shape. | conf=medium used=1 ev=Read by 0x22F1B0
obstacles | .* | type | Collision shape | Physics | | Shape class of the prop (barrel, box, animation...). | conf=high used=1 ev=Read by 0x22F1B0 ("Unknown obj type")
obstacles | .* | model | 3D model | Appearance | file | Model file drawn for the prop. | conf=high used=1 ev=Read by 0x22F1B0 and 0x22C9C8
obstacles | .* | name | Prop name | Appearance | | Name event files use to place the prop (the "dyno" value). | conf=medium used=1 ev=Read by 0x22C9C8
obstacles | .* | drop_height | Drop height | Physics | m | Height from which the prop settles onto the ground at spawn. | conf=medium used=1 ev=Read by 0x22C9C8
obstacles | .* | flammable | Can burn | Damage | yes/no | The prop can catch fire. | conf=high used=1 ev=Read by 0x22F1B0
obstacles | .* | fire_flags | Fire behaviour flags | Damage | | Bit flags controlling burning. | conf=low used=1 ev=Read by 0x22F1B0
obstacles | .* | palette_rad | Palette radius | Size | m | Radius used when the prop is chosen from the event editor palette. | conf=low used=?
ai_lines | .*exception.* | on_lon | Exception starts | Rules | m | Distance along the track where the exception begins. | conf=medium used=1 ev=Read by AI exception loader 0x15DDE0
ai_lines | .*exception.* | off_lon | Exception ends | Rules | m | Distance along the track where the exception ends. | conf=medium used=1 ev=Read by 0x15DDE0
ai_lines | .*exception.* | on_t | Blend-in time | Rules | s | Time to blend into the exception. | conf=low used=1 ev=Read by 0x15DDE0
ai_lines | .*exception.* | width | Allowed width | Rules | | Sideways room the AI has in the exception. | conf=medium used=1 ev=Read by 0x15DDE0
ai_lines | .*exception.* | lookahead_f | Look-ahead factor | Rules | x | Scales how far the AI looks ahead in the exception. | conf=medium used=1 ev=Read by 0x15DDE0
track_fireworks | source | effect | Firework effect | Fireworks | | Name of the effect to play. | conf=high used=1 ev=Read by 0x318980
track_fireworks | source | loc | Position | Fireworks | m | Where the effect starts. | conf=high used=1 ev=Read by 0x318980
track_fireworks | source | dir | Direction | Fireworks | | Direction the effect fires. | conf=high used=1 ev=Read by 0x318980
track_fireworks | source | trigger | Trigger | Fireworks | | Moment that starts the effect (e.g. checkered flag). | conf=high used=1 ev=Read by 0x318980
track_fireworks | source | delay | Delay | Fireworks | s | Wait before the effect starts. | conf=high used=1 ev=Read by 0x318980
firework_effects | .* | (max_count|max_life|gravity|drag|texture|radius|alpha_ramp|alpha_constant|random_spread) | Particle setting | Particles | | Particle setting read by the firework particle loader. | conf=medium used=1 ev=Read by 0x3175A0
firework_effects | smoke | (spew_rate|max_life|start_radius|end_radius|alpha|brightness|speed) | Smoke setting | Smoke | | Smoke puff setting read by the smoke loader. | conf=medium used=1 ev=Read by 0x319710
collision_planes | extents | (min|max) | Bounding box corner | Shape | m | Corner of the box around the shape. | conf=medium used=1 ev=Read by 0x19F860
collision_planes | plane | n | Plane normal | Shape | | Direction the plane faces. | conf=medium used=1 ev=Read by 0x19F860 (plane)
collision_planes | plane | k | Plane offset | Shape | m | Distance of the plane from the origin. | conf=medium used=1 ev=Read by 0x19F860 (plane)
""")


# ======================================================================= batch 3: career, part shop, events, tracks
# Sources: docs/CAREER_UNLOCKS.md, docs/CAREER_UPGRADES_DECOMPILATION.md (CatalogPart, CarTuningBlock), the
# executable's own error strings, and the key->function cross-reference in valt_schema_data.json.
family("career", "Career data", "Tables the Career mode reads: dealer stock, car classes, ratings, challenges, sponsors, news and awards.")
family("catalog", "Part shop", "Every upgrade part sold in Career: name, price, which cars it fits and how much it changes the car. "
       "Values are added to the car's tuning multipliers, which start at 1.0.")
family("eves", "Career events (Eves)", "The races and contests of each town: event type, laps, field size, opponents' skill, prize money "
       "and which cars may enter.")
family("drivers", "AI drivers", "The computer-controlled drivers of a league: name, number, car, colour and skill rank.")
family("parts", "Physics part library", "Generic parts (chassis, engines, tyres, suspension) from the physics engine the game is built on. "
       "No code that opens these files by name was found; each car's own physics.val is what sets its handling.")
family("track_particles", "Track effects", "Animated scenery effects of a track: dust, smoke, water, flags and spinning parts (windmills, helicopter blades).")
family("engine_sound", "Engine sound", "How an engine's sound samples blend with engine speed and throttle.")
family("wheel_models", "Wheel models", "Wheel and tyre model set for one wheel class, with motion-blur textures used at speed.")
family("effects", "Explosions and flares", "Particle and flipbook effects for explosions, flares and wobble effects.")
family("default_props", "Track-wide props", "Props and ambient sounds a track places in every event.")
MEMBER_TITLES.update({
    "buycar.val": ("Car dealer stock", "Every car the Career dealer sells ([car]: chassis, price, description) and every paint/number variant ([vehicle]). "
                   "A [vehicle] needs a matching [car] or the game stops with \"No matching chassis to %s from buy list\"."),
    "carclass.val": ("Car classes", "Puts every chassis in a class (Compact, Midsize, Muscle, Special). The class decides which upgrade parts fit "
                     "and which events the car may enter. Lookup is first match, so a chassis belongs to one class only."),
    "car_stats.val": ("Car ratings", "Speed, handling and durability bars shown at the dealer for each chassis."),
    "catalog.val": ("Part shop", FAMILIES["catalog"]["summary"]),
    "tracks.val": ("Track names", "Short and long display names of every track, and its legal notice."),
    "beat_times.val": ("Target lap times", "Lap times per track and league used by Career time targets."),
    "regions.val": ("Paint shop panels", "For every car, the rectangle of the paint texture that each body panel (hood, roof, doors, fenders, "
                    "deck lid) occupies, so the paint shop can colour or decal one panel."),
    "challenges.val": ("Road challenges", "Challengers who race you for money on the road: track, bet, laps and skill."),
    "home_challenges.val": ("Home town challenges", "Challenges started from your home town, with the reputation they need."),
    "time_challenge.val": ("Time challenges", "Beat-the-clock challenges: bet, gate and time limit."),
    "catchup.val": ("Catch-up help", "How much the computer drivers slow down or speed up to keep the race close, per difficulty."),
    "personnel.val": ("Crew members", "Crew chiefs, engine builders and chassis builders you can hire: price, skill and bonus."),
    "sponsors.val": ("Sponsors", "Sponsor offers: letter text, mission and reward."),
    "invite.val": ("Race invitations", "Invitation letters to special races: date, league, laps and purse."),
    "news.val": ("News items", "Newspaper headlines and what triggers them."),
    "feats.val": ("Feats and prizes", "Feats the player can achieve and the prizes they unlock."),
    "setups.val": ("Setup sheets for sale", "Car setup sheets sold in Career (.gar files). The setup sheets themselves have no effect on handling."),
    "event_desc.val": ("Event descriptions", "The description text shown for each event type."),
    "usenglish_units.val": ("Display units", "Names and conversion factors for the units shown on screen (speed, distance, mass, volume)."),
    "rollcall.val": ("Arcade car list", "Car classes and the cars in each, as listed on the Arcade car select screen."),
    "unlock.val": ("Unlock schedule", "Points needed for each unlock."),
    "award_schemes.val": ("Prize money schemes", "How a purse is split between finishing places."),
    "nicknames.val": ("Driver nicknames", "Nicknames the announcer can call the player, with their speech sample."),
    "vidrank.val": ("Unlockable videos", "Videos unlocked by rank."),
    "champion.val": ("Champion pages", "Pages of the champions screen."),
    "gauntlet.val": ("Gauntlet blockades", "Road blocks and their waypoints for the Gauntlet event."),
    "infotext.val": ("Map info points", "Places in the home town that show a message and camera when you drive near."),
    "awards.val": ("Awards", "Award names and descriptions."),
    "titles.val": ("Titles", "Titles the player can earn."),
})
sections("""
career | car | Dealer car {n}: {item} | One car model the dealer sells, with its price and description.
career | vehicle | Dealer variant {n} | One numbered paint variant of a dealer car (vehicle number, colour, chassis).
career | settings | Dealer settings | Prices for the whole dealer.
career | chassis | Class members | Chassis that belong to this class.
career | (Compact|Midsize|Muscle|Special|Clown) | Class: {name} | One car class. Its member list decides which upgrade parts fit and which events a car may enter.
career | class | Car class {n}: {item} | One class: its name and a link to its members.
career | carstats | Rating {n}: {item} | Dealer rating bars for one chassis.
career | challenge | Challenge {n} | One challenge.
career | ai | Challenger AI | Behaviour of the challenger's car.
catalog | (aero|engine|exhaust|suspension|car|engine_builder|chassis_builder) | {name} part {n}: {item} | One part in the shop.
eves | contest | Event {n}: {item} | One race or contest of the Eve.
eves | eve | Eve {n}: {item} | One Eve: its name, track, date and events.
eves | ai | Opponent AI settings | Starting and flag behaviour of the computer drivers in this event.
eves | chassis_allowed | Allowed cars | Chassis that may enter this event.
eves | contest_defaults | Event defaults | Values used by every event of the file that does not set its own.
drivers | Driver | Driver {n}: {item} | One computer driver.
drivers | Info | League settings | Settings for the whole league.
track_rules | AI | Computer drivers | Racing behaviour of the AI on this track: look-ahead, pit lane and restart rules.
track_rules | Physics | Track physics | Surface and car settings that apply on this track.
track_rules | Skill | AI difficulty | Speed and consistency of AI drivers per difficulty.
track_rules | AI(Easy|Medium|Hard)Sim(Dirt)? | AI difficulty ({name}) | Speed and consistency of the AI for one simulation difficulty.
track_rules | Info | Track info | General track values.
""")
add("""
## ---- dealer (buycar.val)
career | car | chassis | Chassis | Dealer | | Car model (the DATA/CAR file name) sold by this entry. | conf=high used=1 ev=docs/CAREER_UNLOCKS.md; "No matching chassis to %s from buy list"
career | car | price | Price | Dealer | $ | Dealer price before the price multiplier. | conf=high used=1 ev=docs/CAREER_UNLOCKS.md (wealth tiering 0x2E35F0)
career | car | desc | Description | Dealer | | Text shown at the dealer. A backslash starts a new line. | conf=high used=1 ev=docs/MOD_CAPABILITY_ACCEPTANCE.md (desc written and shown)
career | settings | car_price_multiplier | Price multiplier | Dealer | x | Multiplies every dealer price. | conf=medium used=1 ev=Read by the dealer loader; effect from the name
career | vehicle | chassis | Chassis | Dealer | | Chassis this variant belongs to. Must match a [car] entry. | conf=high used=1 ev=docs/CAREER_UNLOCKS.md
career | vehicle | number | Vehicle number | Dealer | | Number of the paint and dealer art (CT.RES <n>.tex, UICAR.RES uicar_<n>). | conf=high used=1 ev=docs/CAREER_UNLOCKS.md
career | vehicle | body_color | Body colour | Dealer | rgb 0 to 255 | Body colour (red, green, blue) of this variant. | conf=medium used=1 ev=Read by the dealer loader
career | vehicle | group_# | Variant group | Dealer | | Group number of the variant. Meaning not decoded. | conf=low used=1
## ---- car classes (carclass.val)
career | class | class_name | Class name | Class | | Name of the class (Compact, Midsize, Muscle, Special). | conf=high used=1 ev=docs/CAREER_UNLOCKS.md (class lookup 0x164218)
career | class | chassis | Members | Class | | Link to the list of chassis in this class. | conf=high used=1 ev=docs/CAREER_UNLOCKS.md
career | chassis | [A-Z].* | Member chassis | Class | | This chassis belongs to the class. The game walks the list, so the name itself is the data; the value is empty. | conf=high used=1 ev=Class lookup 0x164218 walks every entry (docs/CAREER_UNLOCKS.md)
career | chassis | (hurricane|arrow) | Member chassis | Class | | This chassis belongs to the class. | conf=high used=1 ev=Class lookup 0x164218
## ---- ratings (car_stats.val)
career | carstats | chassis | Chassis | Ratings | | Chassis these ratings belong to. | conf=high used=1 ev=CarStatsEntry table 0x439B20 (docs/CAREER_UPGRADES_DECOMPILATION.md)
career | carstats | speed | Speed rating | Ratings | bars | Speed bar shown at the dealer. | conf=high used=1 ev=CarStatsEntry (name, speed, handling, durability)
career | carstats | handling | Handling rating | Ratings | bars | Handling bar shown at the dealer. | conf=high used=1 ev=CarStatsEntry
career | carstats | durability | Durability rating | Ratings | bars | Durability bar shown at the dealer. | conf=high used=1 ev=CarStatsEntry
career | carstats | is_demo_car | Demo car | Ratings | yes/no | Car is available in the demo build. | conf=low used=1
## ---- personnel.val
career | (chassis|engine|crew_chief|crew) | name | Name | Crew | | Name of the crew member. | conf=medium used=1
career | (chassis|engine|crew_chief|crew) | price | Hiring price | Crew | $ | Cost to hire. | conf=medium used=1
career | (chassis|engine|crew_chief|crew) | skill | Skill | Crew | | How good the crew member is. Negative values are worse than the default crew. | conf=low used=1
career | (chassis|engine|crew_chief|crew) | bonus | Bonus | Crew | $ | Bonus paid. Name only. | conf=low used=1
career | (chassis|engine|crew_chief|crew) | default | Starting crew member | Crew | | Marks the crew member you start with. | conf=low used=1
career | (chassis|engine|crew_chief|crew) | (rel|short_course|speedway|super_speed|road_course) | Skill on a track type | Crew | | Skill on one kind of track. The game never looks this name up. | conf=medium used=0
## ---- challenges
career | challenge | bet | Bet | Challenge | $ | Money won or lost. | conf=high used=1 ev=Read by the challenge loader
career | challenge | track | Track | Challenge | | Track of the challenge. | conf=medium used=1
career | challenge | hometown | Home town | Challenge | | Town where the challenger appears. | conf=medium used=1
career | challenge | laps | Laps | Challenge | | Number of laps. | conf=medium used=1
career | challenge | event_type | Event type | Challenge | | Kind of event (normal_race, point to point...). | conf=medium used=1
career | challenge | (speed_factor) | Challenger speed | Challenge | x | Speed of the challenger relative to a normal AI driver. Must be set together with skill_range ("MaltShop: Challenge %d doesn't have both speed_factor and skill_range"). | conf=high used=1
career | challenge | (skill_range) | Challenger consistency | Challenge | | How much the challenger's pace varies. Set together with speed_factor. | conf=high used=1
career | challenge | leader_id | Challenger driver | Challenge | | Driver number of the challenger. | conf=medium used=1
career | challenge | max_cars | Number of cars | Challenge | | Cars in the race. | conf=medium used=1
career | challenge | rep_required | Reputation needed | Challenge | | Reputation the player needs before this challenge appears. | conf=medium used=1
career | challenge | (seconds) | Time limit | Challenge | s | Time to beat. | conf=medium used=1
career | challenge | gate | Gate | Challenge | | Gate the timed run goes through. | conf=low used=1
career | challenge | vengeance_idle_t | Wait before revenge | Challenge | s | Time the challenger waits before coming after you. Name only. | conf=low used=1
career | challenge | (external_table|line_basename) | Event layout and line | Challenge | | Event layout file (c_<name>) and AI line set used. | conf=medium used=1
career | ai | force_ytc_on | Always use the line | Challenge | yes/no | Forces the challenger onto its AI line. Name only. | conf=low used=1
## ---- catch-up help
career | .* | speed_factor | AI speed | Catch-up | x | Speed the AI is pushed towards when the gap grows. | conf=low used=1 ev=catchup.val loader ("Catchup")
career | .* | compare_car_pct | Compared car | Catch-up | 0 to 1 | Which car in the field the gap is measured to, as a share of the field. Name only. | conf=low used=1
career | .* | begin_t | Starts after | Catch-up | s | Time into the race before catch-up starts. | conf=low used=1
career | .* | effect_t | Ramp time | Catch-up | s | Time the catch-up takes to reach full effect. | conf=low used=1
## ---- tracks, times, regions, units
career | [A-Z][A-Za-z]* | name | Short name | Track | | Track name on menus. | conf=high used=1 ev=tracks.val loader
career | [A-Z][A-Za-z]* | longname | Full name | Track | | Full track name. | conf=high used=1
career | [A-Z][A-Za-z]* | legal_string | Legal notice | Track | | Trademark notice shown with the track. | conf=medium used=1
career | [A-Z][A-Za-z]*_(Dirt|Cup|Truck|Modified) | time# | Target lap time | Times | s | Lap time for the place given by the key number. | conf=low used=1 ev=beat_times.val; meaning of the number from the key
career | [A-Z][A-Za-z]* | (hood|roof|decklid|ldoor|rdoor|lfender|rfender|trunk|lquarter|rquarter) | Panel rectangle | Paint panels | px | Left, top, right and bottom of the panel on the car's paint texture. | conf=medium used=1 ev=regions.val (PAINTKIT.RES), read by the paint shop
career | [A-Z][A-Za-z]* | .*_rot | Panel rotation | Paint panels | quarter turns | Rotation of the decal on this panel. | conf=low used=1
career | [A-Z][A-Za-z]* | .*_mirror | Panel mirrored | Paint panels | yes/no | Decals are mirrored on this panel. | conf=low used=1
career | [A-Z][A-Za-z]* | alt_decklid_cam | Alternate deck lid camera | Paint panels | yes/no | Uses a different camera when painting the deck lid. | conf=low used=1
career | .* | conv | Conversion factor | Units | x | Multiplier from the game's internal unit to the unit shown. | conf=medium used=1 ev=usenglish_units.val (LOCALE.RES)
career | .* | name | Name | Text | | Name shown on screen. | conf=medium used=1
career | .* | (desc|text|msg|headline) | Text | Text | | Text shown on screen. A backslash starts a new line. | conf=medium used=1
career | .* | (price|purse|reward|bet|points) | Money or points | Rewards | | Amount of money or points. | conf=medium used=1
career | .* | (letter_address#|letter_contact|letter_text#) | Letter text | Letters | | Line of the letter shown with this offer. The game reads the lines as a numbered list. | conf=medium used=1 ev=Read as a numbered list
career | event_desc | .* | Event description | Text | | Description of this event type shown before the event. | conf=high used=1 ev=event_desc.val; one key per event type
""")
add("""
## ---- part shop (catalog.val). Values are fractions added to the car's tuning multipliers (1.0 = stock).
catalog | .* | name | Part name | Part | | Name in the part shop. | conf=high used=1 ev=CatalogPart (docs/CAREER_UPGRADES_DECOMPILATION.md)
catalog | .* | desc | Description | Part | | Shop text. A backslash starts a new line. | conf=medium used=1
catalog | .* | price | Price | Part | $ | Shop price. | conf=high used=1 ev=CatalogPart; parts resale 0.75 x price
catalog | .* | price_pct | Price as share of car | Part | 0 to 1 | Price as a share of the car's price. | conf=low used=1
catalog | .* | league | League | Fits | | League the part is sold in (Dirt, Modified, Cup, Truck). | conf=medium used=1
catalog | .* | car_class | Car class | Fits | | Class the part fits (compact, midsize, muscle). Every chassis has a class in carclass.val. | conf=high used=1 ev=docs/CAREER_UNLOCKS.md
catalog | .* | power | Power change | Engine | fraction | Change to peak power: 0.05 adds 5 %. | conf=medium used=1 ev=CarTuningBlock power_mult (f_00185018)
catalog | .* | power_rpm | Peak power engine speed change | Engine | fraction | Change to the rpm of peak power. | conf=medium used=1 ev=CarTuningBlock power_rpm_mult
catalog | .* | torque_rpm | Peak torque engine speed change | Engine | fraction | Change to the rpm of peak torque. | conf=medium used=1 ev=CarTuningBlock torque_rpm_mult
catalog | .* | redline | Redline change | Engine | fraction | Change to the redline. | conf=medium used=1 ev=CarTuningBlock redline_mult
catalog | .* | rpm | Engine speed change | Engine | fraction | Change to engine speed. All retail parts use 0. | conf=low used=1
catalog | .* | (f_grip|r_grip) | Grip change (front or rear) | Handling | fraction | Change to front (f_) or rear (r_) tyre grip. | conf=medium used=1 ev=CarTuningBlock f_grip_mult / r_grip_mult
catalog | .* | (f_downforce|r_downforce) | Downforce change (front or rear) | Handling | fraction | Change to front or rear downforce. | conf=low used=1
catalog | .* | (f_compliance|r_compliance) | Suspension compliance change | Handling | fraction | Change to suspension compliance. All retail parts use 0. | conf=low used=1
catalog | .* | drag | Drag change | Aerodynamics | fraction | Change to air drag; negative is less drag. | conf=medium used=1
catalog | .* | mass | Weight change | Body | fraction | Change to the car's weight; negative is lighter. | conf=medium used=1
catalog | .* | brake | Brake change | Handling | fraction | Change to braking force. | conf=medium used=1 ev=CarTuningBlock brake_mult
catalog | .* | damage | Toughness change | Damage | fraction | Change to damage resistance. | conf=medium used=1 ev=CarTuningBlock damage_mult
catalog | car | (chassis|body) | Base car | Part | | Chassis or body this starting car uses. | conf=low used=1
""")
add("""
## ---- Eves
eves | .* | event_type | Event type | Event | | Kind of event (normal_race, battle, jump_race, demo derby...). Each type has a description in event_desc.val. | conf=high used=1
eves | .* | laps | Laps | Event | | Number of laps (or rounds for arena events). | conf=high used=1
eves | .* | max_cars | Number of cars | Event | | Cars in the event, including the player. | conf=high used=1
eves | .* | external_table | Event layout | Event | | Event layout file (the c_<name>.va0 of the track) that places props, gates and outlines. | conf=high used=1 ev=docs/WORLD_FORMAT.md
eves | .* | line_basename | AI line set | Event | | Prefix of the AI racing-line files used (rr, opt2...). | conf=high used=1 ev=docs/ILIN_RACING_LINES.md
eves | .* | (speed_factor) | AI speed | Opponents | x | Speed of the computer drivers relative to normal. Must be set with skill_range ("Eve %s must have both speed_factor and skill_range or neither"). | conf=high used=1
eves | .* | (skill_range) | AI consistency | Opponents | | How much the computer drivers' pace varies. Set with speed_factor. | conf=high used=1
eves | .* | easy_skill_range | AI consistency (Easy) | Opponents | | Consistency on Easy. | conf=medium used=1
eves | .* | (easy|medium|hard)_(sfd|srd|speed_factor|skill_range) | Per-difficulty AI adjustment | Opponents | | Per-difficulty adjustment. The game never looks this name up, so it has no effect. | conf=medium used=0
eves | .* | rtt_speed_factor | Return-to-track AI speed | Opponents | x | AI speed while returning to the track. Name only. | conf=low used=1
eves | .* | purse | Prize money | Rewards | $ | Money for winning. | conf=high used=1
eves | .* | purse_multiplier | Prize multiplier | Rewards | x | Multiplies every prize. | conf=medium used=1
eves | .* | points_scheme | Points scheme | Rewards | | Points table. Never looked up by name. | conf=medium used=0
eves | .* | rtt_points | Points | Rewards | | Points awarded. Name only. | conf=low used=1
eves | .* | (league|League) | Racing league | Event | | League of the event (Dirt, Modified, Cup, Truck). | conf=medium used=1
eves | .* | track | Track | Event | | Track the event runs on. | conf=high used=1
eves | .* | (name|friendly_name) | Name | Event | | Name shown on menus. | conf=high used=1
eves | .* | display_name | Display name | Event | | Never looked up by name; friendly_name is shown instead. | conf=medium used=0
eves | .* | (date|time) | Date and time | Event | | Date or time of day of the Eve. | conf=medium used=1
eves | .* | hidden | Hidden until | Event | | When the event appears (e.g. until_career_done). | conf=medium used=1
eves | .* | (min_rank|max_rank) | Rank range | Event | | Player ranks for which the Eve is offered. | conf=medium used=1
eves | .* | (car_class) | Car class | Entry | | Class of car required. | conf=medium used=1
eves | chassis_allowed | .* | Allowed car | Entry | | This chassis may enter the event. The name itself is the data. | conf=medium used=1 ev=list walked by the Eve loader
eves | .* | interlude_name | Intro sequence | Event | | Intro/outro sequence from intrlude.val. | conf=high used=1
eves | .* | (terrain_mask|terrain_disable_mask|opt_terrain) | Track pieces shown | Event | bits | Which optional track pieces (ramps, gates, barriers) are present in this event. | conf=medium used=1 ev=docs/WORLD_FORMAT.md terrain masks
eves | .* | (start_point|attacker_start|gizmo_start|wrangler_start|arena_place_a|arena_place_b) | Start position | Event | m | Position (x, z) in metres. | conf=medium used=1
eves | .* | (damage_factor|player_damage_tweak) | Damage multiplier | Damage | x | Scales crash damage in this event (player_damage_tweak: only for the player). | conf=medium used=1
eves | .* | trkshad_force_brightness | Track shadow light level | Graphics | 0 to 1 | Overrides every track shadow's light level for this event (see shadow.val). | conf=high used=1 ev=0x32AC00
eves | .* | (flag_offset|greenflag_lon|rtt_greenflag_lon|offtrack_flag_offset|force_ytc_on|rounds|placer_dist|shortcut_check_lon|shortcut_side|whip_around_start_t|over_and_back|gate_race|home_eve|in_demo|mode|champion_page|display_order|player_driver|rtt_driver|pos|default_car|default_pos|soccer_steer|intro_sample) | Event setting | Event | | Setting read by the Eve loader. Exact effect not decoded. | conf=low
""")
add("""
## ---- AI drivers (dirt/cup/modified/truck.val)
drivers | Driver | name | Driver name | Driver | | Name shown in results. | conf=high used=1
drivers | Driver | number | Car number | Driver | | Number on the car. | conf=high used=1
drivers | Driver | chassis | Car | Driver | | Chassis the driver races. | conf=high used=1 ev=docs/CAREER_UNLOCKS.md (special vehicles appear as AI drivers in dirt.val)
drivers | Driver | body_color | Body colour | Driver | | Car colour. | conf=medium used=1
drivers | Driver | body_style | Body style | Driver | | Body style (e.g. chevy). | conf=medium used=1
drivers | Driver | rank | Skill rank | Driver | | Rank of the driver; lower ranks are better. | conf=low used=1
drivers | Driver | (road_course_rank|short_course_rank|speedway_rank|super_speedway_rank|race_image|ui_image) | Rank or picture per track type | Driver | | Never looked up by name. | conf=medium used=0
drivers | Driver | (group_#|sample) | Driver setting | Driver | | Group or speech sample of the driver. | conf=low used=1
drivers | Info | (can_pit|car_len|car_wid|lite_grip|max_pack_size|yellow_flags|is_trailer) | League setting | League | | League-wide setting (pit stops, car size, grip, pack size, yellow flags). | conf=low used=1
## ---- track rules (track.val)
track_rules | AI | min_lookahead | Shortest AI look-ahead | AI | s | Least time ahead the AI aims its steering. |
track_rules | AI | max_lookahead | Longest AI look-ahead | AI | s | Most time ahead the AI aims its steering. |
track_rules | AI | min_lookahead_m | Shortest AI look-ahead distance | AI | m | Least distance ahead the AI aims its steering. | conf=medium used=1 ev=Read by AI steering 0x163560
track_rules | AI | fuel_per_lap | Fuel per lap | AI | gal | Fuel a car uses each lap. | conf=medium used=1 ev=Read by 0x1F12F0
track_rules | AI | pit_side | Pit side | AI | -1 or 1 | Side of the track the pit lane is on. | conf=medium used=1 ev=Read by 0x138DA0
track_rules | AI | (rtt_finish_lon|rtt_pack_lon|rtt_reset_lon|rtt_greenflag_lon) | Rolling start distance | AI | m | Distance along the track for a rolling-start stage (pack up, green flag, reset, finish). | conf=low used=1 ev=Read by 0x1385B4, 0x137BB8, 0x172E5C
track_rules | AI | (checks|moveout_factor|push_lat_sens|yellow_moveout_f|pitlane_penalty#|track_type) | AI setting | AI | | Setting read by the AI. Exact effect not decoded. | conf=low used=1
track_rules | Physics | dirt_track | Dirt track | Physics | yes/no | Cars use their dirt tyre values (FrontDirt/RearDirt in physics.val). | conf=medium used=1 ev=Read by 0x2DC658
track_rules | Physics | restrictor_plate | Restrictor plate | Physics | yes/no | Cars use their restrictor-plate engine (EngineRestrictor in physics.val). | conf=medium used=1 ev=Read by 0x2DC658
track_rules | Physics | qualify_laps | Qualifying laps | Race | | Laps in qualifying. | conf=medium used=1 ev=Read by 0x1B4CB8
track_rules | Physics | wear_factor | Tyre wear | Physics | x | Scales tyre wear. | conf=medium used=1
track_rules | Physics | track_type | Track type | Race | | Road Course, Short Course, Speedway... | conf=medium used=1
track_rules | Skill | easy_skill_range | AI consistency (all difficulties) | AI difficulty | | The only difficulty value of this section the game looks up. | conf=medium used=1 ev=Read by 0x15E528
track_rules | AI.*Sim.* | (speed_factor|skill_range) | AI speed and consistency | AI difficulty | | Speed factor and consistency of the AI at this difficulty. | conf=medium used=1
""")
add("""
## ---- engine sound, wheels, effects (descriptive; readers in valt_schema_data.json)
engine_sound | .* | sound | Sound sample | Sound | file | Sample played (.sfx). | conf=medium used=1
engine_sound | .* | sample_rpm | Recorded engine speed | Sound | rpm | Engine speed the sample was recorded at; it is pitched from there. | conf=medium used=1
engine_sound | .* | (focus_volume|gain) | Volume | Sound | 0 to 1 | Volume of the sample. | conf=medium used=1
engine_sound | .* | nonfocus_volume | Volume for other cars | Sound | 0 to 1 | Never looked up by name. | conf=medium used=0
engine_sound | .* | rpm_factor | Pitch factor | Sound | x | Scales how fast the pitch rises with engine speed. | conf=low used=1
engine_sound | .* | t# | Blend point | Sound | rpm | Engine speed of a cross-fade point. | conf=low used=1
wheel_models | .* | (rim_diameter|tire_diameter|width) | Wheel size | Wheel | in | Size of the wheel model. | conf=medium used=1
wheel_models | .* | model | Model | Wheel | file | Wheel model file. | conf=medium used=1
wheel_models | .* | base | Model base name | Wheel | | Base name of the wheel models. | conf=medium used=1
wheel_models | .* | lod# | Switch distance | Wheel | m, on/off | Distance for a detail level. | conf=low used=1
wheel_models | fast_remap | .*\\.tex | Blur texture | Wheel | file | Texture swapped in for this one when the wheel spins fast. The list is walked, so the name is the data. | conf=medium used=1
track_particles | .* | (texture|model|tex) | Texture or model | Look | file | Image or model the effect draws. | conf=medium used=1
track_particles | .* | (gravity|drag|wind|random_wind|max_life|spew_rate|num_per_spew|alpha_constant|alpha_ramp|random_scalar) | Particle setting | Particles | | How particles fall, drift, live and fade. | conf=low used=1
track_particles | .* | (offset|axis|uv|anchor) | Placement | Placement | | Where the effect sits. | conf=low used=1
track_particles | rotator | rads_per_sec | Spin speed | Motion | rad/s | Speed of the spinning part. | conf=medium used=1
track_particles | .* | (on|off|t#|start|finish) | Timing | Timing | s | When the effect turns on and off. | conf=low used=1
""")


def _parts_fallback(section, key):
    """PART.RES part files share their key names with physics.val; label them the same way, but do not claim
    the game reads them (no code opening these files was found)."""
    for sec in ("Chassis", "EngineNormal", "Transmission", "Front"):
        rule = match("physics", sec, key)
        if rule and rule.spec.get("label") != "Comment":
            spec = dict(rule.spec)
            spec["explanation"] = ("Same meaning as the value of this name in a car's physics.val. This library file is "
                                   "probably not loaded: change the car's physics.val instead.")
            spec["confidence"] = "low"
            spec["used"] = None
            spec["evidence"] = "Key name shared with physics.val; no code opening PART.RES part files was found"
            return Rule(rule.fam, rule.sec, rule.key, spec)
    return None

add("""
## ---- home town places and other packed positions (decoded to metres in the value note)
event_props | .* | (.*_entrance|.*_inside|gate#_start|.*_pos#) | Place marker | Home town | | Position and facing of a home-town place: where you enter it, where you stop inside it, or where a car is parked. Decoded here to metres. | conf=medium used=1 ev=Packed 64-character frame like unfloat_frame64; the names match the home-town places (Malt Shop, paint shop, junkyard, gates)
event_props | .* | frame# | Position and rotation | Placement | | Position and orientation packed into 64 characters. Decoded here to metres. | conf=high used=1 ev=Unpacked at 0x22C1E8
event_props | .* | (basis_x|basis_y|basis_z) | Rotation axis | Placement | | One axis of the 3x3 rotation of the object. | conf=medium used=1
event_props | .* | origin | Position | Placement | m | Position (x, y, z) in metres. | conf=medium used=1
event_props | .* | circle | Circle | Outline | m | Centre (x, z) and radius of a circular area, in metres. | conf=low used=1
event_props | .* | (pt|lp) | Point | Waypoint | m | Map point (x, z) in metres. | conf=medium used=1
event_props | .* | (rad|max_radius|min_radius) | Radius | Waypoint | m | Radius around the point. | conf=medium used=1
event_props | .* | speed | Speed | Waypoint | | Speed at this waypoint. | conf=low used=1
event_props | .* | (min_lookahead|max_lookahead|go_directly_dist|repath_thresh|vel_correct_dist|vel_correct_min|curvature_dist|curvature_scale|start_mod|end_mod|speed_f|correct|min|max|value|base_lon|dir) | Path-following setting | AI path | | Setting of a scripted AI path (look-ahead, re-path and speed correction). | conf=low used=1
track_rules | Info | house_car_frame# | Parked car position | Track info | | Position and facing of a parked house car. Decoded here to metres. | conf=medium used=1 ev=house_car_frame64 is looked up by name
default_props | .* | dyno | Prop type | Prop | | Name of the prop (a section of obstacle.val). | conf=high used=1 ev=Same loader as event props (def_objects.val)
default_props | .* | (unfloat_frame#|frame#) | Position and rotation | Prop | | Packed position and orientation, decoded here to metres. | conf=high used=1
default_props | .* | sound | Ambient sound | Prop | file | Sound the prop plays. | conf=medium used=1
default_props | .* | volume | Ambient sound volume | Prop | 0 to 1 | Volume of the sound. | conf=medium used=1
car_efx | .* | (texture|flare_texture) | Texture | Look | file | Image drawn by the effect. | conf=medium used=1 ev=efx.val loader
car_efx | .* | (offset|arm|dir|axis|anchor) | Placement | Placement | | Where on the car the effect is mounted and which way it points (x, y, z). | conf=medium used=1
car_efx | .* | (gravity|drag|wind|spread|count|col_density) | Particle setting | Particles | | How the smoke or sparks fall, drift and spread. | conf=low used=1
car_efx | .* | (uv|show_model) | Look | Look | | Texture coordinates, or whether the mount model is drawn. | conf=low used=1
effects | .* | (tex|texture|resource|name) | Texture or effect | Look | file | Image or effect file used. | conf=medium used=1
effects | .* | (red|green|blue|alpha|color|blend) | Colour | Look | | Colour and blending of the effect. | conf=medium used=1
effects | .* | (radius|size|offset|slide|u#|v#|start_t|frequency|min_lap) | Effect setting | Effect | | Size, position, timing or frame of the effect. | conf=low used=1
track_fireworks | .* | (system|list|particle|smoke|flipbook|events|type|name|effect) | Effect part | Fireworks | | Name of a particle system, list or flipbook that makes up the firework. | conf=medium used=1 ev=Read by 0x318980
track_fireworks | .* | (alpha|alpha_constant|alpha_ramp|brightness|color|delay|drag|end_radius|gravity|interval|lifetime|max_count|max_life|radius|random_spread|speed|spew_rate|start_radius|texture|trigger) | Particle setting | Fireworks | | How the firework particles look, fall and fade. | conf=medium used=1 ev=Read by 0x3175A0 and 0x319710
track_fireworks | .* | .*_race|eve_win|normal_race | Event type | Fireworks | | Event type this firework list plays after. The list is walked, so the name is the data. | conf=low used=1
""")
