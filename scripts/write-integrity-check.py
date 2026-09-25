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
"""Index write integrity check for `megascope index` (the built binary).

Scenarios, each against a scratch project baked cold first:

  empty_bake       an --isolate-workers bake that cannot create its shard
                   directory must fail and leave the index as it was (it
                   used to save an empty index over it and exit 0);
  concurrent       two `index --force` runs on one index at once, several
                   rounds: both succeed, the result equals a clean bake,
                   and no temp file is left behind;
  lock             while another process holds `<index>.lock`, `index
                   --no-wait` fails at once and `index` waits, says so,
                   and finishes once the lock is released;
  corruption       a flipped byte in each section, and a truncated file,
                   make `info` and a query fail with exit 3, naming the
                   damaged section.

Also a ctest: `ctest -R write_integrity`.

    scripts/write-integrity-check.py --binary build/src/vycor-cpp [-v] [--keep]
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    import fcntl
except ImportError:  # not POSIX: the lock scenario is skipped
    fcntl = None

TUS = 12
FNS = 40

# Section kinds in the v13 header table (Snapshot.cpp).
SECTION_NAMES = {0: "meta", 1: "graph", 2: "control_flow", 3: "channels"}


def tu_source(n: int) -> str:
    lines = [f"void t{n}_f{FNS}() {{}}"]
    for i in range(FNS - 1, -1, -1):
        lines.append(f"void t{n}_f{i}() {{ t{n}_f{i + 1}(); }}")
    if n == 0:
        lines.append("int main() { t0_f0(); return 0; }")
    return "\n".join(lines) + "\n"


class Check:
    def __init__(self, binary: Path, root: Path, verbose: bool) -> None:
        self.binary = binary
        self.root = root
        self.verbose = verbose
        self.failures: list[str] = []

    def fixture(self, name: str) -> Path:
        d = self.root / name
        d.mkdir()
        entries = []
        for n in range(TUS):
            f = d / f"tu{n}.cpp"
            f.write_text(tu_source(n))
            entries.append({"directory": str(d), "file": str(f),
                            "arguments": ["clang++", "-std=c++17", "-c",
                                          str(f)]})
        (d / "compile_commands.json").write_text(json.dumps(entries))
        # Backdated so no stamp is in the unstable window of a bake.
        past = time.time() - 120
        for p in d.iterdir():
            os.utime(p, (past, past))
        return d

    def megascope(self, argv: list[str], cwd: Path,
                  env: dict[str, str] | None = None
                  ) -> tuple[int, str, str]:
        p = subprocess.run([str(self.binary), "megascope", *argv],
                           capture_output=True, text=True, cwd=cwd,
                           env={**os.environ, **(env or {})})
        if self.verbose:
            print(f"$ megascope {' '.join(argv)} -> {p.returncode}",
                  file=sys.stderr)
            if p.stderr:
                print(p.stderr.rstrip(), file=sys.stderr)
        return p.returncode, p.stdout, p.stderr

    def index_argv(self, d: Path, index: Path, *extra: str) -> list[str]:
        return ["index", "--build-path", str(d), "--index", str(index),
                "--threads", "2", *extra]

    def index(self, d: Path, index: Path, *extra: str) -> None:
        code, _, err = self.megascope(self.index_argv(d, index, *extra), d)
        if code != 0:
            raise RuntimeError(f"index failed ({code}): {err}")

    def dump(self, index: Path, d: Path) -> list[str]:
        code, out, err = self.megascope(["dump", "--index", str(index)], d)
        if code not in (0, 1):
            raise RuntimeError(f"dump failed ({code}): {err}")
        return sorted(out.splitlines())

    def expect(self, scenario: str, cond: bool, what: str) -> None:
        if not cond:
            self.failures.append(f"{scenario}: {what}")
            print(f"FAIL {scenario}: {what}", file=sys.stderr)

    # ---- scenarios ---------------------------------------------------------

    def empty_bake(self) -> None:
        name = "empty_bake"
        d = self.fixture(name)
        idx = d / "index.vycs"
        self.index(d, idx)
        before = idx.read_bytes()
        # The shard directory goes under $TMPDIR: point it nowhere.
        code, out, err = self.megascope(
            self.index_argv(d, idx, "--force", "--isolate-workers",
                            "--workers", "2"),
            d, env={"TMPDIR": str(d / "no-such-dir")})
        self.expect(name, code != 0,
                    f"index exited {code} after a bake that could not run "
                    f"(stdout {out.strip()[:200]!r})")
        self.expect(name, idx.read_bytes() == before,
                    "the index was rewritten by a bake that could not run")

    def concurrent(self) -> None:
        name = "concurrent"
        d = self.fixture(name)
        clean = d / "clean.vycs"
        self.index(d, clean)
        expected = self.dump(clean, d)
        idx = d / "index.vycs"
        for rnd in range(4):
            procs = [subprocess.Popen(
                [str(self.binary), "megascope",
                 *self.index_argv(d, idx, "--force")],
                cwd=d, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True) for _ in range(2)]
            results = [p.communicate() + (p.returncode,) for p in procs]
            for out, err, code in results:
                self.expect(name, code == 0,
                            f"round {rnd}: a writer exited {code}: "
                            f"{err.strip()[-300:]}")
            code, _, err = self.megascope(["info", "--index", str(idx)], d)
            self.expect(name, code == 0,
                        f"round {rnd}: info exited {code}: {err.strip()}")
            if code == 0:
                self.expect(name, self.dump(idx, d) == expected,
                            f"round {rnd}: index differs from a clean bake")
        leftovers = sorted(p.name for p in d.iterdir()
                           if ".tmp" in p.name)
        self.expect(name, not leftovers, f"temp files left: {leftovers}")

    def lock(self) -> None:
        name = "lock"
        if fcntl is None:
            print(f"skip {name}: no fcntl on this platform", file=sys.stderr)
            return
        d = self.fixture(name)
        idx = d / "index.vycs"
        self.index(d, idx)
        with open(str(idx) + ".lock", "a+") as held:
            fcntl.flock(held.fileno(), fcntl.LOCK_EX)
            code, _, err = self.megascope(
                self.index_argv(d, idx, "--force", "--no-wait"), d)
            self.expect(name, code != 0,
                        f"--no-wait exited {code} while the lock was held")
            self.expect(name, ".lock" in err,
                        f"--no-wait did not name the lock: {err.strip()}")
            errlog = d / "waiter.err"
            with open(errlog, "w") as ef:
                waiter = subprocess.Popen(
                    [str(self.binary), "megascope",
                     *self.index_argv(d, idx, "--force")],
                    cwd=d, stdout=subprocess.DEVNULL, stderr=ef)
                time.sleep(1.5)
                self.expect(name, waiter.poll() is None,
                            "a second writer did not wait for the lock")
                fcntl.flock(held.fileno(), fcntl.LOCK_UN)
                code = waiter.wait(timeout=120)
            self.expect(name, code == 0,
                        f"the waiting writer exited {code}")
            self.expect(name, "waiting" in errlog.read_text(),
                        "the waiting writer did not say it was waiting")

    def corruption(self) -> None:
        name = "corruption"
        d = self.fixture(name)
        idx = d / "index.vycs"
        self.index(d, idx)
        good = idx.read_bytes()
        table = section_table(good)
        self.expect(name, table is not None,
                    "cannot read the section table (format changed?)")
        if table is None:
            return
        for kind, offset, length in table:
            section = SECTION_NAMES.get(kind, str(kind))
            if length == 0 or offset + length > len(good):
                continue
            bad = bytearray(good)
            bad[offset + length // 2] ^= 0x20
            idx.write_bytes(bytes(bad))
            for argv in (["info"], ["get-callers", "--name", "t0_f3"],
                         ["dump"]):
                code, _, err = self.megascope([*argv, "--index", str(idx)],
                                              d)
                # info verifies every section; a query or dump verifies
                # the sections it decodes.
                decodes = argv[0] == "info" or section == "meta" or (
                    argv[0] == "get-callers" and section == "graph") or (
                    argv[0] == "dump" and section in ("control_flow",
                                                      "channels"))
                if not decodes:
                    continue
                self.expect(name, code == 3,
                            f"{argv[0]} over a damaged {section} section "
                            f"exited {code}")
                self.expect(name, section in err,
                            f"{argv[0]} did not name the damaged "
                            f"{section} section: {err.strip()}")
        idx.write_bytes(good[: len(good) * 2 // 3])
        code, _, err = self.megascope(["info", "--index", str(idx)], d)
        self.expect(name, code == 3,
                    f"info over a truncated index exited {code}")
        idx.write_bytes(good)
        code, _, _ = self.megascope(["info", "--index", str(idx)], d)
        self.expect(name, code == 0, "info over the restored index failed")


def section_table(data: bytes) -> list[tuple[int, int, int]] | None:
    """{kind, offset, length} per section from a v13 header: magic(4)
    version(4) summary(32) count(4), then {kind u8, offset u64, length
    u64, checksum u64} entries."""
    if len(data) < 44 or data[:4] != b"VYCS":
        return None
    (count,) = struct.unpack_from("<I", data, 40)
    out = []
    for i in range(count):
        at = 44 + i * 25
        if at + 25 > len(data):
            return None
        kind = data[at]
        offset, length = struct.unpack_from("<QQ", data, at + 1)
        out.append((kind, offset, length))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--keep", action="store_true",
                    help="keep the scratch directory")
    args = ap.parse_args()
    root = Path(tempfile.mkdtemp(prefix="vycor-write-integrity-"))
    check = Check(args.binary.resolve(), root, args.verbose)
    try:
        for scenario in (check.empty_bake, check.concurrent, check.lock,
                         check.corruption):
            scenario()
    finally:
        if args.keep:
            print(f"scratch kept at {root}", file=sys.stderr)
        else:
            shutil.rmtree(root, ignore_errors=True)
    if check.failures:
        print(f"{len(check.failures)} failure(s)", file=sys.stderr)
        return 1
    print("write integrity: all scenarios passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
