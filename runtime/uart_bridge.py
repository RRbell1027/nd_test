#!/usr/bin/env python3
"""Bridge an exclusive Luckfox UART to one QEMU Unix-socket chardev.
Start the board executor first. This transports bytes only, not runtime logic.
"""
import argparse
import selectors
import socket
import sys
from pathlib import Path
import serial

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--uart', default='/dev/ttyUSB0')
p.add_argument('--socket', required=True)
a = p.parse_args()
path = Path(a.socket)
sel = selectors.DefaultSelector()
received = [0, 0]
bound = False
try:
    with serial.Serial(a.uart, 115200, timeout=0, write_timeout=0, exclusive=True) as uart, socket.socket(socket.AF_UNIX) as server:
        server.bind(str(path))
        bound = True
        server.listen(1)
        print('READY: UART bridge listening at ' + str(path), flush=True)
        conn, _ = server.accept()
        with conn:
            conn.setblocking(False)
            endpoints = [conn, uart]
            pending = [bytearray(), bytearray()]
            for i, endpoint in enumerate(endpoints):
                sel.register(endpoint, selectors.EVENT_READ, i)
            while True:
                for key, events in sel.select():
                    i = key.data
                    if events & selectors.EVENT_READ:
                        block = conn.recv(65536) if i == 0 else uart.read(65536)
                        if not block and i == 0:
                            raise EOFError('QEMU disconnected')
                        received[i] += len(block)
                        pending[1-i].extend(block)
                        if len(pending[1-i]) > 32*1024*1024:
                            raise RuntimeError('bridge queue exceeds 32 MiB')
                    if events & selectors.EVENT_WRITE and pending[i]:
                        try:
                            n = conn.send(pending[i]) if i == 0 else uart.write(pending[i])
                            del pending[i][:n]
                        except (BlockingIOError, serial.SerialTimeoutException):
                            pass
                    for j, endpoint in enumerate(endpoints):
                        sel.modify(endpoint, selectors.EVENT_READ | (selectors.EVENT_WRITE if pending[j] else 0), j)
except (KeyboardInterrupt, EOFError):
    pass
finally:
    sel.close()
    # Only remove the socket created by this process.
    if bound and path.exists():
        path.unlink()
    print('Bridge bytes guest->board=%d board->guest=%d' % tuple(received), file=sys.stderr)
