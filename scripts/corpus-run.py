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
"""The validation corpus runner (docs/validation.md).

Every case under corpus/cases/<name>/ is a small source tree plus a
case.json that says, query by query, which records must be present
(witnesses), which must be absent (negatives), which scalar fields must
hold, and in what order records must come. The runner bakes each case
with the built binary, runs the queries, checks the expectations, and
writes one JSON report: revision, toolchain, corpus version, every
command, expected against observed, missing and false findings, what
the tool itself declared incomplete or unknown, repeated latencies, and
peak memory. Quality and cost are reported side by side, never folded
into one number.

    scripts/corpus-run.py --binary build/src/vycor-cpp [--out report.json]
    scripts/corpus-run.py --binary build/src/vycor-cpp --self-check

Registered with ctest as `corpus` and `corpus_selfcheck`
(tests/CMakeLists.txt). `--self-check` proves the checks can fail: it
runs the corpus clean, then again under each fault injection
(--inject-fault), and demands the right category of failure each time.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORPUS = ROOT / "corpus"
CASES = CORPUS / "cases"
REPORT_VERSION = 1

# Files touched in the same second as a bake are re-checked once on the
# next start (the unstable-stamp window), so every fixture file is
# backdated before it is indexed: two minutes into the past, one second
# later per aging pass so an overlay gets a stamp its predecessor never
# had (same trick as scripts/warm-refresh-check.py).
AGE_SECONDS = 120

# The observed payload's own statements about its scope, copied into the
# report so a reader can tell "wrong" from "declared incomplete".
DECLARED_KEYS = ("status", "complete", "exhaustive", "protection")

FAULTS = {
    "drop-first-record": "the first record of every list is dropped "
                         "(a missing finding must be caught)",
    "inject-record": "each expected-negative record is appended "
                     "(a false finding must be caught)",
    "drop-fields": "every checked scalar field is removed "
                   "(absent output must not pass)",
    "empty-stdout": "stdout is emptied "
                    "(missing output must not be normalized into a pass)",
}


class Fail(Exception):
    pass


# ---------------------------------------------------------------------------
# matching
# ---------------------------------------------------------------------------

def subset(expected, observed) -> bool:
    """expected is contained in observed: every key of an expected object
    must be present and match; lists must have the same length and match
    element-wise; scalars must be equal."""
    if isinstance(expected, dict):
        if not isinstance(observed, dict):
            return False
        return all(k in observed and subset(v, observed[k])
                   for k, v in expected.items())
    if isinstance(expected, list):
        if not isinstance(observed, list) or len(expected) != len(observed):
            return False
        return all(subset(e, o) for e, o in zip(expected, observed))
    return expected == observed


def records(payload, key: str) -> list | None:
    v = payload.get(key) if isinstance(payload, dict) else None
    return v if isinstance(v, list) else None


def check_query(expect: dict, obs: dict) -> list[dict]:
    """Every expectation in `expect` against the observed result; one
    entry per check with its kind, whether it held, and a detail."""
    checks: list[dict] = []

    def add(kind, ok, detail):
        checks.append({"kind": kind, "ok": bool(ok), "detail": detail})

    if "exit" in expect:
        add("exit", obs["exit"] == expect["exit"],
            f"exit {obs['exit']}, expected {expect['exit']}")

    if "lines" in expect:  # text output (anneal)
        out = obs["stdout"]
        for pat in expect["lines"].get("present", []):
            add("line-present", re.search(pat, out, re.M),
                f"expected a line matching {pat!r}")
        for pat in expect["lines"].get("absent", []):
            add("line-absent", not re.search(pat, out, re.M),
                f"expected no line matching {pat!r}")
        return checks

    payload = obs.get("payload")
    needs_payload = any(k in expect for k in
                        ("status", "fields", "regex", "witnesses",
                         "negatives", "count", "sequence"))
    if needs_payload and not isinstance(payload, dict):
        add("payload", False, "stdout is not one JSON object: "
            f"{obs['stdout'][:120]!r}")
        return checks

    if "status" in expect:
        add("status", payload.get("status") == expect["status"],
            f"status {payload.get('status')!r}, expected "
            f"{expect['status']!r}")
    for k, v in expect.get("fields", {}).items():
        add("field", k in payload and subset(v, payload[k]),
            f"{k} = {json.dumps(payload.get(k))[:200]}, expected "
            f"{json.dumps(v)}")
    for k, pat in expect.get("regex", {}).items():
        val = payload.get(k)
        add("regex", isinstance(val, str) and re.search(pat, val),
            f"{k} = {val!r}, expected a match for {pat!r}")
    for k, n in expect.get("count", {}).items():
        recs = records(payload, k)
        add("count", recs is not None and len(recs) == n,
            f"{k} has {len(recs) if recs is not None else 'no'} records, "
            f"expected {n}")
    for w in expect.get("witnesses", []):
        recs = records(payload, w["in"]) or []
        add("witness", any(subset(w["match"], r) for r in recs),
            f"no record in {w['in']} matches {json.dumps(w['match'])}")
    for n in expect.get("negatives", []):
        recs = records(payload, n["in"]) or []
        hits = [r for r in recs if subset(n["match"], r)]
        add("negative", not hits,
            f"{len(hits)} record(s) in {n['in']} match the expected "
            f"absence {json.dumps(n['match'])}")
    for s in expect.get("sequence", []):
        recs = records(payload, s["in"]) or []
        pos = 0
        ok = True
        for m in s["match"]:
            while pos < len(recs) and not subset(m, recs[pos]):
                pos += 1
            if pos == len(recs):
                ok = False
                break
            pos += 1
        add("sequence", ok,
            f"{s['in']} does not contain, in this order: "
            f"{json.dumps(s['match'])[:300]}")
    return checks


# ---------------------------------------------------------------------------
# fault injection (self-check)
# ---------------------------------------------------------------------------

def inject(fault: str | None, expect: dict, obs: dict) -> dict:
    if not fault:
        return obs
    obs = copy.deepcopy(obs)
    payload = obs.get("payload")
    if fault == "empty-stdout":
        obs["stdout"] = ""
        obs["payload"] = None
    elif isinstance(payload, dict):
        if fault == "drop-first-record":
            for k, v in payload.items():
                if isinstance(v, list) and v:
                    del v[0]
        elif fault == "inject-record":
            for n in expect.get("negatives", []):
                payload.setdefault(n["in"], []).append(
                    copy.deepcopy(n["match"]))
        elif fault == "drop-fields":
            for k in list(expect.get("fields", {})) + \
                    list(expect.get("regex", {})):
                payload.pop(k, None)
    return obs


# ---------------------------------------------------------------------------
# running
# ---------------------------------------------------------------------------

def run_measured(argv: list[str], cwd: Path, stdin: str | None = None):
    """Run argv once; return (exit, stdout, stderr, wall_ms, peak_rss_kb).
    The child's own rusage comes from wait4, so the peak RSS is that
    process's, not the runner's."""
    t0 = time.perf_counter()
    with tempfile.TemporaryFile() as err:
        p = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.PIPE
                             if stdin is not None else subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=err, text=True)
        if stdin is not None:
            p.stdin.write(stdin)
            p.stdin.close()
        out = p.stdout.read()
        p.stdout.close()
        _, status, ru = os.wait4(p.pid, 0)
        p.returncode = (-os.WTERMSIG(status) if os.WIFSIGNALED(status)
                        else os.WEXITSTATUS(status))
        wall = (time.perf_counter() - t0) * 1000.0
        err.seek(0)
        stderr = err.read().decode(errors="replace")
    rss_kb = ru.ru_maxrss if platform.system() != "Darwin" \
        else ru.ru_maxrss // 1024
    return p.returncode, out, stderr, wall, rss_kb


class Runner:
    def __init__(self, binary: Path, root: Path, reps: int, verbose: bool,
                 fault: str | None) -> None:
        self.binary = binary
        self.root = root
        self.reps = reps
        self.verbose = verbose
        self.fault = fault
        self.clock = time.time() - AGE_SECONDS

    # ---- fixture -----------------------------------------------------------

    def age(self, files: list[Path]) -> None:
        for p in files:
            os.utime(p, (self.clock, self.clock))
        self.clock += 1

    def overlay(self, src: Path, d: Path) -> None:
        """Copy the overlay's files over the fixture and backdate exactly
        those, so only the TUs they reach look edited."""
        written = []
        for p in src.rglob("*"):
            if p.is_file():
                dest = d / p.relative_to(src)
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(p, dest)
                written.append(dest)
        self.age(written)

    def write_db(self, d: Path, sources: list[str],
                 flags: dict[str, list[str]]) -> None:
        entries = []
        for src in sources:
            f = str(d / src)
            args = ["clang++", *flags.get("*", ["-std=c++17"]),
                    "-I", str(d), *flags.get(src, []), "-c", f]
            entries.append({"directory": str(d), "file": f,
                            "arguments": args})
        (d / "compile_commands.json").write_text(json.dumps(entries,
                                                            indent=1))

    # ---- commands ----------------------------------------------------------

    def command(self, argv: list[str]) -> list[str]:
        return [str(self.binary), *argv]

    def megascope_index(self, d: Path, index: Path, sources: list[str],
                        entry_points: list[str], log: list) -> dict:
        argv = ["megascope", "index", "--build-path", str(d), "--index",
                str(index), "--threads", "2"]
        for s in sources:
            argv += ["--source", str(d / s)]
        for e in entry_points:
            argv += ["--entry-point", e]
        code, out, err, wall, rss = run_measured(self.command(argv), d)
        # The bake's own account of what it refreshed and why (stderr) is
        # the evidence when an index check fails.
        log.append({"argv": argv, "exit": code, "wall_ms": round(wall, 1),
                    "peak_rss_kb": rss, "stderr": err[-2000:]})
        if self.verbose:
            print(f"$ {' '.join(argv)} -> {code} ({wall:.0f} ms)",
                  file=sys.stderr)
        if code != 0:
            raise Fail(f"megascope index failed ({code}): {err[-400:]}")
        try:
            return json.loads(out.strip().splitlines()[-1])
        except (ValueError, IndexError):
            raise Fail(f"megascope index printed no summary: {out[:200]!r}")

    def query(self, q: dict, d: Path, index: Path, sources: list[str]
              ) -> dict:
        argv = list(q["argv"])
        if argv[0] == "anneal":
            argv += ["--build-path", str(d)]
            for s in sources:
                argv += ["--source", str(d / s)]
        else:
            argv = ["megascope", *argv, "--index", str(index)]
        runs = []
        for _ in range(self.reps):
            code, out, err, wall, rss = run_measured(self.command(argv), d)
            runs.append((code, out, err, wall, rss))
        code, out, err, _, _ = runs[0]
        if self.verbose:
            print(f"$ {' '.join(argv)} -> {code}", file=sys.stderr)
        payload = None
        if argv[0] == "megascope":
            try:
                payload = json.loads(out)
            except ValueError:
                payload = None
        obs = {"exit": code, "stdout": out, "stderr": err[-2000:],
               "payload": payload}
        obs = inject(self.fault, q.get("expect", {}), obs)
        checks = check_query(q.get("expect", {}), obs)
        walls = [r[3] for r in runs]
        declared = {}
        if isinstance(obs.get("payload"), dict):
            p = obs["payload"]
            for k in DECLARED_KEYS:
                if k in p:
                    declared[k] = p[k]
            scope = p.get("indexScope")
            if isinstance(scope, dict):
                declared["indexScope.complete"] = scope.get("complete")
                declared["indexScope.freshness"] = scope.get("freshness")
        return {
            "id": q["id"],
            "argv": argv,
            "expected": q.get("expect", {}),
            "observed": {"exit": obs["exit"],
                         "payload": obs["payload"],
                         "stdout": None if obs["payload"] is not None
                         else obs["stdout"][:4000]},
            "declared": declared,
            "checks": checks,
            "ok": all(c["ok"] for c in checks),
            "stable_across_reps": len({(r[0], r[1]) for r in runs}) == 1,
            "latency_ms": {"reps": len(walls),
                           "median": round(statistics.median(walls), 1),
                           "min": round(min(walls), 1),
                           "max": round(max(walls), 1)},
            "peak_rss_kb": max(r[4] for r in runs),
        }

    # ---- cases -------------------------------------------------------------

    def run_case(self, case_dir: Path) -> dict:
        spec = json.loads((case_dir / "case.json").read_text())
        d = self.root / case_dir.name
        shutil.copytree(case_dir / "src", d)
        self.age([p for p in d.rglob("*") if p.is_file()])
        index = d / "index.vycs"
        sources = list(spec["sources"])
        flags = dict(spec.get("flags", {}))
        entry_points = spec.get("entry_points", ["main"])
        phases = spec.get("phases") or [{"name": "single",
                                         "queries": spec.get("queries", [])}]
        report = {"name": case_dir.name, "title": spec.get("title", ""),
                  "commands": [], "phases": [], "ok": True, "error": None}
        try:
            for i, phase in enumerate(phases):
                if phase.get("overlay"):
                    self.overlay(case_dir / phase["overlay"], d)
                if "sources" in phase:
                    sources = list(phase["sources"])
                if "flags" in phase:
                    flags.update(phase["flags"])
                self.write_db(d, sources, flags)
                summary = self.megascope_index(d, index, sources,
                                               entry_points,
                                               report["commands"])
                pr = {"name": phase.get("name", f"phase{i}"),
                      "index_summary": summary, "index_checks": [],
                      "index_stderr": report["commands"][-1]["stderr"],
                      "queries": []}
                for k, v in phase.get("index", {}).items():
                    pr["index_checks"].append({
                        "kind": "index", "ok": summary.get(k) == v,
                        "detail": f"index summary {k} = "
                                  f"{summary.get(k)!r}, expected {v!r}"})
                for q in phase.get("queries", []):
                    qr = self.query(q, d, index, sources)
                    report["commands"].append({"argv": qr["argv"]})
                    pr["queries"].append(qr)
                report["phases"].append(pr)
        except Fail as e:
            report["ok"] = False
            report["error"] = str(e)
        for pr in report["phases"]:
            if not all(c["ok"] for c in pr["index_checks"]) or \
                    not all(q["ok"] for q in pr["queries"]):
                report["ok"] = False
        return report


# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------

def git(*args: str) -> str:
    try:
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                              text=True, check=True).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""


def toolchain(binary: Path) -> str:
    try:
        p = subprocess.run([str(binary), "--version"], capture_output=True,
                           text=True)
        return (p.stdout or p.stderr).strip()
    except OSError as e:
        return f"unavailable: {e}"


def corpus_version() -> str:
    v = CORPUS / "VERSION"
    return v.read_text().strip() if v.exists() else "unversioned"


def summarize(cases: list[dict]) -> dict:
    q = {"queries": 0, "passed": 0, "failed": 0, "checks": 0,
         "missing_findings": 0, "false_findings": 0,
         "field_mismatches": 0, "order_mismatches": 0,
         "unparseable_outputs": 0, "index_check_failures": 0,
         "declared_incomplete": 0, "declared_unknown": 0,
         "unstable_across_reps": 0, "case_errors": 0}
    cost = {"index_bakes": [], "queries": []}
    for c in cases:
        if c["error"]:
            q["case_errors"] += 1
        for cmd in c["commands"]:
            if "wall_ms" in cmd:
                cost["index_bakes"].append({
                    "case": c["name"], "wall_ms": cmd["wall_ms"],
                    "peak_rss_kb": cmd["peak_rss_kb"]})
        for ph in c["phases"]:
            q["index_check_failures"] += sum(
                1 for k in ph["index_checks"] if not k["ok"])
            for r in ph["queries"]:
                q["queries"] += 1
                q["passed" if r["ok"] else "failed"] += 1
                q["checks"] += len(r["checks"])
                for k in r["checks"]:
                    if k["ok"]:
                        continue
                    kind = k["kind"]
                    if kind in ("witness", "count", "line-present"):
                        q["missing_findings"] += 1
                    elif kind in ("negative", "line-absent"):
                        q["false_findings"] += 1
                    elif kind in ("field", "regex", "status", "exit"):
                        q["field_mismatches"] += 1
                    elif kind == "sequence":
                        q["order_mismatches"] += 1
                    elif kind == "payload":
                        q["unparseable_outputs"] += 1
                dec = r["declared"]
                if dec.get("complete") is False or \
                        dec.get("indexScope.complete") is False or \
                        str(dec.get("protection", "")).startswith(
                            "observed_"):
                    q["declared_incomplete"] += 1
                if dec.get("status") in ("not_found", "unavailable") or \
                        dec.get("protection") == "unknown":
                    q["declared_unknown"] += 1
                if not r["stable_across_reps"]:
                    q["unstable_across_reps"] += 1
                cost["queries"].append({
                    "case": c["name"], "id": r["id"],
                    "latency_ms": r["latency_ms"],
                    "peak_rss_kb": r["peak_rss_kb"]})
    q["scope"] = ("every expectation is hand-written against the case's "
                  "sources; rates are over these queries only and say "
                  "nothing about unlisted behavior")
    walls = [x["latency_ms"]["median"] for x in cost["queries"]]
    cost["query_median_of_medians_ms"] = (round(statistics.median(walls), 1)
                                         if walls else None)
    return {"quality": q, "cost": cost}


def run_corpus(args, fault: str | None, case_dirs: list[Path]) -> dict:
    root = Path(tempfile.mkdtemp(prefix="vycor-corpus-"))
    started = datetime.now(timezone.utc)
    runner = Runner(args.binary, root, args.reps, args.verbose, fault)
    cases = [runner.run_case(c) for c in case_dirs]
    finished = datetime.now(timezone.utc)
    if args.keep:
        print(f"scratch kept at {root}", file=sys.stderr)
    else:
        shutil.rmtree(root, ignore_errors=True)
    rev = git("rev-parse", "HEAD")
    dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
    report = {
        "report_version": REPORT_VERSION,
        "revision": {"commit": rev or "unknown", "dirty": dirty,
                     "branch": git("rev-parse", "--abbrev-ref", "HEAD")},
        "toolchain": toolchain(args.binary),
        "binary": str(args.binary),
        "corpus": {"version": corpus_version(),
                   "cases": [c.name for c in case_dirs]},
        "host": {"platform": platform.platform(),
                 "machine": platform.machine(),
                 "cpus": os.cpu_count(),
                 "python": platform.python_version()},
        "started": started.isoformat(timespec="seconds"),
        "finished": finished.isoformat(timespec="seconds"),
        "reps": args.reps,
        "fault_injected": fault,
        "cases": cases,
    }
    report["summary"] = summarize(cases)
    report["ok"] = all(c["ok"] for c in cases)
    return report


def print_summary(report: dict) -> None:
    s = report["summary"]["quality"]
    for c in report["cases"]:
        mark = "ok  " if c["ok"] else "FAIL"
        print(f"{mark} {c['name']}" + (f": {c['error']}" if c["error"]
                                       else ""))
        for ph in c["phases"]:
            failed_index = [k for k in ph["index_checks"] if not k["ok"]]
            for k in failed_index:
                print(f"       [{ph['name']}] {k['detail']}")
            if failed_index:
                for line in ph["index_stderr"].strip().splitlines():
                    print(f"         index: {line}")
            for r in ph["queries"]:
                if not r["ok"]:
                    print(f"       [{ph['name']}] {r['id']}: "
                          f"{' '.join(r['argv'][1:])}")
                    for k in r["checks"]:
                        if not k["ok"]:
                            print(f"         {k['kind']}: {k['detail']}")
    print(f"corpus: {s['passed']}/{s['queries']} queries passed, "
          f"{s['checks']} checks; missing {s['missing_findings']}, false "
          f"{s['false_findings']}, fields {s['field_mismatches']}, order "
          f"{s['order_mismatches']}, unparseable "
          f"{s['unparseable_outputs']}; declared incomplete "
          f"{s['declared_incomplete']}, declared unknown "
          f"{s['declared_unknown']}; unstable across reps "
          f"{s['unstable_across_reps']}")


def self_check(args, case_dirs: list[Path]) -> int:
    """The corpus passes clean and fails, for the right reason, under
    every fault injection."""
    failures = []
    clean = run_corpus(args, None, case_dirs)
    print_summary(clean)
    if not clean["ok"]:
        failures.append("the clean run must pass")
    expected_category = {
        "drop-first-record": "missing_findings",
        "inject-record": "false_findings",
        "drop-fields": "field_mismatches",
        "empty-stdout": "unparseable_outputs",
    }
    for fault, category in expected_category.items():
        r = run_corpus(args, fault, case_dirs)
        q = r["summary"]["quality"]
        detected = (not r["ok"]) and q[category] > 0
        print(f"{'ok  ' if detected else 'FAIL'} inject {fault}: "
              f"{q['failed']}/{q['queries']} queries failed, "
              f"{category} = {q[category]}")
        if not detected:
            failures.append(f"fault {fault} was not detected as "
                            f"{category}")
    for f in failures:
        print(f"self-check FAIL: {f}", file=sys.stderr)
    print("corpus self-check: " + ("OK" if not failures else "FAILED"))
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--case", action="append", default=[],
                    help="run only this case (repeatable)")
    ap.add_argument("--reps", type=int, default=3,
                    help="timing repetitions per query (default 3)")
    ap.add_argument("--out", type=Path, help="write the JSON report here")
    ap.add_argument("--inject-fault", choices=sorted(FAULTS),
                    help="corrupt every observed result this way; the run "
                         "must then fail: " + "; ".join(
                             f"{k}: {v}" for k, v in FAULTS.items()))
    ap.add_argument("--self-check", action="store_true",
                    help="run clean, then under every fault injection, "
                         "and check each is detected")
    ap.add_argument("--keep", action="store_true",
                    help="keep the scratch directories")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    args.binary = args.binary.resolve()

    case_dirs = sorted(p for p in CASES.iterdir()
                       if (p / "case.json").exists())
    if args.case:
        case_dirs = [c for c in case_dirs if c.name in args.case]
        missing = set(args.case) - {c.name for c in case_dirs}
        if missing:
            print(f"unknown case(s): {', '.join(sorted(missing))}",
                  file=sys.stderr)
            return 2
    if not case_dirs:
        print("no cases found", file=sys.stderr)
        return 2

    if args.self_check:
        return self_check(args, case_dirs)

    report = run_corpus(args, args.inject_fault, case_dirs)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=1) + "\n")
        print(f"report written to {args.out}", file=sys.stderr)
    print_summary(report)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
