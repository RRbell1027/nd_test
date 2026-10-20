#!/usr/bin/env python3
"""Install a prebuilt SDK; no source tree, CMake, build or device required."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import tempfile


def install():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--prefix',type=Path,default=Path('/usr/local'))
 a=p.parse_args()
 sdk=Path(__file__).resolve().parent
 prefix=a.prefix.resolve()
 if prefix==sdk:p.error('prefix must differ from the SDK source directory')
 if platform.system()!='Linux':p.error('this SDK requires Linux')
 if platform.machine().lower() not in ('x86_64','amd64'):
  p.error('this SDK requires an amd64/x86_64 Linux host')
 compiler=sdk/'llvm/bin/clang++'
 wrapper=sdk/'bin/nds-clang++'
 tools=[sdk/'llvm/bin/clang-linker-wrapper',sdk/'llvm/bin/clang-offload-packager']
 required=[compiler,wrapper,*tools,sdk/'include/nd.h',sdk/'include/nd_device.h',sdk/'include/nd_runtime.h']
 libraries=['libnd.so','libnd_runtime.so','libomp.so','libomptarget.so']
 required += [sdk/'lib'/name for name in libraries]
 for source in required:
  if not source.is_file():p.error(f'missing SDK file: {source}')
 if not list((sdk/'llvm/lib/clang').glob('*/include/stddef.h')):
  p.error('missing compiler resource headers under llvm/lib/clang')
 for source in [compiler,*tools,*[sdk/'lib'/name for name in libraries]]:
  with source.open('rb') as f:h=f.read(20)
  if len(h)<20 or h[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',h,18)[0]!=62:
   p.error(f'expected an amd64 ELF binary: {source}')
 files=[]
 for directory in ('include','lib'):
  for source in sorted((sdk/directory).rglob('*')):
   if source.is_symlink():
    target=os.readlink(source)
    if os.path.isabs(target) or not source.resolve().is_relative_to((sdk/directory).resolve()):
     p.error(f'library/header symlink escapes SDK: {source}')
   if source.is_file() or source.is_symlink():files.append((source,prefix/source.relative_to(sdk)))
 stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
 metadata=prefix/'share/nds-sdk'
 backup=metadata/'backups'/stamp
 entry=prefix/'bin/nds-clang++'
 config=metadata/'wrapper.json'
 manifest=metadata/'install-manifest.json'
 # All validation above is read-only. Save every overwritten SDK-owned file.
 for dest in [*[dest for _,dest in files],entry,config,manifest]:
  if dest.exists() or dest.is_symlink():
   saved=backup/dest.relative_to(prefix);saved.parent.mkdir(parents=True,exist_ok=True)
   shutil.copy2(dest,saved,follow_symlinks=False)
 installed=[]
 for source,dest in files:
  dest.parent.mkdir(parents=True,exist_ok=True)
  if dest.exists() or dest.is_symlink():dest.unlink()
  if source.is_symlink():dest.symlink_to(os.readlink(source))
  else:shutil.copy2(source,dest)
  installed.append(str(dest))
 entry.parent.mkdir(parents=True,exist_ok=True)
 if entry.exists() or entry.is_symlink():entry.unlink()
 if prefix==Path('/usr/local'):
  entry.symlink_to(wrapper)
 else:
  shutil.copy2(wrapper,entry);entry.chmod(0o755)
 contents=json.dumps({'compiler':str(compiler),'prefix':str(prefix)},indent=2)+'\n'
 metadata.mkdir(parents=True,exist_ok=True);config.write_text(contents)
 if prefix==Path('/usr/local'):
  source_config=sdk/'share/nds-sdk/wrapper.json'
  if source_config.exists():
   saved=backup/'sdk-source-wrapper.json';saved.parent.mkdir(parents=True,exist_ok=True)
   shutil.copy2(source_config,saved)
  source_config.parent.mkdir(parents=True,exist_ok=True);source_config.write_text(contents)
  subprocess.run(['ldconfig'],check=True)
 installed += [str(entry),str(config),str(manifest)]
 manifest.write_text(json.dumps({'sdk':str(sdk),'prefix':str(prefix),'installed':installed,
  'backup':str(backup),'source_libraries':{name:hashlib.sha256((sdk/'lib'/name).read_bytes()).hexdigest() for name in libraries}},indent=2)+'\n')
 # CPU-only smoke check. No USB connection or offload device is needed.
 subprocess.run([str(entry),'--version'],check=True)
 with tempfile.TemporaryDirectory(prefix='nds-install-check-') as temp:
  source=Path(temp)/'check.cpp';binary=Path(temp)/'check'
  source.write_text('#include <nd.h>\nint main(){return nd_file_size(nullptr)!=0 || nd_device_bits(nullptr)!=0;}\n')
  subprocess.run([str(entry),str(source),'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
 print(f'PASS: SDK installed to {prefix}; headers, host link and loader smoke test passed')
 print(f'Install manifest: {manifest}')

if __name__=='__main__':
 try:install()
 except (OSError,subprocess.CalledProcessError) as e:raise SystemExit(f'Installation failed: {e}')
