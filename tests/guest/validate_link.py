#!/usr/bin/env python3
"""Build a complete NDS offload executable and check its embedded ARM image."""
import argparse
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
for name in ('clang', 'platform', 'libomp-dir', 'libomptarget-dir'):
    p.add_argument('--' + name, required=True, type=Path)
p.add_argument('--out', type=Path, default=Path('artifacts/guest-link'))
a = p.parse_args()
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
source = out / 'mapped.c'
source.write_text('int main(void) { int x = 0;\n#pragma omp target map(tofrom:x)\n { x = 42; } return x != 42; }\n')
exe = out / 'omp_test'
cmd = [str(a.clang.resolve()), '-v', '-save-temps=obj', '-fopenmp',
       '-fopenmp-targets=arm-nds-linux-gnueabihf',
       '-nds-platform=' + str(a.platform.resolve()), '-fno-addrsig',
       '-Xopenmp-target=arm-nds-linux-gnueabihf', '-mcpu=cortex-a7',
       str(source), '-o', str(exe)]
for directory in (a.libomp_dir.resolve(), a.libomptarget_dir.resolve()):
    cmd += ['-L' + str(directory), '-Wl,-rpath,' + str(directory)]
proc = subprocess.run(cmd, cwd=out, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(out / 'link.log').write_bytes(proc.stdout)
r = {'command': cmd, 'link_exit': proc.returncode}
def elf(path):
    b = path.read_bytes()
    if len(b) < 20 or b[:6] != b'\x7fELF\x01\x01' and b[:6] != b'\x7fELF\x02\x01':
        raise AssertionError('not a little-endian ELF: ' + str(path))
    return (b[4], int.from_bytes(b[16:18], 'little'), int.from_bytes(b[18:20], 'little'))
try:
    assert proc.returncode == 0, proc.stdout.decode(errors='replace')
    assert elf(exe) in ((2, 2, 62), (2, 3, 62)), elf(exe)
    images = sorted(out.glob('*.img'))
    assert len(images) == 1, [str(i) for i in images]
    assert elf(images[0]) == (1, 3, 40), elf(images[0])
    assert images[0].read_bytes() in exe.read_bytes(), 'device image not embedded'
    r['arm_image'] = str(images[0])
    r['image_embedded'] = True
    print('PASS: complete x86_64 executable with embedded ARM32 shared ELF', flush=True)
    run = subprocess.run([str(exe)], env={**os.environ, 'OMP_TARGET_OFFLOAD': 'DISABLED'},
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    (out / 'host-fallback.log').write_bytes(run.stdout)
    r['host_fallback_exit'] = run.returncode
    assert run.returncode == 0, run.stdout.decode(errors='replace')
    print('PASS: host fallback exit=0 (not Luckfox execution)', flush=True)
    r['pass'] = True
finally:
    (out / 'manifest.json').write_text(json.dumps(r, indent=2))
