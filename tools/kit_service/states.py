"""Explicit state machines for projects, packages and development profiles.

States are derived from the files mod_package.py / mod_install.py /
modding_backend.py already write (project file, profile.json, active/PROFILE.TXT)
plus the last recorded transition, so they cannot drift from disk. Every write
action is checked here first; an invalid action is rejected with a sentence
that says what happened and what to do. After a successful action the new
state is re-derived and logged (entity_state, state_log).

Package (per profile):  draft -> built -> installed -> enabled <-> disabled -> uninstalled
Project:                new -> saved -> packaged (editing a packaged project returns it to saved)
Dev profile:            absent -> empty | inactive | active | blocked, and in-use while a game run holds it
"""
import json
import time
from pathlib import Path

from .common import KitError, ensure_tools_path

ensure_tools_path()


class Machine:
    def __init__(self, kind, states, transitions, messages):
        self.kind, self.states, self.transitions, self.messages = kind, states, transitions, messages

    def allowed(self, state):
        return sorted(a for (s, a) in self.transitions if s == state)

    def target(self, state, action, params=None):
        rule = self.transitions.get((state, action))
        if rule is None:
            return None
        return rule(params or {}) if callable(rule) else rule

    def check(self, state, action, name, params=None, extra=None):
        target = self.target(state, action, params)
        if target is not None:
            return target
        text = self.messages.get((state, action)) or self.messages.get(("*", action)) or self.messages["*"]
        values = dict(name=name, state=state, action=action, **(extra or {}))
        message, hint = (part.format(**values) for part in text)
        raise KitError("INVALID_TRANSITION", message, hint,
                       dict(kind=self.kind, key=name, state=state, action=action, allowed=self.allowed(state)))


def _installed_target(params):
    return "enabled" if params.get("enable", True) else "installed"


PACKAGE = Machine("package", ("draft", "built", "installed", "enabled", "disabled", "uninstalled"), {
    ("draft", "build"): "built",
    ("built", "build"): "built",
    ("built", "install"): _installed_target,
    ("installed", "install"): _installed_target,   # newer version replaces the installed one
    ("installed", "enable"): "enabled",
    ("installed", "disable"): "disabled",
    ("installed", "uninstall"): "uninstalled",
    ("enabled", "install"): _installed_target,
    ("enabled", "enable"): "enabled",              # recomposes active/
    ("enabled", "disable"): "disabled",
    ("enabled", "uninstall"): "uninstalled",
    ("disabled", "install"): _installed_target,
    ("disabled", "enable"): "enabled",
    ("disabled", "disable"): "disabled",
    ("disabled", "uninstall"): "uninstalled",
    ("uninstalled", "install"): _installed_target,
    ("uninstalled", "build"): "built",
}, {
    ("draft", "install"): ("{name} has not been built yet, so there is nothing to install.",
                           "Build the package from its project, then install it."),
    ("*", "enable"): ("{name} is not installed in profile {profile}, so it can't be enabled.",
                      "Install the package into this profile first."),
    ("*", "disable"): ("{name} is not installed in profile {profile}, so it can't be disabled.",
                       "Check the package id; only installed packages can be disabled."),
    ("*", "uninstall"): ("{name} is not installed in profile {profile}, so there is nothing to uninstall.",
                         "Check the package id; it may already have been removed."),
    "*": ("{name} can't {action} while it is {state}.", "Refresh the profile and choose an available action."),
})

PROJECT = Machine("project", ("new", "saved", "packaged"), {
    ("new", "save"): "saved",
    ("new", "import_host"): "saved",
    ("saved", "save"): "saved",
    ("saved", "import_host"): "saved",
    ("saved", "remove_host"): "saved",
    ("saved", "package"): "packaged",
    ("saved", "validate"): "saved",
    ("saved", "launch"): "saved",
    ("packaged", "save"): "saved",
    ("packaged", "import_host"): "saved",
    ("packaged", "remove_host"): "saved",
    ("packaged", "package"): "packaged",
    ("packaged", "validate"): "packaged",
    ("packaged", "launch"): "packaged",
}, {
    ("new", "package"): ("This project has no saved edits, so there is nothing to package.",
                         "Save a texture or vehicle edit first, then build the package."),
    ("new", "validate"): ("This project has not been saved yet, so there is nothing to check.",
                          "Save an edit, then validate or test the project."),
    ("new", "launch"): ("This project has not been saved yet, so there is nothing to test.",
                        "Save an edit, then start the test."),
    ("new", "remove_host"): ("This project has no high-resolution replacements to remove.",
                             "Choose a texture that shows a recorded replacement."),
    "*": ("The project can't {action} while it is {state}.", "Reopen the project and try again."),
})

