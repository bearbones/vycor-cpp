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
"""Paths and source text that are not UTF-8, through the built binary.

A Latin-1 directory name, file name or comment is legal input. Every
string the index holds is index text (include/vycor/callgraph/Utf8.h):
valid UTF-8, converted from the raw bytes by an exact escape, so

  - nothing the binary prints aborts llvm::json in a build with
    assertions (`index`'s summary line, `--stats-json`, `info`, `dump`,
    the tools);
  - two paths that differ in one non-UTF-8 byte stay two TUs: a warm
    refresh of one keeps the other's facts, in-process and in isolated
    workers;
  - two `static helper()` in such files stay two functions (their USRs
    embed the file name);
  - a query names a site by its raw path or by its printed form.

Also a ctest: `ctest -R utf8_paths`.

    scripts/utf8-paths-check.py --binary build/src/vycor-cpp [-v] [--keep]
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

ESCAPE_BASE = 0x10FF00  # byte b (0x80..0xFF) is U+10FF00 + b in index text

SOURCES = {
    b"a\xe9.cpp": b"int fx();\nint ca(int v) {\n"
                  b"  if (v > /* caf\xe9 */ 0)\n    return fx();\n"
                  b"  return 0;\n}\n",
    b"a\xe8.cpp": b"int fx();\nint cb() { return fx(); }\n",
    b"b\xe9.cpp": b"static int helper() { return 1; }\n"
                  b"int ub() { return helper(); }\n",
    b"b\xe8.cpp": b"static int helper() { return 2; }\n"
                  b"int vb() { return helper(); }\n",
    b"fx.cpp": b"int fx() { return 0; }\nint main() { return fx(); }\n",
}

VERBOSE = False
FAILURES: list[str] = []


def check(cond: bool, what: str) -> None:
    if VERBOSE or not cond:
        print(("ok    " if cond else "FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


def to_index_text(raw: bytes) -> str:
    """The index text of raw bytes (Utf8.h toIndexText)."""
    out = []
    i = 0
    while i < len(raw):
        for n in (1, 2, 3, 4):
            try:
                ch = raw[i:i + n].decode("utf-8", errors="strict")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1 and ord(ch) < ESCAPE_BASE + 0x80:
                out.append(ch)
                i += n
                break
            if len(ch) == 1:  # a literal escape-range code point
                out.extend(chr(ESCAPE_BASE + b) for b in raw[i:i + n])
                i += n
                break
        else:
            out.append(chr(ESCAPE_BASE + raw[i]))
            i += 1
    return "".join(out)


def from_index_text(text: str) -> bytes:
    """The raw bytes of index text (Utf8.h fromIndexText)."""
    out = bytearray()
    for ch in text:
        cp = ord(ch)
        if ESCAPE_BASE + 0x80 <= cp <= ESCAPE_BASE + 0xFF:
            out.append(cp - ESCAPE_BASE)
        else:
            out += ch.encode("utf-8")
    return bytes(out)


def run(binary: bytes, args: list[bytes]) -> subprocess.CompletedProcess:
    cmd = [binary, b"megascope"] + args
    if VERBOSE:
        print("$ " + " ".join(os.fsdecode(a) for a in cmd))
    return subprocess.run(cmd, capture_output=True, timeout=600)


def tool_json(binary: bytes, index: bytes, args: list[bytes]) -> tuple[int, dict]:
    r = run(binary, args + [b"--index", index])
    try:
        payload = json.loads(r.stdout.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        payload = {}
    if r.returncode not in (0, 1, 4):
        print(r.stderr.decode("utf-8", errors="replace"), file=sys.stderr)
    return r.returncode, payload


def callers_of(binary: bytes, index: bytes, name: bytes) -> list[str]:
    rc, payload = tool_json(binary, index, [b"get-callers", b"--name", name])
    return sorted(c.get("caller", c.get("callerName", ""))
                  for c in payload.get("callers", []))


def scenario(binary: bytes, root: bytes, threads: bytes) -> None:
    label = "threads=" + os.fsdecode(threads)
    src = os.path.join(root, b"src\xe9-" + threads)
    build = os.path.join(src, b"build")
    os.makedirs(build)
    with open(os.path.join(build, b"compile_flags.txt"), "wb") as f:
        f.write(b"-std=c++17\n")
    sources = []
    for name, text in SOURCES.items():
        path = os.path.join(src, name)
        with open(path, "wb") as f:
            f.write(text)
        # Stable stamps: a file written moments before a bake is recorded
        # with an unknown stamp and re-checked next time (index-provenance).
        old = os.stat(path).st_mtime_ns - 60_000_000_000
        os.utime(path, ns=(old, old))
        sources += [b"--source", path]
    index = os.path.join(root, b"idx\xe9-" + threads + b".vycs")
    stats = os.path.join(root, b"stats\xe9-" + threads + b".json")

    # Cold bake: the summary line names the index (index text), and
    # --stats-json lists every TU.
    common = [b"--build-path", build, b"--index", index,
              b"--threads", threads]
    r = run(binary, [b"index"] + common + sources + [b"--stats-json", stats])
    check(r.returncode == 0,
          f"{label}: cold index exits 0 (got {r.returncode}): "
          + r.stderr.decode("utf-8", errors="replace")[-400:])
    try:
        summary = json.loads(r.stdout.decode("utf-8").strip().splitlines()[-1])
    except (UnicodeDecodeError, json.JSONDecodeError, IndexError):
        summary = {}
    check(summary.get("index") == to_index_text(index),
          f"{label}: summary names the index as index text")
    check(summary.get("indexed") == len(SOURCES),
          f"{label}: every TU indexed ({summary.get('indexed')})")
    try:
        with open(stats, "rb") as f:
            stat_json = json.loads(f.read().decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        stat_json = {}
    files = sorted(t.get("file", "") for t in stat_json.get("tu", []))
    check(to_index_text(os.path.join(src, b"a\xe9.cpp")) in files,
          f"{label}: --stats-json lists the Latin-1 TU as index text")

    check(callers_of(binary, index, b"fx") == ["ca", "cb", "main"],
          f"{label}: fx has three callers after the cold bake")

    # A warm refresh of each Latin-1 TU in turn keeps the other's facts.
    for name in (b"a\xe9.cpp", b"a\xe8.cpp"):
        path = os.path.join(src, name)
        st = os.stat(path)
        os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns + 2_000_000_000))
        r = run(binary, [b"index"] + common)
        try:
            refreshed = json.loads(
                r.stdout.decode("utf-8").strip().splitlines()[-1])
        except (UnicodeDecodeError, json.JSONDecodeError, IndexError):
            refreshed = {}
        check(r.returncode == 0 and refreshed.get("mode") == "warm"
              and refreshed.get("refreshed") == 1,
              f"{label}: warm refresh of {os.fsdecode(name)} refreshes one "
              f"TU ({refreshed})")
        check(callers_of(binary, index, b"fx") == ["ca", "cb", "main"],
              f"{label}: after refreshing {os.fsdecode(name)}, fx still has "
              f"three callers")

    # Two internal-linkage helpers: two functions, so a bare name is
    # ambiguous (exit 4) with two candidates.
    rc, payload = tool_json(binary, index,
                            [b"lookup-function", b"--name", b"helper"])
    check(rc == 4 and len(payload.get("candidates", [])) == 2,
          f"{label}: `helper` is ambiguous between two functions "
          f"(exit {rc})")

    # dump and info print, as UTF-8, the Latin-1 path and comment.
    r = run(binary, [b"dump", b"--index", index])
    check(r.returncode == 0, f"{label}: dump exits 0 (got {r.returncode})")
    try:
        records = [json.loads(line)
                   for line in r.stdout.decode("utf-8").splitlines()]
    except (UnicodeDecodeError, json.JSONDecodeError):
        records = []
    guards = [g.get("conditionText", "") for rec in records
              for g in rec.get("guards", rec.get("enclosingGuards", []))]
    check(any(from_index_text(g).find(b"caf\xe9") >= 0 for g in guards),
          f"{label}: dump prints the Latin-1 comment, reversibly")
    site = next((rec.get("callSite", "") for rec in records
                 if rec.get("calleeName") == "fx"
                 and "a" + chr(ESCAPE_BASE + 0xE9) + ".cpp"
                 in rec.get("callSite", "")), "")
    check(site != "", f"{label}: dump holds the call site in a\\xe9.cpp")
    r = run(binary, [b"info", b"--files", b"--index", index])
    check(r.returncode == 0, f"{label}: info --files exits 0")

    # A site is found by its raw path (as a shell passes it) and by its
    # printed form.
    if site:
        for spelling in (from_index_text(site), site.encode("utf-8")):
            rc, payload = tool_json(
                binary, index,
                [b"query-call-site-context", b"--call-site", spelling])
            check(rc == 0 and payload.get("status") == "ok",
                  f"{label}: call site found by "
                  f"{'raw' if spelling != site.encode() else 'printed'} "
                  f"spelling (exit {rc})")


def main() -> int:
    global VERBOSE
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--binary", required=True)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--keep", action="store_true",
                    help="keep the scratch directory")
    opts = ap.parse_args()
    VERBOSE = opts.verbose
    binary = os.fsencode(os.path.abspath(opts.binary))

    root = os.fsencode(tempfile.mkdtemp(prefix="vycor-utf8-"))
    try:
        # In-process (--threads 1) and in isolated workers (--threads 2).
        for threads in (b"1", b"2"):
            scenario(binary, root, threads)
    finally:
        if opts.keep:
            print("kept " + os.fsdecode(root))
        else:
            shutil.rmtree(root, ignore_errors=True)

    if FAILURES:
        print(f"{len(FAILURES)} check(s) failed", file=sys.stderr)
        return 1
    print("utf8-paths-check: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
