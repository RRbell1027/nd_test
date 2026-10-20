#!/usr/bin/env python3
"""Verify host ND command reaches Luckfox UART; does not verify SD data."""
import argparse
import os
import struct
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--uart', default='/dev/ttyUSB0')
    parser.add_argument('--device', default='/dev/nd0')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--sector', type=int, default=0)
    parser.add_argument('--length', type=int, default=16)
    parser.add_argument('--timeout', type=float, default=5)
    args = parser.parse_args()
    if not (0 <= args.sector <= (2**63 - 1) // 512 and 1 <= args.length <= 4096):
        parser.error('sector must fit signed byte offset; length must be 1..4096')
    expected = f'nd: READ sector={args.sector} length={args.length}'.encode()
    with serial.Serial(args.uart, args.baud, timeout=0.2) as uart:
        uart.reset_input_buffer()
        fd = os.open(args.device, os.O_RDWR | os.O_CLOEXEC | os.O_NONBLOCK)
        try:
            request = struct.pack('<HHIQ', 0x4e44, 1, args.length, args.sector)
            sent = os.write(fd, request)
            if sent != len(request):
                raise RuntimeError(f'short host write: {sent}/{len(request)}')
            # Drain response-v1 data so this probe does not leave a pending transaction.
            try:
                os.read(fd, 4112)
            except BlockingIOError:
                pass
        finally:
            os.close(fd)
        deadline = time.monotonic() + args.timeout
        received = bytearray()
        while time.monotonic() < deadline:
            received.extend(uart.read(4096))
            if expected in received:
                print(f'PASS: UART confirmed {expected.decode()}')
                print('Scope: command reception only; no SD payload or Guest buffer verification.')
                return 0
        print(received.decode(errors='replace'))
        raise RuntimeError('UART did not confirm command before timeout')


if __name__ == '__main__':
    raise SystemExit(main())