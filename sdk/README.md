# Guest SDK libraries 與 compiler wrapper

這個 installer 安裝已建置的 Guest libraries/headers 與 nds-clang++。不改 Clang driver、不修改 platform，也不自動選擇 OpenMP、target、CPU 或 addrsig。

## 使用預建 SDK（不編譯 LLVM）

Guest ~/nds-sdk 已包含 lib/、include/ 安裝來源，以及根目錄 install.sh/install.py：

```sh
cd ~/nds-sdk
sudo ./install.sh                       # /usr/local
./install.sh --prefix "$HOME/.local"    # 不需 root；需將 ~/.local/bin 放入 PATH
```

腳本僅使用這套 SDK 內的成品，不呼叫 CMake，不下載或重建 LLVM，不載入 driver，也不要求接上 Luckfox。

安裝前檢查 Linux/amd64、必要檔案、ELF 架構、offload 工具及 compiler resource headers。將 SDK lib/include 複製到 prefix；覆寫同名檔案前保存到 prefix/share/nds-sdk/backups/<時間>/，另寫 install-manifest.json。預設 /usr/local 執行 ldconfig，最後使用 wrapper 編譯／執行最小 host 程式，驗證 headers、link 與 loader。自訂 prefix 使用自己的 wrapper/config，不更動既有系統 wrapper。

/usr/local 的 wrapper 入口連到 SDK bin；compiler/platform 仍引用 SDK 位置，安裝後請保留 SDK，不宣稱可任意搬移。SDK 原本的 libraries/headers 來源不會被改寫。package-files.json 記錄來源檔案雜湊。

來源 repository 對應 sdk/package-install.sh、sdk/package-install.py；打包時分別放到 SDK 根目錄 install.sh、install.py。

## 從原始碼開發者安裝（既有 build）

在完整 checkout 建置所需元件後安裝：

```sh
make nds-runtime NDS_JOBS=6
sudo bash sdk/install.sh
# 可選：--prefix /some/prefix --build /path/to/build
```

預設安裝到 /usr/local/lib、/usr/local/include、/usr/local/bin，執行 ldconfig。安裝前會保存既有 SDK 同名檔案到 /usr/local/share/nds-sdk/backups/。OpenMP/offload 使用 CMake install，移除 build runtime 路徑；libomptarget 保留 $ORIGIN install RUNPATH。

預設 wrapper.json 記錄既有 build/llvm/bin/clang++ 路徑。若要移除建置目錄，先把 clang-21、clang-linker-wrapper、clang-offload-packager 與 lib/clang resource 目錄安裝到獨立 SDK，並建立 clang/clang++ 指向 clang-21 的連結；再指定：

```sh
sudo bash sdk/install.sh \
  --compiler "$HOME/nds-sdk/llvm/bin/clang++" \
  --wrapper-home "$HOME/nds-sdk"
```

此時 wrapper 在 ~/nds-sdk/bin，/usr/local/bin/nds-clang++ 是其 symlink；libraries/headers 仍放 prefix。這是獨立於 build tree 的工具佈局，但 toolchain/platform/config 含安裝路徑，並不宣稱可任意搬移。

## 使用

```sh
nds-clang++ -fopenmp \
  -fopenmp-targets=arm-nds-linux-gnueabihf \
  -nds-platform=$HOME/nds-sdk/platform/luckfox.ini \
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

## 目前 Guest 佈局

```text
/home/dev/nds-sdk/
  install.sh / install.py    預建成品安裝入口
  include/                  headers 安裝來源
  lib/                      ND/OpenMP libraries 安裝來源
  llvm/bin/                 Clang、linker-wrapper、offload-packager
  llvm/lib/clang/21/        compiler resource headers
  arm-rockchip830-linux-uclibcgnueabihf/   ARM cross toolchain/sysroot
  arm-libffi/               device executor 建置所需的補充 library
  bin/nds-clang++           wrapper
  share/nds-sdk/wrapper.json
  platform/luckfox.ini
  examples/                範例與 input.bin
  VERSION.json
  README.txt
```

原 mini_test、mini_test.c、nd-data、nd_test、near_data_m4、near_data_sdk 已在備份後移除。原始碼/設定/測試資料/Git bundle 備份位於 WSL /sdk/nds-guest/cleanup-backup-20261021/home-source-configs.tar.gz；SHA256 a949b82cbf4f90cf8979fe6228bd40b669b2b25aec5b94d0c0b077285bea3f3e。備份不含可重建的舊 build 與完整 LLVM checkout；正式原始碼及 Guest 比較快照已在 GitHub。

nd_driver 留在 WSL USB host，Guest 使用 ttyS0，不在 Guest 安裝該 kernel module。Libraries/headers 在 /usr/local，與上述 SDK 協同使用。

清理前先移開舊路徑測試，清理後再次重新編譯並 USB mandatory offload：direct SD read 及兩輪各 128 個 float 結果 PASS；map delta=7/19 PASS。最後 /home/dev 非隱藏項目只剩 nds-sdk，Clang InstalledDir 與 resource dir 均在其 llvm 目錄。

Wrapper 單元測試：python3 tests/sdk/wrapper.py。

## 預建 installer 實測

- 缺件／錯誤 ARM host ELF 套件：安裝前拒絕，沒有建立目標目錄。
- 含空白的自訂 prefix、非 root、連續安裝兩次：exit 0；headers/link/loader smoke PASS，覆寫備份存在。
- /usr/local 重裝：exit 0，ldconfig 與 host smoke PASS。
- 重裝後完整 USB nd_read → is_device_ptr → nd_copy：編譯及執行 exit 0，SD read 與兩輪 float kernel 結果皆 PASS。

單元測試：python3 tests/sdk/package_installer.py。安裝日誌與真機證據在 artifacts/sdk-package/。
