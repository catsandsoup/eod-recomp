"""Human-readable schema for every VALT table on the disc.

A VALT table is nested text: sections (named, sometimes with a parent) that
hold key/value pairs. Keys are looked up by name in SLUS_209.10. This module
gives each table, section and key a label, group, unit, valid range, a plain
explanation of the in-game effect, editability and confidence, plus the game
code that reads it. Reference: docs/VALT_TABLES.md.

    describe_table(archive, member, table) -> {title, summary, family, sections, fields}
    describe_field(member, section, key)   -> spec dict

Rule data lives in valt_schema_rules.py; observed retail ranges and the
executable cross-reference live in valt_schema_data.json (rebuilt by
`python3 tools/formats/valt_schema.py build DISC OUT.json`).
"""
import json
import re
from functools import lru_cache
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA_PATH = HERE / "valt_schema_data.json"

# Member name -> family id. First match wins.
FAMILY_RULES = [
    (r"physics\.val", "physics"),
    (r"shadow\.val", "shadow"),
    (r"lod(_hi)?\.val", "lod"),
    (r"c_.*\.va0|c_.*\.val", "event_props"),
    (r"bone\.val|bone\.mvl|bonemod\.bvl", "bones"),
    (r"efx\.val", "car_efx"),
    (r"lights\.val", "car_lights"),
    (r"camera\d*\.val", "camera"),
    (r"track\.val", "track_rules"),
    (r"intrlude\.val", "interlude"),
    (r"obstacle\.val", "obstacles"),
    (r"lineinfo\.val", "ai_lines"),
    (r"trkefx\.val", "track_particles"),
    (r"def_objects\.val", "default_props"),
    (r".*\.pln", "collision_planes"),
    (r"trkfw\.val", "track_fireworks"),
    (r"firework\.val", "firework_effects"),
    (r"(.*_)?eves\.val", "eves"),
    (r"dirt\.val|cup\.val|modified\.val|truck\.val", "drivers"),
    (r"catalog\.val", "catalog"),
    (r"(chassis|engine|tire|wheel|trans|diff|clutch|brake|shock|spring|suspen|swaybar)\.val", "parts"),
    (r"(4stock|4tuned|6tuned|mus\d+|v\d+junker)\.val", "engine_sound"),
    (r"(bus|busrear|com|mid|mus)\.val", "wheel_models"),
    (r"(flare|explosion|expseq|wobefx)\.val", "effects"),
    (r"(buycar|carclass|car_stats|home_challenges|challenges|time_challenge|gauntlet|catchup|"
     r"unlock|feats|setups|cskill|nicknames|champion|vidrank|tracks|beat_times|event_desc|infotext|"
     r"awards|titles|award_schemes|invite|news|personnel|sponsors|rollcall|usenglish_units|regions|drct\d+trk)\.val", "career"),
]

_SKIP = re.compile(r"[^A-Za-z0-9]+")


def family_of(member):
    name = member.lower()
    for pattern, family in FAMILY_RULES:
        if re.fullmatch(pattern, name):
            return family
    return "other"


def generic(text):
    """digits -> '#', the form used for section/key patterns in the rules and data file."""
    return re.sub(r"\d+", "#", text)


@lru_cache(maxsize=1)
def _data():
    try:
        return json.loads(DATA_PATH.read_text())
    except (OSError, ValueError):
        return {"families": {}}


def _rules():
    if __package__:
        from . import valt_schema_rules as r
    else:  # run as a script or from tools/formats on sys.path
        import valt_schema_rules as r
    return r


# ---------------------------------------------------------------- helpers

NUMERIC = re.compile(r"[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?")


def humanize(key):
    """Last-resort label: 'random_spread_(xz)' -> 'Random spread (xz)'. Never the raw key."""
    k = key.strip().lstrip("@")
    k = re.sub(r"^\$\d+\$", "", k)
    k = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", k)
    words = [w for w in re.split(r"[_\s]+", k) if w]
    if not words:
        return "Unnamed value"
    text = " ".join(words)
    text = text[0].upper() + text[1:]
    return text if text != key else text + " (value)"


def widen(lo, hi):
    """Editing range that contains every retail value with headroom on both sides."""
    span = hi - lo
    pad = span * 0.5 if span else max(abs(hi), 1.0) * 0.5
    a, b = lo - pad, hi + pad
    if lo >= 0:
        a = max(0.0, a)
    if hi <= 0:
        b = min(0.0, b)
    return round(a, 6), round(b, 6)


def _fmt(n):
    return ("%g" % n)


def section_ordinals(table):
    seen, out = {}, []
    for section in table["sections"]:
        seen[section["name"]] = seen.get(section["name"], 0) + 1
        out.append(seen[section["name"]])
    return out


def _lookup_rule(family, section, key):
    return _rules().match(family, section, key)


