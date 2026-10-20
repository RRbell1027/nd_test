#!/usr/bin/env python3
"""Execute real NDS OpenMP compile jobs on Linux; never use -### as proof."""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--clang', required=True)
p.add_argument('--platform', required=True)
p.add_argument('--out', default='artifacts/guest-compile')
a = p.parse_args()
out = Path(a.out).resolve()
out.mkdir(parents=True, exist_ok=True)
source = out / 'mapped.c'
source.write_text('int main(void) { int x = 0;\n#pragma omp target map(tofrom:x)\n { x = 42; } return x != 42; }\n')
base = [str(Path(a.clang).resolve()), '-fopenmp',
        '-fopenmp-targets=arm-nds-linux-gnueabihf',
        '-nds-platform=' + str(Path(a.platform).resolve()), '-fno-addrsig',
        '-Xopenmp-target=arm-nds-linux-gnueabihf', '-mcpu=cortex-a7', str(source)]
results = {}
for name, flags, suffix in [('device-object', ['--offload-device-only', '-c'], '.o'),
                            ('host-device-object', ['-c'], '.o'),
                            ('device-shared', ['--offload-device-only', '-shared'], '.so')]:
    output = out / (name + suffix)
    cmd = base + flags + ['-o', str(output)]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / (name + '.log')).write_bytes(proc.stdout)
    ok = proc.returncode == 0 and output.is_file() and output.stat().st_size > 0
    results[name] = {'command': cmd, 'exit': proc.returncode, 'pass': ok}
    print(('PASS' if ok else 'FAIL') + ': ' + name + ' exit=' + str(proc.returncode), flush=True)
    if not ok:
        print(proc.stdout.decode(errors='replace'), flush=True)
(out / 'manifest.json').write_text(json.dumps(results, indent=2))
raise SystemExit(0 if all(r['pass'] for r in results.values()) else 1)
