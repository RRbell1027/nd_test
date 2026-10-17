#!/usr/bin/env python3
"""Run in luckfox-sdk WSL; preserve SDK and NDS driver test evidence."""
import hashlib
import json
import pathlib
import re
import shlex
import subprocess

REPO = pathlib.Path(__file__).resolve().parents[2]
OUT = REPO / "artifacts/luckfox-sdk"
OUT.mkdir(parents=True, exist_ok=True)
ROOT = pathlib.Path("/sdk/arm-rockchip830-linux-uclibcgnueabihf")
SDK = pathlib.Path("/sdk/arm-buildroot-linux-uclibcgnueabihf_sdk-buildroot")
TRIPLE = "arm-rockchip830-linux-uclibcgnueabihf"
GCC = ROOT / "bin" / (TRIPLE + "-gcc")
GXX = ROOT / "bin" / (TRIPLE + "-g++")
READELF = ROOT / "bin" / (TRIPLE + "-readelf")
CLANG = REPO / "llvm-project/build/bin/clang.exe"


def run(name, args, check=True):
    args = list(map(str, args))
    proc = subprocess.run(args, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    (OUT / (name + ".log")).write_text(
        "$ " + shlex.join(args) + "\n" + proc.stdout + proc.stderr)
    print(name, "PASS" if proc.returncode == 0 else "FAIL", flush=True)
    if proc.returncode and check:
        raise RuntimeError(proc.stdout + proc.stderr)
    return proc


def win(path):
    return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()


def linux(arg):
    if re.match(r"^[A-Za-z]:[/\\]", arg):
        return subprocess.check_output(["wslpath", "-u", arg], text=True).strip()
    return arg


sysroot = pathlib.Path(run("gcc-sysroot", [GCC, "-print-sysroot"]).stdout.strip()).resolve()
libgcc = pathlib.Path(run("gcc-libgcc", [GCC, "-print-libgcc-file-name"]).stdout.strip()).resolve()
assert libgcc.is_relative_to(ROOT), "Relocated GCC still uses original SDK"
assert sysroot.is_relative_to(ROOT), "Relocated sysroot still uses original SDK"
run("gcc-version", [GCC, "--version"])
assert run("gcc-triple", [GCC, "-dumpmachine"]).stdout.strip() == TRIPLE

config = REPO / "platform/luckfox-rv1106.ini"
config.write_text(
    "# luckfox-sdk WSL; GCC 8.3.0, ARMv7 hard-float, uClibc\n"
    "path-layout=explicit\n"
    f"toolchain-triple={TRIPLE}\n"
    "gcc-version=8.3.0\n"
    f"toolchain-path={ROOT}\n"
    f"gcc-install-path={libgcc.parent}\n"
    f"gcc-parent-lib-path={ROOT}/lib\n"
    f"sysroot={sysroot}\n"
    f"assembler-path={ROOT}/bin/{TRIPLE}-as\n"
    f"linker-driver-path={GXX}\n")

c = OUT / "hello.c"
cpp = OUT / "hello.cpp"
c.write_text('#include <stdio.h>\nint main(void) { puts("luckfox C SDK OK"); return 0; }\n')
cpp.write_text('#include <iostream>\nint main() { std::cout << "luckfox C++ SDK OK\\n"; }\n')
flags = ["-mcpu=cortex-a7", "-mfpu=vfpv4-d16", "-mfloat-abi=hard"]
run("gcc-c", [GCC, *flags, c, "-o", OUT / "hello-c"])
run("gcc-cpp", [GXX, *flags, cpp, "-o", OUT / "hello-cpp"])
run("exported-sdk-c", [SDK / "bin" / (TRIPLE + "-gcc"), c, "-o", OUT / "hello-exported-sdk"])
for name in ("hello-c", "hello-cpp", "hello-exported-sdk"):
    elf = run(name + "-elf", [READELF, "-h", "-A", "-l", "-d", OUT / name]).stdout
    assert "Machine:" in elf and "ARM" in elf
    assert "hard-float ABI" in elf and "VFP registers" in elf
    assert "/lib/ld-uClibc.so.0" in elf

# The existing custom Clang is a Windows executable. NDS selection requires
# OpenMP offloading; a plain --target=arm-nds-linux does not select this class.
source = OUT / "nds-add.c"
source.write_text("int nds_add(int a, int b) { return a + b; }\n")
assembly = OUT / "nds-add.s"
obj = OUT / "nds-add.o"
shared = OUT / "nds-add.so"
omp = OUT / "nds-target.c"
omp.write_text("int main(void) {\nint value = 0;\n"
               "#pragma omp target map(tofrom:value)\n"
               "{ value = 42; }\nreturn value != 42;\n}\n")
target = "arm-nds-linux-gnueabihf"
common = [CLANG, "-fopenmp", "-fopenmp-targets=" + target,
          "-nds-platform=" + win(config), "--offload-device-only",
          "-save-temps=obj"]
for flag in flags:
    common.extend(["-Xopenmp-target=" + target, flag])
dry = run("nds-offload-dry", [*common, "-###", win(omp), "-o", win(obj)])
assert "Using NDS ToolChain" in dry.stdout
commands = [shlex.split(line) for line in dry.stderr.splitlines()
            if line.lstrip().startswith('"')]
assembler = str(ROOT / "bin" / (TRIPLE + "-as"))
assert len([cmd for cmd in commands if cmd[0] == assembler]) == 1
offload_failure = None
for index, cmd in enumerate(commands):
    if cmd[0].lower().endswith("clang.exe"):
        cmd[0] = linux(cmd[0])
    elif cmd[0] == assembler:
        cmd = [linux(arg) for arg in cmd]
    else:
        raise RuntimeError("Unexpected generated command: " + repr(cmd))
    result = run("nds-offload-stage-" + str(index), cmd, check=False)
    if result.returncode:
        offload_failure = result.stdout + result.stderr
        break
# Isolate the actual SDK tools from failures in the OpenMP frontend. Ordinary
# ARM codegen produces assembly; invoke the assembler selected by NDS and the
# configured g++ independently. This is not an end-to-end offload test.
run("arm-codegen", [CLANG, "--target=arm-linux-gnueabihf", *flags,
                    "-fPIC", "-fno-addrsig", "-S", win(source), "-o", win(assembly)])
run("configured-assembler", [assembler, assembly, "-o", obj])
run("configured-linker-shared", [GXX, "-shared", "-Wl,--no-undefined",
                                "-Wl,-Bsymbolic", obj, "-o", shared])
elf = run("nds-shared-elf", [READELF, "-h", "-A", "-Ws", shared]).stdout
assert "DYN (Shared object file)" in elf and "hard-float ABI" in elf
assert "VFP registers" in elf and "nds_add" in elf
manifest = {
    "gcc_triple": TRIPLE, "gcc_version": "8.3.0", "toolchain": str(ROOT),
    "sysroot": str(sysroot), "config": str(config),
    "ndstest": "PlatformConfig/offload dry-run passed; assembler and g++ tested independently in WSL",
    "offload_frontend_failure": offload_failure,
    "hardware_execution": "not tested", "openmp_runtime_image": "not tested",
    "files": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
              for p in (OUT / "hello-c", OUT / "hello-cpp", shared)},
}
(OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print("SDK, PlatformConfig dry-run, assembler and linker checks passed.")
print("OpenMP frontend:", offload_failure or "PASS")
print("Hardware/offload runtime not tested.")
