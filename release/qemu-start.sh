#!/bin/sh
set -eu
if [ "$#" -ne 1 ]; then echo "Usage: $0 /dev/disk/by-id/YOUR-SD (writes persist to SD)" >&2; exit 2; fi
disk=$(readlink -f "$1")
[ -b "$disk" ] || { echo "Not a block device: $disk" >&2; exit 1; }
[ -c /dev/nd0 ] || { echo "Load nd_driver and connect Luckfox first" >&2; exit 1; }
if lsblk -nr -o MOUNTPOINTS "$disk" | grep -q '[^[:space:]]'; then
 echo "Unmount the SD and all its partitions on the host first" >&2; exit 1
fi
exec qemu-system-x86_64 -enable-kvm -cpu host -m "${ND_GUEST_MEMORY:-4G}"  -drive "file=$disk,format=raw,if=virtio,cache=none"  -chardev pipe,id=nd,path=/dev/nd0 -serial chardev:nd  -serial mon:stdio -display none
