#!/usr/bin/env python3
"""EOD Creation Kit development profiles (docs/CREATION_KIT_TEXTURE_CONTRACTS.md section 6).

Install verified mod packages into a profile, then compose the profile's
`active/` overlay root (EOD_MOD_DIR) from the retail archives. Library
functions return dicts and raise ValueError with user-facing messages.
"""
import argparse
import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from modding_backend import atomic_write, directory
import mod_package as mp

DEFAULT_ROOT = Path("~/Library/Application Support/EODCreationKit/profiles")
NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")


def jdump(obj):
    return (json.dumps(obj, indent=2, sort_keys=True) + "\n").encode()


# ------------------------------------------------------------------ profile

def profile_dir(root, name):
    if not NAME_RE.fullmatch(name or ""):
        raise ValueError("Profile name must match [A-Za-z0-9][A-Za-z0-9._-]{0,63}")
    return Path(root or DEFAULT_ROOT).expanduser().resolve() / name


@contextlib.contextmanager
def locked(P):
    P.mkdir(parents=True, exist_ok=True)
    with open(P / ".lock", "a+") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise ValueError("Another Kit operation is changing this profile; try again when it finishes") from None
        yield


def load(P, name):
    path = P / "profile.json"
    if not path.is_file():
        return dict(schemaVersion=1, name=name, disc=None, packages={}, order=[], accepted={})
    try:
        prof = json.loads(path.read_text())
        assert prof["schemaVersion"] == 1 and isinstance(prof["packages"], dict)
        assert sorted(prof["order"]) == sorted(prof["packages"])
    except (ValueError, KeyError, AssertionError, TypeError):
        raise ValueError(f"profile.json is damaged: {path}") from None
    return prof


def use_disc(prof, disc):
    if disc:
        root = str(mp.disc_root(disc))
        if prof["disc"] and prof["disc"] != root and prof["packages"]:
            raise ValueError("This profile is bound to another game folder: " + prof["disc"])
        prof["disc"] = root
    if not prof["disc"]:
        raise ValueError("Select the extracted game folder (--disc)")
    return prof["disc"]


def pkg_dir(P, rec_id, version):
    return P / "packages" / rec_id / version


def check_deps(prof, m, enabled_only=False):
    for dep in m["dependencies"]:
        rec = prof["packages"].get(dep["id"])
        if not rec or (enabled_only and not rec["enabled"]):
            raise ValueError(f"{m['id']} requires {dep['id']} {dep['minVersion']} or newer, which is not "
                             f"{'enabled' if rec else 'installed'}")
        if mp.semver_key(rec["version"]) < mp.semver_key(dep["minVersion"]):
            raise ValueError(f"{m['id']} requires {dep['id']} {dep['minVersion']} or newer; {rec['version']} is installed")


def dependents(P, prof, pid, enabled_only=False):
    out = []
    for other, rec in prof["packages"].items():
        if other != pid and (rec["enabled"] or not enabled_only):
            m = mp.load_manifest(pkg_dir(P, other, rec["version"]))
            if any(d["id"] == pid for d in m["dependencies"]):
                out.append(other)
    return out


# ------------------------------------------------------------------ conflicts

def contributions(P, prof, everything=False):
    """{conflict key: {package id: (sha256, entry)}} over enabled (or all installed) packages, from manifests."""
    found = {}
    for pid in prof["order"]:
        rec = prof["packages"][pid]
        if not rec["enabled"] and not everything:
            continue
        m = mp.load_manifest(pkg_dir(P, pid, rec["version"]))
        for e in m["native"]:
            found.setdefault(f"{e['archive']}#{e['member']}".lower(), {})[pid] = (e["sha256"], e)
        for e in m["host"]:
            found.setdefault("host:" + e["assetId"].lower(), {})[pid] = (e["sha256"], e)
    return found


