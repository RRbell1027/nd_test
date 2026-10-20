# ND 原型架構與 kernel 整合

## 資料路徑

目前 repository 的 libnd 路徑：

```text
test_nd → libnd / FIEMAP → write(/dev/nd0)
        → Host nd_driver → USB Bulk OUT → Luckfox f_nd worker → SD read
        ← buffer ← /dev/nd0 read/poll ← USB Bulk IN response ←
```

Host character-device command 在 `include/nd_driver.h`，USB wire command 在
`include/nd_protocol.h`。Host driver 將前者轉成 little-endian USB 命令。
`/dev/nd0` 是 byte stream，driver 累積完整命令後才傳送，適應分段 write。
目前 driver 的 read/poll 已提供 response header 與 payload；一次僅支援一個交易。

歷史 QEMU 實驗另曾使用 Guest `/dev/ttyS0` → QEMU serial chardev → Host
`/dev/nd0`。目前 `src/libnd.cpp` 預設開啟 `/dev/nd0`，可用 `ND_TRANSPORT_PATH=/dev/ttyS0`
選用 raw serial transport。2026-10-20 已重新驗證 Guest buffer 回填。

## Host driver

```sh
make driver KDIR=/path/to/prepared/kernel
```

Makefile 將 Host source 與兩個 protocol headers 複製到 `build/`，再用 kernel
module build system 產生 `build/nd_driver.ko`。這個目標不會載入 module。
載入前須確認 kernel version、config、Module.symvers 與 vermagic 相符。

## Luckfox gadget

`drivers/gadget/f_nd.c` 是 Linux kernel USB gadget function 原始碼，使用
kernel tree 內部的 `u_f.h`。它不是使用者空間程式，也沒有被根目錄 Makefile
的 Host module 目標建置。

整合至 Luckfox SDK 時：

1. 將 `drivers/gadget/f_nd.c` 與 `include/nd_protocol.h` 放入對應 kernel 的
   `drivers/usb/gadget/function/`。
2. 核對該 SDK 的 Kconfig/Makefile 已登錄 ND function 與 configfs 選項。
3. 依 Luckfox SDK 流程建置 kernel / boot image，確認實際使用的是新映像。

歷史配置使用 `CONFIG_USB_F_ND` 與 `CONFIG_USB_CONFIGFS_ND`；這個 repository
目前沒有完整的 kernel patch 或自動部署目標。

## UMS + ND 啟動腳本

`etc/init.d/S50usbdevice` 是裝置端 configfs 腳本；使用 `/dev/mmcblk1` 作為
整張 SD 卡的 UMS backing device，並建立 ND function。它不是 WSL 啟動腳本。

部署前先核對裝置名稱、partition 與 kernel function；啟動腳本會卸載 SD
partition、設定 gadget 並綁定 UDC。UMS export 期間，裝置端不得同時 mount
並寫入相同 filesystem。本次 repository 整理沒有執行部署、綁定或燒錄。

## 原型限制

- `nd_read` 只使用第一個 extent，固定 partition 起點為 sector 4096。
- 成功回傳實際回填 buffer 的 bytes，count 超過檔案大小時短讀。
- libnd 不適用尚未處理的 LVM/device-mapper、fragmented file 等一般化映射。
- 最小 device response 與 Guest buffer 回填已驗證；多 extent、offset API、完整邊界與一致性防護仍待完成。
- OpenMP runtime / executor 尚未完成。工具鏈可編譯 ARM ELF，不等於 offload
  runtime image 或板卡執行已通過。

## 2026-10-20 實機重驗

目前硬體、WSL BTF 修復、QEMU 命令與已驗證範圍見 [實機基線](hardware-baseline-2026-10-20.md)。基線核對後已加入並驗證最小 SD 讀取與 response，見 [回傳原型](nd-response-prototype.md)。
