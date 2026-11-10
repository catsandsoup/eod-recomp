"""Resources and prompts."""
import base64
import json
import mimetypes
from pathlib import Path

from .safety import NAME, ToolError, redact
from . import tools as T

REPO = T.TOOLS_DIR.parent
DOCS = ("CREATION_KIT_TEXTURE_CONTRACTS", "TEXTURE_BATCH_EXPORT", "MOD_PROFILES", "MCP_SERVER", "MODDER_JOURNEYS", "MCP_SERVER_SPEC")
TEXT_EXT = {".json", ".txt", ".md", ".obj", ".mtl", ".tsv", ".log", ".jsonl"}
MAX_RES = 8_000_000

TEMPLATES = [
    ("eod://census/assets{?role,type,archive}", "Filtered census assets", "application/json"),
    ("eod://asset/{archive}/{member}", "One census record", "application/json"),
    ("eod://asset/{archive}/{member}/preview.png", "Mip-zero preview", "image/png"),
    ("eod://manifest/{run_id}", "Export manifest", "application/json"),
    ("eod://export/{run_id}/{path}", "Exported file", "application/octet-stream"),
    ("eod://project/{name}", "Project edits", "application/json"),
    ("eod://profile/{name}", "Profile description", "application/json"),
    ("eod://package/{path}", "Validated package manifest", "application/json"),
    ("eod://jobs/{job_id}", "Job state", "application/json"),
    ("eod://run/{run_id}/capture.png", "Capture", "image/png"),
    ("eod://run/{run_id}/game.log", "Game log (redacted)", "text/plain"),
    ("eod://run/{run_id}/meta.json", "Run meta", "application/json"),
    ("eod://docs/{name}", "Whitelisted docs", "text/markdown"),
]


def _j(uri, obj):
    return dict(uri=uri, mimeType="application/json", text=json.dumps(obj, indent=1, default=str))


def list_resources(ctx):
    out = [dict(uri="eod://status", name="status", mimeType="application/json"),
           dict(uri="eod://census", name="census summary", mimeType="application/json"),
           dict(uri="eod://audit", name="audit log (last 200)", mimeType="application/jsonl")]
    out += [dict(uri=f"eod://docs/{d}", name=d, mimeType="text/markdown") for d in DOCS if (REPO / "docs" / f"{d}.md").is_file()]
    ws = ctx.config.workspace
    for j in ctx.jobs.list()[:50]:
        out.append(dict(uri=f"eod://jobs/{j['job_id']}", name=f"job {j['kind']} {j['state']}", mimeType="application/json"))
    if (ws / "exports").is_dir():
        for d in sorted((ws / "exports").iterdir()):
            if (d / "export-manifest.json").is_file():
                out.append(dict(uri=f"eod://manifest/{d.name}", name=f"manifest {d.name}", mimeType="application/json"))
    if (ws / "captures").is_dir():
        for d in sorted((ws / "captures").iterdir())[-30:]:
            if (d / "capture.png").is_file():
                out.append(dict(uri=f"eod://run/{d.name}/capture.png", name=f"capture {d.name}", mimeType="image/png"))
    return out


def list_templates():
    return [dict(uriTemplate=u, name=n, mimeType=m) for u, n, m in TEMPLATES]


def _file(ctx, uri, rel):
    p = ctx.config.ws_path(rel, must_exist=True, what="resource")
    if not p.is_file() or p.stat().st_size > MAX_RES:
        raise ToolError("UNSUPPORTED", "Resource is not a readable file within 8 MB")
    mime = mimetypes.guess_type(p.name)[0] or "application/octet-stream"
    if p.suffix.lower() in TEXT_EXT:
        text = redact(p.read_text(errors="replace")) if p.suffix.lower() in (".log", ".txt") else p.read_text(errors="replace")
        return dict(uri=uri, mimeType="application/json" if p.suffix == ".json" else mime if mime != "application/octet-stream" else "text/plain", text=text)
    return dict(uri=uri, mimeType=mime, blob=base64.b64encode(p.read_bytes()).decode())


def read_resource(ctx, uri):
    if not uri.startswith("eod://"):
        raise ToolError("UNSUPPORTED", "Unknown URI scheme")
    path, _, query = uri[6:].partition("?")
    parts = path.split("/")
    kind = parts[0]
    if kind == "status":
        return _j(uri, T.call(ctx, "eod_kit_status", {}).data)
    if kind == "census" and len(parts) == 1:
        c = ctx.census()
        return _j(uri, dict(totals=c["totals"], disc=c["disc"], created=c["created"]))
    if kind == "census" and parts[1] == "assets":
        q = dict(x.split("=", 1) for x in query.split("&") if "=" in x)
        return _j(uri, T.call(ctx, "eod_asset_search", {k: v for k, v in q.items() if k in ("role", "type", "archive")}).data)
    if kind == "audit":
        return dict(uri=uri, mimeType="application/jsonl", text="\n".join(ctx.audit.tail(200)))
    if kind == "docs" and len(parts) == 2:
        if parts[1] not in DOCS:
            raise ToolError("UNSAFE_PATH", "Doc not whitelisted")
        return dict(uri=uri, mimeType="text/markdown", text=(REPO / "docs" / f"{parts[1]}.md").read_text())
    if kind == "asset":
        rest = "/".join(parts[1:])
        if rest.endswith("/preview.png"):
            aid = rest[:-len("/preview.png")]
            archive, _, member = aid.rpartition("/")
            w, h, px, _ = ctx.rgba(f"{archive}#{member}")
            from . import imaging
            return dict(uri=uri, mimeType="image/png", blob=base64.b64encode(imaging.fit(w, h, px)[2]).decode())
        archive, _, member = rest.rpartition("/")
        return _j(uri, T.call(ctx, "eod_asset_get", dict(asset_id=f"{archive}#{member}")).data)
    if kind == "manifest" and len(parts) == 2 and NAME.fullmatch(parts[1]):
        return _file(ctx, uri, f"exports/{parts[1]}/export-manifest.json")
    if kind == "export" and len(parts) >= 3 and NAME.fullmatch(parts[1]):
        return _file(ctx, uri, f"exports/{parts[1]}/" + "/".join(parts[2:]))
    if kind == "project" and len(parts) == 2:
        return _file(ctx, uri, f"projects/{ctx.config.project_file(parts[1]).parent.name}/project.json")
    if kind == "profile" and len(parts) == 2:
        return _j(uri, T.call(ctx, "eod_profile_describe", dict(name=parts[1])).data)
    if kind == "package" and len(parts) >= 2:
        return _j(uri, T.call(ctx, "eod_package_inspect", dict(path="/".join(parts[1:]))).data)
    if kind == "jobs" and len(parts) == 2:
        return _j(uri, ctx.jobs.status(parts[1]))
    if kind == "run" and len(parts) == 3 and NAME.fullmatch(parts[1]) and parts[2] in ("capture.png", "game.log", "meta.json"):
        return _file(ctx, uri, f"captures/{parts[1]}/{parts[2]}")
    raise ToolError("UNSUPPORTED", "Unknown resource: " + uri)


