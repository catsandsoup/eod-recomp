"""Server-enforced safety: configuration, path guards, plan ids, audit log.

Nothing here trusts tool arguments. Every write target goes through
Config.ws_path(..., write=True); every apply goes through Plans.verify().
"""
import hashlib
import json
import os
import re
import threading
import time
from pathlib import Path

PLAN_TTL = 1800
DEV_PROFILE = re.compile(r"^dev-[a-z0-9][a-z0-9-]{0,56}$")
NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
MARKER = ".eod-dev-profile"


class ToolError(Exception):
    def __init__(self, code, message, hint=None, details=None):
        super().__init__(message)
        self.code, self.message, self.hint, self.details = code, message, hint, details

    def payload(self):
        out = dict(code=self.code, message=self.message)
        if self.hint:
            out["hint"] = self.hint
        if self.details is not None:
            out["details"] = self.details
        return out


def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fingerprint(path):
    """sha256 of a file, or a stable digest of a directory tree; None if absent."""
    path = Path(path)
    if path.is_symlink():
        return "symlink"
    if path.is_file():
        return sha_file(path)
    if path.is_dir():
        h = hashlib.sha256()
        for p in sorted(path.rglob("*")):
            if p.is_file() and not p.is_symlink():
                h.update(p.relative_to(path).as_posix().encode() + b"\0" + sha_file(p).encode() + b"\n")
        return "dir:" + h.hexdigest()
    return None


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), default=str)


def inside(path, root):
    return path == root or root in path.parents


class Config:
    def __init__(self, env=None):
        env = os.environ if env is None else env
        disc = env.get("EOD_DISC")
        self.disc = Path(disc).expanduser().resolve() if disc else None
        ws = env.get("EOD_KIT_WORKSPACE") or env.get("EOD_KIT_HOME") or "~/Library/Application Support/EODCreationKit"
        self.workspace = Path(ws).expanduser().resolve()
        self.allow_run = env.get("EOD_KIT_ALLOW_RUN") == "1"
        exe = env.get("EOD_GAME_EXECUTABLE") or env.get("EOD_GAME_EXE")
        self.executable = Path(exe).expanduser().resolve() if exe else None
        self.import_dirs = [Path(p).expanduser().resolve() for p in env.get("EOD_KIT_IMPORT_DIRS", "").split(os.pathsep) if p]
        self.home = Path(os.path.expanduser("~")).resolve()
        self.workspace.mkdir(parents=True, exist_ok=True)
        if self.disc and (inside(self.workspace, self.disc) or inside(self.disc, self.workspace)):
            raise ToolError("UNSAFE_PATH", "Workspace and disc folders must not contain each other",
                            "Set EOD_KIT_WORKSPACE to a separate folder")

    # ---- disc
    def need_disc(self):
        if self.disc is None:
            raise ToolError("NOT_CONFIGURED", "EOD_DISC is not set", "Set EOD_DISC to the extracted game folder in the MCP server env")
        if not (self.disc / "DATA").is_dir():
            raise ToolError("DISC_NOT_FOUND", "EOD_DISC has no DATA folder: " + str(self.disc))
        return self.disc

    def slus_sha(self):
        p = self.disc / "SLUS_209.10" if self.disc else None
        return sha_file(p) if p and p.is_file() else None

    # ---- paths
    def forbidden_reason(self, path):
        """Why `path` may never be a write target, or None."""
        if self.disc and inside(path, self.disc):
            return "inside the game disc folder (read-only)"
        s = str(path)
        if s == "/Applications" or s.startswith("/Applications/") or any(p.endswith(".app") for p in path.parts):
            return "inside an installed application"
        appsupport = self.home / "Library" / "Application Support"
        if inside(path, appsupport) and not inside(path, self.workspace):
            return "inside another application's support folder"
        if not inside(path, self.workspace):
            return "outside the workspace"
        return None

    def ws_path(self, rel, write=False, read_import=False, must_exist=False, what="path"):
        """Resolve a workspace-relative path. No `..`, no symlinks, no escape."""
        if not isinstance(rel, str) or not rel or "\0" in rel or "\\" in rel:
            raise ToolError("UNSAFE_PATH", f"Invalid {what}")
        raw = Path(rel)
        if raw.is_absolute():
            candidate = raw
        else:
            if ".." in raw.parts or raw.parts[0] == "~":
                raise ToolError("UNSAFE_PATH", f"{what} must not contain '..': {rel}")
            candidate = self.workspace / raw
        if ".." in candidate.parts:
            raise ToolError("UNSAFE_PATH", f"{what} must not contain '..': {rel}")
        cur = Path(candidate.anchor)
        for part in candidate.parts[1:]:
            cur = cur / part
            if cur.is_symlink() and not inside(self.workspace, cur.resolve()):
                raise ToolError("UNSAFE_PATH", f"{what} crosses a symbolic link: {rel}")
        resolved = candidate.resolve()
        if write:
            reason = self.forbidden_reason(resolved)
            if reason:
                raise ToolError("UNSAFE_PATH", f"Refusing to write {what}: {reason}", "Use a path relative to the workspace")
            parent = resolved.parent
            if parent.is_dir():
                clash = [c.name for c in parent.iterdir() if c.name != resolved.name and c.name.casefold() == resolved.name.casefold()]
                if clash:
                    raise ToolError("CONFLICT", f"Case-folded name collision with {clash[0]}")
        else:
            roots = [self.workspace] + (self.import_dirs if read_import else [])
            if not any(inside(resolved, r) for r in roots):
                raise ToolError("UNSAFE_PATH", f"{what} is outside the workspace", "Put the file under the workspace or EOD_KIT_IMPORT_DIRS")
        if must_exist and not resolved.exists():
            raise ToolError("NOT_CONFIGURED", f"{what} does not exist: {rel}")
        return resolved

    def rel(self, path):
        try:
            return Path(path).resolve().relative_to(self.workspace).as_posix()
        except ValueError:
            return str(path)

    # ---- names / profiles
    def project_file(self, name):
        if not NAME.fullmatch(name or ""):
            raise ToolError("UNSAFE_PATH", "Project name must match [A-Za-z0-9][A-Za-z0-9._-]{0,63}")
        return self.ws_path(f"projects/{name}/project.json", write=True, what="project")

    def profiles_root(self):
        return self.ws_path("profiles", write=True, what="profiles root")

    def profile_dir(self, name, must_exist=True, create=False):
        if not DEV_PROFILE.fullmatch(name or ""):
            raise ToolError("UNSAFE_PATH", "Only development profiles are allowed: name must match dev-[a-z0-9-]+")
        d = self.ws_path(f"profiles/{name}", write=True, what="profile")
        if not create:
            if not d.is_dir():
                raise ToolError("NOT_CONFIGURED", f"No profile named {name}", "Call eod_profile_create")
            if not (d / MARKER).is_file():
                raise ToolError("UNSAFE_PATH", f"Profile {name} lacks the {MARKER} marker; refusing", "Only profiles made by eod_profile_create are editable")
        return d


