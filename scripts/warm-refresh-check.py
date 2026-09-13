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
"""Warm-refresh equivalence check for `megascope index`.

For every kind of change a warm start must notice — a TU edit, a header
edit, a compile-flag change, a working-directory change, a bake-environment
change, a narrower selection, and a TU whose parse failed and later
recovers — this script bakes an index cold, applies the change, refreshes
the index warm, and demands that `megascope dump` over the refreshed index
equals `dump` over a clean `--force` rebuild of the same tree. It also
checks the refresh accounting the `index` summary prints (what was
refreshed and why) and the coverage facts `info` reports.

Every scenario runs with the in-process bake and again under
--isolate-workers. Also a ctest: `ctest -R warm_refresh`.

    scripts/warm-refresh-check.py --binary build/src/vycor-cpp [-v] [--keep]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TU = """\
#include "target.h"
void alpha();
void beta();
#ifdef NEW_TARGET
void entry() { beta(); }
#else
void entry() { alpha(); }
#endif
int main() { entry(); via_header(); return 0; }
"""
TARGET_H_V1 = "void gamma();\ninline void via_header() { gamma(); }\n"
TARGET_H_V2 = "void delta();\ninline void via_header() { delta(); }\n"
OTHER = "void other() {}\nvoid another() { other(); }\n"
BROKEN = '#include "missing.h"\nvoid broken_fn() { after_include(); }\n'
# Untouched ballast: an edit plus the retried TU must stay under half the
# selection, or the warm start (correctly) prefers a full rebuild.
PAD = "void pad{n}_a() {{}}\nvoid pad{n}_b() {{ pad{n}_a(); }}\n"
PADS = ("pad1.cpp", "pad2.cpp")
MISSING_H = "void after_include();\n"

# Files touched in the same second as a bake are re-checked once on the
# next start (the unstable-stamp window), so every file this script writes
# is backdated before it is indexed: two minutes into the past, one second
# later per aging pass so an edited file (even one of the same size) gets
# a stamp its previous version never had.
AGE_SECONDS = 120

# Tool queries whose raw stdout must be identical over a warm-refreshed
# index and a clean rebuild (the order of every list is contractual;
# only the bake reference differs).
RAW_QUERIES = (
    ["get-callers", "--name", "alpha"],
    ["get-callees", "--name", "main"],
    ["find-call-chain", "--to", "gamma"],
    ["search-functions", "--query", "a", "--limit", "3"],
    ["list-callback-sites"],
    ["analyze-dead-code"],
    ["graph-summary"],
)
BAKE_RE = re.compile(r'"bake":"[0-9a-f]+@[0-9]+"')


def strip_bake(text: str) -> str:
    return BAKE_RE.sub('"bake":"<bake>"', text)


class Check:
    def __init__(self, binary: Path, root: Path, isolate: bool,
                 verbose: bool) -> None:
        self.binary = binary
        self.root = root
        self.isolate = isolate
        self.verbose = verbose
        self.failures: list[str] = []
        self.clock = time.time() - AGE_SECONDS

    # ---- fixture -----------------------------------------------------------

    def fixture(self, name: str) -> Path:
        d = self.root / name
        d.mkdir()
        (d / "tu.cpp").write_text(TU)
        (d / "target.h").write_text(TARGET_H_V1)
        (d / "other.cpp").write_text(OTHER)
        (d / "broken.cpp").write_text(BROKEN)
        for n, pad in enumerate(PADS, 1):
            (d / pad).write_text(PAD.format(n=n))
        self.write_db(d)
        self.age(d)
        return d

    @staticmethod
    def write_db(d: Path, flags: dict[str, list[str]] | None = None,
                 directory: dict[str, str] | None = None) -> None:
        entries = []
        for tu in ("tu.cpp", "other.cpp", "broken.cpp", *PADS):
            f = str(d / tu)
            extra = (flags or {}).get(tu, [])
            entries.append({
                "directory": (directory or {}).get(tu, str(d)),
                "file": f,
                "arguments": ["clang++", "-std=c++17", *extra, "-c", f],
            })
        (d / "compile_commands.json").write_text(json.dumps(entries,
                                                            indent=1))

    def age(self, d: Path) -> None:
        """Backdate the files written since the last aging pass."""
        self.clock += 1
        recent = time.time() - AGE_SECONDS / 2
        for p in d.rglob("*"):
            if p.is_file() and p.stat().st_mtime > recent:
                os.utime(p, (self.clock, self.clock))

    # ---- running -----------------------------------------------------------

    def megascope(self, argv: list[str], cwd: Path) -> tuple[int, str, str]:
        p = subprocess.run([str(self.binary), "megascope", *argv],
                           capture_output=True, text=True, cwd=cwd)
        if self.verbose:
            print(f"$ megascope {' '.join(argv)} -> {p.returncode}",
                  file=sys.stderr)
            if p.stderr:
                print(p.stderr.rstrip(), file=sys.stderr)
        return p.returncode, p.stdout, p.stderr

    def index(self, d: Path, index: Path, *extra: str,
              force: bool = False) -> dict:
        argv = ["index", "--build-path", str(d), "--index", str(index),
                "--threads", "2", *extra]
        if force:
            argv.append("--force")
        if self.isolate:
            argv += ["--isolate-workers", "--workers", "2"]
        code, out, err = self.megascope(argv, d)
        if code != 0:
            raise RuntimeError(f"index failed ({code}): {err}")
        summary = json.loads(out)
        summary["_stderr"] = err
        return summary

    def dump(self, index: Path, d: Path) -> list[str]:
        code, out, _ = self.megascope(["dump", "--index", str(index)], d)
        # Exit 1 is the empty-result code: an index with no call sites.
        if code not in (0, 1):
            raise RuntimeError(f"dump failed ({code})")
        return sorted(out.splitlines())

    def info(self, index: Path, d: Path) -> dict:
        code, out, _ = self.megascope(["info", "--index", str(index)], d)
        if code != 0:
            raise RuntimeError(f"info failed ({code})")
        return json.loads(out)

    # ---- assertions --------------------------------------------------------

    def expect(self, scenario: str, cond: bool, what: str) -> None:
        if not cond:
            self.failures.append(f"[{self.mode()}] {scenario}: {what}")
            print(f"FAIL [{self.mode()}] {scenario}: {what}",
                  file=sys.stderr)

    def mode(self) -> str:
        return "isolated" if self.isolate else "in-process"

    def equal_to_clean(self, scenario: str, d: Path, warm: Path,
                       *extra: str) -> None:
        clean = d / "clean.vycs"
        self.index(d, clean, *extra, force=True)
        a, b = self.dump(warm, d), self.dump(clean, d)
        self.expect(scenario, a == b,
                    f"warm dump differs from clean rebuild\n"
                    f"  warm-only:  {sorted(set(a) - set(b))[:3]}\n"
                    f"  clean-only: {sorted(set(b) - set(a))[:3]}")
        ia, ib = self.info(warm, d), self.info(clean, d)
        self.expect(scenario, ia["coverage"] == ib["coverage"],
                    f"coverage differs: warm {ia['coverage']} vs clean "
                    f"{ib['coverage']}")
        # The tool answers, raw: every list in its contractual order
        # (docs/deterministic-output.md), not as a sorted multiset.
        for argv in RAW_QUERIES:
            ca, oa, _ = self.megascope([*argv, "--index", str(warm)], d)
            cb, ob, _ = self.megascope([*argv, "--index", str(clean)], d)
            oa, ob = strip_bake(oa), strip_bake(ob)
            self.expect(scenario, (ca, oa) == (cb, ob),
                        f"{' '.join(argv)}: warm answer differs from "
                        f"clean rebuild (exit {ca} vs {cb})\n"
                        f"  warm:  {oa[:200]!r}\n  clean: {ob[:200]!r}")
        # The semantic diff of the two must be empty: same functions, same
        # calls, same call-site contexts (docs/change-impact.md).
        rc, out, _ = self.megascope(
            ["diff", "--before", str(warm), "--after", str(clean)], d)
        try:
            payload = json.loads(out)
        except json.JSONDecodeError:
            payload = {}
        n = payload.get("summary", {}).get("changeCount")
        self.expect(scenario, rc == 1 and payload.get("status") == "ok"
                    and n == 0,
                    f"diff warm/clean: exit {rc}, status "
                    f"{payload.get('status')!r}, {n} changes\n"
                    f"  {out[:300]!r}")

    def expect_summary(self, scenario: str, s: dict, **fields: int | str
                       ) -> None:
        for k, v in fields.items():
            self.expect(scenario, s.get(k) == v,
                        f"summary {k} = {s.get(k)!r}, expected {v!r}")

    # ---- scenarios ---------------------------------------------------------

    def run_all(self) -> None:
        for scenario in (self.unchanged, self.tu_edit, self.header_edit,
                         self.compile_flag, self.working_directory,
                         self.environment, self.selection,
                         self.partial_recovery, self.chronic_failures):
            scenario()

    def cold(self, name: str) -> tuple[Path, Path, dict]:
        d = self.fixture(f"{name}-{self.mode()}")
        idx = d / "warm.vycs"
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="cold", files=5, indexed=4,
                            partial=1, failed=0)
        self.expect(name, "broken.cpp" in s["_stderr"] or
                    "1 partial" in s["_stderr"],
                    "cold bake did not warn about the partial TU")
        return d, idx, s

    def unchanged(self) -> None:
        name = "unchanged"
        d, idx, _ = self.cold(name)
        # Nothing changed: the partial TU stays as recorded (a retry would
        # cost the full load and save) and the refresh is meta-only.
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="warm", refreshed=0, retried=0,
                            refreshed_for_inputs=0,
                            refreshed_for_headers=0, dropped=0,
                            indexed=4, partial=1)
        self.expect(name, "left as recorded" in s["_stderr"] and
                    "skipping re-save" in s["_stderr"],
                    "an unchanged refresh with a failed TU was not meta-only")
        # Asked for, the retry happens (and fails the same way).
        s = self.index(d, idx, "--retry-failed")
        self.expect_summary(name, s, mode="warm", refreshed=1, retried=1,
                            refreshed_for_inputs=0,
                            refreshed_for_headers=0, dropped=0,
                            indexed=4, partial=1)
        # Make broken.cpp whole so the index can be fully clean, then an
        # unchanged refresh touches nothing at all.
        (d / "missing.h").write_text(MISSING_H)
        self.age(d)
        s = self.index(d, idx, "--retry-failed")
        self.expect_summary(name, s, mode="warm", refreshed=1, retried=1,
                            indexed=5, partial=0)
        self.age(d)
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="warm", refreshed=0, retried=0,
                            indexed=5)
        self.expect(name, "skipping re-save" in s["_stderr"],
                    "an unchanged refresh re-saved the index")
        self.equal_to_clean(name, d, idx)

    def tu_edit(self) -> None:
        name = "tu-edit"
        d, idx, _ = self.cold(name)
        (d / "other.cpp").write_text(OTHER + "void third() { another(); }\n")
        self.age(d)
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="warm", refreshed=2, retried=1,
                            refreshed_for_inputs=0, refreshed_for_headers=0)
        self.equal_to_clean(name, d, idx)

    def header_edit(self) -> None:
        name = "header-edit"
        d, idx, _ = self.cold(name)
        (d / "target.h").write_text(TARGET_H_V2)
        self.age(d)
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="warm", refreshed=2,
                            refreshed_for_headers=1, retried=1)
        self.equal_to_clean(name, d, idx)
        dump = self.dump(idx, d)
        self.expect(name, any('"delta"' in line for line in dump),
                    "refreshed index does not see the new header callee")

    def compile_flag(self) -> None:
        # The 2026-09-08 regression: only the compile command changes.
        name = "compile-flag"
        d, idx, _ = self.cold(name)
        self.write_db(d, flags={"tu.cpp": ["-DNEW_TARGET"]})
        self.age(d)
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="warm", refreshed=2,
                            refreshed_for_inputs=1, refreshed_for_headers=0,
                            retried=1)
        self.equal_to_clean(name, d, idx)
        dump = self.dump(idx, d)
        self.expect(name, any('"beta"' in line for line in dump),
                    "refreshed index still calls the old callee")
        self.expect(name, not any('"alpha"' in line for line in dump),
                    "refreshed index still calls alpha")

    def working_directory(self) -> None:
        # Same arguments, different working directory: a relative -I
        # resolves to a different header.
        name = "working-directory"
        d, idx, _ = self.cold(name)
        # The quoted include would find the header beside the TU first.
        (d / "target.h").unlink()
        for sub, text in (("wd1", TARGET_H_V1), ("wd2", TARGET_H_V2)):
            (d / sub / "inc").mkdir(parents=True)
            (d / sub / "inc" / "target.h").write_text(text)
        self.write_db(d, flags={"tu.cpp": ["-Iinc"]},
                      directory={"tu.cpp": str(d / "wd1")})
        self.age(d)
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="warm", refreshed=2,
                            refreshed_for_inputs=1, retried=1)
        self.write_db(d, flags={"tu.cpp": ["-Iinc"]},
                      directory={"tu.cpp": str(d / "wd2")})
        self.age(d)
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="warm", refreshed=2,
                            refreshed_for_inputs=1, retried=1)
        self.equal_to_clean(name, d, idx)
        dump = self.dump(idx, d)
        self.expect(name, any('"delta"' in line for line in dump),
                    "refreshed index does not see the wd2 header")

    def environment(self) -> None:
        # A bake-wide input (--extra-arg) changes every TU's fingerprint:
        # the warm start rebuilds and says why.
        name = "environment"
        d, idx, _ = self.cold(name)
        self.age(d)
        s = self.index(d, idx, "--extra-arg=-DNEW_TARGET")
        self.expect_summary(name, s, mode="cold", refreshed=0)
        self.expect(name, "bake environment differs" in s["_stderr"],
                    "no environment message on stderr")
        self.equal_to_clean(name, d, idx, "--extra-arg=-DNEW_TARGET")
        dump = self.dump(idx, d)
        self.expect(name, any('"beta"' in line for line in dump),
                    "rebuilt index does not honor the extra arg")

    def selection(self) -> None:
        name = "selection"
        d, idx, _ = self.cold(name)
        self.age(d)
        s = self.index(d, idx, "--source-re", r"(tu|broken)\.cpp$")
        self.expect_summary(name, s, mode="warm", files=2, dropped=3,
                            refreshed=1, retried=1)
        dump = self.dump(idx, d)
        self.expect(name, not any('"another"' in line for line in dump),
                    "dropped TU still contributes call sites")
        # A bare refresh keeps the narrowed scope.
        self.age(d)
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="warm", files=2, dropped=0)
        clean = d / "clean.vycs"
        self.index(d, clean, "--source-re", r"(tu|broken)\.cpp$", force=True)
        self.expect(name, self.dump(idx, d) == self.dump(clean, d),
                    "narrowed warm index differs from a clean narrow bake")

    def partial_recovery(self) -> None:
        # broken.cpp fails to parse until missing.h appears. Nothing the
        # stamps watch changes (the header never existed), so only the
        # recorded outcome can bring it back.
        name = "partial-recovery"
        d, idx, _ = self.cold(name)
        info = self.info(idx, d)
        self.expect(name, info["coverage"] == {"requested": 5, "indexed": 4,
                                               "partial": 1, "failed": 0,
                                               "complete": False},
                    f"cold coverage {info['coverage']}")
        self.expect(name, info["provenance"]["analyzer"].startswith(
                        "vycor-cpp "), "provenance lacks the analyzer")
        self.expect(name, info["provenance"]["toolchain"].startswith(
                        "LLVM "), "provenance lacks the toolchain")
        code, out, _ = self.megascope(["info", "--index", str(idx),
                                       "--files", "--format", "ndjson"], d)
        rows = [json.loads(line) for line in out.splitlines()[1:]]
        by_path = {Path(r["path"]).name: r for r in rows}
        self.expect(name, by_path["broken.cpp"]["status"] == "partial" and
                    by_path["broken.cpp"]["detail"] == "parse errors",
                    f"broken.cpp row: {by_path.get('broken.cpp')}")
        self.expect(name, by_path["tu.cpp"]["status"] == "indexed" and
                    len(by_path["tu.cpp"]["fingerprint"]) == 40,
                    f"tu.cpp row: {by_path.get('tu.cpp')}")
        (d / "missing.h").write_text(MISSING_H)
        self.age(d)
        # Nothing recorded changed (the header never existed to be
        # stamped): a bare refresh keeps the recorded outcome and says so.
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="warm", refreshed=0, retried=0,
                            indexed=4, partial=1)
        self.expect(name, "left as recorded" in s["_stderr"],
                    "bare refresh did not report the failed TU")
        # An explicit retry recovers it.
        s = self.index(d, idx, "--retry-failed")
        self.expect_summary(name, s, mode="warm", refreshed=1, retried=1,
                            indexed=5, partial=0, failed=0)
        self.equal_to_clean(name, d, idx)
        dump = self.dump(idx, d)
        self.expect(name, any('"after_include"' in line for line in dump),
                    "recovered TU's call site is missing")

    def chronic_failures(self) -> None:
        # Three of five TUs never parse. A bare refresh stays meta-only;
        # an edit to a healthy TU re-parses it and retries the three
        # alongside — a warm refresh, not a full rebuild, because retries
        # do not count toward the "more than half changed" rule.
        name = "chronic-failures"
        d = self.fixture(f"{name}-{self.mode()}")
        for pad in PADS:
            (d / pad).write_text(BROKEN.replace("broken_fn", pad[:-4]))
        self.age(d)
        idx = d / "warm.vycs"
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="cold", files=5, indexed=2,
                            partial=3, failed=0)
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="warm", refreshed=0, retried=0,
                            indexed=2, partial=3)
        self.expect(name, "skipping re-save" in s["_stderr"],
                    "bare refresh with chronic failures was not meta-only")
        (d / "other.cpp").write_text(OTHER + "void third() { another(); }\n")
        self.age(d)
        s = self.index(d, idx)
        self.expect_summary(name, s, mode="warm", refreshed=4, retried=3,
                            refreshed_for_inputs=0, refreshed_for_headers=0,
                            indexed=2, partial=3)
        self.equal_to_clean(name, d, idx)
        # Fixing the inputs recovers all three on the piggybacked retry.
        (d / "missing.h").write_text(MISSING_H)
        self.age(d)
        s = self.index(d, idx, "--retry-failed")
        self.expect_summary(name, s, mode="warm", refreshed=3, retried=3,
                            indexed=5, partial=0, failed=0)
        self.equal_to_clean(name, d, idx)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--binary", required=True, type=Path)
    ap.add_argument("--keep", action="store_true",
                    help="leave the scratch directory behind")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        print(f"no such binary: {binary}", file=sys.stderr)
        return 2

    root = Path(tempfile.mkdtemp(prefix="vycor-warm-refresh-"))
    failures: list[str] = []
    try:
        for isolate in (False, True):
            check = Check(binary, root, isolate, args.verbose)
            check.run_all()
            failures += check.failures
    finally:
        if args.keep:
            print(f"scratch kept at {root}", file=sys.stderr)
        else:
            shutil.rmtree(root, ignore_errors=True)
    if failures:
        print(f"{len(failures)} failure(s)", file=sys.stderr)
        return 1
    print("warm-refresh-check: 9 scenarios x 2 bake modes OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
