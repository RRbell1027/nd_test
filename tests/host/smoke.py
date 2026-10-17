#!/usr/bin/env python3
"""Check libnd/CLI build integration without sending requests to hardware."""
import ctypes
import errno
import os
from pathlib import Path
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
cli = subprocess.run([str(build / "test_nd")], capture_output=True, text=True)
assert cli.returncode == 2 and "Usage:" in cli.stderr, cli

lib = ctypes.CDLL(str(build / "libnd.so"), use_errno=True)
lib.nd_open.argtypes = [ctypes.c_char_p]
lib.nd_open.restype = ctypes.c_void_p
lib.nd_file_size.argtypes = [ctypes.c_void_p]
lib.nd_file_size.restype = ctypes.c_uint64
lib.nd_extent_count.argtypes = [ctypes.c_void_p]
lib.nd_extent_count.restype = ctypes.c_size_t
lib.nd_close.argtypes = [ctypes.c_void_p]
lib.nd_close.restype = ctypes.c_int

assert not lib.nd_open(None)
assert ctypes.get_errno() == errno.EINVAL

# Use WSL's Linux filesystem, not /mnt/d, for FIEMAP support.
with tempfile.NamedTemporaryFile(dir="/tmp") as sample:
    payload = b"Near-Data host build smoke test\n"
    sample.write(payload)
    sample.flush()
    os.fsync(sample.fileno())
    handle = lib.nd_open(os.fsencode(sample.name))
    assert handle, os.strerror(ctypes.get_errno())
    try:
        assert lib.nd_file_size(handle) == len(payload)
        assert lib.nd_extent_count(handle) > 0
    finally:
        assert lib.nd_close(handle) == 0

print("PASS: CLI loads libnd; API opens a real file and obtains FIEMAP extents.")
print("No /dev/nd0 request, module load, or gadget operation was performed.")