class Plans:
    def __init__(self):
        self.issued = {}
        self.lock = threading.Lock()

    @staticmethod
    def make_id(tool, args, fp):
        norm = {k: v for k, v in args.items() if k not in ("dry_run", "plan_id", "confirm")}
        return hashlib.sha256(canonical([tool, norm, fp]).encode()).hexdigest()[:16]

    def issue(self, tool, args, fp):
        pid = self.make_id(tool, args, fp)
        with self.lock:
            self.issued[pid] = time.time()
        return pid

    def verify(self, tool, args, fp):
        supplied = args.get("plan_id")
        if not supplied:
            raise ToolError("NEEDS_CONFIRM", "Applying requires plan_id from a dry run", f"Call {tool} with dry_run:true first")
        current = self.make_id(tool, args, fp)
        if supplied != current:
            raise ToolError("STALE_PLAN", "plan_id does not match the current arguments or input files", f"Repeat {tool} with dry_run:true")
        t = self.issued.get(current)
        if t is None or time.time() - t > PLAN_TTL:
            raise ToolError("STALE_PLAN", "plan_id is unknown or older than 30 minutes", f"Repeat {tool} with dry_run:true")
        return current


class Audit:
    def __init__(self, config):
        self.path = config.workspace / "audit.jsonl"
        self.lock = threading.Lock()

    def write(self, tool, args, plan_id, files, ok=True, note=None):
        entry = dict(ts=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), tool=tool,
                     argsSha256=sha_bytes(canonical(args).encode()), planId=plan_id, ok=ok,
                     files=files)
        if note:
            entry["note"] = note
        with self.lock, open(self.path, "a") as f:
            f.write(json.dumps(entry) + "\n")

    def tail(self, n=200):
        if not self.path.is_file():
            return []
        return self.path.read_text().splitlines()[-n:]


class audited:
    """Context manager: records before/after fingerprints of write targets."""

    def __init__(self, ctx, tool, args, plan_id, paths):
        self.ctx, self.tool, self.args, self.plan_id = ctx, tool, args, plan_id
        self.paths = [Path(p) for p in paths]

    def __enter__(self):
        self.before = {str(p): fingerprint(p) for p in self.paths}
        return self

    def __exit__(self, et, ev, tb):
        after = {str(p): fingerprint(p) for p in self.paths}
        files = [dict(path=self.ctx.config.rel(p), before=self.before[str(p)], after=after[str(p)]) for p in self.paths]
        self.ctx.audit.write(self.tool, self.args, self.plan_id, files, ok=et is None, note=None if et is None else str(ev)[:200])
        return False


REDACT_HOME = re.compile(re.escape(str(Path.home())))


def redact(text):
    return REDACT_HOME.sub("~", text)
