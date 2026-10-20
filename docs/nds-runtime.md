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

## 本輪已驗證與尚未完成

- Runtime client/executor 已在本機 Unix socket 與 Luckfox ARM32 UART 實測。
- 板端只部署通用 executor；兩份同名函式、內容不同的 ARM ELF（各 5276 bytes）都由 client 當次傳送。以 input=20 分別得到 27、39，驗證並非使用板上固定 kernel。
- 路徑 payload 被拒絕；missing symbol、越界 retrieve、unload 後舊 handle 均回錯誤；正常 alloc/submit/launch/retrieve/free/unload 通過。
- NDS plugin 已以目前 LLVM headers 編譯成 Linux x86_64 ELF object；尚未完成整個 Linux libomptarget library 連結及 OpenMP 程式載入 plugin 的驗證。此證據不能視為 LEO-25 完成。
- 實機 transport 目前是 UART binary RPC。現有 USB ND function 只有 SD READ；USB image/RPC adapter 尚未實作，不能把 UART 測試稱為 USB offload 成功。
- 目前只接受 native little-endian ET_DYN ELF、單裝置、同步執行、單 thread/team、最多 16 個 mapped-pointer arguments。Scalar/aggregate/firstprivate ABI、parallel kernel、完整 OpenMP outlined image 仍待接通；沒有繞過 Clang 64-bit host/32-bit target 檢查。
- Wire 使用固定 little-endian 欄位與 uint64 address/handle slots；裝置依自己的 uintptr_t/libffi pointer ABI 轉換。主機不傳原生指標 struct，也不以參數 storage 地址差猜大小。
- 任一連線最多 64 image/region/symbol slots；單 frame/總 allocation 限 16 MiB。這是個人原型，不提供程式執行隔離；kernel 以 executor 權限執行。

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
- frontend 僅針對 NDS vendor + ARM32 開放 target 與不同指標寬度；其他 target 保留原有檢查。開放檢查不代表參數 ABI 或完整 offload 已驗證。
