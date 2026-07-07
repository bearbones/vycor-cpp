#!/usr/bin/env python3
# Copyright (c) 2026 The vycor-cpp Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Efficiency-analysis harness for the megascope MCP server.
#
# Measures, for a given binary + compilation database:
#   * cold index bake: wall time, per-phase and per-TU parse timings,
#     parse-failure/crash counts, graph sizes, peak RSS
#     (via megascope --stats-json)
#   * snapshot save/load and warm-start cost (--snapshot)
#   * MCP tool query latencies over newline-delimited stdio (--queries)
#
# Outputs a self-contained run-report JSON plus a human summary, and can
# diff two run reports (--compare) for A/B work.
#
# Examples:
#   # Cold bake + query latencies over vycor-cpp's own sources:
#   python3 scripts/bench.py --binary build-release/src/vycor-cpp \
#       --build-path build-release --source-re '/vycor-cpp/src/' \
#       --queries --label baseline --out bench-out
#
#   # Include snapshot warm-start measurement:
#   python3 scripts/bench.py ... --snapshot
#
#   # Compare two runs:
#   python3 scripts/bench.py --compare bench-out/baseline.json bench-out/pr.json

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


# ---------------------------------------------------------------------------
# MCP client (newline-delimited JSON-RPC over stdio)
# ---------------------------------------------------------------------------

class McpClient:
    def __init__(self, proc: subprocess.Popen):
        self.proc = proc
        self._id = 0

    def request(self, method: str, params: dict | None = None) -> dict:
        self._id += 1
        msg = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            msg["params"] = params
        line = json.dumps(msg, separators=(",", ":")) + "\n"
        self.proc.stdin.write(line.encode())
        self.proc.stdin.flush()
        while True:
            resp_line = self.proc.stdout.readline()
            if not resp_line:
                raise RuntimeError(f"server closed pipe during {method}")
            resp_line = resp_line.strip()
            if not resp_line:
                continue
            resp = json.loads(resp_line)
            if resp.get("id") == self._id:
                return resp

    def tool(self, name: str, args: dict | None = None) -> dict:
        resp = self.request("tools/call",
                            {"name": name, "arguments": args or {}})
        result = resp.get("result", {})
        content = result.get("content", [])
        if content and content[0].get("type") == "text":
            try:
                return json.loads(content[0]["text"])
            except (json.JSONDecodeError, KeyError):
                return {"raw": content[0].get("text")}
        return result


# ---------------------------------------------------------------------------
# Source selection
# ---------------------------------------------------------------------------

def select_sources(build_path: Path, source_re: str, max_tus: int) -> list[str]:
    cc_path = build_path / "compile_commands.json"
    if not cc_path.exists():
        sys.exit(f"error: {cc_path} not found")
    entries = json.loads(cc_path.read_text())
    pattern = re.compile(source_re)
    seen: set[str] = set()
    files: list[str] = []
    for e in entries:
        f = e["file"]
        if not os.path.isabs(f):
            f = os.path.normpath(os.path.join(e.get("directory", "."), f))
        if pattern.search(f) and f not in seen:
            seen.add(f)
            files.append(f)
    if max_tus > 0:
        files = files[:max_tus]
    return files


# ---------------------------------------------------------------------------
# Server lifecycle
# ---------------------------------------------------------------------------

READY_MARKER = b"server started, waiting for requests"


def launch_megascope(binary: Path, build_path: Path, files: list[str],
                     extra_args: list[str], threads: int,
                     stats_json: Path, snapshot: Path | None,
                     collapse: list[str],
                     log_path: Path) -> tuple[subprocess.Popen, float, str]:
    """Start megascope; wait until ready. Returns (proc, wall_to_ready_s, log)."""
    cmd = [str(binary), "megascope", "--build-path", str(build_path),
           "--threads", str(threads), "--stats-json", str(stats_json)]
    for f in files:
        cmd += ["--source", f]
    for a in extra_args:
        cmd += [f"--extra-arg={a}"]
    for c in collapse:
        cmd += ["--collapse-paths", c]
    if snapshot is not None:
        cmd += ["--snapshot", str(snapshot)]

    log = open(log_path, "wb")
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=log)
    # Wait for readiness by polling the stderr log file for the marker.
    ready_s = None
    while proc.poll() is None:
        data = log_path.read_bytes()
        if READY_MARKER in data:
            ready_s = time.monotonic() - t0
            break
        time.sleep(0.05)
    if ready_s is None:
        raise RuntimeError(
            f"megascope exited before ready (see {log_path})")
    return proc, ready_s, str(log_path)