PROFILE = Machine("profile", ("absent", "empty", "inactive", "active", "blocked", "in-use"), {
    **{(s, a): "derived" for s in ("absent", "empty", "inactive", "active", "blocked")
       for a in ("install",)},
    **{(s, a): "derived" for s in ("empty", "inactive", "active", "blocked")
       for a in ("enable", "disable", "uninstall", "order", "accept_conflict", "compose", "delete")},
    ("active", "launch"): "in-use",
}, {
    ("absent", "*"): ("Profile {name} doesn't exist yet.", "Install a package into it first."),
    ("absent", "launch"): ("Profile {name} doesn't exist yet, so there is nothing to test.",
                           "Install and enable a package first."),
    ("empty", "launch"): ("Profile {name} has no packages, so there is nothing to test.",
                          "Install and enable a package first."),
    ("inactive", "launch"): ("No package is enabled in profile {name}, so there is nothing to test.",
                             "Enable a package, then start the test."),
    ("blocked", "launch"): ("Profile {name} has unresolved conflicts: {blocked}.",
                            "Choose a winner for each conflict or disable one of the packages, then test again."),
    ("in-use", "launch"): ("A game test is already running on profile {name}.", "Wait for it to finish or stop it first."),
    ("in-use", "*"): ("Profile {name} is in use by a running game test, so its packages were not changed.",
                      "Stop the test, then change the profile."),
    "*": ("Profile {name} doesn't exist yet.", "Install a package into it first."),
})
for _state in ("absent", "in-use"):
    for _action in ("enable", "disable", "uninstall", "order", "accept_conflict", "compose", "delete"):
        PROFILE.messages.setdefault((_state, _action), PROFILE.messages[(_state, "*")])
PROFILE.messages[("in-use", "install")] = PROFILE.messages[("in-use", "*")]

MACHINES = dict(package=PACKAGE, project=PROJECT, profile=PROFILE)


# --------------------------------------------------------------------------- derivation

def _recorded(conn, kind, key):
    if conn is None:
        return None
    row = conn.execute("SELECT state, updated, detail FROM entity_state WHERE kind=? AND key=?", (kind, key)).fetchone()
    return dict(state=row["state"], updated=row["updated"], detail=json.loads(row["detail"] or "{}")) if row else None


def profile_location(request):
    import mod_install
    name = request.get("profile") or "dev"
    return name, mod_install.profile_dir(request.get("profileRoot"), name)


def profile_state(conn, request, game_holder=None):
    import mod_install
    name, folder = profile_location(request)
    detail = dict(profile=name, root=str(folder), blocked="")
    if game_holder:
        detail["holder"] = game_holder
        return "in-use", detail
    if not (folder / "profile.json").is_file():
        return "absent", detail
    status = mod_install.status(name, request.get("profileRoot"))
    detail["blocked"] = ", ".join(status["blocked"])
    detail["packages"] = {p["id"]: dict(version=p["version"], enabled=p["enabled"]) for p in status["packages"]}
    if not status["packages"]:
        return "empty", detail
    if status["blocked"]:
        return "blocked", detail
    if not any(p["enabled"] for p in status["packages"]) or not status["active"]["composed"]:
        return "inactive", detail
    return "active", detail


def package_state(conn, request, package_id, profile_detail=None):
    name, folder = profile_location(request)
    detail = profile_detail if profile_detail is not None else profile_state(conn, request)[1]
    record = (detail.get("packages") or {}).get(package_id)
    key = f"{folder}#{package_id}"
    recorded = _recorded(conn, "package", key)
    if record is None:
        if recorded and recorded["state"] in ("installed", "enabled", "disabled", "uninstalled"):
            return "uninstalled", key
        return (recorded["state"] if recorded and recorded["state"] in ("draft", "built") else "built"), key
    if record["enabled"]:
        return "enabled", key
    return ("installed" if recorded and recorded["state"] == "installed" else "disabled"), key


def project_state(conn, request):
    path = Path(request["project"]).expanduser().resolve()
    key = str(path)
    if not path.is_file():
        return "new", key
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError):
        return "new", key
    if not (data.get("archives") or data.get("hostTextures")):
        return "new", key
    recorded = _recorded(conn, "project", key)
    if recorded and recorded["state"] == "packaged" and recorded["detail"].get("mtimeNs") == path.stat().st_mtime_ns:
        return "packaged", key
    return "saved", key


def record(conn, kind, key, state, action, before, ok=True, message=None, detail=None):
    if conn is None:
        return
    now = time.time()
    conn.execute("INSERT OR REPLACE INTO entity_state(kind, key, state, updated, detail) VALUES(?,?,?,?,?)",
                 (kind, key, state, now, json.dumps(detail or {})))
    conn.execute("INSERT INTO state_log(ts, kind, key, action, from_state, to_state, ok, message) VALUES(?,?,?,?,?,?,?,?)",
                 (now, kind, key, action, before, state, int(ok), message))


