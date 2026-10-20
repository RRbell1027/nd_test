# Ubuntu Guest M4 validation

The intended compiler and application host is the x86_64 Ubuntu Guest. WSL only
hosts QEMU/USBIP; the Windows Clang build is not the target execution environment.

Clone both repositories at the recorded commits:

```sh
git clone --branch codex/nds-arm32-runtime --single-branch \
  https://github.com/RRbell1027/nd_test.git near_data_m4
cd near_data_m4
git submodule update --init --depth 1
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++ libffi-dev zlib1g-dev libzstd-dev libedit-dev python3
make runtime-check
```

Build a native Linux Clang. This bring-up build disables optimization to reduce
initial build time; it still emits code for both the x86_64 host and ARM device.
Choose a job count appropriate for available memory.

```sh
cmake -G Ninja -S llvm-project/llvm -B build/llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS_RELEASE='-O0 -DNDEBUG' \
  -DCMAKE_CXX_FLAGS_RELEASE='-O0 -DNDEBUG' \
  -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD='X86;ARM' \
  -DLLVM_PARALLEL_LINK_JOBS=1 -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DCLANG_INCLUDE_TESTS=OFF
cmake --build build/llvm --target clang clang-offload-packager clang-linker-wrapper -j3
```

Install the Linux ARM SDK inside the Guest, then copy
`platform/luckfox-rv1106.ini` to `platform/guest.local.ini` and replace the
`/sdk/arm-rockchip830-linux-uclibcgnueabihf` prefix with its actual Guest path.
The platform file selects the assembler and linker driver, not their execution
OS: they must be executable in the Guest itself.

```sh
python3 tests/guest/validate_compile.py \
  --clang build/llvm/bin/clang --platform platform/guest.local.ini
```

The test executes real compiler jobs and records separate results/logs under
`artifacts/guest-compile/`. A device object, packaged host/device object, and
shared-image command are tested separately. A successful object compile alone
does not prove final device-image linking or execution. Likewise,
`make runtime-check` uses a local Guest executor and does not prove execution on
Luckfox. Full board execution must be tested separately.
