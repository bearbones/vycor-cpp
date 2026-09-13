#!/usr/bin/env python3
# Copyright (c) 2026 The vycor-cpp Authors
# Original author: Alex Mason
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Control-flow access cost of one-shot megascope queries.

The matched workloads behind docs/control-flow-access.md: one process
per query, warm page cache, on one index.

  exact_site     query-call-site-context at one call site
  per_function   query-exception-safety for one function (its call
                 sites' contexts plus the bounded path search)
  path_context   query-all-path-contexts for the same function (every
                 hop's context along the paths)
  full_dump      dump --format ndjson (every context, streamed)
  graph_only     get-callers (no control-flow section: the floor)

Each rep records wall, user and system time, peak RSS, page faults
(/usr/bin/time), the per-section decode split from the CLI's -v line,
and a digest of the normalized answer (`indexScope.bake` and the index
path stripped, dump lines sorted) so two binaries or two index formats
can be shown to answer identically.

  scripts/cf-access-bench.py --binary B --index I --out base.json
  scripts/cf-access-bench.py --compare base.json proto.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import statistics
import subprocess
import sys
from pathlib import Path

LOADED_RE = re.compile(r"loaded .* in ([0-9.]+) ms:")
SECTION_RE = re.compile(r"(\w+) (?:([0-9.]+) ms|skipped)/(\d+) KiB")
TIME_FMT = "VYCOR_TIME %e %U %S %M %F %R"


def normalize(payload):
    if isinstance(payload, dict):
        return {k: normalize(v) for k, v in payload.items()
                if k not in ("bake", "index", "bake_start_ns")}
    if isinstance(payload, list):
        return [normalize(x) for x in payload]
    return payload


def run_timed(cmd: list[str], stdout_path: Path | None) -> dict:
    """One process under /usr/bin/time. Returns timings, exit code,
    stderr, and either the stdout text or the path it was written to."""
    full = ["/usr/bin/time", "-f", TIME_FMT, *cmd]
    if stdout_path is None:
        p = subprocess.run(full, capture_output=True, text=True)
        out = p.stdout
    else:
        with open(stdout_path, "w") as fh:
            p = subprocess.run(full, stdout=fh, stderr=subprocess.PIPE,
                               text=True)
        out = None
    rec: dict = {"exit": p.returncode, "stdout": out, "stderr": p.stderr}
    for line in p.stderr.splitlines():
        if line.startswith("VYCOR_TIME "):
            f = line.split()[1:]
            rec.update(wall_s=float(f[0]), user_s=float(f[1]),
                       sys_s=float(f[2]), rss_kb=int(f[3]),
                       major_faults=int(f[4]), minor_faults=int(f[5]))
        elif line.startswith("megascope: loaded"):
            m = LOADED_RE.search(line)
            if m:
                rec["load_ms"] = float(m.group(1))
            rec["sections"] = {
                name: ({"skipped": True, "kib": int(kib)} if ms == ""
                       else {"ms": float(ms), "kib": int(kib)})
                for name, ms, kib in SECTION_RE.findall(line)}
    if "wall_s" not in rec:
        raise RuntimeError(f"no timing line from {' '.join(cmd)}:\n"
                           f"{p.stderr[-500:]}")
    return rec


def digest_json(text: str) -> str:
    try:
        payload = json.loads(text)
    except json.JSONDecodeError:
        return "unparseable:" + hashlib.sha256(text.encode()).hexdigest()
    canon = json.dumps(normalize(payload), sort_keys=True)
    return hashlib.sha256(canon.encode()).hexdigest()


def digest_dump(path: Path) -> tuple[str, int]:
    """sha256 of the sorted lines of an ndjson dump (order is not part
    of the dump's contract), and the line count."""
    h = hashlib.sha256()
    n = 0
    with open(path, "rb") as fh:
        lines = fh.read().split(b"\n")
    # The leading summary line names the bake; the records do not.
    lines = [ln for ln in lines if ln and not ln.startswith(b'{"_summary"')]
    n = len(lines)
    for ln in sorted(lines):
        h.update(ln)
        h.update(b"\n")
    return h.hexdigest(), n


def pick_target(binary: Path, index: Path, target: str | None) -> tuple:
    """A function with callers, one call site of it, and that site's
    caller (the entry point the path workloads start from, so they walk
    at least one path on any index)."""
    base = [str(binary), "megascope"]
    if target is None:
        p = subprocess.run([*base, "search-functions", "--query", "run",
                            "--limit", "50", "--index", str(index)],
                           capture_output=True, text=True)
        names = [m.get("qualifiedName")
                 for m in json.loads(p.stdout).get("matches", [])]
        target = next((n for n in names if n), "main")
    p = subprocess.run([*base, "get-callers", "--name", target,
                        "--index", str(index)],
                       capture_output=True, text=True)
    if p.returncode not in (0, 1):
        sys.exit(f"get-callers --name {target} failed: {p.stderr.strip()}")
    callers = json.loads(p.stdout).get("callers", [])
    if not callers:
        sys.exit(f"{target} has no callers; pass --target")
    return target, callers[0]["callSite"], callers[0]["callerName"]


def summarize(reps: list[dict]) -> dict:
    def med(key):
        vals = [r[key] for r in reps if key in r]
        return statistics.median(vals) if vals else None
    out = {
        "reps": len(reps),
        "exit": reps[0]["exit"],
        "wall_median_s": med("wall_s"),
        "wall_min_s": min(r["wall_s"] for r in reps),
        "wall_max_s": max(r["wall_s"] for r in reps),
        "user_median_s": med("user_s"),
        "sys_median_s": med("sys_s"),
        "rss_median_kb": med("rss_kb"),
        "rss_max_kb": max(r["rss_kb"] for r in reps),
        "major_faults_median": med("major_faults"),
        "minor_faults_median": med("minor_faults"),
        "load_median_ms": med("load_ms"),
        "sections": reps[-1].get("sections", {}),
    }
    return out


def run_bench(args) -> dict:
    binary, index = Path(args.binary).resolve(), Path(args.index).resolve()
    target, site, entry = pick_target(binary, index, args.target)
    base = [str(binary), "megascope"]
    common = ["--index", str(index), "-v"]
    starts = ["--entry-points", entry]
    workloads = {
        "graph_only": [*base, "get-callers", "--name", target, *common],
        "exact_site": [*base, "query-call-site-context", "--call-site",
                       site, *common],
        "per_function": [*base, "query-exception-safety", "--function",
                         target, *starts, *common],
        "path_context": [*base, "query-all-path-contexts", "--function",
                         target, "--max-paths", "20", *starts, *common],
        "full_dump": [*base, "dump", "--format", "ndjson", "--index",
                      str(index)],
    }
    report = {
        "label": args.label,
        "binary": str(binary),
        "index": str(index),
        "index_bytes": index.stat().st_size,
        "target": target,
        "site": site,
        "entry_point": entry,
        "reps": args.reps,
        "workloads": {},
    }
    scratch = Path(args.scratch) if args.scratch else Path("/tmp")
    for name, cmd in workloads.items():
        reps = []
        n = args.dump_reps if name == "full_dump" else args.reps
        digest, lines = None, None
        for i in range(n):
            if name == "full_dump":
                out_path = scratch / "cf-access-bench.dump"
                rec = run_timed(cmd, out_path)
                if i == 0:
                    digest, lines = digest_dump(out_path)
            else:
                rec = run_timed(cmd, None)
                if i == 0:
                    digest = digest_json(rec["stdout"])
            rec.pop("stdout", None)
            rec.pop("stderr", None)
            reps.append(rec)
            if args.verbose:
                print(f"  {name} rep {i + 1}: {rec['wall_s']:.2f} s, "
                      f"{rec['rss_kb'] // 1024} MB", file=sys.stderr)
        summary = summarize(reps)
        summary["result_sha256"] = digest
        if lines is not None:
            summary["lines"] = lines
        summary["argv"] = cmd
        report["workloads"][name] = summary
        print(f"{name:13s} wall {summary['wall_median_s']:6.2f} s  "
              f"rss {summary['rss_median_kb'] / 1024:7.0f} MB  "
              f"load {summary['load_median_ms'] or 0:7.0f} ms  "
              f"exit {summary['exit']}")
    return report


def compare(a_path: str, b_path: str, gate_wall: float, gate_rss: float,
            regression: float) -> int:
    a = json.loads(Path(a_path).read_text())
    b = json.loads(Path(b_path).read_text())
    print(f"A = {a['label']} ({a['binary']}, {a['index_bytes']} bytes)")
    print(f"B = {b['label']} ({b['binary']}, {b['index_bytes']} bytes)")
    print(f"index bytes: {b['index_bytes'] / a['index_bytes']:.3f}x")
    print(f"{'workload':13s} {'wall A':>8s} {'wall B':>8s} {'ratio':>6s} "
          f"{'rss A':>8s} {'rss B':>8s} {'ratio':>6s}  same answer")
    ok = True
    for name, wa in a["workloads"].items():
        wb = b["workloads"].get(name)
        if not wb:
            print(f"{name:13s} missing in B")
            ok = False
            continue
        wr = wa["wall_median_s"] / wb["wall_median_s"]
        rr = wa["rss_median_kb"] / wb["rss_median_kb"]
        same = wa["result_sha256"] == wb["result_sha256"]
        ok &= same
        print(f"{name:13s} {wa['wall_median_s']:7.2f}s {wb['wall_median_s']:7.2f}s "
              f"{wr:5.2f}x {wa['rss_median_kb'] / 1024:7.0f}M "
              f"{wb['rss_median_kb'] / 1024:7.0f}M {rr:5.2f}x  "
              f"{'yes' if same else 'NO'}")
    ex_a = a["workloads"]["exact_site"]
    ex_b = b["workloads"]["exact_site"]
    wr = ex_a["wall_median_s"] / ex_b["wall_median_s"]
    rr = ex_a["rss_median_kb"] / ex_b["rss_median_kb"]
    print(f"\ngate: exact_site wall {wr:.2f}x (need >= {gate_wall}), "
          f"rss {rr:.2f}x (need >= {gate_rss})")
    gate = wr >= gate_wall and rr >= gate_rss
    regressed = False
    for name in ("full_dump",):
        wa, wb = a["workloads"][name], b["workloads"][name]
        slower = wb["wall_median_s"] / wa["wall_median_s"] - 1.0
        flag = "REGRESSION" if slower > regression else "ok"
        print(f"{name}: {slower * 100:+.1f}% wall ({flag})")
        regressed |= slower > regression
    print("answers identical:", "yes" if ok else "NO")
    verdict = gate and ok and not regressed
    print("verdict:", "GO" if verdict else "NO-GO")
    return 0 if verdict else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--binary", type=Path)
    ap.add_argument("--index", type=Path)
    ap.add_argument("--target", help="function name (default: a search hit)")
    ap.add_argument("--reps", type=int, default=9)
    ap.add_argument("--dump-reps", type=int, default=3)
    ap.add_argument("--label", default="run")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--scratch", help="directory for the dump output")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--compare", nargs=2, metavar=("A.json", "B.json"))
    ap.add_argument("--gate-wall", type=float, default=2.0)
    ap.add_argument("--gate-rss", type=float, default=2.0)
    ap.add_argument("--regression", type=float, default=0.10)
    args = ap.parse_args()
    if args.compare:
        return compare(args.compare[0], args.compare[1], args.gate_wall,
                       args.gate_rss, args.regression)
    if not args.binary or not args.index:
        ap.error("--binary and --index are required (or --compare)")
    report = run_bench(args)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=1) + "\n")
        print(f"report written to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
