# ND 實機基線（2026-10-20）

本次依 Linear LEO-15 重新核對。開發優先順序為先在 Luckfox 跑通最小資料讀取，再逐步完善協定、多 extent、完整防護與效能測試。

## 原始碼

- Repository: https://github.com/RRbell1027/nd_test.git
- Branch: main
- Baseline: ad0406bbb33120c32765cfc93045c406b2a1cc88
- LLVM submodule: 4cfb09c2b5359c8aa4b5bdba38f9595d7bb22438
- 另有 /sdk/nd_test 舊工作副本，不能當作目前 workspace 的自動同步副本。

## 本次實測

- luckfox-sdk WSL2 kernel: 5.15.167.4-microsoft-standard-WSL2。
- Luckfox kernel: 5.10.160 #2 Thu Oct 1 11:20:51 CST 2026 armv7l。
- USB 2207:0000，產品字串 Luckfox Near-Data Storage；interface 0 為 UMS，interface 1 為 ND Bulk OUT 0x02。
- usbipd 掛接 Luckfox 與 PL2303 成功。BUSID 會變，操作前先執行 usbipd list。
- PL2303 在 WSL 為 /dev/ttyUSB0，115200 baud；UART shell 登入成功。憑證不記錄於 repository。
- ND 模組載入後 /dev/nd0 存在。送出 READ sector=0 length=16，UART dmesg 實際出現對應命令。
- UMS 約 59.5 GiB；本次辨識為 /dev/sdd，GPT partition 2 起點為 4096 sectors。裝置名稱與 offset 都必須每次核對。
- 板端 /proc/mounts 沒有 SD filesystem 掛載。
- make check PASS：CLI/shared library/FIEMAP；此測試不代表資料回傳成功。
- QEMU 6.2、KVM 啟動成功，Ubuntu 22.04.5 LTS 已到 host-workspace tty1 登入畫面。此輪使用 snapshot=on，只驗證開機與 chardev 可開啟，尚未做 Guest 端 ND 測試。

## 目前資料路徑

Host command 與 USB command 都是 16 bytes：little-endian u16 magic=0x4e44、u16 opcode=1、u32 byte length、u64 sector（目前主機為 little-endian x86_64）。

目前 f_nd 的 READ 分支只記錄請求，沒有讀 SD，且只有 Bulk OUT；host driver 的 read 回 EAGAIN、poll 不回報可讀；libnd 不回填 buffer。repo、/sdk/nd_test 及 SDK kernel source 都有此限制。歷史「板端讀到檔案」不能當作目前 binary 已具備完整回傳功能。

Guest 原型歷史上透過 /dev/ttyS0，現在 libnd 固定開 /dev/nd0，尚未內建 serial transport 選項。request-response、buffer 回填及 Guest 測試仍是 LEO-16/17 工作。

## QEMU 基線命令

```sh
# 先核對磁碟，保持 WSL 運行並完成 USB 掛接、模組載入。
qemu-system-x86_64 -enable-kvm -cpu host -m 2G \
  -drive file=/dev/sdd,format=raw,if=virtio,snapshot=on \
  -display none \
  -monitor unix:/tmp/near-data-monitor.sock,server,nowait \
  -qmp unix:/tmp/near-data-qmp.sock,server,nowait \
  -chardev pipe,id=nd,path=/dev/nd0 -serial chardev:nd \
  -daemonize -pidfile /tmp/near-data-qemu.pid
```

snapshot 的新寫入位於暫存層，不能用來驗證 Guest FIEMAP 對應實體 SD。正式實體資料測試需正常關閉此 VM，核對沒有其他 filesystem 寫入者，再使用直接磁碟路徑。

## WSL BTF 問題與載入

既有模組 vermagic 雖相同，BTF 與目前 kernel 不相容，insmod 回 Invalid parameters；dmesg 為 failed to validate module [nd_driver] BTF: -22。

```sh
cp -n build/nd_driver.ko build/nd_driver.ko.with-btf.bak
objcopy --remove-section=.BTF --remove-section=.BTF.ext build/nd_driver.ko
insmod build/nd_driver.ko
```

本次另將修正副本保存為 /sdk/wsl-modules-backup/nd_driver.ko，systemd nd-driver.service 從該持久路徑載入。先前直接安裝到 /lib/modules 的副本在 WSL 重啟後未保留，因此不把該路徑當作唯一來源。重啟全流程驗證仍待完成；USB/IP 每次 WSL 停止後也需重新掛接。