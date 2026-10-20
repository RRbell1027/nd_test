#!/usr/bin/env python3
"""Exercise runtime RPC through /dev/nd0 or Guest /dev/ttyS0; no UART bridge."""
import argparse
import ctypes as C
import errno

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--lib', required=True)
p.add_argument('--transport', default='/dev/nd0')
a = p.parse_args()
l = C.CDLL(a.lib, use_errno=True)
ptr, u64, size = C.c_void_p, C.c_uint64, C.c_size_t
def api(name, result, *args):
    f = getattr(l, 'nd_rt_' + name)
    f.restype, f.argtypes = result, args
    return f
connect = api('open', ptr, C.c_char_p)
close = api('close', None, ptr)
error = api('error', C.c_char_p, ptr)
bits = api('device_bits', C.c_int, ptr)
alloc = api('alloc', C.c_int, ptr, size, C.POINTER(u64))
free = api('free', C.c_int, ptr, u64)
put = api('submit', C.c_int, ptr, u64, ptr, size)
get = api('retrieve', C.c_int, ptr, ptr, u64, size)
r = connect(a.transport.encode())
if not r: raise OSError(C.get_errno(), 'runtime connect failed')
def check(rc):
    if rc: raise RuntimeError(f'errno={C.get_errno()} {error(r).decode()}')
try:
    assert bits(r) == 32, bits(r)
    for n in (1, 4, 496, 512, 4096, 8193):
        address = u64()
        check(alloc(r, n, C.byref(address)))
        payload = bytes((i*37+11) % 256 for i in range(n))
        source = C.create_string_buffer(payload)
        target = C.create_string_buffer(n)
        check(put(r, address, source, n))
        check(get(r, target, address, n))
        assert target.raw == payload, n
        rc = get(r, target, u64(address.value+n), 1)
        assert rc == -1 and C.get_errno() == errno.EFAULT, (rc, C.get_errno())
        check(free(r, address))
        print(f'PASS: USB runtime {n} bytes, range error propagated', flush=True)
finally:
    close(r)
