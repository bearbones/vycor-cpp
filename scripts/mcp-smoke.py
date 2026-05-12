#!/usr/bin/env python3
# Copyright (c) 2026 The vycor-cpp Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
#
# MCP smoke harness. Spawns `vycor-cpp megascope` pointed at
# examples/deep_chains/, drives every tool through a realistic sequence,
# captures request/response pairs in scripts/mcp-smoke-out/, and produces
# a summary.md used by docs/mcp-review.md.
#
# Usage:
#   python3 scripts/mcp-smoke.py
#
# The harness exits 0 as long as the server answered every request —
# `isError: true` content responses are logged but do not fail the run,
# since exercising negative cases is an explicit goal.

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[1]
BIN = REPO / "build" / "src" / "vycor-cpp"
FIXTURE = REPO / "examples" / "deep_chains"
OUT = REPO / "scripts" / "mcp-smoke-out"


def frame(payload: dict) -> bytes:
    body = json.dumps(payload).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def read_framed(stream) -> dict | None:
    # Read headers.
    headers: dict[str, str] = {}
    while True:
        line = stream.readline()
        if not line:
            return None
        line = line.rstrip(b"\r\n")
        if line == b"":
            break
        k, _, v = line.decode("ascii", errors="replace").partition(":")
        headers[k.strip().lower()] = v.strip()
    length = int(headers.get("content-length", "0"))
    if length <= 0:
        return None
    body = b""
    while len(body) < length:
        chunk = stream.read(length - len(body))
        if not chunk:
            return None
        body += chunk
    return json.loads(body)


class Server:
    def __init__(self, sources: list[Path], build_path: Path,
                 entry_point: str):
        args = [str(BIN), "megascope",
                "--build-path", str(build_path),
                "--entry-point", entry_point]
        for s in sources:
            args += ["--source", str(s)]
        self.args = args
        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self._next_id = 1

    def call(self, method: str, params: dict | None = None,
             notification: bool = False) -> dict | None:
        msg: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if not notification:
            msg["id"] = self._next_id
            self._next_id += 1
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write(frame(msg))
        self.proc.stdin.flush()
        if notification:
            return None
        return read_framed(self.proc.stdout)

    def shutdown(self) -> str:
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        return self.proc.stderr.read().decode("utf-8", errors="replace")


def dump(step: int, label: str, req: dict | None, resp: dict | None):
    path = OUT / f"{step:02d}-{label}.json"
    obj = {"request": req, "response": resp}
    path.write_text(json.dumps(obj, indent=2) + "\n")


def summarize(rows: list[dict], stderr: str):
    lines = [
        "# MCP smoke run summary",
        "",
        f"Server command: `{shlex.join(rows[0]['server_args'])}`" if rows else "",
        "",
        "| step | label | method | ok | isError | note |",
        "|---:|---|---|---|---|---|",
    ]
    for r in rows:
        lines.append(
            "| {step} | {label} | {method} | {ok} | {ie} | {note} |".format(**r))
    lines += ["", "## Server stderr", "", "```", stderr.rstrip(), "```", ""]
    (OUT / "summary.md").write_text("\n".join(lines) + "\n")


