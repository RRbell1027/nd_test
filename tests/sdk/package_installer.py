#!/usr/bin/env python3
"""Installer must reject incomplete/wrong-architecture packages before writes."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as temp:
 sdk=Path(temp)/'sdk';sdk.mkdir();installer=sdk/'install.py'
 shutil.copy2(root/'sdk/package-install.py',installer)
 prefix=Path(temp)/'destination'
 r=subprocess.run(['python3',str(installer),'--prefix',str(prefix)],capture_output=True,text=True)
 assert r.returncode!=0 and 'missing SDK file' in r.stderr and not prefix.exists()
 for path in ['llvm/bin','bin','lib','include']:(sdk/path).mkdir(parents=True,exist_ok=True)
 def elf(machine):
  b=bytearray(20);b[:6]=b'\x7fELF\x02\x01';struct.pack_into('<H',b,18,machine);return bytes(b)
 (sdk/'llvm/bin/clang++').write_bytes(elf(40))
 for name in ['clang-linker-wrapper','clang-offload-packager']:(sdk/'llvm/bin'/name).write_bytes(elf(62))
 resource=sdk/'llvm/lib/clang/21/include';resource.mkdir(parents=True);(resource/'stddef.h').write_text('')
 shutil.copy2(root/'sdk/nds-clang++',sdk/'bin/nds-clang++')
 for n in ['nd.h','nd_device.h','nd_runtime.h']:(sdk/'include'/n).write_text('')
 for n in ['libnd.so','libnd_runtime.so','libomp.so','libomptarget.so']:(sdk/'lib'/n).write_bytes(elf(62))
 r=subprocess.run(['python3',str(installer),'--prefix',str(prefix)],capture_output=True,text=True)
 assert r.returncode!=0 and 'expected an amd64 ELF' in r.stderr and not prefix.exists()
 print('PASS: incomplete and ARM-host packages rejected before installation writes')
