#!/usr/bin/env python3
"""Read one SD range through ND Bulk IN and compare with UMS (LEO-16).
Requires root, libusb-1.0, and new response-v1 firmware. Temporarily detaches
nd_host from interface 1; stop QEMU and other ND clients before running.
"""
import argparse
import ctypes as C
import ctypes.util
import os
from pathlib import Path
import struct


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--disk', required=True, help='verified UMS whole-disk device')
    p.add_argument('--sector', type=int, default=0)
    p.add_argument('--length', type=int, default=512)
    args = p.parse_args()
    if not (0 <= args.sector <= (2**63 - 1 - args.length) // 512 and 1 <= args.length <= 4096):
        p.error('sector must fit signed byte offset; length must be 1..4096')
    devices = []
    for d in Path('/sys/bus/usb/devices').iterdir():
        if (d / 'idVendor').exists() and (d / 'idProduct').exists():
            if (d / 'idVendor').read_text().strip() == '2207' and (d / 'idProduct').read_text().strip() == '0000':
                devices.append(d)
    if len(devices) != 1:
        raise RuntimeError(f'expected exactly one Luckfox, found {len(devices)}')
    d = devices[0]
    interfaces = [i for i in d.iterdir() if (i / 'bInterfaceNumber').exists() and (i / 'bInterfaceNumber').read_text().strip() == '01']
    if len(interfaces) != 1:
        raise RuntimeError('ND interface 1 not found')
    endpoints = {}
    for ep in interfaces[0].glob('ep_*'):
        if (ep / 'type').read_text().strip() == 'Bulk':
            endpoints[(ep / 'direction').read_text().strip()] = int((ep / 'bEndpointAddress').read_text().strip(), 16)
    if 'in' not in endpoints or 'out' not in endpoints:
        raise RuntimeError('ND Bulk IN/OUT missing: firmware update required')
    # Require that comparison disk belongs to this exact USB device.
    disk = Path(args.disk).resolve()
    if d.resolve() not in Path('/sys/class/block', disk.name).resolve().parents:
        raise RuntimeError('comparison disk is not on the selected Luckfox USB device')
    lib = C.CDLL(ctypes.util.find_library('usb-1.0'))
    def api(name, result, *params):
        f = getattr(lib, 'libusb_' + name); f.restype = result; f.argtypes = params
        return f
    init = api('init', C.c_int, C.POINTER(C.c_void_p))
    finish = api('exit', None, C.c_void_p)
    open_device = api('open_device_with_vid_pid', C.c_void_p, C.c_void_p, C.c_uint16, C.c_uint16)
    close = api('close', None, C.c_void_p)
    auto_detach = api('set_auto_detach_kernel_driver', C.c_int, C.c_void_p, C.c_int)
    claim = api('claim_interface', C.c_int, C.c_void_p, C.c_int)
    release = api('release_interface', C.c_int, C.c_void_p, C.c_int)
    bulk = api('bulk_transfer', C.c_int, C.c_void_p, C.c_ubyte, C.c_void_p, C.c_int, C.POINTER(C.c_int), C.c_uint)
    def check(rc, operation):
        if rc < 0:
            raise RuntimeError(f'{operation}: libusb error {rc}')
    ctx = C.c_void_p(); check(init(C.byref(ctx)), 'init')
    handle = None; claimed = False
    try:
        handle = open_device(ctx, 0x2207, 0)
        if not handle: raise RuntimeError('cannot open Luckfox')
        check(auto_detach(handle, 1), 'enable automatic kernel-driver reattach')
        check(claim(handle, 1), 'claim interface 1'); claimed = True
        command = C.create_string_buffer(struct.pack('<HHIQ', 0x4e44, 1, args.length, args.sector))
        actual = C.c_int()
        check(bulk(handle, endpoints['out'], command, 16, C.byref(actual), 5000), 'request')
        if actual.value != 16: raise RuntimeError('short request')
        reply = C.create_string_buffer(16 + 4096)
        check(bulk(handle, endpoints['in'], reply, len(reply), C.byref(actual), 5000), 'response')
        if actual.value < 16: raise RuntimeError('truncated response header')
        magic, version, status, length, reserved = struct.unpack_from('<HHiII', reply.raw)
        if (magic, version, reserved) != (0x4e44, 1, 0): raise RuntimeError('invalid response header')
        if status: raise OSError(-status, os.strerror(-status))
        if length != args.length or actual.value != 16 + length: raise RuntimeError('short/invalid payload')
        fd = os.open(disk, os.O_RDONLY | os.O_CLOEXEC)
        try: reference = os.pread(fd, args.length, args.sector * 512)
        finally: os.close(fd)
        if reply.raw[16:16 + length] != reference: raise RuntimeError('ND payload differs from UMS')
        print(f'PASS: ND Bulk IN matches UMS: sector={args.sector} bytes={length}')
        print('Scope: USB device response only; Guest libnd buffer not yet tested.')
    finally:
        if handle:
            if claimed: release(handle, 1)
            close(handle)
        finish(ctx)


if __name__ == '__main__':
    main()