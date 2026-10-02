#!/usr/bin/env python3
# Copyright (c) 2026 The vycor-cpp Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Efficiency-analysis harness for megascope.
#
# Measures, for a given binary + compilation database:
#   * cold index bake (`megascope index`): wall time, per-phase and per-TU
#     parse timings, parse-failure/crash counts, graph sizes, peak RSS
#     (via --stats-json)
#   * index size and the cost of a no-change warm `megascope index`
#     (--snapshot)
#   * resident query latencies over one `megascope batch` process, the
#     index loaded once (--queries)
#   * one-shot query latencies, one process per query (--cli)
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
#   # Include the index size and a no-change warm `megascope index`:
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
# Batch client (NDJSON requests over one `megascope batch` process)
# ---------------------------------------------------------------------------

class BatchClient:
    def __init__(self, proc: subprocess.Popen):
        self.proc = proc
        self._id = 0

    def tool(self, name: str, args: dict | None = None) -> dict:
        """One request; returns the tool payload (the response's result)."""
        self._id += 1
        msg = {"id": self._id, "tool": name, "args": args or {}}
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError(f"megascope batch closed its output during {name}")
        resp = json.loads(line)
        if "result" not in resp:
            raise RuntimeError(f"{name}: {resp.get('error', resp)}")
        return resp["result"]


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
# Index bake and the batch process
# ---------------------------------------------------------------------------

# get_callers / get_callees page at 200 records by default
# (docs/result-contract.md, "Paging"); a limit this large asks for all.
ALL_RECORDS = 1_000_000_000


def run_index(binary: Path, build_path: Path, source_list: Path,
              extra_args: list[str], threads: int, stats_json: Path,
              index: Path, collapse: list[str], log_path: Path) -> float:
    """Run `megascope index` to completion. Returns its wall time (s)."""
    cmd = [str(binary), "megascope", "index", "--build-path", str(build_path),
           "--source-list", str(source_list), "--index", str(index),
           "--threads", str(threads), "--stats-json", str(stats_json)]
    for a in extra_args:
        cmd += [f"--extra-arg={a}"]
    for c in collapse:
        cmd += ["--collapse-paths", c]
    with open(log_path, "wb") as log:
        t0 = time.monotonic()
        p = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=log)
        wall_s = time.monotonic() - t0
    if p.returncode != 0:
        raise RuntimeError(
            f"megascope index exited {p.returncode} (see {log_path})")
    return wall_s


def launch_batch(binary: Path, index: Path) -> subprocess.Popen:
    return subprocess.Popen(
        [str(binary), "megascope", "batch", "--index", str(index)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL)


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


def run_query_benchmark(client: BatchClient, reps: int) -> dict:
    out: dict[str, dict] = {}

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
        # callerCount is the full count; the callers list is paged.
        res = client.tool("get_callers", {"name": name, "limit": 1})
        n = res.get("callerCount", -1)
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
    # The hub timings ask for the whole list (the default page is 200),
    # so they stay comparable with measurements taken before paging.
    out["get_callers_hub"] = timed(
        lambda: client.tool("get_callers",
                            {"name": target, "limit": ALL_RECORDS}), reps)
    out["get_callees_hub"] = timed(
        lambda: client.tool("get_callees",
                            {"name": target, "limit": ALL_RECORDS}), reps)
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
# CLI (one-shot) benchmark — docs/megascope-cli-review.md §4.3
# ---------------------------------------------------------------------------

SECTION_RE = re.compile(r"(\w+) (?:skipped|([\d.]+) ms)/(\d+) KiB")
LOADED_RE = re.compile(r"in ([\d.]+) ms:")


def cli_argv(tool: str, args: dict) -> list[str]:
    """Tool arguments as the schema-derived flags the CLI accepts."""
    out = [tool.replace("_", "-")]
    for k, v in args.items():
        flag = "--" + k.replace("_", "-")
        if isinstance(v, bool):
            out.append(flag if v else f"{flag}=false")
        elif isinstance(v, list):
            for x in v:
                out += [flag, str(x)]
        else:
            out += [flag, str(v)]
    return out


def cli_call(binary: Path, index: Path, tool: str,
             args: dict) -> tuple[dict, float, dict]:
    """One `megascope <tool> -v` process. Returns (payload, wall_ms,
    load) where load is {"total_ms", "sections": {name: {ms|skipped,
    kib}}} parsed from the -v line."""
    cmd = [str(binary), "megascope", *cli_argv(tool, args),
           "--index", str(index), "-v"]
    t0 = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, text=True)
    wall_ms = (time.perf_counter() - t0) * 1000.0
    if p.returncode not in (0, 1):
        raise RuntimeError(
            f"{' '.join(cmd)} exited {p.returncode}: {p.stderr.strip()}")
    load: dict = {"sections": {}}
    for line in p.stderr.splitlines():
        if not line.startswith("megascope: loaded"):
            continue
        m = LOADED_RE.search(line)
        if m:
            load["total_ms"] = float(m.group(1))
        for name, ms, kib in SECTION_RE.findall(line):
            load["sections"][name] = (
                {"skipped": True, "kib": int(kib)} if ms == ""
                else {"ms": float(ms), "kib": int(kib)})
    payload = json.loads(p.stdout) if p.stdout.strip() else {}
    return payload, wall_ms, load


