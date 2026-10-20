# Near-Data prototype (nd_test)

Luckfox RV1106 的 Near-Data 原型：主機用 FIEMAP 取得檔案 extent，將讀取請求
經 `/dev/nd0` 與 USB ND function 傳到裝置。SD 同時由另一個 USB Mass Storage
interface 提供存取。LLVM/OpenMP offloading 工具鏈放在獨立的子模組。

## 目錄

| 路徑 | 用途 |
|---|---|
| `include/` | libnd API、Host command 與 USB wire protocol |
| `src/` | 使用者空間 libnd 實作 |
| `drivers/host/` | Linux Host USB driver，提供 `/dev/nd0` |
| `drivers/gadget/` | Luckfox kernel 的 `f_nd` USB gadget function |
| `examples/` | libnd 請求範例、最小 OpenMP target 草稿 |
| `etc/init.d/` | Luckfox 的 UMS + ND configfs 啟動腳本 |
| `platform/` | NDS 平台組態；`examples/` 下是使用假路徑的格式範例 |
| `tests/luckfox-sdk/` | SDK、GCC、ABI 與 NDS driver 驗證腳本及報告 |
| `docs/` | 架構與 kernel 整合說明 |
| `llvm-project/` | `llvm-project-nds` 子模組，追蹤 `nds` 分支 |
| `build/`、`artifacts/` | 本機建置與測試產物，不提交 |

## 取得原始碼

```sh
git clone https://github.com/RRbell1027/nd_test.git
cd nd_test
# 僅在需要修改或建置自訂 Clang 時下載大型 LLVM 子模組。
git submodule update --init llvm-project
```

子模組固定於已提交的版本；`.gitmodules` 的 `branch = nds` 不會讓一般
`submodule update --init` 自動切到最新遠端提交。

## Linux / WSL 建置

需要 Linux C++17 編譯器與 make；Host driver 另需對應 kernel 的 headers、
設定與已準備好的 module build tree。

```sh
make userspace        # build/libnd.so、build/test_nd
make check            # CLI / shared library / FIEMAP smoke test，不操作硬體
make print-config
make driver           # build/nd_driver.ko
make                  # userspace + driver
```

在 Windows PowerShell 使用既有 WSL 環境：

```powershell
wsl -d luckfox-sdk -- make -C /mnt/d/develops/near_data userspace
wsl -d luckfox-sdk -- make -C /mnt/d/develops/near_data driver KDIR=/sdk/WSL2-Linux-Kernel
```

Makefile 在 WSL 預設使用 `/sdk/WSL2-Linux-Kernel`，一般 Linux 則使用
`/lib/modules/$(uname -r)/build`。可透過 `KDIR=...` 覆寫；產物只有在 kernel
版本與設定相符時才適合載入。

連接並準備好 ND 裝置後，範例使用方式為：

```sh
./build/test_nd /path/to/file-on-exported-sd
```

目前 `nd_read(file, offset, bytes, device_ptr)` 直接將 SD 資料讀入 Luckfox allocation，搭配 OpenMP `is_device_ptr` 與 `nd_copy` 取回結果。舊 host-buffer 探針改名 `nd_read_host_legacy`，會限制 EOF。
單次 wire transfer 最多 4 KiB，較大的單 extent 檔案會分多次請求；跨 extent
尚未支援。預設 partition start 為本卡已核對的 4096 sectors，可用
`ND_PARTITION_START_SECTOR` 覆寫。Guest 使用 `ND_TRANSPORT_PATH=/dev/ttyS0`。

2026-10-20 已驗證主機與 QEMU Guest 的實際資料比對；範圍、重跑方式與限制見
[最小回傳原型](docs/nd-response-prototype.md)。完整協定與多 extent 尚未完成；OpenMP USB 原型已完成真機驗證。

`make clean` 移除本專案的 `build/`；SDK 測試紀錄保留在 `artifacts/`。

## Luckfox SDK / NDS 平台組態

已驗證 GCC/G++ 8.3.0、ARMv7 hard-float、uClibc。實際安裝與 `make sdk`
匯出的限制見 [SDK 測試報告](tests/luckfox-sdk/README.md)。

`platform/luckfox-rv1106.ini` 對應 `luckfox-sdk` WSL 的
`/sdk/arm-rockchip830-linux-uclibcgnueabihf`。其他環境可複製為
`platform/<name>.local.ini` 並調整路徑；這類本機組態已列入忽略規則。

```sh
make sdk-check
```

目前測試腳本需要 `luckfox-sdk` WSL、已匯出的 SDK，以及子模組中既有的
Windows `build/bin/clang.exe`。它會建立組態並保存測試紀錄；成功退出代表 SDK
檢查完成，OpenMP frontend 的失敗仍會明確寫入 manifest。

NDS toolchain 透過 OpenMP offload 路徑選用；組態提供實體 GCC triple，
offload target 使用 `nds` vendor。CPU/FPU 編譯參數仍須另外指定。

## 裝置端與通訊

[架構與整合說明](docs/architecture.md) 記錄 Host driver、Luckfox gadget、
UMS 啟動腳本的使用範圍與原型限制。

## NDS runtime bring-up

新版 NDS runtime 直接上傳 ELF image bytes，再由 Luckfox executor 載入執行，不依賴板端預存 kernel 路徑。client/executor 與 Guest OpenMP integration 已通過 USB ND 實機測試。建置與範圍見 [NDS runtime](docs/nds-runtime.md)。

### Guest OpenMP runtime on Luckfox

`make nds-runtime` builds the native Guest LLVM/OpenMP/NDS runtime;
`make runtime-arm` cross-builds the Luckfox executor. With the runtime channel
connected, `make nds-check` compiles and executes mandatory ARM32 offload tests
for mapped scalars and int32/float/double vector element buffers. Both test
inputs passed on Luckfox over USB. The runtime channel is Guest `/dev/ttyS0`
through QEMU and WSL `/dev/nd0`, USB `f_nd`, and board `/dev/nd_rpc`.
Use `make nds-usb-check`; UART remains available for board management. See
[docs/nds-runtime.md](docs/nds-runtime.md) for setup and supported types.

Guest SD 直接運算範例：`make nds-file-check NDS_JOBS=6`，見 [examples/omp_file_read.cpp](examples/omp_file_read.cpp)。公開 libnd 與 OpenMP plugin 共用 `libnd_runtime.so` session。Guest 已改為直接使用實體 SD，新增檔案同步後也可供 Luckfox 讀取；啟動方式見 [直接 SD 模式](docs/guest-direct-sd.md)。

Guest compiler wrapper 與 libraries/headers 安裝見 [SDK 使用方式](sdk/README.md)。使用者保留 OpenMP/target/platform/addrsig 選項，wrapper 補上 SDK include 與 host link 設定。

目前 SD Ubuntu 已整理為 `/home/dev/nds-sdk/` 獨立工具目錄（LLVM、ARM 工具鏈、wrapper、platform 與範例）；不再依賴 Guest 內的主專案 checkout。Host libraries/headers 在 `/usr/local`，USB nd_driver 留在 WSL。部署與備份細節見 [SDK 使用方式](sdk/README.md)。
