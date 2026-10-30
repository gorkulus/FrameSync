"""Build isolated predicate mutants and require deterministic take-loss failures.

Production source is never edited. A timeout/crash is NOT a successful reproduction.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default='g++')
args = parser.parse_args()
root = Path('build/race-mutants')
root.mkdir(parents=True, exist_ok=True)
source = Path('src/CaptureSession.cpp').read_text()
old = 'bool cancel = !keepArmed_.load() && !started && cancelUnstarted();'
assert source.count(old) == 1
cas = 'return (available == State::Ready || available == State::Priming)\n        && state_.compare_exchange_strong(available, State::Finishing);'
assert source.count(cas) == 1
mutants = [
    ('old-predicate', source.replace(old, 'bool cancel = !keepArmed_.load() && !started;'),
     ['published-before-start', 'drained-before-recording', 'drained-before-finishing']),
    ('snapshot-guard', source.replace(cas, 'return available == State::Ready || available == State::Priming;'),
     ['stale-ready-snapshot']),
]
logs = []
for name, code, cases in mutants:
    cpp = root / (name + '.cpp')
    exe = root / (name + ('.exe' if os.name == 'nt' else ''))
    cpp.write_text(code)
    flags = ['-static-libstdc++', '-static-libgcc'] if os.name == 'nt' else []
    subprocess.run(shlex.split(args.cxx) + ['-std=c++17', '-O2', '-pthread', '-DFRAMESYNC_TEST_HOOKS',
                   '-Isrc', 'tests/race_test.cpp', str(cpp), *flags, '-o', str(exe)], check=True, timeout=60)
    for case in cases:
        result = subprocess.run([str(exe.resolve()), case], capture_output=True, text=True, timeout=25)
        output = result.stdout + result.stderr
        assert result.returncode == 1 and f'REGRESSION FAILED: {case}' in output, output
        logs.append(f'{name}: {output.strip()}')
        print(f'EXPECTED FAILURE ({name}): {case}')
Path('build/race-reproduction.log').write_text('\n'.join(logs) + '\n')
