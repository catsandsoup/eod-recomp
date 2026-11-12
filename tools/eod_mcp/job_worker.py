"""Child process for long jobs: python3 -I job_worker.py <job_dir>.

Reads job.json {kind, params}, appends progress.jsonl lines, writes result.json.
tools/ is appended to sys.path so tools/ modules never shadow the stdlib.
"""
import dis, inspect, json, os, signal, subprocess, sys, threading, time  # noqa: E401
from pathlib import Path

TOOLS = str(Path(__file__).resolve().parents[1])
sys.path.append(TOOLS)

job_dir = Path(sys.argv[1]) if __name__ == "__main__" else None  # kit_service/worker.py sets its own sinks
cancel = threading.Event()
child = {"proc": None}


def on_term(signum, frame):
    cancel.set()
    p = child["proc"]
    if p and p.poll() is None:
        try:
            os.killpg(p.pid, signal.SIGTERM)
        except OSError:
            pass


def install_signals():
    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)


def progress(done=None, total=None, message="", current=None):
    with open(job_dir / "progress.jsonl", "a") as f:
        f.write(json.dumps(dict(ts=time.time(), done=done, total=total, message=message, current=current)) + "\n")


def write_result(obj):
    tmp = job_dir / "result.json.tmp"
    tmp.write_text(json.dumps(obj, indent=1, default=str))
    os.replace(tmp, job_dir / "result.json")


DELAY = float(os.environ.get("EOD_MCP_JOB_DELAY", "0") or 0)  # test hook: slow progress so cancel can be exercised


def census(p):
    import asset_registry
    out = Path(p["out"])

    def cb(ev):
        if ev["done"] % 25 == 0 or ev["done"] == ev["total"]:
            progress(ev["done"], ev["total"], "census", ev.get("assetId"))
    progress(0, None, "inventory")
    result = asset_registry.census(p["disc"], p.get("jobs", 1), cb, cancel)
    if result.get("cancelled"):
        return dict(ok=True, status="cancelled", message="census cancelled; nothing cached")
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp")
    tmp.write_text(json.dumps(result))
    os.replace(tmp, out)
    Path(p["meta"]).write_text(json.dumps(dict(slusSha256=result["disc"]["slusSha256"], tool="asset_registry",
                                              created=result["created"])))
    return dict(ok=True, status="complete", totals=result["totals"], seconds=result["seconds"], path=str(out))


def texture_export(p):
    import texture_export as te

    def cb(ev):
        if ev["event"] == "progress":
            progress(ev["done"], ev["total"], "export", ev.get("assetId"))
            time.sleep(DELAY)
        elif ev["event"] == "start":
            progress(0, ev["total"], "start")
    m = te.export_batch(p["disc"], p["dest"], select=p.get("select", ()), archives=p.get("archives", ()),
                        role=p.get("role"), resume=p.get("resume", False), jobs=p.get("jobs", 1),
                        progress=cb, cancel=cancel)
    st = m["run"]["status"]
    return dict(ok=st in ("complete", "cancelled"), status=st, summary=m["summary"], manifest=str(Path(p["dest"]) / te.MANIFEST),
                done=m["run"]["done"], total=m["run"]["total"])


def cli(p):
    import importlib, io, contextlib
    mod = importlib.import_module(p["module"])
    progress(0, 1, "running " + p["module"])
    buf, err = io.StringIO(), io.StringIO()
    code = 0
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
        try:
            code = mod.main(p["argv"]) or 0
        except SystemExit as e:
            code = e.code if isinstance(e.code, int) else 1
        except Exception as e:  # noqa: BLE001
            err.write(f"{type(e).__name__}: {e}\n")
            code = 1
    progress(1, 1, "finished")
    return dict(ok=code == 0, status="complete" if code == 0 else "failed", exit=code, stdout=buf.getvalue()[-8000:],
                stderr=err.getvalue()[-4000:], outputs=p.get("outputs", {}))


def test_run(p):
    run = Path(p["run_dir"])
    run.mkdir(parents=True, exist_ok=True)
    log = run / "game.log"
    progress(0, 1, "launching game")
    started = time.time()
    with open(log, "wb") as lf:
        proc = subprocess.Popen([p["executable"], *p["arguments"]], env=p["env"], cwd=p.get("cwd") or str(run),
                                stdout=lf, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        child["proc"] = proc
        timed_out = False
        while proc.poll() is None:
            if cancel.is_set():
                break
            if time.time() - started > p["timeout_s"]:
                timed_out = True
                break
            progress(int(time.time() - started), p["timeout_s"], "game running")
            time.sleep(0.5)
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except OSError:
                pass
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
    code = proc.returncode
    capture = Path(p["capture"])
    have = capture.is_file() and capture.stat().st_size > 0
    text = log.read_text(errors="replace")
    import re
    host = dict(loaded=len(re.findall(r"HOSTTEX.*(?:load|replacement)", text, re.I)),
                hit=len(re.findall(r"HOSTTEX.*\bhit\b", text, re.I)), miss=len(re.findall(r"HOSTTEX.*\bmiss\b", text, re.I)),
                fallback=len(re.findall(r"HOSTTEX.*fallback", text, re.I)))
    status = "cancelled" if cancel.is_set() else ("failed" if not have else "complete")
    meta = dict(exitCode=code if code is None or code >= 0 else None, signal=-code if code is not None and code < 0 else None,
                timedOut=timed_out, seconds=round(time.time() - started, 1), env=sorted(p["env"]), arguments=p["arguments"],
                captureProduced=have, executableSha256=p.get("executable_sha"))
    (run / "meta.json").write_text(json.dumps(meta, indent=1))
    warnings = []
    if not have:
        warnings.append("No capture was produced; the run is NOT evidence that the change works.")
    if timed_out:
        warnings.append("Game hit the timeout and was terminated.")
    return dict(ok=have and not cancel.is_set(), status=status, capture_png=str(capture) if have else None, game_log=str(log),
                exit=dict(code=meta["exitCode"], signal=meta["signal"]), warnings=warnings, hosttex=host, run_id=p["run_id"])


KINDS = dict(census=census, texture_export=texture_export, cli=cli, test_run=test_run)

if __name__ == "__main__":
    install_signals()
    job = json.loads((job_dir / "job.json").read_text())
    try:
        res = KINDS[job["kind"]](job["params"])
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        res = dict(ok=False, status="failed", error=f"{type(e).__name__}: {e}")
    write_result(res)
    sys.exit(0 if res.get("ok") else 1)
