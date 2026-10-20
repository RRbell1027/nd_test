#!/usr/bin/env python3
"""Attach the existing UART bridge as a dedicated QEMU virtio runtime port."""
import argparse
import json
import socket
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--qmp', default='/tmp/near-data-qmp.sock')
p.add_argument('--socket', required=True)
a = p.parse_args()
with socket.socket(socket.AF_UNIX) as s:
    s.connect(a.qmp)
    f = s.makefile('rwb', buffering=0)
    f.readline()
    def call(execute, arguments=None):
        cmd = {'execute': execute}
        if arguments is not None:
            cmd['arguments'] = arguments
        f.write((json.dumps(cmd) + '\n').encode())
        while True:
            answer = json.loads(f.readline())
            if 'error' in answer:
                raise RuntimeError(answer['error'])
            if 'return' in answer:
                return answer['return']
    call('qmp_capabilities')
    chars = {x['label']: x for x in call('query-chardev')}
    if 'nds-rpc' in chars:
        # QEMU 6.2 reports a connected Unix client as just 'unix:'; it
        # does not expose the peer path in query-chardev. Reuse our dedicated
        # id without replacing its live connection.
        filename = chars['nds-rpc']['filename']
        if filename != 'unix:' and a.socket not in filename:
            raise RuntimeError('existing nds-rpc uses a different backend')
    else:
        call('chardev-add', {'id': 'nds-rpc', 'backend': {'type': 'socket', 'data': {
            'addr': {'type': 'unix', 'data': {'path': a.socket}}, 'server': False,
            'reconnect': 1}}})
    devices = {x['name'] for x in call('qom-list', {'path': '/machine/peripheral'})}
    if 'nds-serial' not in devices:
        call('device_add', {'driver': 'virtio-serial-pci', 'id': 'nds-serial'})
    if 'nds-port' not in devices:
        call('device_add', {'driver': 'virtserialport', 'bus': 'nds-serial.0',
                           'chardev': 'nds-rpc', 'id': 'nds-port', 'name': 'nds-runtime'})
print('READY: Guest port /dev/virtio-ports/nds-runtime')