def tool_call(server: Server, step: int, name: str,
              args: dict, rows: list[dict]):
    params = {"name": name, "arguments": args}
    resp = server.call("tools/call", params)
    req_for_dump = {"jsonrpc": "2.0", "id": server._next_id - 1,
                    "method": "tools/call", "params": params}
    dump(step, name, req_for_dump, resp)
    is_err = False
    note = ""
    if resp is None:
        note = "no response"
    else:
        result = resp.get("result") or {}
        err = resp.get("error")
        if err:
            note = f"error {err.get('code')}: {err.get('message', '')[:60]}"
        elif isinstance(result, dict) and result.get("isError"):
            is_err = True
            content = result.get("content") or []
            if content and isinstance(content[0], dict):
                note = content[0].get("text", "")[:80]
    rows.append({
        "step": step, "label": name, "method": "tools/call",
        "ok": resp is not None, "ie": is_err, "note": note,
        "server_args": server.args,
    })


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixture", default=str(FIXTURE))
    ap.add_argument("--entry-point", default="main")
    args = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.json"):
        old.unlink()
    for old in OUT.glob("summary.md"):
        old.unlink()

    fixture = Path(args.fixture)
    cc = fixture / "compile_commands.json"
    if not cc.exists():
        print(f"error: {cc} missing; run gen_compile_commands.sh first",
              file=sys.stderr)
        return 2

    sources = sorted(fixture.glob("*.cpp"))
    if not BIN.exists():
        print(f"error: {BIN} not built; run cmake --build build first",
              file=sys.stderr)
        return 2

    server = Server(sources, fixture, args.entry_point)
    rows: list[dict] = []
    step = 0

    try:
        # 1. initialize
        step += 1
        resp = server.call("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "mcp-smoke", "version": "0.1"},
        })
        dump(step, "initialize",
             {"method": "initialize"}, resp)
        rows.append({"step": step, "label": "initialize",
                     "method": "initialize", "ok": resp is not None,
                     "ie": False, "note": "",
                     "server_args": server.args})

        # notifications/initialized (no response expected)
        server.call("notifications/initialized", {}, notification=True)

        # 2. tools/list
        step += 1
        resp = server.call("tools/list", {})
        dump(step, "tools_list", {"method": "tools/list"}, resp)
        tools = []
        if resp and resp.get("result"):
            tools = resp["result"].get("tools", [])
        rows.append({"step": step, "label": "tools/list",
                     "method": "tools/list", "ok": resp is not None,
                     "ie": False, "note": f"{len(tools)} tools listed",
                     "server_args": server.args})

        # 3. happy-path tool calls (one per tool)
        step += 1; tool_call(server, step, "lookup_function",
                             {"name": "Pipeline::run"}, rows)
        step += 1; tool_call(server, step, "get_callees",
                             {"name": "Pipeline::run",
                              "min_confidence": "Plausible"}, rows)
        step += 1; tool_call(server, step, "get_callers",
                             {"name": "stage5_sink"}, rows)
        step += 1; tool_call(server, step, "find_call_chain",
                             {"from": "main", "to": "Registry::invoke",
                              "max_depth": 10, "max_paths": 5}, rows)
        step += 1; tool_call(server, step, "query_exception_safety",
                             {"function": "stage4_dispatch"}, rows)
        step += 1; tool_call(server, step, "query_call_site_context",
                             {"call_site":
                                 f"{fixture}/stage4_dispatch.cpp:21:5"}, rows)
        step += 1; tool_call(server, step, "analyze_dead_code",
                             {"entry_points": ["main"]}, rows)
        step += 1; tool_call(server, step, "get_class_hierarchy",
                             {"class_name": "Plugin",
                              "include_transitive": True,
                              "include_overrides": True}, rows)

        # 4. adversarial — unknown function name
        step += 1; tool_call(server, step, "lookup_function",
                             {"name": "::does_not_exist"}, rows)

        # 5. adversarial — missing required arg
        step += 1; tool_call(server, step, "get_callees", {}, rows)

        # 6. adversarial — chain with no path
        step += 1; tool_call(server, step, "find_call_chain",
                             {"from": "Pipeline::runAsync",
                              "to": "cbs::startupHook",
                              "max_depth": 2}, rows)

        # 7. adversarial — malformed call_site
        step += 1; tool_call(server, step, "query_call_site_context",
                             {"call_site": "nope"}, rows)

        # 8. adversarial — unknown method
        step += 1
        resp = server.call("tools/run", {"name": "lookup_function"})
        dump(step, "unknown_method",
             {"method": "tools/run"}, resp)
        note = ""
        if resp and resp.get("error"):
            note = f"code={resp['error'].get('code')}"
        rows.append({"step": step, "label": "unknown_method",
                     "method": "tools/run", "ok": resp is not None,
                     "ie": False, "note": note,
                     "server_args": server.args})

        # 9. Chain B happy path — virtual dispatch through Scheduler
        step += 1; tool_call(server, step, "get_callees",
                             {"name": "Scheduler::schedule"}, rows)
        step += 1; tool_call(server, step, "find_call_chain",
                             {"from": "main", "to": "tcpWriteBytes",
                              "max_depth": 10, "max_paths": 5}, rows)

    finally:
        stderr = server.shutdown()

    summarize(rows, stderr)
    ok = all(r["ok"] for r in rows)
    print(f"{len(rows)} requests, all ok={ok}. Artifacts in {OUT}/")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
