#!/usr/bin/env python3
"""Validate NDS driver selection/config diagnostics without invoking SDK tools."""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--clang', type=Path)
a = p.parse_args()
repo = Path(__file__).resolve().parents[2]
clang = a.clang or repo / 'llvm-project/build/bin/clang.exe'
out = repo / 'artifacts/m4-driver'
out.mkdir(parents=True, exist_ok=True)
source = out / 'target.c'
source.write_text('int main(void) { int x=0;\n#pragma omp target map(tofrom:x)\n{x=42;} return x!=42; }\n')
valid = (repo / 'platform/luckfox-rv1106.ini').read_text()
base = [str(clang), '-fopenmp', '-fopenmp-targets=arm-nds-linux-gnueabihf', '--offload-device-only', '-###', str(source), '-o', str(out / 'target.o')]
results = {}
def case(name, config, expected, marker):
    args = base.copy()
    if config is not None:
        path = out / (name + '.ini')
        path.write_text(config)
        args += ['-nds-platform=' + str(path)]
    proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    text = (proc.stdout + proc.stderr).decode('utf-8', errors='replace')
    (out / (name + '.log')).write_text(text, encoding='utf-8')
    assert proc.returncode == expected, (name, proc.returncode, text)
    assert marker in text, (name, text)
    results[name] = {'exit': proc.returncode, 'pass': True}
    print('PASS:', name)
case('explicit', valid, 0, 'arm-rockchip830-linux-uclibcgnueabihf-as')
case('missing-option', None, 1, '-nds-platform')
case('unknown-key', valid + '\nunrecognized-key=value\n', 1, 'unrecognized-key')
case('missing-required', '\n'.join(x for x in valid.splitlines() if not x.startswith('toolchain-triple=')), 1, 'toolchain-triple')
case('bad-layout', valid.replace('path-layout=explicit', 'path-layout=invalid'), 1, 'path-layout')
case('buildroot', 'path-layout=buildroot\nbase-env-path=/sdk/luckfox-pico\ntoolchain-triple=arm-rockchip830-linux-uclibcgnueabihf\ngcc-version=8.3.0\n', 0, 'Using NDS ToolChain')
(out / 'manifest.json').write_text(json.dumps(results, indent=2))