def conflicts(P, prof, everything=False):
    out = []
    for key, by_pkg in sorted(contributions(P, prof, everything).items()):
        shas = sorted({s for s, _ in by_pkg.values()})
        if len(shas) < 2:
            continue
        acc = prof["accepted"].get(key)
        ok = bool(acc) and acc["winner"] in by_pkg and set(shas) <= set(acc["sha256"])
        out.append(dict(key=key, packages=sorted(by_pkg), sha256=shas, accepted=ok,
                        winner=acc["winner"] if ok else None))
    return out


def blocked_message(blocked):
    return ("Conflicting edits must be resolved before continuing: "
            + "; ".join(f"{c['key']} edited by {', '.join(c['packages'])}" for c in blocked)
            + ". Use accept-conflict ARCHIVE#MEMBER WINNER to choose a winner.")


# ------------------------------------------------------------------ host textures

def box_levels(width, height, rgba):
    """RGBA8 mip chain (bytes per level), 2x2 box filter per channel (straight alpha)."""
    levels = [bytes(rgba)]
    planes = [bytes(rgba[c::4]) for c in range(4)]
    w, h = width, height
    while w > 1 or h > 1:
        nw, nh = max(1, w >> 1), max(1, h >> 1)
        out = []
        for plane in planes:
            new = bytearray()
            for y in range(nh):
                r0 = plane[2 * y * w:(2 * y + 1) * w]
                y1 = min(2 * y + 1, h - 1)
                r1 = plane[y1 * w:(y1 + 1) * w]
                rows = []
                for r in (r0, r1):
                    odd = r[1::2] + (r[-1:] if len(r) % 2 else b"")
                    if not odd:
                        odd = r
                    rows.append((r[0::2][:nw], odd[:nw]))
                new += bytes((a + b + c + d + 2) >> 2 for a, b, c, d in zip(*rows[0], *rows[1]))
            out.append(bytes(new))
        planes, w, h = out, nw, nh
        merged = bytearray(w * h * 4)
        for c in range(4):
            merged[c::4] = planes[c]
        levels.append(bytes(merged))
    return levels


def eodhtx(png_path):
    from tex import read_png
    w, h, rgba = read_png(str(png_path))
    levels = box_levels(w, h, rgba)
    return b"EODHTX1\0" + struct.pack("<4H", w, h, len(levels), 0) + b"".join(levels)


def check_eodhtx(data):
    if data[:8] != b"EODHTX1\0":
        raise ValueError("Bad .eodhtx header")
    w, h, n, flags = struct.unpack_from("<4H", data, 8)
    size = sum(max(1, w >> i) * max(1, h >> i) * 4 for i in range(n))
    if flags or n != max(w, h).bit_length() or len(data) != 16 + size:
        raise ValueError("Bad .eodhtx level chain")
    return w, h, n


# ------------------------------------------------------------------ composition

