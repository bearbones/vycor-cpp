#!/usr/bin/env python3
# Copyright 2025 Alex Mason
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""End-to-end check of the megascope CLI output contract.

Runs the built binary over examples/deep_chains/ — `megascope index` into
a scratch directory, then every query in QUERIES — and compares each
query's exit code and stdout against examples/deep_chains/cli-golden/.
Unit tests cover the handlers; this is the only thing that exercises the
binary's stdout, exit codes, and flag surface, which are the API
(docs/megascope-cli-review.md §4.3).

    scripts/cli-golden.py --binary build/src/vycor-cpp          # check
    scripts/cli-golden.py --binary build/src/vycor-cpp --update # regenerate

Registered with ctest as `cli_golden` (tests/CMakeLists.txt).

Comparison is order-insensitive (a sorted multiset of lines) and
normalized: the fixture and scratch paths become placeholders, JSON lines
are re-serialized with sorted keys, and records that mention a path
outside the fixture (system headers: their set and spellings vary with
the host's standard library) are dropped, along with the volatile
`_summary` counts that depend on them.
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "examples" / "deep_chains"
GOLDEN_DIR = FIXTURE / "cli-golden"

# The fixture's TUs (gen_compile_commands.sh's list; not-indexed.cpp is
# deliberately absent so the "not indexed" paths stay testable).
SOURCES = [
    "main.cpp", "pipeline.cpp", "stage1_ingest.cpp", "stage2_parse.cpp",
    "stage3_transform.cpp", "stage4_dispatch.cpp", "stage5_sink.cpp",
    "plugins.cpp", "workers.cpp", "tokenizer.cpp", "scheduler.cpp",
    "callbacks.cpp", "async_workers.cpp", "lambda_callbacks.cpp",
]

# (name, argv after "megascope", stdin, golden?). `{fixture}`, `{build}`,
# and `{index}` are substituted; a query with golden=False is checked for
# its exit code only (its stdout depends on the host toolchain).
# `{callsite:CALLER->CALLEE}` resolves to that edge's call site through
# get-callees at run time.
QUERIES: list[tuple[str, list[str], str | None, bool]] = [
    ("tools", ["tools"], None, True),
    ("tools-json", ["tools", "--format", "json"], None, True),
    ("info", ["info"], None, False),
    ("graph-summary", ["graph-summary"], None, False),
    ("list-entry-points",
     ["list-entry-points", "--format", "ndjson"], None, True),
    ("get-callers",
     ["get-callers", "--name", "stage3_transform", "--format", "ndjson"],
     None, True),
    ("get-callers-pretty",
     ["get-callers", "--name", "stage5_sink", "--pretty"], None, True),
    ("get-callees",
     ["get-callees", "--name", "Pipeline::run", "--format", "ndjson"],
     None, True),
    ("get-callees-tsv",
     ["get-callees", "--name", "Pipeline::run", "--format", "tsv"],
     None, True),
    ("find-call-chain",
     ["find-call-chain", "--from", "main", "--to", "stage5_sink",
      "--max-depth", "10", "--format", "ndjson"], None, True),
    ("search-functions",
     ["search-functions", "--query", "stage", "--limit", "20",
      "--format", "ndjson"], None, True),
    ("lookup-function", ["lookup-function", "--name", "stage2_parse"],
     None, True),
    ("call-args",
     ["call", "lookup_function", "--args", '{"name":"stage2_parse"}'],
     None, True),
    ("query-exception-safety",
     ["query-exception-safety", "--function", "stage5_sink",
      "--exception-type", "std::exception"], None, True),
    ("query-throw-propagation",
     ["query-throw-propagation", "--function", "stage5_sink",
      "--exception-type", "std::exception", "--format", "ndjson"],
     None, True),
    ("query-all-path-contexts",
     ["query-all-path-contexts", "--function", "stage5_sink",
      "--max-paths", "5", "--format", "ndjson"], None, True),
    ("query-nearest-catches",
     ["query-nearest-catches", "--function", "stage5_sink",
      "--format", "ndjson"], None, True),
    ("query-call-site-context",
     ["query-call-site-context", "--call-site",
      "{callsite:Pipeline::run->stage1_ingest}"], None, True),
    ("query-raii-scopes",
     ["query-raii-scopes-at-callsite", "--call-site",
      "{callsite:Pipeline::run->stage1_ingest}", "--format", "ndjson"],
     None, True),
    ("analyze-dead-code",
     ["analyze-dead-code", "--format", "ndjson"], None, True),
    ("get-class-hierarchy",
     ["get-class-hierarchy", "--class-name", "Plugin"], None, True),
    ("dump", ["dump"], None, True),
    ("dump-json", ["dump", "--format", "json"], None, True),
    ("batch", ["batch"],
     '{"id":1,"tool":"get_callers","args":{"name":"stage2_parse"}}\n'
     '{"id":2,"tool":"lookup_function","args":{"name":"nope"}}\n'
     '{"id":3,"tool":"no_such_tool","args":{}}\n', True),
    ("not-found", ["get-callers", "--name", "does_not_exist"], None, True),
    ("usage-unknown-flag", ["get-callers", "--bogus", "x"], None, True),
    ("usage-missing-arg", ["get-callers"], None, True),
    ("no-index", ["get-callers", "--index", "{build}/none.vycs",
                  "--name", "main"], None, True),
    ("ephemeral",
     ["get-callees", "--build-path", "{build}", "--source",
      "{fixture}/main.cpp", "--name", "main", "--threads", "1",
      "--format", "ndjson"], None, True),
    ("ephemeral-dump",
     ["dump", "--build-path", "{build}", "--source-re",
      "deep_chains/stage5_sink\\.cpp$", "--threads", "1"], None, True),
]

# Keys whose values depend on the host, the run, or the scratch location.
VOLATILE_KEYS = {"index", "index_bytes", "dependency_count"}
PATH_RE = re.compile(r'"/[^"]*"')
# Standard-library USRs spell out template signatures that shift between
# standard-library versions; display names do not. Records that name a
# system-header location are dropped outright (which declarations a
# header carries is host-dependent too).
STD_USR_RE = re.compile(r'"c:@N@(std|__gnu_cxx)@[^"]*"')


def write_compile_commands(build: Path) -> None:
    entries = []
    for src in SOURCES:
        f = str(FIXTURE / src)
        entries.append({
            "directory": str(FIXTURE),
            "file": f,
            "arguments": ["clang++", "-std=c++17", "-I", str(FIXTURE),
                          "-c", f],
        })
    (build / "compile_commands.json").write_text(json.dumps(entries,
                                                            indent=1))


def scrub(v):
    if isinstance(v, dict):
        return {k: scrub(x) for k, x in v.items() if k not in VOLATILE_KEYS}
    if isinstance(v, list):
        return [scrub(x) for x in v]
    return v


def normalize(text: str, subs: list[tuple[str, str]]) -> list[str]:
    out = []
    for line in text.splitlines():
        for real, placeholder in subs:
            line = line.replace(real, placeholder)
        try:
            v = json.loads(line)
        except ValueError:
            out.append(line)
            continue
        if isinstance(v, dict) and "_summary" in v:
            # Counts here may include toolchain records dropped below.
            v["_summary"] = {k: x for k, x in v["_summary"].items()
                             if not isinstance(x, int)}
        line = json.dumps(scrub(v), sort_keys=True, separators=(",", ":"))
        # Fixture paths are placeholders now; any absolute path left names
        # a system header, and the record is host-dependent.
        if PATH_RE.search(line):
            continue
        out.append(STD_USR_RE.sub('"<std-usr>"', line))
    return sorted(out)


def run_megascope(binary: Path, argv: list[str], stdin: str | None,
                  cwd: Path) -> tuple[int, str, str]:
    p = subprocess.run([str(binary), "megascope", *argv], input=stdin,
                       capture_output=True, text=True, cwd=cwd)
    return p.returncode, p.stdout, p.stderr


def resolve_callsite(binary: Path, index: Path, spec: str,
                     cwd: Path) -> str:
    caller, callee = spec.split("->")
    code, out, err = run_megascope(
        binary, ["get-callees", "--index", str(index), "--name", caller],
        None, cwd)
    if code != 0:
        sys.exit(f"cli-golden: get-callees {caller} failed ({code}): {err}")
    for e in json.loads(out)["callees"]:
        if e["calleeName"] == callee:
            return e["callSite"]
    sys.exit(f"cli-golden: no edge {caller} -> {callee} in the index")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--update", action="store_true",
                    help="rewrite the goldens from this run")
    ap.add_argument("--keep", action="store_true",
                    help="keep the scratch build directory")
    args = ap.parse_args()
    binary = args.binary.resolve()
    if not binary.exists():
        sys.exit(f"cli-golden: no binary at {binary}")

    build = Path(tempfile.mkdtemp(prefix="vycor-cli-golden-"))
    index = build / "deep_chains.vycs"
    subs = [(str(FIXTURE), "{fixture}"), (str(build), "{build}")]
    failures: list[str] = []
    try:
        write_compile_commands(build)
        code, out, err = run_megascope(
            binary, ["index", "--build-path", str(build), "--index",
                     str(index), "--threads", "1"], None, build)
        if code != 0:
            sys.exit(f"cli-golden: megascope index failed ({code}):\n{err}")

        GOLDEN_DIR.mkdir(exist_ok=True)
        seen = set()
        for name, argv, stdin, golden in QUERIES:
            seen.add(name)
            real = []
            for a in argv:
                a = a.replace("{fixture}", str(FIXTURE)) \
                     .replace("{build}", str(build)) \
                     .replace("{index}", str(index))
                m = re.fullmatch(r"\{callsite:(.+)\}", a)
                if m:
                    a = resolve_callsite(binary, index, m.group(1), build)
                real.append(a)
            uses_index = not any(
                a.startswith("--source") or a == "--index" for a in real) \
                and argv[0] not in ("tools",)
            if uses_index:
                real += ["--index", str(index)]
            code, out, err = run_megascope(binary, real, stdin, build)
            shown = " ".join(argv)
            lines = normalize(out, subs)
            header = f"# megascope {shown} -> exit {code}"
            path = GOLDEN_DIR / f"{name}.txt"
            if args.update:
                body = [header] + (lines if golden else [])
                path.write_text("\n".join(body) + "\n")
                continue
            if not path.exists():
                failures.append(f"{name}: no golden at {path} "
                                f"(run with --update)")
                continue
            want = path.read_text().splitlines()
            want_header, want_lines = want[0], want[1:]
            if want_header != header:
                failures.append(f"{name}: {want_header!r} != {header!r}"
                                f"\n  stderr: {err.strip()}")
            if golden and want_lines != lines:
                diff = "\n".join(difflib.unified_diff(
                    want_lines, lines, "golden", "actual", lineterm="",
                    n=1))
                failures.append(f"{name}: stdout differs\n{diff}")
        stale = {p.stem for p in GOLDEN_DIR.glob("*.txt")} - seen
        if stale and not args.update:
            failures.append(f"stale goldens with no query: {sorted(stale)}")
        elif stale:
            for s in stale:
                (GOLDEN_DIR / f"{s}.txt").unlink()
    finally:
        if args.keep:
            print(f"cli-golden: scratch kept at {build}", file=sys.stderr)
        else:
            shutil.rmtree(build, ignore_errors=True)

    if args.update:
        print(f"cli-golden: wrote {len(QUERIES)} goldens to {GOLDEN_DIR}")
        return 0
    if failures:
        print("\n\n".join(failures))
        print(f"\ncli-golden: {len(failures)} of {len(QUERIES)} queries "
              f"differ (scripts/cli-golden.py --update to accept)")
        return 1
    print(f"cli-golden: {len(QUERIES)} queries match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