def run_cli_benchmark(binary: Path, index: Path, reps: int,
                      target: str | None) -> dict:
    """Wall time of one-shot query processes (what an agent pays per
    call), with the index load split out per section."""
    if target is None:
        res, _, _ = cli_call(binary, index, "search_functions",
                             {"query": "run", "limit": 20})
        names = [m.get("qualifiedName") for m in res.get("matches", [])]
        target = next((n for n in names if n), "main")
    queries = {
        "graph_summary": ("graph_summary", {}),
        "search_functions": ("search_functions", {"query": "run",
                                                  "limit": 50}),
        "lookup_function": ("lookup_function", {"name": target}),
        "get_callers_hub": ("get_callers",
                            {"name": target, "limit": ALL_RECORDS}),
        "get_callees_hub": ("get_callees",
                            {"name": target, "limit": ALL_RECORDS}),
        "find_call_chain": ("find_call_chain",
                            {"from": "main", "to": target,
                             "max_depth": 10, "max_paths": 5}),
        "list_entry_points": ("list_entry_points", {}),
        "query_exception_safety": ("query_exception_safety",
                                   {"function": target}),
        "analyze_dead_code": ("analyze_dead_code", {}),
    }
    out: dict = {"_target": target}
    for label, (tool, args) in queries.items():
        n = max(1, reps // 3) if tool == "analyze_dead_code" else reps
        walls, loads, last = [], [], {}
        for _ in range(n):
            _, wall, last = cli_call(binary, index, tool, args)
            walls.append(wall)
            if "total_ms" in last:
                loads.append(last["total_ms"])
        out[label] = {
            "min_ms": min(walls),
            "median_ms": statistics.median(walls),
            "max_ms": max(walls),
            "reps": n,
            "load_median_ms": statistics.median(loads) if loads else None,
            "sections": last.get("sections", {}),
        }
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
        + f"index command {report.get('cold_wall_s', 0):.2f}s")
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
            f"warm index (no change): {report.get('warm_wall_s', 0):.2f}s "
            f"(meta load {snap.get('load_ms', 0)/1000:.2f}s, "
            f"{snap.get('refreshed_tus')} refreshed); index size "
            f"{report.get('snapshot_bytes', 0)/1e6:.2f} MB; peak RSS "
            f"{(ws.get('peak_rss_kb') or 0)/1e6:.2f} GB")
    q = report.get("queries", {})
    if q:
        lines.append("resident query latencies over batch (median ms):")
        for name, v in q.items():
            if name.startswith("_"):
                continue
            lines.append(f"  {name:24s} {v['median_ms']:9.2f}")
        tgt = q.get("_target", {})
        if tgt:
            lines.append(
                f"  (hub target: {tgt.get('name')} with "
                f"{tgt.get('callers')} callers)")
    c = report.get("cli", {})
    if c:
        lines.append("")
        lines.append(f"cli one-shot latency (target {c.get('_target')}; "
                     "wall = process + load + query, ms):")
        for name, t in c.items():
            if name.startswith("_"):
                continue
            load = t.get("load_median_ms")
            load_s = f"  load {load:8.1f}" if load is not None else ""
            lines.append(f"  {name:24s} median {t['median_ms']:8.1f}"
                         f"  min {t['min_ms']:8.1f}  max {t['max_ms']:8.1f}"
                         f"{load_s}  (n={t['reps']})")
            if report.get("_sections") and t.get("sections"):
                parts = []
                for sec, d in t["sections"].items():
                    parts.append(f"{sec} skipped/{d['kib']}KiB"
                                 if d.get("skipped") else
                                 f"{sec} {d['ms']:.1f}ms/{d['kib']}KiB")
                lines.append("      sections: " + ", ".join(parts))
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
    row("cold index command", (a.get("cold_wall_s") or 0) * 1000,
        (b.get("cold_wall_s") or 0) * 1000, "ms")
    row("warm index (no change)", (a.get("warm_wall_s") or 0) * 1000,
        (b.get("warm_wall_s") or 0) * 1000, "ms")
    row("index bytes", a.get("snapshot_bytes"), b.get("snapshot_bytes"), "B")
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
                    help="also measure the index size and a no-change "
                         "warm `megascope index`")
    ap.add_argument("--queries", action="store_true",
                    help="also measure resident query latencies over one "
                         "`megascope batch` process")
    ap.add_argument("--cli", action="store_true",
                    help="also measure one-shot CLI query latency "
                         "(process start + index load + query)")
    ap.add_argument("--sections", action="store_true",
                    help="with --cli, print the per-section index load "
                         "split each verb reports under -v")
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
        # ---- cold bake ------------------------------------------------
        snap_path = workdir / "megascope.vycs"
        source_list = workdir / "sources.txt"
        source_list.write_text("".join(f + "\n" for f in files))
        stats_path = workdir / "cold-stats.json"
        print("[bench] cold bake...", file=sys.stderr)
        cold_s = run_index(
            args.binary, args.build_path, source_list, args.extra_args,
            args.threads, stats_path, snap_path, args.collapse,
            workdir / "cold-stderr.log")
        report["cold_wall_s"] = cold_s
        report["cold_stats"] = json.loads(stats_path.read_text())
        report["cold_tu_digest"] = tu_digest(report["cold_stats"])
        print(f"[bench] cold index in {cold_s:.2f}s", file=sys.stderr)

        # ---- resident queries over one batch process -------------------
        if args.queries:
            print("[bench] query benchmark...", file=sys.stderr)
            proc = launch_batch(args.binary, snap_path)
            try:
                report["queries"] = run_query_benchmark(
                    BatchClient(proc), args.query_reps)
            finally:
                shutdown(proc)

        # ---- no-change warm index --------------------------------------
        if args.snapshot:
            report["snapshot_bytes"] = snap_path.stat().st_size
            warm_stats = workdir / "warm-stats.json"
            print("[bench] warm index...", file=sys.stderr)
            warm_s = run_index(
                args.binary, args.build_path, source_list, args.extra_args,
                args.threads, warm_stats, snap_path, args.collapse,
                workdir / "warm-stderr.log")
            report["warm_wall_s"] = warm_s
            report["warm_stats"] = json.loads(warm_stats.read_text())
            print(f"[bench] warm index in {warm_s:.2f}s", file=sys.stderr)

        # ---- one-shot CLI queries against the saved index --------------
        if args.cli:
            print("[bench] cli benchmark...", file=sys.stderr)
            target = (report.get("queries", {}).get("_target") or {}).get(
                "name")
            report["cli"] = run_cli_benchmark(
                args.binary, snap_path, args.query_reps, target)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if args.sections:
        report["_sections"] = True
    out_path = args.out / f"{args.label}.json"
    out_path.write_text(json.dumps(report, indent=2))
    print(f"[bench] report: {out_path}", file=sys.stderr)
    print()
    print(summarize(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