# ------------------------------------------------------------------ prompts

def _p(name, desc, args, body, needs=()):
    return dict(name=name, description=desc, arguments=[dict(name=k, description=v, required=True) for k, v in args.items()], body=body, needs=needs)


COMMON = ("Rules: use only eod_* tools; never edit the disc or installed apps; every write is dry run first, show the plan, then apply with plan_id; "
          "dev-* profiles only; never claim success without a capture or log; if the game cannot run, say the change is unverified in game.\n\n")

PROMPTS = [
    _p("texture_pack_workflow", "Replace a scope of textures with higher-resolution art", dict(scope="role (sky, vehicle-paint, ...) or archive", scale="2 or 4", pack_id="package id"),
       COMMON + "Steps for scope={scope}, scale={scale}x, pack {pack_id}:\n1. eod_kit_status; confirm tools and disc.\n2. eod_census_start (poll eod_job_status) if no census.\n"
       "3. eod_asset_search for the scope; eod_asset_preview with sheet:true.\n4. eod_texture_export_start dry run, show counts, apply, poll with wait_seconds.\n"
       "5. The user upscales PNGs outside the Kit; ask for the folder. eod_texture_compare each candidate (sizes must be orig*{scale}, indexed).\n"
       "6. eod_texture_replace mode host (dry run, apply) per asset.\n7. eod_package_build (dry run, apply), eod_package_validate.\n"
       "8. eod_profile_create dev-<name>; eod_mod_install (plan, apply).\n9. eod_test_run_start if runs are enabled; eod_capture_get; eod_log_tail grep HOSTTEX for hit/miss.\n"
       "10. Report unmatched assets and what is unverified."),
    _p("share_mod", "Package and verify a project for sharing", dict(project="project name"),
       COMMON + "For project {project}: eod_project_validate; eod_package_build (dry run, apply); eod_package_validate; eod_package_inspect; "
       "eod_profile_create on a fresh dev- profile and eod_conflicts_check, then eod_mod_install to prove it installs."),
    _p("new_livery", "Repaint a vehicle", dict(vehicle="vehicle name", style="description"),
       COMMON + "Vehicle {vehicle}, style {style}: eod_asset_search role vehicle-paint; export; the user edits; eod_texture_compare; eod_texture_replace; package; test; show the capture."),
    _p("debug_missing_replacement", "Find why a replacement did not appear", dict(run_id="run id"),
       COMMON + "For run {run_id}: eod_log_tail grep HOSTTEX; check eod_asset_get runtimeKeyShared (ambiguous keys never bind); confirm EOD_HOST_TEXTURES is not 0; "
       "eod_capture_get; compare with eod_capture_diff against a baseline run."),
    _p("cleanup_profile", "Remove mods from a profile safely", dict(profile="profile name"),
       COMMON + "For {profile}: eod_profile_describe; for each package ask the user, then eod_mod_disable or eod_mod_uninstall (confirm:<id>)."),
    _p("remove_boundary", "Lower or remove a track wall", dict(track="track", where="location or record hints"),
       COMMON + "Track {track}, near {where}: eod_collision_query; show the tallest walls with record indices; ask the user to confirm indices; eod_collision_edit dry run, apply; "
       "state clearly that the result is a patched archive copy that is unverified in game and not packaged automatically.", ("world_collision",)),
    _p("adjust_props", "Move or remove props", dict(track="track", selector="what", change="how"),
       COMMON + "Export the world with eod_world_export_start and inspect placements.json; edits are not yet exposed as a tool.", ("world_export",)),
]


def list_prompts(ctx):
    return [dict(name=p["name"], description=p["description"], arguments=p["arguments"]) for p in PROMPTS if not any(ctx.modules.get(m) for m in p["needs"])]


def get_prompt(ctx, name, args):
    p = next((x for x in PROMPTS if x["name"] == name and not any(ctx.modules.get(m) for m in x["needs"])), None)
    if not p:
        raise ToolError("UNSUPPORTED", "Unknown prompt: " + name)
    missing = [a["name"] for a in p["arguments"] if a["name"] not in args]
    if missing:
        raise ToolError("NOT_CONFIGURED", "Missing prompt arguments: " + ", ".join(missing))
    text = p["body"]
    for k, v in args.items():
        text = text.replace("{" + k + "}", str(v))
    return dict(description=p["description"], messages=[dict(role="user", content=dict(type="text", text=text))])
