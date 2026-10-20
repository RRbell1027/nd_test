# NDS runtime：上傳 image bytes 後執行

## 與 sylph 的差異

舊 sylph 將自訂 image header 後的文字解釋成板端 `.so` 路徑，再以 OpenMPI 請 worker 開啟。新 NDS plugin 是獨立 backend，舊 sylph/MPI 保留作參考。

NDS `loadBinaryImpl` 直接傳送 `__tgt_device_image::ImageStart..ImageEnd` 的完整 ELF bytes。Luckfox executor 收到後建立唯一 `/tmp/nd-image-XXXXXX`、完整寫入、`dlopen(RTLD_NOW)`、刪除暫存路徑；之後由 image handle 管理生命週期。不接受路徑字串，不尋找預先部署的 kernel。

資料流：

```text
libomptarget NDS plugin → libnd_runtime → exclusive binary stream
                                          ↓
Luckfox nd-executor → temporary ELF → dlopen/dlsym → libffi launch
                                          ↑
                 allocate / submit / retrieve / free / unload
```

## 已驗證與範圍

- Guest Ubuntu 原生 Clang/OpenMP/NDS plugin → 傳送 ARM ELF bytes → Luckfox executor → mapped buffer/kernel launch → 結果回傳，已實機通過。
- `make nds-check NDS_JOBS=6` exit 0：`OMP_TARGET_OFFLOAD=MANDATORY` 下，delta=7、19 均通過；kernel 回傳 `sizeof(void*)*8 == 32`，不允許 host fallback。
- `examples/omp_runtime_smoke.cpp` 驗證 mapped int32 scalar，以及 vector<int32_t>、vector<float>、vector<double> 的 data() 連續元素。逐項與 host reference 比較；不傳 std::vector 物件。
- `make runtime-arm` 在 Guest 交叉建置 ARM32/uClibc executor，exit 0。Native `make runtime-check` 與先前兩份不同 ARM image 的 RPC 測試也通過。
- 修正 OpenMP 隱含 launch-environment sentinel、runtime ELF32 檢查/symbol lookup，以及 NDS device 誤用 host C++/glibc headers 的問題。最初失敗的 mandatory 測試 exit 134 已被後續成功結果取代；原 log 保留。
- 實機 transport 是 Guest virtio serial → WSL byte bridge → Luckfox UART。USB ND interface 目前只有 SD READ；USB runtime adapter 尚未完成，不能稱為 USB offload 成功。
- 初版只涵蓋 ARM32 little-endian ET_DYN、單裝置、同步單 thread/team、最多 16 個呼叫參數（包含隱含 launch 參數）。Scalar 必須明確 mapping；一般 firstprivate/value ABI、含指標 aggregate、nested vector、parallel kernel 尚未支援。
- Wire 使用固定 little-endian 欄位與 uint64 address/handle slots；executor 轉成原生 uintptr_t/libffi pointer ABI。主機不直接截短 host pointer。
- 連線最多 64 image/region/symbol slots，單 frame/總 allocation 限 16 MiB；UART 大型 payload 效能尚未驗證。Kernel 以 executor 權限執行。

## 建置與本機測試

主機需 CMake、C compiler、pthread；executor 另需 libffi development files。

```sh
make runtime-check
# 只建立 client 可設定：
cmake -S runtime -B build/runtime-client -DND_BUILD_EXECUTOR=OFF
```

目前 luckfox-sdk WSL 的 native libffi 位於 SDK host 目錄，初次設定可使用：

```sh
make runtime-check RUNTIME_CMAKE_ARGS='-DFFI_INCLUDE_DIR=/sdk/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/include -DFFI_LIBRARY=/sdk/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/lib/libffi.so'
```

ARM executor 用實際 SDK GCC 與 Buildroot target libffi 建置，避免連到 host libffi：

```sh
TC=/sdk/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc
BR=/sdk/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot
mkdir -p build/runtime/arm
$TC -std=c11 -Os -s -mcpu=cortex-a7 -mfpu=vfpv4-d16 -mfloat-abi=hard \
  -I$BR/usr/include runtime/executor.c -L$BR/usr/lib -Wl,-rpath,/usr/lib \
  -lffi -ldl -o build/runtime/arm/nd-executor
$TC -shared -fPIC -Os -s -mcpu=cortex-a7 -mfpu=vfpv4-d16 -mfloat-abi=hard \
  examples/runtime_kernel.c -o build/runtime/arm/kernel-a.so
sed 's/+ 7/+ 19/' examples/runtime_kernel.c > build/runtime/kernel-b.c
$TC -shared -fPIC -Os -s -mcpu=cortex-a7 -mfpu=vfpv4-d16 -mfloat-abi=hard \
  build/runtime/kernel-b.c -o build/runtime/arm/kernel-b.so
```