def log_rejection(conn, kind, key, action, state, message):
    if conn is not None:
        conn.execute("INSERT INTO state_log(ts, kind, key, action, from_state, to_state, ok, message) VALUES(?,?,?,?,?,?,0,?)",
                     (time.time(), kind, key, action, state, state, message))


# --------------------------------------------------------------------------- backend action guard

PROJECT_ACTIONS = dict(save="save", save_format="save", import_host="import_host", remove_host="remove_host",
                       package_project="package", validate="validate", launch_plan="launch")
PROFILE_ACTIONS = dict(profile_install="install", profile_enable="enable", profile_disable="disable",
                       profile_uninstall="uninstall", profile_accept_conflict="accept_conflict", launch_profile="launch")


class Guard:
    """Validate one backend action before it runs; record the resulting state after it succeeds."""

    def __init__(self, conn, request, game_holder=None):
        self.conn, self.request, self.action = conn, request, request.get("action")
        self.game_holder = game_holder
        self.after = []

    def check(self):
        action = self.action
        if action in PROJECT_ACTIONS and self.request.get("project"):
            state, key = project_state(self.conn, self.request)
            step = PROJECT_ACTIONS[action]
            self._check(PROJECT, state, step, key, Path(key).parent.name or key)
            self.after.append(("project", key, step, state))
        if action in PROFILE_ACTIONS:
            name, folder = profile_location(self.request)
            pstate, detail = profile_state(self.conn, self.request, self.game_holder)
            step = PROFILE_ACTIONS[action]
            self._check(PROFILE, pstate, step, str(folder), name, detail)
            self.after.append(("profile", str(folder), step, pstate))
            if step in ("enable", "disable", "uninstall") and pstate != "in-use":
                pid = self.request.get("id") or ""
                state, key = package_state(self.conn, self.request, pid, detail)
                self._check(PACKAGE, state, step, key, pid, dict(profile=name))
                self.after.append(("package", key, step, state))
        return self

    def _check(self, machine, state, step, key, name, extra=None):
        try:
            machine.check(state, step, name, self.request, extra)
        except KitError as error:
            log_rejection(self.conn, machine.kind, key, step, state, error.message)
            raise

    def done(self, result):
        """Re-derive and record states after a successful action."""
        if self.conn is None:
            return
        for kind, key, step, before in self.after:
            try:
                if kind == "project":
                    path = Path(key)
                    state = "packaged" if step == "package" else project_state(None, self.request)[0]
                    record(self.conn, kind, key, state, step, before,
                           detail=dict(mtimeNs=path.stat().st_mtime_ns if path.is_file() else None))
                    if step == "package" and isinstance(result, dict) and result.get("id"):
                        record(self.conn, "package", f"built#{result['id']}", "built", "build", "draft",
                               detail=dict(path=result.get("path"), version=result.get("version")))
                elif kind == "profile":
                    state, detail = profile_state(self.conn, self.request)
                    record(self.conn, kind, key, state, step, before, detail=dict(blocked=detail.get("blocked")))
                    if step == "install" and isinstance(result, dict):
                        installed = str(result.get("installed", "")).split(" ")[0]
                        if installed:
                            pkey = f"{key}#{installed}"
                            target = "enabled" if self.request.get("enable", True) else "installed"
                            prev = _recorded(self.conn, "package", pkey)
                            record(self.conn, "package", pkey, target, "install", prev["state"] if prev else "built")
                elif kind == "package":
                    target = PACKAGE.target(before, step, self.request)
                    record(self.conn, kind, key, target, step, before)
            except Exception:  # noqa: BLE001 - recording state must never fail a completed action
                pass


def describe(conn, kind, request):
    """Current state and allowed actions for state.get."""
    if kind == "project":
        state, key = project_state(conn, request)
    elif kind == "profile":
        state, detail = profile_state(conn, request)
        key = detail["root"]
    elif kind == "package":
        state, key = package_state(conn, request, request.get("id") or "")
    else:
        raise KitError("INVALID_ARGUMENT", "kind must be project, profile or package")
    machine = MACHINES[kind]
    history = [dict(r) for r in conn.execute(
        "SELECT ts, action, from_state, to_state, ok, message FROM state_log WHERE kind=? AND key=? ORDER BY ts DESC LIMIT 20",
        (kind, key))] if conn is not None else []
    return dict(kind=kind, key=key, state=state, allowed=machine.allowed(state), history=history)
