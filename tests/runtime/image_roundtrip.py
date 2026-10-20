#!/usr/bin/env python3
"""Test actual image bytes, memory RPC and launch; socket executor or board tty."""
import argparse
import ctypes as C
import errno
import os
from pathlib import Path
import socket
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--lib',required=True)
p.add_argument('--executor')
p.add_argument('--tty')
p.add_argument('images',nargs=2)
a=p.parse_args()
lib=C.CDLL(a.lib,use_errno=True)
def api(name,restype,*args):
 f=getattr(lib,'nd_rt_'+name);f.restype=restype;f.argtypes=args;return f
ptr=C.c_void_p;u64=C.c_uint64;sz=C.c_size_t
connect=api('connect_fd',ptr,C.c_int);open_rt=api('open',ptr,C.c_char_p)
close=api('close',None,ptr);error=api('error',C.c_char_p,ptr)
load=api('load_image',C.c_int,ptr,ptr,sz,C.POINTER(u64))
unload=api('unload_image',C.c_int,ptr,u64)
alloc=api('alloc',C.c_int,ptr,sz,C.POINTER(u64));free=api('free',C.c_int,ptr,u64)
put=api('submit',C.c_int,ptr,u64,ptr,sz);get=api('retrieve',C.c_int,ptr,ptr,u64,sz)
symbol=api('symbol',C.c_int,ptr,u64,C.c_char_p,sz,C.POINTER(u64))
launch=api('launch',C.c_int,ptr,u64,C.POINTER(u64),sz);shutdown=api('shutdown',C.c_int,ptr)
# A misconfigured serial path must not overwrite an image or any regular file.
original=Path(a.images[0]).read_bytes()
assert not open_rt(os.fsencode(a.images[0])) and C.get_errno()==errno.ENOTTY
assert Path(a.images[0]).read_bytes()==original
print('PASS: regular-file transport rejected without modifying bytes',flush=True)
child=None
if a.tty:
 rt=open_rt(os.fsencode(a.tty))
else:
 if not a.executor:p.error('--executor or --tty required')
 host,device=socket.socketpair()
 child=subprocess.Popen([a.executor,'--fd',str(device.fileno())],pass_fds=[device.fileno()])
 device.close();rt=connect(host.detach())
if not rt:raise OSError(C.get_errno(),os.strerror(C.get_errno()))
def check(rc):
 if rc:raise RuntimeError(f'errno={C.get_errno()} {error(rt).decode()}')
def rejected(rc,expected):
 assert rc==-1 and C.get_errno()==expected,(rc,C.get_errno(),error(rt))
try:
 fake=C.create_string_buffer(b'/tmp/preinstalled-image.so'+b'\0'*64)
 image=u64();rejected(load(rt,fake,len(fake),C.byref(image)),errno.ENOEXEC)
 print('PASS: pathname payload rejected; requires ELF image bytes',flush=True)
 for path,expected in zip(a.images,[27,39]):
  content=Path(path).read_bytes();blob=C.create_string_buffer(content)
  check(load(rt,blob,len(content),C.byref(image)))
  missing=u64();rejected(symbol(rt,image,b'no_such_symbol',0,C.byref(missing)),errno.ENOENT)
  source=u64();target=u64();check(alloc(rt,4,C.byref(source)));check(alloc(rt,4,C.byref(target)))
  value=C.c_int32(20);check(put(rt,source,C.byref(value),4))
  function=u64();check(symbol(rt,image,b'nd_test_kernel',0,C.byref(function)))
  args=(u64*2)(source.value,target.value);check(launch(rt,function,args,2))
  result=C.c_int32();check(get(rt,C.byref(result),target,4));assert result.value==expected,(result.value,expected)
  rejected(get(rt,C.byref(result),u64(target.value+4),4),errno.EFAULT)
  check(free(rt,source));check(free(rt,target));check(unload(rt,image))
  rejected(launch(rt,function,args,2),errno.ENOENT)
  print(f'PASS: transferred {len(content)} ELF bytes, mapped-pointer launch result={result.value}; unloaded handles rejected',flush=True)
 check(shutdown(rt))
finally:
 close(rt)
 if child:
  try:assert child.wait(timeout=5)==0
  except subprocess.TimeoutExpired:child.kill();child.wait();raise