def compose_stage(P, prof):
    """Build the next `active/` in a sibling temp dir; returns (stage, report). Touches nothing else."""
    disc = prof["disc"]
    blocked = [c for c in conflicts(P, prof) if not c["accepted"]]
    if blocked:
        raise ValueError(blocked_message(blocked))
    stage = Path(tempfile.mkdtemp(prefix=".active.tmp-", dir=P))
    try:
        won = {}  # conflict key -> (package id, entry) in profile order, accepted winner overriding
        manifests, lines = {}, []
        for pid in prof["order"]:
            rec = prof["packages"][pid]
            if not rec["enabled"]:
                continue
            folder = pkg_dir(P, pid, rec["version"])
            info = mp.verify(folder, disc)  # tamper, game and retail source hashes
            if info["manifestSha256"] != rec["manifestSha256"]:
                raise ValueError(f"Installed package {pid} changed after installation; reinstall it")
            m = manifests[pid] = mp.load_manifest(folder)
            lines.append(f"package {pid} {rec['version']} manifest {info['manifestSha256']}")
            for kind, items in (("native", m["native"]), ("host", m["host"])):
                for e in items:
                    key = (f"{e['archive']}#{e['member']}" if kind == "native" else "host:" + e["assetId"]).lower()
                    acc = prof["accepted"].get(key)
                    if key not in won or not acc or acc["winner"] == pid:
                        won[key] = (pid, kind, e)
        by_archive, hosts = {}, []
        for key, (pid, kind, e) in sorted(won.items()):
            folder = pkg_dir(P, pid, prof["packages"][pid]["version"])
            if kind == "native":
                by_archive.setdefault(e["archive"].lower(), []).append((pid, e, folder))
            else:
                hosts.append((pid, e, folder))
        report = dict(archives={}, host=len(hosts))
        for items in by_archive.values():
            path, spelled = mp.retail_archive(disc, items[0][1]["archive"])
            names = {x["name"].lower(): x["name"] for x in directory(path)}
            changes = {}
            for pid, e, folder in items:
                if e["member"].lower() not in names:
                    raise ValueError(f"{pid}: {spelled} has no member {e['member']}")
                changes[names[e["member"].lower()]] = (folder / e["file"]).read_bytes()
            built, method = mp.build_archive(path, changes)
            mp.atomic_write(stage / spelled, built)
            report["archives"][spelled] = dict(method=method, members=sorted(changes), sha256=mp.sha(built))
            lines.append(f"native {spelled} {mp.sha(built)} members {','.join(sorted(changes))}")
        if hosts:
            rows = ["eod-host-textures 1"]
            for n, (pid, e, folder) in enumerate(sorted(hosts, key=lambda h: h[1]["assetId"].lower())):
                rel = f"{pid}/{n:04d}-{e['indexKey']}.eodhtx"
                data = eodhtx(folder / e["file"])
                check_eodhtx(data)
                mp.atomic_write(stage / "HOSTTEX" / rel, data)
                rows.append("\t".join((e["indexKey"], e["paletteKey"], str(e["origWidth"]), str(e["origHeight"]),
                                       rel, e["assetId"], pid)))
                lines.append(f"host {e['assetId']} {pid} {mp.sha(data)}")
            mp.atomic_write(stage / "HOSTTEX/replacements.tsv", ("\n".join(rows) + "\n").encode("utf-8"))
        mp.atomic_write(stage / "PROFILE.TXT", ("\n".join([f"profile {prof['name']}", f"game {mp.slus_sha(disc)}"] + lines) + "\n").encode())
        for spelled in report["archives"]:  # validate what will be published
            directory(stage / spelled)
        return stage, report
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise


def commit(P, prof, stage):
    """Swap `stage` in as active/ and write profile.json; any failure restores the previous state."""
    active, old = P / "active", P / f".active.old-{os.getpid()}"
    shutil.rmtree(old, ignore_errors=True)
    had = active.exists()
    try:
        if had:
            os.rename(active, old)
        os.rename(stage, active)
        atomic_write(P / "profile.json", jdump(prof))
    except BaseException:
        if active.exists():
            shutil.rmtree(active, ignore_errors=True)
        if had and old.exists():
            os.rename(old, active)
        shutil.rmtree(stage, ignore_errors=True)
        raise
    shutil.rmtree(old, ignore_errors=True)


def apply(P, prof):
    stage, report = compose_stage(P, prof)
    commit(P, prof, stage)
    return report


def summary(prof, extra=None):
    out = dict(profile=prof["name"], disc=prof["disc"], order=prof["order"],
               enabled=[p for p in prof["order"] if prof["packages"][p]["enabled"]])
    out.update(extra or {})
    return out


# ------------------------------------------------------------------ operations

