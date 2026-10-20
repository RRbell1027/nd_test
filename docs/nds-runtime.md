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
- `make nds-usb-check NDS_JOBS=6` exit 0：`OMP_TARGET_OFFLOAD=MANDATORY` 下，delta=7、19 均通過；kernel 回傳 `sizeof(void*)*8 == 32`，不允許 host fallback。
- `examples/omp_runtime_smoke.cpp` 驗證 mapped int32 scalar，以及 vector<int32_t>、vector<float>、vector<double> 的 data() 連續元素。逐項與 host reference 比較；不傳 std::vector 物件。
- `make runtime-arm` 在 Guest 交叉建置 ARM32/uClibc executor，exit 0。Native `make runtime-check` 與先前兩份不同 ARM image 的 RPC 測試也通過。
- 修正 OpenMP 隱含 launch-environment sentinel、runtime ELF32 檢查/symbol lookup，以及 NDS device 誤用 host C++/glibc headers 的問題。最初失敗的 mandatory 測試 exit 134 已被後續成功結果取代；原 log 保留。
- 實機 transport 是 Guest ttyS0 → QEMU pipe → WSL /dev/nd0 → USB ND interface → Luckfox /dev/nd_rpc → executor；USB offload 已通過，UART 僅用於管理。
- 初版只涵蓋 ARM32 little-endian ET_DYN、單裝置、同步單 thread/team、最多 16 個呼叫參數（包含隱含 launch 參數）。Scalar 必須明確 mapping；一般 firstprivate/value ABI、含指標 aggregate、nested vector、parallel kernel 尚未支援。
- Wire 使用固定 little-endian 欄位與 uint64 address/handle slots；executor 轉成原生 uintptr_t/libffi pointer ABI。主機不直接截短 host pointer。
- 連線最多 64 image/region/symbol slots，單 frame/總 allocation 限 16 MiB；大型 payload 吞吐效能尚未量測。Kernel 以 executor 權限執行。

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
  -Iinclude -I$BR/usr/include runtime/executor.c -L$BR/usr/lib -Wl,-rpath,/usr/lib \
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
NDS_RUNTIME_LIBRARY=<near_data>/build/runtime/libnd_runtime.so
```

執行時 `ND_RUNTIME_DEVICE_PATH` 指向 runtime 通道：Guest 預設 `/dev/ttyS0`，由 QEMU pipe chardev 連接 WSL `/dev/nd0`。新版 host driver 將 runtime frame 分裝成 USB RPC commands，與原有 SD READ 共用 ND interface；同一時間只使用一個 client。

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

## USB runtime 執行入口

正式測試路徑為 Guest `/dev/ttyS0` → QEMU `-chardev pipe,id=nd,path=/dev/nd0 -serial chardev:nd` → WSL `nd_driver.ko` → USB bulk → Luckfox `f_nd` → `/dev/nd_rpc` → executor。UART 留作登入與部署。Guest 管理 console 使用第二個 serial port ttyS1。

板端將 `make runtime-usb-bundle` 產生的 ARM executor 安裝到 `/root/nd-runtime/nd-executor`，service 安裝到 `/etc/init.d/S99nd-runtime` 並設為可執行。service 使用 `--device /dev/nd_rpc`；HELLO/reset 或斷線會使舊 executor 結束，由 supervisor 重啟。kernel 必須包含新版 f_nd，WSL 亦需新版 nd_driver。

```sh
# WSL：QEMU 尚未使用 /dev/nd0 時
make runtime-usb-check
# Guest：賦予使用者 ttyS0 存取權後
make runtime-usb-check ND_USB_TRANSPORT=/dev/ttyS0
make nds-usb-check NDS_JOBS=6
```

USB adapter 保留 16-byte READ 協定。runtime RPC body 上限 16 MiB，以最多 4096 bytes 的 ND payload 分段，RPC_OUT 使用固定 4112-byte USB transfer。

## 舊 UART 測試入口（歷史 bring-up）

Guest SDK/ARM libffi 路徑見 Makefile 的 `NDS_DEVICE_CC`、`NDS_DEVICE_FFI_ROOT`，可覆寫。`platform/guest.local.ini` 指向 Guest SDK；FFI 必須使用 ARM 版，不能使用 x86_64 headers/library。

```sh
# Guest：建置 host runtime、NDS plugin，以及板端 executor
make nds-runtime NDS_JOBS=6
make runtime-arm
# 通道已由管理端啟動，且 dev 可存取此 virtio port 時：
make nds-check NDS_JOBS=6 ND_RUNTIME_DEVICE_PATH=/dev/virtio-ports/nds-runtime
```

管理端（目前為 WSL）準備好可用的 ARM executor 檔案後，以下各步使用同一個 `NDS_BRIDGE_SOCKET`：

```sh
make runtime-uart-start NDS_UART=/dev/ttyUSB0 NDS_EXECUTOR=/path/to/arm/nd-executor
# 此命令佔用前景執行；另開管理 terminal 執行下一步。
make runtime-uart-bridge NDS_UART=/dev/ttyUSB0
make runtime-guest-port NDS_QMP=/tmp/near-data-qmp.sock
```

Guest port 為 `/dev/virtio-ports/nds-runtime`；首次需由管理端賦予 dev 存取權。UART 只能有一個使用者，不可同時開 shell terminal 與 bridge。只上傳通用 executor；kernel image 每次由編譯後的執行檔交給 plugin 傳送。

舊 UART 路徑不會隨 USB 測試自動啟動。停止時應先用 runtime SHUTDOWN RPC 結束 executor，待 tty 還原後停止 bridge；重新開機或重設 UART 後需重新建立通道。

本輪證據保存在 Guest `artifacts/guest-link/make-nds-check.log`、`.exit` 及 `runtime-board2.log`；本機亦保存對應 log。

## USB 真機驗證（2026-10-20）

Guest `make nds-usb-check NDS_JOBS=6` 退出碼 0；MANDATORY offload 的 delta=7/19 皆 PASS，kernel 回報指標 32 bits，int32/float/double vector 元素逐項符合 host reference。Guest `make runtime-usb-check ND_USB_TRANSPORT=/dev/ttyS0` 退出碼 0，1/4/496/512/4096/8193 bytes 回傳及越界錯誤回報皆 PASS。WSL 直連 USB 測試與三組 legacy READ 比對亦 PASS。

新版 kernel 為 5.10.160 #6；boot 寫入後讀回逐位元組相符。測試紀錄保存在 `artifacts/usb-runtime/guest-make.log`、`guest-make.rc`、`guest-smoke.log`、`guest-smoke.rc`（本機與 Guest）。QEMU 未啟動 UART runtime bridge。此結果涵蓋目前支援的 mapped buffers，不代表深層 mapping 或全部 OpenMP 語意已實作。

## 公開裝置記憶體 API

`#include <nd_device.h>`（亦由 `nd.h` 引入）提供 `nd_device_open/close/error/bits`、`nd_alloc/free/copy`。這些 API 共用 nd_rt 實作；`nd_device` 是 `nd_runtime` 的別名。公開 API 與 OpenMP plugin 經 nd_rt_acquire/release 使用同一個 refcounted session，重複開啟同一路徑不會送第二次 HELLO。兩者必須連結同一份 shared libnd_runtime，不能讓 plugin 與應用程式各自靜態嵌入 runtime 副本。一次支援一個裝置路徑；不同路徑在 active session 期間回報 EBUSY。

