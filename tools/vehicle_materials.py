"""Verified retail body material identifiers shared by tooling.

These names are material aliases. Numbered paint resources are selected from
COMMON.RES driver records and CT.RES, not by searching every archive.
"""
BODY_TEXTURES = {
    "algar": ("Algar.tex",), "ambulance": ("amby.tex",), "arrow": ("arrow.tex",),
    "blitz": ("mira.tex",), "bulldog": ("Marmot.tex",), "bus": ("Bus1.tex", "Bus2.tex"),
    "cannon": ("Cruiser.tex",), "copcar": ("police.tex",), "coyote": ("Bandimer.tex",),
    "desio": ("desio.tex",), "digit": ("Digit.tex",), "durand": ("Durand.tex",),
    "empire": ("ariel.tex",), "fillmore": ("fillmore.tex",), "gladiator": ("muscle.tex",),
    "guerilla": ("Jagger.tex",), "hearse": ("hearse.tex",), "hunter": ("Dirtcar.tex",),
    "hurricane": ("Vaquero.tex",), "limo": ("limo.tex",), "mongoose": ("gisarme.tex",),
    "moth": ("Gizmo.tex",), "ocelot": ("ocey.tex",), "orbital": ("Junker.tex",),
    "outlaw": ("guerrero.tex",), "palo": ("Palo.tex",), "postal": ("pojeep.tex",),
    "prince": ("caravel.tex",), "rocket": ("Olympia.tex",), "taxi": ("taxi.tex",),
    "top": ("Courier.tex",), "venom": ("pony.tex",),
}

BODY_TEXTURE_ALIASES = {**BODY_TEXTURES, "prince": BODY_TEXTURES["prince"] + ("arrow.tex",)}
