#!/usr/bin/env python3
"""Install already-built Guest libraries/headers and compiler wrapper."""
import argparse
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
p=argparse.ArgumentParser(description=__doc__)
root=Path(__file__).resolve().parent.parent
p.add_argument('--prefix',type=Path,default=Path('/usr/local'))
p.add_argument('--build',type=Path,default=root/'build')
a=p.parse_args();prefix=a.prefix.resolve();build=a.build.resolve()
compiler=build/'llvm/bin/clang++'
required=[compiler,build/'runtime/libnd.so',build/'runtime/libnd_runtime.so',build/'openmp/runtime/src/libomp.so',build/'offload-native/libomptarget.so']
for path in required:
 if not path.is_file():p.error(f'missing build product: {path}; build nds-runtime first')
# Preserve existing SDK-owned names before installation.
backup=prefix/'share/nds-sdk/backups'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
for folder,names in [('lib',['libnd.so','libnd.a','libnd_runtime.so','libnd_runtime.a','libomp.so','libomptarget.so','libomptarget.so.21.1']),('include',['nd.h','nd_device.h','nd_runtime.h']),('bin',['nds-clang++']),('share/nds-sdk',['wrapper.json'])]:
 for name in names:
  path=prefix/folder/name
  if path.exists() or path.is_symlink():
   dest=backup/folder/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,dest,follow_symlinks=False)
for directory,extra in [('runtime',[]),('openmp',[]),('offload-native',['--component','omptarget'])]:
 subprocess.run(['cmake','--install',str(build/directory),'--prefix',str(prefix),*extra],check=True)
(prefix/'bin').mkdir(parents=True,exist_ok=True)
wrapper=prefix/'bin/nds-clang++';shutil.copyfile(root/'sdk/nds-clang++',wrapper);wrapper.chmod(0o755)
config=prefix/'share/nds-sdk/wrapper.json';config.parent.mkdir(parents=True,exist_ok=True)
config.write_text(json.dumps({'compiler':str(compiler),'prefix':str(prefix)},indent=2)+'\n')
# /usr/local/lib is already in Ubuntu's loader configuration. Other prefixes
# use wrapper-provided RUNPATH and may use LD_LIBRARY_PATH for indirect deps.
if prefix==Path('/usr/local'):
 subprocess.run(['ldconfig'],check=True)
print(f'Installed {wrapper}; native compiler remains at {compiler}')