```c
nd_device *device = nd_device_open("/dev/ttyS0");
nd_device_ptr address;
int32_t input = 42, output = 0;
/* Production callers must check each return value. */
nd_alloc(device, sizeof input, &address);
nd_copy(device, address, &input, sizeof input, ND_HOST_TO_DEVICE);
nd_copy(device, &output, address, sizeof output, ND_DEVICE_TO_HOST);
nd_free(device, address);
nd_device_close(device);
```

公開 device address 為不透明 void*（nd_device_ptr），僅支援 64-bit Guest，不可在 host 解參考；使用 uintptr_t 在 allocation 範圍內計算 byte offset。Wire 仍為固定 uint64_t，轉換僅針對 nd_copy direction 指定的 device operand。nd_copy 目前支援 ND_HOST_TO_DEVICE、ND_DEVICE_TO_HOST，其他方向回報 EINVAL。操作為同步，成功回傳 0，失敗 -1 並設定 errno。close 不得與操作並行，應先 free；reset 後 address 失效。`nd_read(file, offset, bytes, device_ptr)` 使用 public allocation registry 找到 session，將 FIEMAP 換算後的 SD byte offset 傳給 executor，直接 pread 到 device allocation；資料不經 Guest buffer。舊 host-buffer SD 探針改名 nd_read_host_legacy。一般 Guest 讀檔使用 POSIX read/pread。

