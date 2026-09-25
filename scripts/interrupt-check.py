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
"""Crash/hang containment, end to end (docs/design-f12-subprocess-workers.md,
"Failure modes").

Builds a scratch project whose TUs spend minutes in constexpr evaluation,
then checks, against the real binary:

  timeout    `megascope index --worker-timeout 2`: the slow TU's worker is
             killed, the TU is recorded `timeout` (`info --files`), the
             fast TU is `indexed`, and no worker directory is left.
  sigint     SIGINT to `megascope index` mid-bake: the process dies by
             SIGINT, every worker it had running is gone, and no
             vycor-workers-* directory is left.
  sigterm    SIGTERM to `anneal --isolate-workers` mid-analysis: same, for
             the anneal dispatcher (vycor-anneal-workers-*).

The scratch TMPDIR makes the worker directories observable. Also a ctest:
`ctest -R interrupt_cleanup`.

    scripts/interrupt-check.py --binary build/src/vycor-cpp [-v] [--keep]
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Minutes of constexpr evaluation (the step limit is raised in the
# compile command), so a worker is reliably mid-parse when signalled.
SLOW = """\
constexpr long spin(long n) {
  long s = 0;
  for (long i = 0; i < n; ++i)
    s += i % 7;
  return s;
}
static_assert(spin(2000000000L) >= 0, "");
void slow_entry() {}
"""
FAST = "void fast_callee() {}\nint main() { fast_callee(); return 0; }\n"


def log(verbose: bool, *msg: object) -> None:
    if verbose:
        print(*msg, file=sys.stderr)


def make_project(root: Path, slow: list[str]) -> Path:
    proj = root / "proj"
    proj.mkdir()
    entries = []
    for name, text in [(n, SLOW) for n in slow] + [("fast.cpp", FAST)]:
        (proj / name).write_text(text)
        entries.append({
            "directory": str(proj),
            "file": str(proj / name),
            "arguments": ["clang++", "-std=c++17",
                          "-fconstexpr-steps=2147483647", "-c",
                          str(proj / name)],
        })
    (proj / "compile_commands.json").write_text(json.dumps(entries))
    return proj


def worker_dirs(tmp: Path) -> list[str]:
    return sorted(p.name for p in tmp.iterdir()
                  if p.name.startswith(("vycor-workers-",
                                        "vycor-anneal-workers-")))


def children_of(pid: int) -> list[int]:
    out = subprocess.run(["ps", "-A", "-o", "pid=,ppid="],
                         capture_output=True, text=True).stdout
    kids = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and int(parts[1]) == pid:
            kids.append(int(parts[0]))
    return kids


def alive(pid: int) -> bool:
    """Running (a zombie waiting for a reaper counts as gone)."""
    st = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)],
                        capture_output=True, text=True).stdout.strip()
    return bool(st) and not st.startswith("Z")


def env_with_tmp(tmp: Path) -> dict[str, str]:
    env = dict(os.environ)
    env["TMPDIR"] = str(tmp)
    return env


def check_timeout(binary: Path, root: Path, verbose: bool) -> list[str]:
    errors = []
    tmp = root / "tmp"
    tmp.mkdir()
    proj = make_project(root, ["slow.cpp"])
    index = root / "t.vycs"
    t0 = time.monotonic()
    p = subprocess.run(
        [str(binary), "megascope", "index", "--build-path", str(proj),
         "--index", str(index), "--threads", "2", "--worker-timeout", "2"],
        capture_output=True, text=True, env=env_with_tmp(tmp), timeout=300)
    log(verbose, p.stderr)
    took = time.monotonic() - t0
    if p.returncode != 0:
        return [f"timeout: index exited {p.returncode}: {p.stderr}"]
    if took > 120:
        errors.append(f"timeout: index took {took:.0f}s")
    info = subprocess.run(
        [str(binary), "megascope", "info", "--index", str(index), "--files"],
        capture_output=True, text=True, timeout=60)
    status = {Path(f["path"]).name: f["status"]
              for f in json.loads(info.stdout)["files"]}
    if status.get("slow.cpp") != "timeout":
        errors.append(f"timeout: slow.cpp recorded {status.get('slow.cpp')}")
    if status.get("fast.cpp") != "indexed":
        errors.append(f"timeout: fast.cpp recorded {status.get('fast.cpp')}")
    if worker_dirs(tmp):
        errors.append(f"timeout: left {worker_dirs(tmp)}")
    return errors


def check_signal(binary: Path, root: Path, argv: list[str], sig: int,
                 name: str, verbose: bool) -> list[str]:
    tmp = root / "tmp"
    tmp.mkdir()
    proj = make_project(root, ["slow1.cpp", "slow2.cpp"])
    full = [str(binary)] + [a.replace("@PROJ@", str(proj))
                            .replace("@ROOT@", str(root)) for a in argv]
    # An index already in place must come through the interrupt untouched:
    # a partial bake is never saved over it.
    sentinel = b"previous index (not a snapshot)\n"
    index = root / "i.vycs"
    index.write_bytes(sentinel)
    proc = subprocess.Popen(full, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True,
                            env=env_with_tmp(tmp))
    # Wait until the dispatcher has its directory and workers running.
    workers: list[int] = []
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline and proc.poll() is None:
        workers = children_of(proc.pid)
        if worker_dirs(tmp) and len(workers) >= 2:
            break
        time.sleep(0.1)
    if proc.poll() is not None or not workers:
        proc.kill()
        _, err = proc.communicate()
        return [f"{name}: no running workers to interrupt: {err}"]
    time.sleep(0.5)  # let the workers get into the parse
    log(verbose, f"{name}: workers {workers}, dirs {worker_dirs(tmp)}")
    proc.send_signal(sig)
    try:
        _, err = proc.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        _, err = proc.communicate()
        return [f"{name}: still running 30s after the signal"]
    log(verbose, err)
    errors = []
    if proc.returncode != -sig:
        errors.append(f"{name}: exit {proc.returncode}, expected death by "
                      f"signal {sig}")
    time.sleep(0.2)
    left = [w for w in workers if alive(w)]
    if left:
        errors.append(f"{name}: workers still running: {left}")
        for w in left:
            try:
                os.kill(w, signal.SIGKILL)
            except OSError:
                pass
    if worker_dirs(tmp):
        errors.append(f"{name}: left {worker_dirs(tmp)}")
    if index.read_bytes() != sentinel:
        errors.append(f"{name}: the interrupted run replaced {index.name}")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    binary = args.binary.resolve()

    base = Path(tempfile.mkdtemp(prefix="vycor-interrupt-"))
    errors: list[str] = []
    try:
        cases = [
            ("timeout", lambda r: check_timeout(binary, r, args.verbose)),
            ("sigint", lambda r: check_signal(
                binary, r,
                ["megascope", "index", "--build-path", "@PROJ@", "--index",
                 "@ROOT@/i.vycs", "--threads", "2", "--isolate-workers",
                 "--worker-timeout", "0"],
                signal.SIGINT, "sigint", args.verbose)),
            ("sigterm", lambda r: check_signal(
                binary, r,
                ["anneal", "--build-path", "@PROJ@", "--source",
                 "@PROJ@/slow1.cpp", "--source", "@PROJ@/slow2.cpp",
                 "--isolate-workers", "--workers", "2", "--worker-timeout",
                 "0"],
                signal.SIGTERM, "sigterm", args.verbose)),
        ]
        for name, fn in cases:
            root = base / name
            root.mkdir()
            errs = fn(root)
            print(f"{name}: {'FAIL' if errs else 'ok'}")
            errors += errs
    finally:
        if not args.keep:
            subprocess.run(["rm", "-rf", str(base)])
    for e in errors:
        print("  " + e, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
