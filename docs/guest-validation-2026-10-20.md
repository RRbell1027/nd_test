# Ubuntu Guest 驗證 — 2026-10-20

本文保留當時的歷史測試。後續已將 overlay 合併回實體 SD，現行啟動改用 raw 可寫 SD，見 [直接 SD 模式](guest-direct-sd.md)。

最終 compiler/application host 為 Ubuntu Guest；WSL 只提供 QEMU、USBIP 與 SDK 傳入。此次沒有用 Windows Clang 的結果替代 Guest 驗證。

## GitHub 與環境

- 主專案與 LLVM 子模組均已 push 到 `codex/nds-arm32-runtime` 分支。
- Guest 從 GitHub clone 主專案並取得記錄的子模組 commit `fca4a674090701735f150da513cf5ff3205145d3`。
- Ubuntu 22.04.5、x86_64、kernel 5.15.0-194-generic；10 vCPU、12 GiB RAM。
- Guest checkout：`/home/dev/near_data_m4`。
- ARM SDK：`/home/dev/nds-sdk/arm-rockchip830-linux-uclibcgnueabihf`。
- `platform/guest.local.ini` 使用該 Guest 路徑，不依賴 WSL 的 `/sdk`。
- Guest 使用獨立 qcow2 overlay `/sdk/nds-guest/m4-build.qcow2`；建置不寫回原始 SD。下次啟動前仍需核對 backing device 是同一張 SD。

## 實際結果

| 項目 | 結果 |
|---|---|
| Guest 從 GitHub clone 與取得 LLVM 子模組 | PASS |
| Guest native runtime-check | PASS，兩份 image 回傳 27、39，錯誤處理測試通過 |
| Guest SDK assembler 啟動 | PASS，exit 0 |
| Guest SDK g++ 生成普通 ARM32 shared ELF | PASS，exit 0；不是 OpenMP image |
| Guest 原生 Clang、offload-packager、linker-wrapper 建置 | PASS，最終 exit 0 |
| 含 omp target 的 device-only object | PASS，exit 0，ARM ELF32 ET_REL |
| 含 omp target 的 host/device object | PASS，exit 0，x86_64 ELF64 ET_REL 且包含 .llvm.offloading |
| device-only 加 -shared 產生可載入 image | FAIL，command exit 0 但 -shared 被忽略，仍是 ARM ET_REL；產物驗證 exit 1 |
| 完整 OpenMP 執行檔連結 | FAIL，exit 1：linker wrapper 找不到 -lomp |
| Guest 編譯的 OpenMP 執行檔在 Luckfox offload | NOT TESTED，尚未產生完整執行檔 |

原生 Clang 初次以 `-O0 -DNDEBUG` 建置，最後連結失敗於未使用的 debug helper 引用 `MachineInstr::dump()`。加入 executable linker `--gc-sections` 移除未使用 sections 後，整組工具建置成功。這個設定已記入 `tests/guest/README.md`。

## 證據與後續

本機與 Guest 均保存 `artifacts/guest-compile/` 的 manifest、各編譯 log 與 runtime-check log。本機另保存完整連結錯誤與工具建置結束碼。這些測試產物不加入 Git。

已確認 nds-platform 所指定的 Linux as/g++ 在 Guest 可執行；先前 Windows 啟動 Linux ELF 的問題不再是 Guest 的阻塞。後續需補齊 Guest host OpenMP runtime，再接通 device shared-image 連結。原始碼檢查另顯示 linker wrapper 的 linkDevice 架構清單沒有 ARM32；完整連結目前先被缺少 -lomp 擋住，尚未實測到該階段。

目前通過的 object 編譯不能當成完整參數 ABI、image 載入或板端執行已通過。

## 補齊 OpenMP runtime 後重試

- 同版 `llvm-project/openmp` standalone build 通過，exit 0，產出 `build/openmp/runtime/src/libomp.so`。
- 同版 `llvm-project/offload` 以 Guest clang++ 與 NDS plugin 建置通過，exit 0，產出 `build/offload-native/libomptarget.so`。先前 clang C++ 連結模式設定錯誤已修正。
- 完整命令加入兩個 build library 目錄的 `-L`／rpath 後，缺少 `-lomp`、`-lomptarget` 的問題解除。
- 完整連結仍 FAIL，exit 1；此次實際錯誤為 `clang-linker-wrapper: error: arm linking is not supported`。這是 wrapper 的 ARM32 device-link 支援缺口，尚未修正。
- 尚未產出或執行完整 OpenMP 執行檔。此輪不處理 device-only 加 -shared 的替代用法。
- 證據：`artifacts/guest-compile/full-link-with-runtimes.log` 與 `.exit`，以及同目錄的 runtime build logs。

## NDS linker wrapper 接通後

- LLVM commit `1dbd14f88`：wrapper 依 NDS vendor 進入 device linking、傳遞 nds-platform；普通 clang --target=NDS 呼叫也選擇 NDSToolChain。NDS image 的 libraries 來自平台設定，不混入 host libomp/libomptarget。
- Guest Clang 與 linker wrapper 增量重建 PASS，exit 0。
- 原本的最小 omp target 程式完整編譯／連結 PASS，exit 0。verbose log 確認實際呼叫 Guest SDK g++，經 NDS linker 產生 shared image。
- `tests/guest/validate_link.py` 在 Guest 實測 PASS，exit 0：host 為 x86_64 executable；保留的 device image 為 ARM32 ELF ET_DYN；完整 image bytes 確實嵌入 host executable。
- `OMP_TARGET_OFFLOAD=DISABLED` 的 host fallback 執行 PASS，exit 0。這不是 Luckfox offload 執行。
- Guest 證據：`artifacts/guest-link/manifest.json`、`link.log`、`host-fallback.log` 與保留的 ARM image；本機保存 manifest 與 log 摘錄。
- 此結果只涵蓋最小 mapped int 的編譯／連結，不代表 scalar/vector 等跨位寬參數 ABI 或實機 kernel launch 已驗證。

## 真正 Luckfox runtime 執行通過

- `make nds-compile` 在隔離 host/device C++ headers 後 PASS，exit 0。
- 實機首輪 mandatory 執行 FAIL，exit 134；共用 ELF 檢查只支援 ELF64。加入 ELF32 image/symbol 支援後 PASS，exit 0。
- Guest→virtio→WSL byte bridge→Luckfox UART，kernel image 實際上傳並在 ARM32 executor 執行。
- 最終 `make nds-check NDS_JOBS=6` PASS，exit 0。delta=7、19 均確認 mapped scalar、int32/float/double vector 元素與 reference 一致，且 kernel 回傳指標寬度32；設定 MANDATORY，沒有 fallback。
- `make runtime-arm` 亦 PASS，exit 0，產出 ARM32/uClibc executor。
- 新 Makefile 入口包含 llvm-guest、openmp-runtime、offload-runtime、nds-runtime、nds-compile、nds-check、runtime-arm、runtime-uart-start、runtime-uart-bridge、runtime-guest-port。
- USB runtime adapter 尚未實作；此結果不能當作 USB ND transport 通過。完整啟動與型別限制見 docs/nds-runtime.md。