def install(package, disc=None, profile="default", root=None, enable=True):
    P = profile_dir(root, profile)
    with locked(P):
        prof = load(P, profile)
        disc = use_disc(prof, disc)
        package = Path(package).expanduser().resolve()
        with tempfile.TemporaryDirectory(prefix="eod-install-") as temp:
            if package.is_file():
                package = Path(mp.unzip(package, Path(temp) / "pkg")["path"])
            info = mp.verify(package, disc)
            m = mp.load_manifest(package)
            pid, version = m["id"], m["version"]
            check_deps(prof, m)
            previous = prof["packages"].get(pid)
            if previous and mp.semver_key(previous["version"]) >= mp.semver_key(version):
                raise ValueError(f"{pid} {previous['version']} is installed; uninstall it to install {version}")
            target = pkg_dir(P, pid, version)
            if target.exists():
                raise ValueError(f"Leftover folder from an interrupted install; remove it first: {target}")
            target.parent.mkdir(parents=True, exist_ok=True)
            staged = Path(tempfile.mkdtemp(prefix=f".{version}.tmp-", dir=target.parent))
            try:
                for rel in info["files"]:
                    mp.atomic_write(staged / rel, (package / rel).read_bytes())
                mp.verify(staged, disc)
                os.rename(staged, target)
            finally:
                shutil.rmtree(staged, ignore_errors=True)
        new = json.loads(json.dumps(prof))
        new["packages"][pid] = dict(version=version, enabled=enable, manifestSha256=info["manifestSha256"],
                                    files=[f"packages/{pid}/{version}/{rel}" for rel in info["files"]])
        if not previous:
            new["order"].append(pid)
        try:
            report = apply(P, new)
        except BaseException:
            shutil.rmtree(target, ignore_errors=True)
            with contextlib.suppress(OSError):
                target.parent.rmdir()
            raise
        if previous:
            remove_owned(P, previous["files"])
        return summary(new, dict(installed=f"{pid} {version}", compose=report))


def remove_owned(P, files):
    root = P.resolve()
    for rel in files:
        path = P / rel
        if root not in path.parent.resolve().parents and path.parent.resolve() != root:
            raise ValueError("Refusing to delete outside the profile: " + rel)
        if path.is_file() or path.is_symlink():
            path.unlink()
    for rel in files:  # prune only folders that became empty
        parent = (P / rel).parent
        while parent != root / "packages" and root in parent.parents:
            try:
                parent.rmdir()
            except OSError:
                break
            parent = parent.parent


def _mutate(profile, root, disc, change):
    P = profile_dir(root, profile)
    if not (P / "profile.json").is_file():
        raise ValueError(f"No profile named {profile}")
    with locked(P):
        prof = load(P, profile)
        use_disc(prof, disc)
        new = json.loads(json.dumps(prof))
        extra = change(P, new)
        report = apply(P, new)
        return summary(new, dict(extra or {}, compose=report)), prof


def set_enabled(pid, value, profile="default", root=None, disc=None):
    def change(P, new):
        rec = new["packages"].get(pid)
        if not rec:
            raise ValueError(f"{pid} is not installed")
        if value:
            check_deps(new, mp.load_manifest(pkg_dir(P, pid, rec["version"])), enabled_only=True)
        else:
            users = dependents(P, new, pid, enabled_only=True)
            if users:
                raise ValueError(f"{', '.join(users)} depend on {pid}; disable them first")
        rec["enabled"] = value
    return _mutate(profile, root, disc, change)[0]


def enable(pid, profile="default", root=None, disc=None):
    return set_enabled(pid, True, profile, root, disc)


def disable(pid, profile="default", root=None, disc=None):
    return set_enabled(pid, False, profile, root, disc)


def uninstall(pid, profile="default", root=None, disc=None):
    def change(P, new):
        rec = new["packages"].get(pid)
        if not rec:
            raise ValueError(f"{pid} is not installed")
        users = dependents(P, new, pid)
        if users:
            raise ValueError(f"{', '.join(users)} depend on {pid}; uninstall them first")
        del new["packages"][pid]
        new["order"].remove(pid)
        new["accepted"] = {k: v for k, v in new["accepted"].items() if v["winner"] != pid}
        return dict(removed=rec["files"])
    result, _ = _mutate(profile, root, disc, change)
    remove_owned(profile_dir(root, profile), result.pop("removed"))
    return result


def order(ids, profile="default", root=None, disc=None):
    def change(P, new):
        if sorted(ids) != sorted(new["order"]):
            raise ValueError("Order must list every installed package exactly once: " + ", ".join(new["order"]))
        new["order"] = list(ids)
    return _mutate(profile, root, disc, change)[0]


