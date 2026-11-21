#!/usr/bin/env python3
# Copyright (c) 2026 The vycor-cpp Authors
# SPDX-License-Identifier: Apache-2.0
"""hot-path-effects check verb: root sources, profile gates and exit codes."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile

SOURCE = '''#include <mutex>
std::mutex mu;
void sink() { std::lock_guard<std::mutex> g(mu); }
void hot() { sink(); }
int main() { for (int i = 0; i < 10; ++i) hot(); }
'''


def profile(filename):
    """A minimal llvm-cov export: sink and hot entered 10 times, main once."""
    segments, functions = [], []
    for name, line, count in (('sink', 3, 10), ('hot', 4, 10), ('main', 5, 1)):
        segments.append([line, 1, count, True, True, False])
        segments.append([line, 400, 0, False, False, False])
        functions.append(dict(name=name, count=count, filenames=[filename],
                              regions=[[line, 1, line, 400, count, 0, 0, 0]]))
    segments.append([5, 30, 10, True, True, False])  # the loop body
    segments.sort(key=lambda s: (s[0], s[1]))
    return json.dumps(dict(type='llvm.coverage.json.export', version='2.0.1',
                           data=[dict(files=[dict(filename=filename,
                                                  segments=segments)],
                                      functions=functions)]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix='vycor_hot_path_') as tmp:
        root = Path(tmp)
        source = root / 'hot.cpp'
        source.write_text(SOURCE)
        (root / 'compile_commands.json').write_text(json.dumps([dict(
            directory=tmp, file=str(source),
            arguments=['clang++', '-std=c++17', '-c', str(source)])]))
        good = root / 'good.json'
        good.write_text(profile('/profiled/elsewhere/hot.cpp'))
        wrong = root / 'wrong.json'
        wrong.write_text(profile('/profiled/elsewhere/other.cpp'))
        org = root / 'org.json'
        org.write_text(json.dumps({'hotPaths': {'roots': ['hot'], 'minCount': 50}}))

        def run(extra, expected, checks='-all,hot-path-effects'):
            command = [str(binary), 'check', '--build-path', tmp,
                       '--project-root', tmp, '--threads=1',
                       '--checks=' + checks, '--format=json',
                       '--source', str(source), *extra]
            process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True,
                                       start_new_session=True)
            try:
                stdout, stderr = process.communicate(timeout=120)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()
                raise
            assert process.returncode == expected, (command, process.returncode, stderr, stdout)
            findings = None
            if expected != 2:
                payload = json.loads(stdout)
                assert payload['summary']['failed'] == 0, payload
                findings = [f for f in payload['findings']
                            if f['check'] == 'hot-path-effects']
            return findings, stderr

        # Configured root: the lock under sink is found, with no profile.
        findings, _ = run(['--hot-root', 'hot'], 1)
        assert findings and all('lock' in f['message'] for f in findings), findings
        assert all('unprofiled' in f['message'] for f in findings), findings

        # A root that names nothing, and no root at all, are usage errors.
        _, stderr = run(['--hot-root', 'no_such_function'], 2)
        assert "names no function" in stderr, stderr
        _, stderr = run([], 2)
        assert 'no hot roots' in stderr, stderr

        # --hot-* flags without the check enabled are reported, not fatal.
        findings, stderr = run(['--hot-root', 'hot'], 0,
                               checks='-all,callback-under-lock')
        assert not findings and '--hot-root' in stderr, stderr

        # Profile roots: observed findings naming the profile.
        findings, _ = run(['--hot-profile', str(good), '--hot-min-count', '5'], 1)
        assert findings and all('observed' in f['message'] and good.name in f['message']
                                for f in findings), findings

        # A profile that joins but where nothing is hot enough: a quiet run.
        findings, stderr = run(['--hot-profile', str(good), '--hot-min-count', '1000'], 0)
        assert not findings and 'nothing to check' in stderr, stderr

        # A profile of another file joins nothing and gives no root.
        _, stderr = run(['--hot-profile', str(wrong), '--hot-min-count', '1'], 2)
        assert 'matched no call site' in stderr, stderr

        # An explicit minimum count needs a profile; the org config's does not.
        run(['--hot-root', 'hot', '--hot-min-count', '5'], 2)
        findings, _ = run(['--org-config', str(org)], 1)
        assert findings, findings

        # An index baked with --collapse-paths lacks the edges inside them
        # (here hot -> sink): the check must not read it and pass, and the
        # tool must not call it clean.
        index = root / '.vycor' / 'megascope.vycs'
        bake = subprocess.run(
            [str(binary), 'megascope', 'index', '--build-path', tmp,
             '--index', str(index), '--threads', '1',
             '--collapse-paths', root.name],
            capture_output=True, text=True, timeout=120)
        assert bake.returncode == 0, (bake.stdout, bake.stderr)
        query = subprocess.run(
            [str(binary), 'megascope', 'query-path-effects', '--index',
             str(index), '--roots', 'hot', '--effects', 'lock'],
            capture_output=True, text=True, timeout=120)
        payload = json.loads(query.stdout)
        assert payload['collapsed'] is True and payload['exhaustive'] is False, payload
        assert payload['verdict'] != 'clean', payload
        findings, _ = run(['--hot-root', 'hot'], 1)
        assert findings, findings
        print('PASS: hot-path-effects configured/profile/org roots, quiet and wrong '
              'profiles, ignored flags, usage errors and collapsed indexes')


if __name__ == '__main__':
    main()
