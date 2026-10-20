# Guest SDK libraries 與 compiler wrapper

這個 installer 安裝已建置的 Guest libraries/headers 與 nds-clang++。不改 Clang driver、不修改 platform，也不自動選擇 OpenMP、target、CPU 或 addrsig。

## 安裝

在完整 checkout 建置所需元件後安裝：

```sh
make nds-runtime NDS_JOBS=6
sudo bash sdk/install.sh
# 可選：--prefix /some/prefix --build /path/to/build
```

預設安裝到 /usr/local/lib、/usr/local/include、/usr/local/bin，執行 ldconfig。安裝前會保存既有 SDK 同名檔案到 /usr/local/share/nds-sdk/backups/。OpenMP/offload 使用 CMake install，移除 build runtime 路徑；libomptarget 保留 $ORIGIN install RUNPATH。

這版不複製整套 Clang binaries：wrapper.json 記錄既有 build/llvm/bin/clang++ 路徑，該 compiler 與其工具/資源目錄必須保留。完整可搬移 compiler SDK packaging 尚未實作。

## 使用

```sh
nds-clang++ -fopenmp \
  -fopenmp-targets=arm-nds-linux-gnueabihf \
  -nds-platform=/home/dev/near_data_sdk/platform/guest.local.ini \
  -fno-addrsig app.cpp -o app

OMP_TARGET_OFFLOAD=MANDATORY ND_RUNTIME_DEVICE_PATH=/dev/ttyS0 ./app
```

不需要手動 -I、-L、-lnd、-lnd_runtime 或 rpath。使用者仍需提供相容 device cross toolchain/platform 與可用 runtime transport。-fno-addrsig 是 Luckfox 舊 assembler 的相容選項，wrapper 不會自動添加。-mcpu 不會自動添加，近期測試已驗證預設 ARM CPU 能執行目前的範例。

```sh
nds-clang++ --show ...  # 只顯示實際命令
nds-clang++ -c ...     # 不加入 link libraries
nds-clang++ -E ...     # 不加入 link libraries
```

Wrapper 使用 argv 原樣轉交，不經 shell eval；支援空白路徑與 response-file 階段判斷。-nostdlib/-nodefaultlibs 不會自動加入 SDK link libraries。目前不支援 -- separator，可用 ./ 開頭表示以 dash 起始的檔名。

## 本次 Guest 部署

原 /home/dev/near_data_m4 工作目錄保留。自動審核拒絕 hard reset 後，另建立 /home/dev/near_data_sdk 乾淨 worktree；ND libraries 在此重建，LLVM/OpenMP/offload 的 build 路徑以 symlink 重用已驗證產物。只複製 ignored 的本機 platform 設定，沒有直接複製或覆蓋 tracked source。

完整 file offload 測試使用安裝後 wrapper，使用者命令沒有 include/library/rpath 額外參數；編譯與執行 exit 0，SD 讀取與兩輪各 128 個 float 結果皆 PASS。ldd 顯示 libnd、libnd_runtime、libomp、libomptarget 均從 /usr/local/lib 載入。

Wrapper 單元測試：python3 tests/sdk/wrapper.py。