def accept_conflict(asset, winner, profile="default", root=None, disc=None):
    key = asset.lower() if asset.lower().startswith("host:") else mp.split_asset(asset) and asset.lower()

    def change(P, new):
        match = next((c for c in conflicts(P, new, True) if c["key"] == key), None)
        if not match:
            raise ValueError(f"{asset} is not a conflict between installed packages")
        if winner not in match["packages"]:
            raise ValueError(f"Winner must be one of: {', '.join(match['packages'])}")
        new["accepted"][key] = dict(winner=winner, sha256=match["sha256"])
    return _mutate(profile, root, disc, change)[0]


def compose(profile="default", root=None, disc=None):
    P = profile_dir(root, profile)
    if not (P / "profile.json").is_file():
        raise ValueError(f"No profile named {profile}")
    with locked(P):
        prof = load(P, profile)
        use_disc(prof, disc)
        return summary(prof, dict(compose=apply(P, prof)))


def status(profile="default", root=None):
    P = profile_dir(root, profile)
    prof = load(P, profile)
    packages = []
    for pid in prof["order"]:
        rec = prof["packages"][pid]
        try:
            m = mp.load_manifest(pkg_dir(P, pid, rec["version"]))
            detail = dict(native=len(m["native"]), host=len(m["host"]), dependencies=m["dependencies"], name=m["name"])
        except ValueError as error:
            detail = dict(error=str(error))
        packages.append(dict(id=pid, version=rec["version"], enabled=rec["enabled"], **detail))
    active = P / "active"
    try:
        found = conflicts(P, prof)
    except ValueError:
        found = []
    text = (active / "PROFILE.TXT")
    return dict(profile=prof["name"], root=str(P), disc=prof["disc"], packages=packages, order=prof["order"],
                conflicts=found, blocked=[c["key"] for c in found if not c["accepted"]],
                active=dict(path=str(active), composed=text.is_file(),
                            profileTxtSha256=mp.file_sha(text) if text.is_file() else None,
                            archives=sorted(str(p.relative_to(active)) for p in active.glob("DATA/**/*") if p.is_file()),
                            hostTextures=(active / "HOSTTEX/replacements.tsv").is_file()))


def launch_env(profile="default", root=None):
    active = profile_dir(root, profile) / "active"
    if not (active / "PROFILE.TXT").is_file():
        raise ValueError("Profile has no composed overlay; install and enable a package first")
    return dict(EOD_MOD_DIR=str(active))


# ------------------------------------------------------------------ CLI

def main(argv=None):
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--profile", default="default")
    common.add_argument("--root")
    common.add_argument("--disc")
    parser = argparse.ArgumentParser(description="Install and compose EOD mod packages in a development profile")
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name, arg in (("install", "package"), ("enable", "id"), ("disable", "id"), ("uninstall", "id")):
        sub.add_parser(name, parents=[common]).add_argument(arg)
    sub.add_parser("order", parents=[common]).add_argument("ids", nargs="+")
    p = sub.add_parser("accept-conflict", parents=[common])
    p.add_argument("asset"); p.add_argument("winner")
    for name in ("status", "compose", "launch-env"):
        sub.add_parser(name, parents=[common])
    a = parser.parse_args(argv)
    try:
        k = dict(profile=a.profile, root=a.root)
        if a.cmd == "install":
            result = install(a.package, a.disc, **k)
        elif a.cmd in ("enable", "disable", "uninstall"):
            result = globals()[a.cmd](a.id, disc=a.disc, **k)
        elif a.cmd == "order":
            result = order(a.ids, disc=a.disc, **k)
        elif a.cmd == "accept-conflict":
            result = accept_conflict(a.asset, a.winner, disc=a.disc, **k)
        elif a.cmd == "compose":
            result = compose(disc=a.disc, **k)
        elif a.cmd == "status":
            result = status(**k)
        else:
            result = launch_env(**k)
    except (ValueError, OSError) as error:
        print(json.dumps(dict(ok=False, error=str(error))))
        return 1
    print(json.dumps(result if a.cmd in ("status", "launch-env") else dict(ok=True, result=result), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
