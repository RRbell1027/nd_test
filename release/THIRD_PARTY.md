# Third-party components

LLVM/Clang/OpenMP: Apache-2.0 with LLVM exceptions; licenses/LLVM-LICENSE.txt.
Modified sources: https://github.com/RRbell1027/llvm-project-nds at the llvm_commit in VERSION.json.

GNU ARM GCC/binutils and support libraries are provided by the Luckfox toolchain.
They retain their upstream GPL/LGPL and GCC Runtime Library Exception licenses.
See licenses/ and the toolchain source/version references in the upstream SDK.
Upstream SDK: https://github.com/LuckfoxTECH/luckfox-pico (revision in VERSION.json).
The ARM toolchain's included license files are retained in the SDK archive.

Linux kernel and NDS kernel driver additions are GPL-2.0; kernel source is the matching
Luckfox SDK source plus the included NDS integration sources/configuration.
libffi retains its MIT-style license (licenses/libffi-LICENSE).

The main near_data repository currently has no project-wide license declaration.
This release does not grant a new license for that code; upstream component licenses remain applicable.
