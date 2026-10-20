#!/usr/bin/env python3
"""Upload only the generic executor, then reserve UART for binary runtime RPC.
Run image_roundtrip.py --tty after this prints READY. Shutdown RPC restores tty.
Board password, if needed, is read from ND_BOARD_PASSWORD, never written to logs.
"""
import argparse
import base64
import os
from pathlib import Path
import serial
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--uart',default='/dev/ttyUSB0')
p.add_argument('--executor',required=True,type=Path)
a=p.parse_args()
blob=base64.b64encode(a.executor.read_bytes())
started=False
with serial.Serial(a.uart,115200,timeout=.2) as s:
 def until(markers,timeout=10):
  data=bytearray();deadline=time.monotonic()+timeout
  while time.monotonic()<deadline:
   data.extend(s.read(32768))
   if any(m in data for m in markers):return data
  raise RuntimeError('UART did not reach expected prompt')
 s.reset_input_buffer();s.write(b'\r')
 data=until([b'# ',b'login:',b'Password:'])
 if b'login:' in data:
  s.write((os.environ.get('ND_BOARD_USER','root')+'\r').encode())
  data=until([b'# ',b'Password:'])
 if b'Password:' in data:
  password=os.environ.get('ND_BOARD_PASSWORD')
  if password is None:raise RuntimeError('Set ND_BOARD_PASSWORD to log in')
  s.write((password+'\r').encode());until([b'# '])
 s.write(b'mkdir -p /tmp/nd-runtime; stty -echo\r');until([b'# '])
 try:
  s.write(b"base64 -d > /tmp/nd-runtime/nd-executor <<'NDR_EXEC_END'\n")
  for i in range(0,len(blob),76):s.write(blob[i:i+76]+b'\n')
  s.write(b'NDR_EXEC_END\n');until([b'# '],20)
  command=b'chmod 700 /tmp/nd-runtime/nd-executor; NDR_TTY=$(stty -g); NDR_LOG=$(cut -f1 /proc/sys/kernel/printk); dmesg -n1; stty raw -echo; printf "\\nNDR_READY\\n"; /tmp/nd-runtime/nd-executor; stty "$NDR_TTY"; stty echo; dmesg -n"$NDR_LOG"; printf "\\nNDR_STOPPED\\n"\r'
  s.write(command);until([b'\nNDR_READY\n']);started=True
  print('READY: executor owns UART; send ELF image bytes using the runtime client')
 finally:
  if not started:s.write(b'\x03stty sane; stty echo\r')