板端已確認 `/usr/lib/libffi.so.8` 存在。UART 請獨占使用，勿同時開終端機；啟動 executor 時會暫停 console echo、降低 kernel console loglevel，收到 shutdown 後恢復。

```sh
# 若尚未登入，先在環境提供 ND_BOARD_USER / ND_BOARD_PASSWORD；不寫進 repo。
python3 tests/runtime/start_uart_executor.py --executor build/runtime/arm/nd-executor
python3 tests/runtime/image_roundtrip.py --lib build/runtime/libnd_runtime.so \
  --tty /dev/ttyUSB0 build/runtime/arm/kernel-a.so build/runtime/arm/kernel-b.so
```

不要預先把 kernel-a/b.so 複製到板端；測試應透過 runtime LOAD opcode 傳送其內容。

## LLVM plugin

原生 Linux LLVM/offload build 選取：

```text
LIBOMPTARGET_PLUGINS_TO_BUILD=nds
NDS_RUNTIME_INCLUDE_DIR=<near_data>/include
NDS_RUNTIME_LIBRARY=<near_data>/build/runtime/libnd_runtime.a
```

執行時 `ND_RUNTIME_DEVICE_PATH` 必須指向已啟動 executor 的專用通道，例如目前的 `/dev/ttyUSB0`。它不是原有 SD-read `/dev/nd0` 協定，不能直接互換。USB adapter 完成前不設定為 `/dev/nd0`。

Windows 開發環境可用 `tests/runtime/compile_plugin.py` 搭配既有 LLVM build、WSL Linux sysroot 與 `/sdk/nds-llvm-headers` 做 compile-only 檢查。檢查使用真實 TableGen JSON enum records；一般完整 offload build 仍由 offload-tblgen 產生 headers。

## 相關檔案

- include/nd_runtime.h：host API
- runtime/client.c、wire.h：stream RPC client/framing
- runtime/executor.c：板端 image、memory、symbol、libffi executor
- llvm-project/offload/plugins-nextgen/nds：libomptarget adapter
- tests/runtime：native 與板端測試

另已修正 host ND USB driver 的 match，限定 Luckfox 2207:0000 的 ND interface，避免 WSL 重啟時誤綁 PL2303 UART。

## NDS 跨位寬原型約定

- Device 保留 ARM32 原生 data layout；不新增會將 device 指標改成 64-bit 的 LLVM 架構。
- 通訊使用 64-bit device address/handle，由 executor 解析成原生指標；host 指標不可直接截短使用。
- 第一版目標為固定寬度整數、float、double，以及以 data()/元素數量映射的 vector 連續元素；不是直接映射 std::vector 物件。
- 跨裝置資料避免 long、unsigned long、size_t、ptrdiff_t、long double；含指標 struct、巢狀 vector 與深層 mapping 暫不支援。這些是使用規範，不新增 frontend 型別拒絕。
- frontend 針對 NDS vendor 的已知架構開放 target 與不同指標寬度；未知架構及非 NDS target 保留原有檢查。開放檢查不代表參數 ABI 或完整 offload 已驗證。

## Guest 與 Luckfox 執行入口

Guest SDK/ARM libffi 路徑見 Makefile 的 `NDS_DEVICE_CC`、`NDS_DEVICE_FFI_ROOT`，可覆寫。`platform/guest.local.ini` 指向 Guest SDK；FFI 必須使用 ARM 版，不能使用 x86_64 headers/library。

```sh
# Guest：建置 host runtime、NDS plugin，以及板端 executor
make nds-runtime NDS_JOBS=6
make runtime-arm
# 通道已由管理端啟動，且 dev 可存取此 virtio port 時：
make nds-check NDS_JOBS=6
```

管理端（目前為 WSL）準備好可用的 ARM executor 檔案後，以下各步使用同一個 `NDS_BRIDGE_SOCKET`：

```sh
make runtime-uart-start NDS_UART=/dev/ttyUSB0 NDS_EXECUTOR=/path/to/arm/nd-executor
# 此命令佔用前景執行；另開管理 terminal 執行下一步。
make runtime-uart-bridge NDS_UART=/dev/ttyUSB0
make runtime-guest-port NDS_QMP=/tmp/near-data-qmp.sock
```

Guest port 為 `/dev/virtio-ports/nds-runtime`；首次需由管理端賦予 dev 存取權。UART 只能有一個使用者，不可同時開 shell terminal 與 bridge。只上傳通用 executor；kernel image 每次由編譯後的執行檔交給 plugin 傳送。

目前保留 executor/bridge/Guest port 運行，可直接重跑 Guest `make nds-check`。停止時應先用 runtime SHUTDOWN RPC 結束 executor，待 tty 還原後停止 bridge；重新開機或重設 UART 後需重新建立通道。

本輪證據保存在 Guest `artifacts/guest-link/make-nds-check.log`、`.exit` 及 `runtime-board2.log`；本機亦保存對應 log。
