# NDS preview release

The release has three independent archives:

- `nds-sdk-linux-x86_64.tar.gz`: prebuilt Guest LLVM, ARM GCC, headers, libraries, wrapper and examples. Extract to a permanent directory and run `sudo ./install.sh`. Do not move it after installation without reinstalling.
- `nds-host-linux.tar.gz`: Linux USB driver sources and QEMU launch example. Build against the running host kernel. Native Linux is supported as a build target; the current end-to-end validation was on WSL2.
- `nds-device-luckfox-rv1106.tar.gz`: verified RV1106 boot partition image and rootfs add-on (executor/service). This is an update for an existing matching Luckfox SDK installation, not a complete factory recovery image.

Ubuntu Guest's SD disk image is not included. The SDK host binaries were built on Ubuntu 22.04 x86_64; host C/C++ development files and Python 3.10+ must be installed. The device requires the matching ARM32 hard-float uClibc base system.

Setup order: device firmware/add-on, host driver + QEMU, then SDK installation inside Guest. See each archive's README. Run `sha256sum -c SHA256SUMS` before extracting.

Compilation (inside Guest):

```sh
nds-clang++ -fopenmp -fopenmp-targets=arm-nds-linux-gnueabihf \
  -nds-platform="$HOME/nds-sdk/platform/luckfox.ini" -fno-addrsig \
  "$HOME/nds-sdk/examples/omp_file_read.cpp" -o omp_file_read
OMP_TARGET_OFFLOAD=MANDATORY ND_RUNTIME_DEVICE_PATH=/dev/ttyS0 \
  ./omp_file_read "$HOME/nds-sdk/examples/input.bin"
```

This preview supports synchronous single-thread device kernels, explicitly mapped fixed-width scalar and contiguous element buffers, and is_device_ptr. It does not implement all OpenMP semantics, deep pointer mapping, or arbitrary cross-width C++ object layout. See docs/nds-runtime.md. Existing hardware tests passed; a release packaging test alone is not a new hardware execution test.

Packaging is performed by `python3 release/package.py --sdk /path/to/exported/nds-sdk --boot /path/to/verified/boot.img --executor /path/to/ARM/nd-executor --output /path/to/output`. The script does not flash hardware or modify the source SDK.
