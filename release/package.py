#!/usr/bin/env python3
"""Assemble the three preview archives from explicitly selected verified inputs."""
import argparse, hashlib, json, shutil, subprocess, tarfile, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def git(*args):
 return subprocess.check_output(['git','-C',str(ROOT),*args],text=True).strip()
def copy(src,dst):
 dst.parent.mkdir(parents=True,exist_ok=True)
 shutil.copy2(src,dst)
def digest(path):
 with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest() if hasattr(hashlib,'file_digest') else hashlib.sha256(f.read()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--sdk',type=Path,required=True);p.add_argument('--boot',type=Path,required=True)
 p.add_argument('--executor',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
 p.add_argument('--version',default='v0.1.0-preview.1');p.add_argument('--licenses',type=Path,required=True)
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
 for f in [a.sdk/'install.sh',a.boot,a.executor]:
  if not f.is_file():p.error('Missing input: '+str(f))
 if a.executor.read_bytes()[:5]!=b'\x7fELF\x01':p.error('executor must be ELF32')
 metadata={'version':a.version,'main_commit':git('rev-parse','HEAD'),
  'llvm_commit':git('rev-parse','HEAD:llvm-project'),'boot_sha256':digest(a.boot),
  'executor_sha256':digest(a.executor),'device_base':'luckfox-pico 824b817f889c2cbff1d48fcdb18ab494a68f69d1',
  'device_kernel':'5.10.160','validation':'Prior USB hardware tests passed; see release README and docs/nds-runtime.md'}
 with tempfile.TemporaryDirectory(prefix='nds-release-') as tmp:
  base=Path(tmp)
  sdk=base/'nds-sdk';shutil.copytree(a.sdk,sdk,symlinks=True)
  for name in ['install.sh','install.py']:
   copy(ROOT/'sdk'/('package-'+name),sdk/name)
  copy(ROOT/'sdk/nds-clang++',sdk/'bin/nds-clang++')
  for f in [sdk/'install.sh',sdk/'install.py',sdk/'bin/nds-clang++']:f.chmod(0o755)
  # Remove source-machine paths. install.sh generates these for the extraction location.
  (sdk/'share/nds-sdk/wrapper.json').write_text(json.dumps({'compiler':'RUN_INSTALL_SH_FIRST','prefix':'/usr/local'},indent=2)+'\n')
  platform=sdk/'platform/luckfox.ini'
  text=platform.read_text().replace('/home/dev/nds-sdk','/INSTALL_RUN_INSTALL_SH_FIRST')
  platform.write_text(text)
  (sdk/'README.txt').write_text('Prebuilt Ubuntu 22.04 x86_64 SDK. Extract permanently, then sudo ./install.sh.\nSee README.md for usage.\n')
  copy(ROOT/'release/README.md',sdk/'README.md')
  # The old inventory described the source machine before packaging.
  (sdk/'package-files.json').unlink(missing_ok=True)
  host=base/'nds-host-linux';host.mkdir()
  copy(ROOT/'drivers/host/nd_driver.c',host/'nd_driver.c')
  for name in ['nd_driver.h','nd_protocol.h','nd_rpc_wire.h']:copy(ROOT/'include'/name,host/'include'/name)
  copy(ROOT/'release/host-Makefile',host/'Makefile')
  copy(ROOT/'release/qemu-start.sh',host/'qemu-start.sh');(host/'qemu-start.sh').chmod(0o755)
  (host/'README.md').write_text("""# Linux host support

Install make, a C compiler, the running kernel headers, QEMU x86 and KVM support.
Build with `make` or `make KDIR=/path/to/prepared/kernel/build`; then `sudo insmod ./nd_driver.ko`.
The source is the release artifact: a precompiled module cannot cover every kernel.
WSL2 users need the matching Microsoft kernel build tree and usbipd attach on Windows.
If WSL module loading fails specifically on BTF, remove .BTF/.BTF.ext with objcopy before retrying.
This release was hardware-tested on WSL2 5.15.167.4. Native Linux compilation must target its own kernel.

Connect Luckfox's USB composite device. Check /dev/nd0 and identify its UMS SD with lsblk.
The supplied qemu-start.sh takes the whole SD block device explicitly:
`sudo ./qemu-start.sh /dev/disk/by-id/YOUR-SD`
Do not use a system disk. Guest writes go directly to this SD; no overlay is used.
Do not mount the SD concurrently on the host or Luckfox. Stop other VMs using it first.
First serial port is ND data (/dev/ttyS0); second is Guest console (/dev/ttyS1).
Configure the Ubuntu Guest console with console=ttyS1,115200n8 and serial-getty@ttyS1.service.
Grant Guest user access to ttyS0 (e.g. dialout group); enable KVM access on the host.
QEMU example is foreground; shut down Ubuntu normally before disconnecting SD/USB.
Ubuntu Guest and its boot configuration are not included in this package.
""")
  device=base/'nds-device-luckfox-rv1106';device.mkdir()
  copy(a.boot,device/'images/boot.img');copy(a.executor,device/'rootfs/root/nd-runtime/nd-executor')
  copy(ROOT/'runtime/nd-executor-service.sh',device/'rootfs/etc/init.d/S99nd-runtime')
  for f in (ROOT/'etc/init.d').iterdir():
   if f.is_file():copy(f,device/'rootfs/etc/init.d'/f.name)
  for f in (device/'rootfs').rglob('*'):
   if f.is_file():f.chmod(0o755)
  for f in ['drivers/gadget/f_nd.c','runtime/executor.c','runtime/wire.h','include/nd_rpc_wire.h','include/nd_protocol.h']:
   copy(ROOT/f,device/'source'/f)
  copy(ROOT/'docs/architecture.md',device/'source/architecture.md')
  shutil.copytree(ROOT/'release/device-source',device/'source/build',dirs_exist_ok=True)
  (device/'README.md').write_text("""# Luckfox RV1106 device update

This is a boot-partition update plus rootfs add-on for the matching ARM32/uClibc Luckfox base.
It is NOT a complete recovery image and does not contain Ubuntu or SD partitions.
Kernel: 5.10.160, USB f_nd + UMS. Executor requires ARM libffi.so.8 in /usr/lib.

Back up the existing firmware before flashing. Use the official Luckfox flashing tool's
boot-partition operation to write images/boot.img to the boot partition only.
Do not use a whole-device update operation with this single partition image.
Follow the official board-specific flashing guide: https://wiki.luckfox.com/Luckfox-Pico/Linux-MacOS-Burn-Image/
The boot image was previously flashed and verified by byte-for-byte readback on our board.
Do not infer compatibility with every Luckfox variant from the RV1106 chip name alone.

Copy the rootfs/ contents to the corresponding paths on the running board:
/root/nd-runtime/nd-executor, /etc/init.d/S99nd-runtime, and the USB gadget init script.
Preserve executable permissions. Reboot, or start the USB gadget service followed by S99nd-runtime start.
Confirm /dev/nd_rpc exists, the executor is running, and UMS exports /dev/mmcblk1.
The SD must not be mounted by the board while exported to the host.
This update leaves the existing base rootfs/libffi in place; first-time device provisioning
must use the matching upstream Luckfox SDK image before applying this package.

Kernel/base source: https://github.com/LuckfoxTECH/luckfox-pico
Upstream revision: 824b817f889c2cbff1d48fcdb18ab494a68f69d1
NDS modifications are in source/ and the matching near_data release tag.
Build/USB integration notes are included in source/architecture.md.
""")
  for folder in [sdk,host,device]:
   (folder/'VERSION.json').write_text(json.dumps(metadata,indent=2)+'\n')
   shutil.copytree(a.licenses,folder/'licenses',dirs_exist_ok=True)
   copy(ROOT/'release/THIRD_PARTY.md',folder/'THIRD_PARTY.md')
   manifest={str(f.relative_to(folder)):digest(f) for f in sorted(folder.rglob('*')) if f.is_file()}
   (folder/'CONTENTS-SHA256.json').write_text(json.dumps(manifest,indent=2)+'\n')
  for folder,name in [(sdk,'nds-sdk-linux-x86_64'),(host,'nds-host-linux'),(device,'nds-device-luckfox-rv1106')]:
   dest=a.output/(name+'.tar.gz')
   with tarfile.open(dest,'w:gz') as t:t.add(folder,arcname=folder.name)
   print(str(dest),dest.stat().st_size,flush=True)
 files=sorted(a.output.glob('nds-*.tar.gz'))
 (a.output/'SHA256SUMS').write_text(''.join(digest(f)+'  '+f.name+'\n' for f in files))
 copy(ROOT/'release/README.md',a.output/'README.md')
 (a.output/'VERSION.json').write_text(json.dumps(metadata,indent=2)+'\n')
if __name__=='__main__':main()
