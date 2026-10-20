#!/usr/bin/env python3
"""Exercise FIEMAP -> storage RPC -> public device buffer using sparse storage."""
import ctypes as C
import errno
import os
from pathlib import Path
import pty
import subprocess
import sys
import tempfile
import tty
build=Path(sys.argv[1]).resolve()
lib=C.CDLL(str(build/'libnd_runtime.so'),use_errno=True)
files=C.CDLL(str(build/'libnd.so'),use_errno=True)
P=C.c_void_p; U=C.c_uint64; Z=C.c_size_t
class Extent(C.Structure):
 _fields_=[('logical',U),('physical',U),('length',U),('flags',C.c_uint32)]
def bind(lib,name,result,*args):
 f=getattr(lib,name);f.restype=result;f.argtypes=args;return f
open_dev=bind(lib,'nd_device_open',P,C.c_char_p)
close_dev=bind(lib,'nd_device_close',None,P)
alloc=bind(lib,'nd_alloc',C.c_int,P,Z,C.POINTER(P))
free=bind(lib,'nd_free',C.c_int,P,P)
copy=bind(lib,'nd_copy',C.c_int,P,P,P,Z,C.c_int)
open_file=bind(files,'nd_open',P,C.c_char_p)
close_file=bind(files,'nd_close',C.c_int,P)
get_extent=bind(files,'nd_get_extent',C.c_int,P,Z,C.POINTER(Extent))
read=bind(files,'nd_read',C.c_int64,P,U,Z,P)
def ok(rc):
 assert rc==0,(rc,C.get_errno())
with tempfile.TemporaryDirectory(dir='/tmp') as tmp:
 source=Path(tmp)/'source';payload=bytes(i%251 for i in range(8192))
 with source.open('wb') as f:f.write(payload);f.flush();os.fsync(f.fileno())
 file=open_file(os.fsencode(source));assert file
 ext=Extent();ok(get_extent(file,0,C.byref(ext)));assert ext.logical==0 and ext.length>=len(payload)
 storage=Path(tmp)/'storage'
 with storage.open('wb') as f:f.seek(4096*512+ext.physical);f.write(payload)
 master,slave=pty.openpty();tty.setraw(slave)
 env=dict(os.environ,ND_STORAGE_PATH=str(storage))
 child=subprocess.Popen([str(build/'nd-executor'),'--fd',str(master)],pass_fds=[master],env=env)
 os.close(master)
 d=open_dev(os.fsencode(os.ttyname(slave)));assert d
 d2=open_dev(os.fsencode(os.ttyname(slave)));assert d2==d
 assert not open_dev(b'/different/transport') and C.get_errno()==errno.EBUSY
 address=P();ok(alloc(d,1024,C.byref(address)))
 close_dev(d) # d2 + allocation keep stream alive, no second HELLO.
 try:
  assert read(file,3,1024,address)==1024,C.get_errno()
  buf=(C.c_ubyte*1024)();ok(copy(d2,buf,address,1024,2));assert bytes(buf)==payload[3:1027]
  assert read(file,0,1025,address)==-1 and C.get_errno()==errno.EFAULT
  assert read(file,len(payload),20,address)==0
  assert read(file,0,1,P(1))==-1 and C.get_errno()==errno.EFAULT
  assert read(file,len(payload)-7,20,address)==7
  ok(copy(d2,buf,address,7,2));assert bytes(buf[:7])==payload[-7:]
  ok(free(d2,address))
  assert read(file,0,1,address)==-1 and C.get_errno()==errno.EFAULT
  ok(bind(lib,'nd_rt_shutdown',C.c_int,P)(d2))
  print('PASS: direct storage offsets, EOF, bounds, stale pointers and shared-session lifetime')
 finally:
  close_file(file);close_dev(d2);os.close(slave)
  try:assert child.wait(timeout=5)==0
  except subprocess.TimeoutExpired:child.kill();child.wait();raise
