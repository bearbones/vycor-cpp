#!/usr/bin/env python3
# Copyright (c) 2026 The vycor-cpp Authors
# SPDX-License-Identifier: Apache-2.0
"""Callback-under-lock report, worker, suppression and freshness regression."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix='vycor_callback_') as tmp:
        root = Path(tmp)
        header = root / 'callback.hpp'
        sources = [root / 'a.cpp', root / 'b.cpp']
        for source in sources:
            source.write_text('#include "callback.hpp"\n')
        unsafe = '''#include <mutex>
#include <functional>
inline std::mutex mutex;
inline void notify(std::function<void()> cb) {
#ifdef SAFE_CALLBACK
  { std::lock_guard<std::mutex> guard(mutex); }
#else
  std::lock_guard<std::mutex> guard(mutex);
#endif
  cb();
}
'''
        header.write_text(unsafe)

        def database(safe=False):
            rows = []
            for source in sources:
                flags = ['-DSAFE_CALLBACK'] if safe else []
                rows.append(dict(directory=tmp, file=str(source), arguments=[
                    'clang++', '-std=c++17', *flags, '-c', str(source)]))
            (root / 'compile_commands.json').write_text(json.dumps(rows))

        def run(extra=(), expected=1, format='json'):
            command = [str(binary), 'check', '--build-path', tmp,
                       '--project-root', tmp, '--threads=1',
                       '--checks=-all,callback-under-lock', '--format='+format]
            for source in sources:
                command.extend(['--source', str(source)])
            process = subprocess.Popen(command+list(extra), stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True,
                                       start_new_session=True)
            try:
                stdout, stderr = process.communicate(timeout=120)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()
                raise
            assert process.returncode == expected, (command, process.returncode, stderr, stdout)
            payload = json.loads(stdout)
            if format == 'json':
                assert payload['summary']['failed'] == 0, payload
            return payload

        database()
        original = run()
        assert len(original['findings']) == 1, original
        finding = original['findings'][0]
        assert finding['check'] == 'callback-under-lock', finding
        checkpoint = str(root / 'checkpoint')
        for flags in [('--threads=2',), ('--checkpoint', checkpoint),
                      ('--checkpoint', checkpoint),
                      ('--isolate-workers', '--workers=2'),
                      ('--isolate-workers', '--workers=2', '--checkpoint', checkpoint)]:
            assert run(flags) == original, flags
        sarif = run(format='sarif')
        assert sarif['runs'][0]['results'][0]['ruleId'] == 'callback-under-lock'
        baseline = root / 'baseline.json'
        run(('--write-baseline', str(baseline)), expected=0)
        assert not run(('--baseline', str(baseline)), expected=0)['findings']
        header.write_text('\n\n' + unsafe)
        assert run()['findings'][0]['fingerprint'] == finding['fingerprint']
        header.write_text(unsafe.replace('  cb();', '  cb(); // vycor: ignore[callback-under-lock]'))
        assert not run(expected=0)['findings']
        header.write_text(unsafe)
        database(safe=True)
        assert not run(('--checkpoint', checkpoint), expected=0)['findings']
        database()
        run(('--checkpoint', checkpoint))
        header.write_text(unsafe.replace('#ifdef SAFE_CALLBACK', '#if 1'))
        assert not run(('--checkpoint', checkpoint), expected=0)['findings']
        print('PASS: callback reports, parallel/workers, SARIF, baseline, suppression, fingerprint and fresh header/flag changes')


if __name__ == '__main__':
    main()
