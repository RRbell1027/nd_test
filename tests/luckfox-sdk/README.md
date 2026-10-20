# Luckfox SDK validation — 2026-10-17

Run in Windows PowerShell:

```powershell
wsl -d luckfox-sdk -- python3 /mnt/d/develops/near_data/tests/luckfox-sdk/validate.py
```

The script checks the installed paths, creates `platform/luckfox-rv1106.ini`,
and records commands, ELF inspection and failures in `artifacts/luckfox-sdk/`.
A successful script exit means the SDK checks completed, **not** that OpenMP
offloading works. See `offload_frontend_failure` in `manifest.json`.

## SDK export performed

```sh
env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  make -C /sdk/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6 sdk
```

This completed successfully. `sdk` depends on `world`: it also finalized target
packages/rootfs and generated `output/images/rootfs.tar`. It did not flash a board.
The initial attempt failed because the inherited Windows PATH contained spaces.
No Buildroot configuration was changed.

Archive:

```
/sdk/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/images/arm-buildroot-linux-uclibcgnueabihf_sdk-buildroot.tar.gz
SHA256 c562402d9bcff94e0b647d4c73af7a1cc488a40b0f13898193044f6d3615d3b5
```

It was extracted to `/sdk/arm-buildroot-linux-uclibcgnueabihf_sdk-buildroot`,
and its `relocate-sdk.sh` completed.

## External-toolchain limitation

The existing Buildroot configuration uses `BR2_TOOLCHAIN_EXTERNAL_PREINSTALLED=y`.
Its SDK archive contains a GCC wrapper and absolute binutils symlinks pointing at
`/sdk/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf`.
Relocation does not make this archive self-contained. The exported wrapper was
tested successfully, but still needs that original toolchain.

To provide a separate, usable installation without modifying the original SDK,
the entire external toolchain was copied with `cp -a` to:

```
/sdk/arm-rockchip830-linux-uclibcgnueabihf
```

The new PlatformConfig uses this copy and its own sysroot. Its GCC reports
libgcc and sysroot inside the copied directory. The exported Buildroot sysroot
remains available separately; this test does not claim the two sysroots are
identical or that arbitrary SDK packages were tested.

## Results

| Check | Result |
|---|---|
| `make sdk`, archive extraction, relocation | PASS |
| Copied GCC/G++ 8.3.0, triple and sysroot queries | PASS |
| C and C++ executable compilation/link | PASS |
| Exported Buildroot wrapper C compilation/link | PASS, depends on original external toolchain |
| ELF machine/ABI/interpreter | ARMv7, EABI5 hard-float, VFP arguments, `/lib/ld-uClibc.so.0` |
| NDS OpenMP driver reads real PlatformConfig | PASS in `-###` output |
| Driver selects configured assembler | PASS in `-###` output |
| Clang ARM assembly → configured assembler → configured G++ shared ELF | PASS, tools invoked independently |
| Actual OpenMP offload frontend | FAIL: `OpenMP target is invalid: 'arm-nds-linux-gnueabihf'` |
| Board execution, ND runtime image/header, full offload | NOT TESTED |

The available custom Clang is the existing **Windows** build, version 21.1.8,
reporting revision `35d72874042d75f44ce634304138951878ddb3f3`. The validation
script starts it from WSL, parses its dry-run commands, and runs SDK tools on
Linux. This is not proof of a native Linux Clang build or a single-command
end-to-end offload pipeline.

`NDSToolChain` is selected in `Driver::getOffloadToolChain`; simply specifying
`--target=arm-nds-linux-gnueabihf` selects the ordinary target toolchain instead.
The SDK target triple belongs in PlatformConfig; the offload target vendor is
`nds`. CPU/FPU arguments still need to be passed separately.

## Findings requiring follow-up

1. `CompilerInvocation.cpp` OpenMP target validation excludes ARM32. The failure
   occurs in the first frontend job, before device assembly. Pointer-size checks
   also need consideration when pairing an x86_64 host and ARM32 device.
2. Clang emits `.addrsig` by default for ordinary ARM codegen, but the SDK
   assembler rejects it. `-fno-addrsig` passed the isolated assembler/link test.
3. Device-only offload dry-run produces an object; it does not exercise
   `nds::Linker`. The configured G++ was tested independently. NDS runtime image
   format/header and output naming still require validation.
4. Current `nds::Linker::ConstructJob` does not forward PlatformConfig `lib-dir`
   entries as `-L` arguments. The minimal test does not exercise custom libraries.

LEO-22 should remain in progress: the GCC/platform baseline is verified, but
its complete runtime-image acceptance criteria are not satisfied.

## 2026-10-20 M4 follow-up

- `tests/luckfox-sdk/validate_driver.py` verifies explicit/buildroot selection and missing option, unknown key, missing required key, invalid layout diagnostics. All six cases pass, including after the Windows Clang incremental rebuild. LEO-20 and LEO-21 are Done; their historical descriptions were stale.
- `image_probe.c` compiled with the real SDK GCC and `-ldl` loads the prior `nds-add.so` on Luckfox using dlopen/dlsym. Actual output: `NDS ARM image result=42 expected=42`, exit 0. Evidence: `artifacts/luckfox-sdk/board-image-probe.log`. This verifies ordinary ARM shared-ELF ABI/loading/function execution, not OpenMP outlined-region execution.
- Added PlatformConfig library-directory forwarding to `nds::Linker::ConstructJob`; Clang rebuild passes. Current full/device-shared dry runs still do not invoke this linker, so this is not yet an end-to-end linker/image validation.
- Remaining LEO-22 work: wire the device image link into the actual offload pipeline; resolve ARM32 frontend support and the 64-bit host/32-bit device ABI; agree on the runtime image representation. The old SYLPH runtime expects a custom header plus ELF path, whereas the current linker emits raw ELF.
- Runtime loading/transport/execution integration is LEO-23/24; the full OpenMP example is LEO-25. These now depend on the required prototype capabilities rather than completion of every M3 protocol/multi-extent hardening item.

Recheck driver configuration on Windows:

```powershell
# Use an installed Python interpreter.
python tests/luckfox-sdk/validate_driver.py
```

Build the isolated board loader in WSL:

```sh
/sdk/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc \
  -mcpu=cortex-a7 -mfpu=vfpv4-d16 -mfloat-abi=hard -Os \
  tests/luckfox-sdk/image_probe.c -ldl -o artifacts/luckfox-sdk/image-probe
```

Place `image-probe` and `nds-add.so` under `/tmp/nd-m4` on the board, then:

```sh
/tmp/nd-m4/image-probe /tmp/nd-m4/nds-add.so
```