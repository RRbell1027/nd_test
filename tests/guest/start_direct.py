#!/usr/bin/env python3
"""Start the known ND Guest on the physical SD (writes are persistent)."""
import argparse
import hashlib
import json
from pathlib import Path
import stat
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--disk',default='/dev/sdd')
p.add_argument('--transport',default='/dev/nd0')
p.add_argument('--memory',default='12G')
p.add_argument('--cpus',type=int,default=10)
p.add_argument('--print-only',action='store_true')
a=p.parse_args()
migration=Path('/sdk/nds-guest/direct-sd-migration-20261020')
if migration.exists() and not (migration/'verified').exists():
 p.error('SD migration has not completed readback verification')
disk=Path(a.disk).resolve()
if not stat.S_ISBLK(disk.stat().st_mode):p.error('disk must be a physical block device')
if int(subprocess.check_output(['blockdev','--getsize64',str(disk)]))!=63864569856:
 p.error('disk size differs from the verified Luckfox SD')
with disk.open('rb',buffering=0) as f:
 if hashlib.sha256(f.read(4096)).hexdigest()!='78d0691736df5faa3cdd5e7723ab8b51f80988ae3664c46d01a7d95e35c30e9e':
  p.error('disk GPT fingerprint differs from the verified SD')
info=json.loads(subprocess.check_output(['lsblk','-J','-o','NAME,MOUNTPOINTS',str(disk)]))
def mounted(entries):
 return any(any(e.get('mountpoints') or []) or mounted(e.get('children',[])) for e in entries)
if mounted(info['blockdevices']):p.error('SD or partition is mounted in WSL')
if not Path(a.transport).exists():p.error('ND transport is missing; attach USB and load nd_driver first')
pidfile=Path('/tmp/near-data-qemu.pid')
if pidfile.exists():
 pid=pidfile.read_text().strip()
 if pid.isdigit() and Path('/proc',pid,'cmdline').exists():p.error('Guest is already running')
cmd=['qemu-system-x86_64','-enable-kvm','-cpu','host','-smp',str(a.cpus),'-m',a.memory,
 '-drive',f'file={disk},format=raw,if=virtio,cache=none',
 '-display','none','-monitor','unix:/tmp/near-data-monitor.sock,server,nowait',
 '-qmp','unix:/tmp/near-data-qmp.sock,server,nowait',
 '-chardev',f'pipe,id=nd,path={a.transport}','-serial','chardev:nd',
 '-serial','unix:/tmp/near-data-guest-console.sock,server,nowait',
 '-netdev','user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22',
 '-device','virtio-net-pci,netdev=net0','-daemonize','-pidfile',str(pidfile)]
print('Physical SD Guest: '+str(disk)+'; all Guest disk writes persist to SD.',flush=True)
if a.print_only:
 import shlex
 print(shlex.join(cmd))
else:subprocess.run(cmd,check=True)
