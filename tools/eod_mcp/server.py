#!/usr/bin/env python3
"""eod-creation-kit MCP server: newline-delimited JSON-RPC 2.0 over stdio (stdlib only).

    python3 -I tools/eod_mcp/server.py

stdout carries protocol only; logs go to stderr. See docs/MCP_SERVER.md.
"""
import dis, inspect  # noqa: E401,F401  (load stdlib dis before tools/ can shadow it)
import base64
import json
import os
import sys
import threading
import traceback
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PKG_PARENT = str(Path(__file__).resolve().parents[1])
if PKG_PARENT not in sys.path:
    sys.path.append(PKG_PARENT)

from eod_mcp import PROTOCOL, VERSION, content, tools as T  # noqa: E402
from eod_mcp.safety import Config, ToolError  # noqa: E402

SUPPORTED = ("2025-06-18", "2025-03-26", "2024-11-05")


class Server:
    def __init__(self, ctx, out=None):
        self.ctx = ctx
        self.out = out or sys.stdout
        self.wlock = threading.Lock()
        self.pool = ThreadPoolExecutor(max_workers=8)
        self.cancelled = set()
        ctx.jobs.on_change = lambda: self.notify("notifications/resources/list_changed")

    def send(self, obj):
        line = json.dumps(obj, separators=(",", ":"))
        with self.wlock:
            self.out.write(line + "\n")
            self.out.flush()

    def notify(self, method, params=None):
        msg = dict(jsonrpc="2.0", method=method)
        if params is not None:
            msg["params"] = params
        self.send(msg)

    def error(self, rid, code, message, data=None):
        e = dict(code=code, message=message)
        if data is not None:
            e["data"] = data
        self.send(dict(jsonrpc="2.0", id=rid, error=e))

    # ---- handlers
    def initialize(self, p):
        want = p.get("protocolVersion")
        return dict(protocolVersion=want if want in SUPPORTED else PROTOCOL,
                    capabilities=dict(tools=dict(listChanged=True), resources=dict(subscribe=False, listChanged=True),
                                      prompts=dict(listChanged=False), logging={}),
                    serverInfo=dict(name="eod-creation-kit", title="EOD Creation Kit", version=VERSION),
                    instructions="Call eod_kit_status first. Writes are dry run first; apply needs plan_id. "
                                 "Never claim a gameplay change works without a capture from eod_test_run_start.")

    def tools_list(self, p):
        return dict(tools=[t.listing() for t in T.visible(self.ctx)])

    def tools_call(self, p, rid, meta):
        name, args = p.get("name"), p.get("arguments") or {}
        t = T.REG.get(name)
        if t is None:
            raise RpcError(-32602, f"Unknown tool: {name}")
        errs = T.validate(t.schema, args)
        if errs:
            raise RpcError(-32602, "Invalid arguments: " + "; ".join(errs[:5]), dict(errors=errs))
        token = (meta or {}).get("progressToken")
        if token is not None:
            counter = [0]

            def prog(done, total, message):
                counter[0] += 1
                params = dict(progressToken=token, progress=counter[0], message=f"{message}: {done}/{total}")
                if total:
                    params["total"] = total
                self.notify("notifications/progress", params)
            self.ctx.progress = prog
        else:
            self.ctx.progress = None
        try:
            out = T.call(self.ctx, name, args)
        except ToolError as e:
            return self.err_result(e.payload())
        except Exception as e:  # noqa: BLE001
            traceback.print_exc(file=sys.stderr)
            return self.err_result(dict(code="BACKEND_ERROR", message=f"{type(e).__name__}: {e}"))
        data = dict(out.data) if isinstance(out.data, dict) else dict(result=out.data)
        data = dict(summary=out.summary[:200], **data) if out.summary else data
        text = json.dumps(data, default=str)
        data = json.loads(text)
        content_blocks = [dict(type="text", text=text)]
        for png in out.images:
            content_blocks.append(dict(type="image", mimeType="image/png", data=base64.b64encode(png).decode()))
        for link in out.links:
            content_blocks.append(dict(type="resource_link", uri=link, name=link.rsplit("/", 1)[-1]))
        return dict(content=content_blocks, structuredContent=data, isError=False)

    @staticmethod
    def err_result(payload):
        return dict(content=[dict(type="text", text=json.dumps(dict(error=payload)))], structuredContent=dict(error=payload), isError=True)

    def resources_read(self, p):
        try:
            return dict(contents=[content.read_resource(self.ctx, p.get("uri", ""))])
        except ToolError as e:
            raise RpcError(-32002, e.message, e.payload())

    def prompts_get(self, p):
        try:
            return content.get_prompt(self.ctx, p.get("name"), p.get("arguments") or {})
        except ToolError as e:
            raise RpcError(-32602, e.message)

    def dispatch(self, method, p, rid, meta):
        if method == "initialize":
            return self.initialize(p)
        if method == "ping":
            return {}
        if method == "tools/list":
            return self.tools_list(p)
        if method == "tools/call":
            return self.tools_call(p, rid, meta)
        if method == "resources/list":
            return dict(resources=content.list_resources(self.ctx))
        if method == "resources/templates/list":
            return dict(resourceTemplates=content.list_templates())
        if method == "resources/read":
            return self.resources_read(p)
        if method == "prompts/list":
            return dict(prompts=content.list_prompts(self.ctx))
        if method == "prompts/get":
            return self.prompts_get(p)
        if method == "logging/setLevel":
            return {}
        raise RpcError(-32601, "Method not found: " + method)

    def handle(self, msg):
        rid, method = msg.get("id"), msg.get("method")
        p = msg.get("params") or {}
        try:
            result = self.dispatch(method, p, rid, p.get("_meta"))
            if rid is not None and rid not in self.cancelled:
                self.send(dict(jsonrpc="2.0", id=rid, result=result))
        except RpcError as e:
            if rid is not None:
                self.error(rid, e.code, e.message, e.data)
        except Exception as e:  # noqa: BLE001
            traceback.print_exc(file=sys.stderr)
            if rid is not None:
                self.error(rid, -32603, f"Internal error: {type(e).__name__}: {e}")

    def run(self, stdin=None):
        for line in (stdin or sys.stdin):
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                self.error(None, -32700, "Parse error")
                continue
            if not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0":
                self.error(msg.get("id") if isinstance(msg, dict) else None, -32600, "Invalid Request")
                continue
            method = msg.get("method")
            if method is None:
                continue  # a response to a server request; none are issued
            if "id" not in msg:
                if method == "notifications/cancelled":
                    self.cancelled.add(msg.get("params", {}).get("requestId"))
                continue
            self.pool.submit(self.handle, msg)
        self.pool.shutdown(wait=True)


class RpcError(Exception):
    def __init__(self, code, message, data=None):
        super().__init__(message)
        self.code, self.message, self.data = code, message, data


def main():
    hidden = tuple(x for x in os.environ.get("EOD_MCP_HIDE_MODULES", "").split(",") if x)
    try:
        ctx = T.Ctx(Config(), hidden)
    except ToolError as e:
        print(f"eod-creation-kit: {e.message}", file=sys.stderr)
        return 2
    out = sys.stdout
    sys.stdout = sys.stderr  # stray prints from backends must never corrupt the protocol
    Server(ctx, out).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
