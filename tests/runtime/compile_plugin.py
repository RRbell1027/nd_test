#!/usr/bin/env python3
"""Compile the NDS adapter to a Linux ELF object using Windows Clang + WSL headers.
This checks LLVM API compatibility, not final libomptarget linking/execution.
Normal Linux LLVM builds generate OffloadErrcodes/Info with offload-tblgen.
Here the equivalent enum data comes from real llvm-tblgen JSON records.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--wsl',default='luckfox-sdk')
a=p.parse_args()
if os.name!='nt':p.error('use native Linux offload CMake build, or run this checker in Windows Python')
r=Path(__file__).resolve().parents[2];out=r/'artifacts/m4-driver';generated=out/'nds-generated';generated.mkdir(parents=True,exist_ok=True)
api=r/'llvm-project/offload/liboffload/API'
subprocess.run([str(r/'llvm-project/build/bin/llvm-tblgen.exe'),'--dump-json','-I',str(api),str(api/'OffloadAPI.td'),'-o',str(out/'offload-api-records.json')],check=True)
data=json.loads((out/'offload-api-records.json').read_text())
for record,macro,file in [('ErrorCode','OFFLOAD_ERRC','OffloadErrcodes.inc'),('DeviceInfo','OFFLOAD_DEVINFO','OffloadInfo.inc')]:
 lines=['// Generated from real TableGen JSON for compile-only validation.']
 for i,ref in enumerate(data[record]['etors']):
  e=data[ref['def']];lines.append(f"{macro}({e['name']}, {json.dumps(e['desc'])}, {i})")
 (generated/file).write_text('\n'.join(lines)+'\n')
w='//wsl.localhost/'+a.wsl
args=[str(r/'llvm-project/build/bin/clang.exe'),'--target=x86_64-pc-linux-gnu','--sysroot='+w,'-std=c++17','-fPIC','-c','-nostdinc++','-DTARGET_NAME=NDS','-DDEBUG_PREFIX="NDS"']
for directory in [w+'/usr/include/c++/11',w+'/usr/include/x86_64-linux-gnu/c++/11',w+'/usr/include',w+'/usr/include/x86_64-linux-gnu']:
 args+=['-isystem',directory]
for directory in [r/'include',generated,Path(w+'/sdk/nds-llvm-headers/include'),r/'llvm-project/build/include',r/'llvm-project/llvm/include',r/'llvm-project/offload/include',r/'llvm-project/offload/plugins-nextgen/common/include']:
 args+=['-I',str(directory)]
args += [str(r/'llvm-project/offload/plugins-nextgen/nds/src/rtl.cpp'),'-o',str(out/'nds-plugin.o')]
result=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
text=result.stdout.decode(errors='replace');(out/'nds-plugin-compile.log').write_text(text);print(text,end='')
if result.returncode:raise SystemExit(result.returncode)
print('PASS: NDS plugin compiled to Linux ELF object (not full libomptarget link)')