```sh
make runtime-install PREFIX=/tmp/nd-sdk
cc examples/device_memory.c -I/tmp/nd-sdk/include \
  -L/tmp/nd-sdk/lib -lnd_runtime -pthread -Wl,-rpath,/tmp/nd-sdk/lib -o /tmp/device_memory
/tmp/device_memory /dev/ttyS0
# 或在 Guest checkout：
make device-api-check ND_USB_TRANSPORT=/dev/ttyS0
```

安裝提供 nd.h、nd_device.h、nd_runtime.h、libnd.a/.so 與 libnd_runtime.a/.so。libnd 靜態庫仍依賴共用的 shared runtime，OpenMP 整合必須使用同一份 libnd_runtime.so。

公開 API 驗證：Guest `make device-api-check ND_USB_TRANSPORT=/dev/ttyS0` 退出碼 0，靜態連結範例的 roundtrip 與越界錯誤恢復 PASS；既有 USB OpenMP delta=7 回歸 PASS。Native image/launch 測試、安裝後 C 靜態連結及 C++ header 編譯皆 PASS。

void* / nd_copy 更新驗證：Guest USB `make device-api-check ND_USB_TRANSPORT=/dev/ttyS0` 退出碼 0，雙向 copy、無效方向 EINVAL、越界 EFAULT 及錯誤後連線恢復均 PASS；C/C++ 嚴格編譯、安裝後靜態連結及 native runtime image/launch 回歸 PASS。公開 nd_submit/nd_retrieve 已由 nd_copy 取代，內部 nd_rt_submit/nd_rt_retrieve 保留。

## 直接讀檔後使用 is_device_ptr

`examples/omp_file_read.cpp` 展示 nd_open → nd_alloc → nd_read → target is_device_ptr → nd_copy → nd_free/nd_close。nd_read 回傳讀取 byte 數，EOF 回傳 0，錯誤 -1/errno；支援第一個 extent 範圍內的非 sector 對齊 offset，超出該 extent、洞或不安全 FIEMAP flags 回報 ENOTSUP，allocation 邊界錯誤回報 EFAULT。錯誤讀取可能已改寫部分 device buffer，呼叫端不可使用其內容當作完整結果。

```sh
# Guest，ND_RUNTIME_DEVICE_PATH 與 nd_device_open 使用同一路徑
make nds-file-check NDS_JOBS=6 NDS_INPUT_FILE=/usr/share/common-licenses/GPL-3
make runtime-file-check
```

檔案必須位於 Luckfox 匯出的同一張 SD，且執行時不得修改檔案或 extent。預設 partition start 仍為本卡已驗證的 4096 sectors（ND_PARTITION_START_SECTOR 可覆寫）；executor 預設 /dev/mmcblk1。先前 qcow2 overlay 新建／修改的檔案不在板端原始 SD；目前已改成直接 raw SD 模式，因此 Guest 新增檔案 flush/fsync 後亦可作輸入。切換與啟動方式見 [guest-direct-sd.md](guest-direct-sd.md)。測試先與 Guest host reference 比對，再計算。

Executor 僅接收實體 offset、長度與已登記的 device allocation，不接收 host 檔案內容。ND_STORAGE_PATH 可為 executor 指定 storage，native 整合測試使用 sparse image；板端 service 預設保持 /dev/mmcblk1。

2026-10-20 真機結果：Guest `make nds-file-check NDS_JOBS=6` 重測退出碼 0，offset=3 的 direct SD read 與 host reference 完全一致；同一 allocation 執行兩輪 is_device_ptr float kernel，每輪 128 個結果皆 PASS。首次測試因 SDK fcntl/timespec header 錯誤編譯失敗（exit 2），範例改用標準 stdio reference 後通過，失敗與成功日誌均保留。Native runtime-file-check 的位移、EOF、越界、stale pointer 與 session lifetime 亦 PASS。

回歸結果：公開 nd_copy 與既有 mapped-buffer OpenMP delta=7/19 均 PASS，Guest regression 退出碼 0；ldd 確認應用與 libomptarget 依賴同一路徑的 libnd_runtime.so。完整真機紀錄匯出至 artifacts/nd-file-offload/，包含首次失敗、修正後成功與回歸的 log/rc。