def describe_field(member, section, key, value=None):
    """Spec for one key: label, group, unit, explanation, confidence, used, min, max, editable, evidence."""
    family = family_of(member)
    if key == "//":
        return dict(label="Comment", group="Notes", unit="", explanation="Free text left by the original authors. The game ignores it.",
                    confidence="high", used=False, min=None, max=None, editable=True, evidence="comment line", curated=True, family=family)
    marker = key.startswith("@")
    base_key = key[1:] if marker else key
    rule = _lookup_rule(family, section, base_key)
    spec = dict(rule.spec) if rule else {}
    curated = bool(rule)
    entry = _data().get("families", {}).get(family, {}).get(generic(section) + "\t" + generic(key), {})
    spec.setdefault("label", humanize(base_key))
    spec.setdefault("group", _rules().SECTION_GROUP_FALLBACK)
    spec.setdefault("unit", "")
    spec.setdefault("explanation", "")
    spec.setdefault("confidence", "low")
    if "used" not in spec:
        spec["used"] = entry.get("used")
    if not spec["explanation"]:
        spec["explanation"], spec["confidence"], spec["used"] = _untraced(family, section, spec["used"])
    fns = entry.get("fns") or []
    evidence = spec.pop("evidence", "")
    if fns and not evidence:
        evidence = "Read by " + ", ".join("0x%s" % f for f in fns[:4])
    elif fns:
        evidence += "; also referenced by " + ", ".join("0x%s" % f for f in fns[:3])
    if not evidence:
        if spec["used"] is False:
            evidence = "The key text does not occur in SLUS_209.10"
        else:
            evidence = "Key text occurs in SLUS_209.10 but no reader was traced" if spec["used"] is None else "Referenced by the executable"
    spec["evidence"] = evidence
    spec.pop("derive", None)
    # No limits are invented: min/max exist only when a rule states a code clamp. The observed
    # retail spread is explanation text, not a bound.
    if "lo" in entry and spec.get("min") is None and spec.get("max") is None:
        spread = "Retail files use %s" % (_fmt(entry["lo"]) if entry["lo"] == entry["hi"] else "%s to %s" % (_fmt(entry["lo"]), _fmt(entry["hi"])))
        spec["explanation"] = (spec["explanation"] + " " + spread + ".").strip()
    spec["observed"] = [entry["lo"], entry["hi"]] if "lo" in entry else None
    spec.setdefault("min", None)
    spec.setdefault("max", None)
    spec.pop("derive", None)
    spec.setdefault("editable", True)
    if marker:
        spec["used"] = True
        spec["evidence"] = "VALT nesting marker resolved by the table loader"
        spec["label"] = spec["label"] + " (nested table)"
        spec["explanation"] = "Points to the nested table below that holds these values. Edit that table; this link stays as it is."
        spec["confidence"] = "high"
        spec["editable"] = False
        spec["min"] = spec["max"] = None
    spec["curated"] = curated
    spec["family"] = family
    if value is not None:
        spec["valueNote"] = _rules().value_note(family, section, key, value)
    return spec


def _section_lookups(family, section):
    """How many keys of this (family, section) the game looks up by name."""
    stats = _data().setdefault("_sections", {})
    ident = family + "\t" + generic(section)
    if ident not in stats:
        rows = [e for k, e in _data().get("families", {}).get(family, {}).items()
                if k.split("\t")[0] == generic(section) and not k.split("\t")[1].startswith("@")]
        stats[ident] = (sum(1 for e in rows if e.get("used") is True), len(rows))
    return stats[ident]


def _untraced(family, section, used):
    """Default text for a key no rule covers. A key whose name never appears in the executable is only
    called unused when the game looks up other keys of the same section by name; tables the game walks
    as a list (class members, paint panels) have names that never appear and are still read."""
    looked_up, total = _section_lookups(family, section)
    if used is False and looked_up:
        return ("The game never looks this name up, while it does read %d other value%s of this section by name, "
                "so this value has no effect." % (looked_up, "" if looked_up == 1 else "s")), "medium", False
    if used is False:
        return ("No code looks this name up directly; this section may be read as a list. Effect not confirmed."), "low", None
    if used is True:
        return "Read by the game. Exact effect not yet traced.", "low", True
    return _rules().UNTRACED, "low", None


def describe_section(member, section, ordinal, pairs=None, parent=""):
    family = family_of(member)
    return _rules().section_info(family, section, ordinal, pairs or [], parent)


def describe_table(archive, member, table):
    """Full description of one decoded table (parse_binary_valt output)."""
    family = family_of(member)
    info = _rules().FAMILIES.get(family, _rules().FAMILIES["other"])
    ordinals = section_ordinals(table)
    sections, fields = [], {}
    for index, section in enumerate(table["sections"]):
        label, explanation = describe_section(member, section["name"], ordinals[index], section["pairs"], section.get("parent", ""))
        sections.append(dict(id=index, name=section["name"], label=label, explanation=explanation))
        for key, value in section["pairs"]:
            fields[(section["name"], key)] = describe_field(member, section["name"], key)
    title = info["title"]
    summary = info["summary"]
    specific = _rules().MEMBER_TITLES.get(member.lower())
    if specific:
        title, summary = specific
    return dict(title=title, summary=summary, family=family, sections=sections, fields=fields, archive=archive, member=member)


def coverage(tables):
    """tables: iterable of (member, parsed table). -> {family: {files, sections, keys, curated, used, unused, untraced}}"""
    out = {}
    for member, table in tables:
        family = family_of(member)
        row = out.setdefault(family, dict(files=0, keys=set(), sections=set(), curated=set(), used=set(), unused=set()))
        row["files"] += 1
        for section in table["sections"]:
            row["sections"].add(generic(section["name"]))
            for key, _ in section["pairs"]:
                ident = (generic(section["name"]), generic(key))
                row["keys"].add(ident)
                spec = describe_field(member, section["name"], key)
                if spec["curated"] and spec["confidence"] != "low":
                    row["curated"].add(ident)
                if spec["used"] is True:
                    row["used"].add(ident)
                elif spec["used"] is False:
                    row["unused"].add(ident)
    return {f: dict(files=r["files"], sections=len(r["sections"]), keys=len(r["keys"]), traced=len(r["curated"]),
                    used=len(r["used"]), unused=len(r["unused"])) for f, r in out.items()}


def value_note(family, section, key, value):
    """Per-value text for packed keys (64-symbol position frames decode to metres)."""
    return _rules().value_note(family, section, key, value)