def shutdown(proc: subprocess.Popen):
    try:
        proc.stdin.close()
        proc.wait(timeout=30)
    except Exception:
        proc.kill()


# ---------------------------------------------------------------------------
# Query benchmark
# ---------------------------------------------------------------------------

def timed(fn, reps: int) -> dict:
    samples = []
    for _ in range(reps):
        t0 = time.perf_counter()
        fn()
        samples.append((time.perf_counter() - t0) * 1000.0)
    return {
        "min_ms": min(samples),
        "median_ms": statistics.median(samples),
        "max_ms": max(samples),
        "reps": reps,
    }


def run_query_benchmark(client: McpClient, reps: int) -> dict:
    out: dict[str, dict] = {}

    client.request("initialize", {
        "protocolVersion": "2025-06-18",
        "capabilities": {},
        "clientInfo": {"name": "bench.py", "version": "1"},
    })

    summary = client.tool("graph_summary")
    out["_graph_summary"] = summary

    # Discover interesting targets: search for common substrings, take the
    # highest-caller-count function we can find among candidates.
    candidates: list[str] = []
    for probe in ("run", "get", "process", "main", "a"):
        res = client.tool("search_functions",
                          {"query": probe, "limit": 20})
        for m in (res.get("matches") or res.get("results") or []):
            name = m.get("qualifiedName") or m.get("name")
            if name:
                candidates.append(name)
        if len(candidates) >= 20:
            break
    if not candidates:
        candidates = ["main"]

    hub = None
    hub_callers = -1
    for name in candidates[:20]:
        res = client.tool("get_callers", {"name": name})
        n = len(res.get("callers", []))
        if n > hub_callers:
            hub, hub_callers = name, n
    target = hub or candidates[0]
    out["_target"] = {"name": target, "callers": hub_callers}

    out["graph_summary"] = timed(lambda: client.tool("graph_summary"), reps)
    out["search_functions"] = timed(
        lambda: client.tool("search_functions", {"query": "run", "limit": 50}),
        reps)
    out["lookup_function"] = timed(
        lambda: client.tool("lookup_function", {"name": target}), reps)
    out["get_callers_hub"] = timed(
        lambda: client.tool("get_callers", {"name": target}), reps)
    out["get_callees_hub"] = timed(
        lambda: client.tool("get_callees", {"name": target}), reps)
    out["find_call_chain"] = timed(
        lambda: client.tool("find_call_chain",
                            {"from": "main", "to": target,
                             "max_depth": 10, "max_paths": 5}), reps)
    out["list_entry_points"] = timed(
        lambda: client.tool("list_entry_points"), reps)
    out["analyze_dead_code"] = timed(
        lambda: client.tool("analyze_dead_code"), max(1, reps // 3))
    return out


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def tu_digest(stats: dict) -> dict:
    tus = stats.get("tu", [])
    if not tus:
        return {}
    by_phase: dict[int, list[float]] = {}
    for t in tus:
        by_phase.setdefault(t["phase"], []).append(t["ms"])
    digest = {}
    for phase, ms in sorted(by_phase.items()):
        digest[f"phase{phase}"] = {
            "n": len(ms),
            "sum_ms": sum(ms),
            "avg_ms": sum(ms) / len(ms),
            "max_ms": max(ms),
        }
    slowest = sorted(tus, key=lambda t: -t["ms"])[:5]
    digest["slowest"] = [
        {"file": os.path.basename(t["file"]), "phase": t["phase"],
         "ms": round(t["ms"], 1)} for t in slowest]
    return digest


def summarize(report: dict) -> str:
    lines = []
    s = report.get("cold_stats", {})
    g = s.get("graph", {})
    lines.append(f"# bench: {report['label']}")
    lines.append(f"binary: {report['binary']}")
    lines.append(f"TUs: {s.get('files')}  threads: {s.get('threads')}")
    if s.get("phase1_wall_ms"):
        phases = (f"(phase1 {s.get('phase1_wall_ms', 0)/1000:.2f}s, "
                  f"phase2+3 {s.get('phase2_wall_ms', 0)/1000:.2f}s), ")
    else:
        phases = "(single-parse), "
    lines.append(
        f"cold bake: {s.get('bake_wall_ms', 0)/1000:.2f}s wall "
        + phases
        + f"ready-to-serve {report.get('cold_ready_s', 0):.2f}s")
    lines.append(
        f"graph: {g.get('nodes')} nodes, {g.get('edges')} edges, "
        f"{g.get('call_sites')} call sites, "
        f"interner {g.get('interner_strings')} strings "
        f"({(g.get('interner_payload_bytes') or 0)/1e6:.2f} MB)")
    lines.append(
        f"outcomes: {s.get('parse_errors')} parse errors, "
        f"{s.get('crashes')} crashes; peak RSS "
        f"{(s.get('peak_rss_kb') or 0)/1e6:.2f} GB")
    snap = report.get("warm_stats", {}).get("snapshot", {})
    if snap.get("loaded"):
        ws = report["warm_stats"]
        lines.append(
            f"warm start: ready {report.get('warm_ready_s', 0):.2f}s "
            f"(snapshot load {snap.get('load_ms', 0)/1000:.2f}s, "
            f"{snap.get('refreshed_tus')} refreshed); snapshot size "
            f"{report.get('snapshot_bytes', 0)/1e6:.2f} MB; peak RSS "
            f"{(ws.get('peak_rss_kb') or 0)/1e6:.2f} GB")
    ri = report.get("reindex")
    if ri:
        lines.append(
            f"reindex_tu ({os.path.basename(ri['tu'])}): "
            f"{ri['median_ms']:.0f} ms median")
    q = report.get("queries", {})
    if q:
        lines.append("query latencies (median ms):")
        for name, v in q.items():
            if name.startswith("_"):
                continue
            lines.append(f"  {name:24s} {v['median_ms']:9.2f}")
        tgt = q.get("_target", {})
        if tgt:
            lines.append(
                f"  (hub target: {tgt.get('name')} with "
                f"{tgt.get('callers')} callers)")
    return "\n".join(lines)


def compare(a_path: str, b_path: str) -> str:
    a = json.loads(Path(a_path).read_text())
    b = json.loads(Path(b_path).read_text())
    lines = [f"# compare: {a['label']} -> {b['label']}"]

    def row(name, va, vb, unit="", invert=False):
        if va in (None, 0) or vb is None:
            return
        delta = (vb - va) / va * 100.0
        better = delta < 0
        if invert:
            better = not better
        mark = "+" if delta >= 0 else ""
        lines.append(f"  {name:28s} {va:12.2f} -> {vb:12.2f} {unit:3s} "
                     f"({mark}{delta:.1f}%)")

    sa, sb = a.get("cold_stats", {}), b.get("cold_stats", {})
    row("cold bake wall", sa.get("bake_wall_ms"), sb.get("bake_wall_ms"), "ms")
    row("  phase1 wall", sa.get("phase1_wall_ms"), sb.get("phase1_wall_ms"), "ms")
    row("  phase2+3 wall", sa.get("phase2_wall_ms"), sb.get("phase2_wall_ms"), "ms")
    row("peak RSS", sa.get("peak_rss_kb"), sb.get("peak_rss_kb"), "KB")
    ga, gb = sa.get("graph", {}), sb.get("graph", {})
    row("nodes", ga.get("nodes"), gb.get("nodes"))
    row("edges", ga.get("edges"), gb.get("edges"))
    row("call sites", ga.get("call_sites"), gb.get("call_sites"))
    row("warm ready", (a.get("warm_ready_s") or 0) * 1000,
        (b.get("warm_ready_s") or 0) * 1000, "ms")
    row("snapshot bytes", a.get("snapshot_bytes"), b.get("snapshot_bytes"), "B")
    ra, rb = a.get("reindex"), b.get("reindex")
    if ra and rb:
        row("reindex_tu", ra["median_ms"], rb["median_ms"], "ms")
    qa, qb = a.get("queries", {}), b.get("queries", {})
    for name in qa:
        if name.startswith("_") or name not in qb:
            continue
        row(f"q:{name}", qa[name]["median_ms"], qb[name]["median_ms"], "ms")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path)
    ap.add_argument("--build-path", type=Path)
    ap.add_argument("--source-re", default=".",
                    help="regex selecting TUs from compile_commands.json")
    ap.add_argument("--max-tus", type=int, default=0,
                    help="cap the number of TUs (0 = all matches)")
    ap.add_argument("--threads", type=int, default=0)
    ap.add_argument("--extra-arg", action="append", default=[],
                    dest="extra_args")
    ap.add_argument("--collapse-paths", action="append", default=[],
                    dest="collapse")
    ap.add_argument("--label", default="run")
    ap.add_argument("--out", type=Path, default=Path("bench-out"))
    ap.add_argument("--snapshot", action="store_true",
                    help="also measure snapshot save + warm start")
    ap.add_argument("--queries", action="store_true",
                    help="also measure MCP tool query latencies")
    ap.add_argument("--reindex", metavar="TU_PATH",
                    help="also measure reindex_tu latency for this TU "
                         "(pick a small TU so removal cost is visible "
                         "against the parse)")
    ap.add_argument("--query-reps", type=int, default=9)
    ap.add_argument("--compare", nargs=2, metavar=("A.json", "B.json"))
    args = ap.parse_args()

    if args.compare:
        print(compare(*args.compare))
        return 0

    if not args.binary or not args.build_path:
        ap.error("--binary and --build-path are required (or use --compare)")

    files = select_sources(args.build_path, args.source_re, args.max_tus)
    if not files:
        sys.exit(f"error: no TUs match {args.source_re!r}")
    print(f"[bench] {len(files)} TUs selected", file=sys.stderr)

    args.out.mkdir(parents=True, exist_ok=True)
    workdir = Path(tempfile.mkdtemp(prefix="megascope-bench-"))
    report: dict = {
        "label": args.label,
        "binary": str(args.binary),
        "build_path": str(args.build_path),
        "source_re": args.source_re,
        "n_files": len(files),
        "threads": args.threads,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }

    try:
        # ---- cold run (optionally saving a snapshot) --------------------
        snap_path = workdir / "graph.snapshot" if args.snapshot else None
        stats_path = workdir / "cold-stats.json"
        print("[bench] cold bake...", file=sys.stderr)
        proc, ready_s, _ = launch_megascope(
            args.binary, args.build_path, files, args.extra_args,
            args.threads, stats_path, snap_path, args.collapse,
            workdir / "cold-stderr.log")
        report["cold_ready_s"] = ready_s
        print(f"[bench] cold ready in {ready_s:.2f}s", file=sys.stderr)

        client = None
        if args.queries:
            print("[bench] query benchmark...", file=sys.stderr)
            client = McpClient(proc)
            report["queries"] = run_query_benchmark(client, args.query_reps)
        if args.reindex:
            print("[bench] reindex benchmark...", file=sys.stderr)
            if client is None:
                client = McpClient(proc)
                client.request("initialize", {
                    "protocolVersion": "2025-06-18", "capabilities": {},
                    "clientInfo": {"name": "bench.py", "version": "1"}})
            report["reindex"] = timed(
                lambda: client.tool("reindex_tu",
                                    {"file": args.reindex}), 3)
            report["reindex"]["tu"] = args.reindex
        shutdown(proc)
        report["cold_stats"] = json.loads(stats_path.read_text())
        report["cold_tu_digest"] = tu_digest(report["cold_stats"])

        # ---- warm run --------------------------------------------------
        if args.snapshot and snap_path and snap_path.exists():
            report["snapshot_bytes"] = snap_path.stat().st_size
            warm_stats = workdir / "warm-stats.json"
            print("[bench] warm start...", file=sys.stderr)
            proc, warm_ready_s, _ = launch_megascope(
                args.binary, args.build_path, files, args.extra_args,
                args.threads, warm_stats, snap_path, args.collapse,
                workdir / "warm-stderr.log")
            shutdown(proc)
            report["warm_ready_s"] = warm_ready_s
            report["warm_stats"] = json.loads(warm_stats.read_text())
            print(f"[bench] warm ready in {warm_ready_s:.2f}s",
                  file=sys.stderr)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    out_path = args.out / f"{args.label}.json"
    out_path.write_text(json.dumps(report, indent=2))
    print(f"[bench] report: {out_path}", file=sys.stderr)
    print()
    print(summarize(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
