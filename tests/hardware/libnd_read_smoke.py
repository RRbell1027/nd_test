#!/usr/bin/env python3
"""Verify real libnd buffer bytes against a file on the exported SD filesystem."""
import argparse
import ctypes as C
import os
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--lib', required=True)
p.add_argument('--file', required=True)
p.add_argument('--transport', default='/dev/nd0')
p.add_argument('--partition-start', type=int, default=4096)
a = p.parse_args()
os.environ['ND_TRANSPORT_PATH'] = a.transport
os.environ['ND_PARTITION_START_SECTOR'] = str(a.partition_start)
lib = C.CDLL(a.lib, use_errno=True)
lib.nd_open.argtypes = [C.c_char_p]; lib.nd_open.restype = C.c_void_p
lib.nd_read_host_legacy.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]; lib.nd_read_host_legacy.restype = C.c_int64
lib.nd_close.argtypes = [C.c_void_p]; lib.nd_close.restype = C.c_int
reference = Path(a.file).read_bytes()
if len(reference) > 1024 * 1024:
    raise RuntimeError('choose a <=1 MiB single-extent fixture')
handle = lib.nd_open(os.fsencode(a.file))
if not handle: raise OSError(C.get_errno(), os.strerror(C.get_errno()))
try:
    assert lib.nd_read_host_legacy(handle, None, 0) == 0
    for requested in sorted({1, min(4096, len(reference)), len(reference) + 17}):
        buf = (C.c_ubyte * (requested + 16))(*([0xa5] * (requested + 16)))
        result = lib.nd_read_host_legacy(handle, buf, requested)
        if result < 0: raise OSError(C.get_errno(), os.strerror(C.get_errno()))
        expected = min(requested, len(reference))
        assert result == expected, (result, expected)
        assert bytes(buf[:result]) == reference[:result], 'buffer differs from filesystem bytes'
        assert bytes(buf[result:]) == b'\xa5' * (len(buf) - result), 'buffer overrun'
        print(f'PASS: {a.transport} requested={requested} returned={result} bytes match, sentinel intact')
finally:
    lib.nd_close(handle)