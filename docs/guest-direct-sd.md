# Ubuntu Guest 直接使用實體 SD

目標配置是 QEMU raw disk → Luckfox UMS → 實體 SD，不經 qcow2 overlay。
Guest 的系統更新、Git checkout、LLVM 建置與新檔案都會寫到卡上。

## 啟動與日常使用

WSL luckfox-sdk 運行、Luckfox USB 已透過 usbipd attach，且 nd_driver 已建立 /dev/nd0 後：

```sh
cd /mnt/d/develops/near_data
make guest-direct-start ND_GUEST_DISK=/dev/sdd
```

實際裝置名稱可能改變。啟動工具核對卡片容量與 GPT 指紋、WSL 沒有掛載其分區、既有 Guest 沒有運行；首次遷移未完成時也會拒絕啟動。Guest 磁碟為 `file=/dev/sdd,format=raw,if=virtio,cache=none`，沒有 snapshot 或 backing overlay。

Guest 一般存檔仍遵循 Linux page cache 語意，建立 ND 輸入檔後應 flush/fsync，再 nd_open/nd_read；FIEMAP 會使用 FIEMAP_FLAG_SYNC。運算期間不要修改該檔案。現在新建檔案也能由 Luckfox 直接從 SD 讀取。

Guest 的 /dev/ttyS0 經 QEMU pipe 連 WSL /dev/nd0；管理 console 是 /dev/ttyS1。UART 留給 Luckfox 管理，不承載 runtime 資料。

Luckfox 不掛載 SD 檔案系統，僅透過 UMS 匯出並由 executor 讀原始區塊。WSL 也不要同時掛載 SD 分區。Guest 關機使用 `sudo systemctl poweroff`，確認 QEMU 退出後再拔除 SD/USB。

## 首次遷移與回復材料

原工作環境位於 `/sdk/nds-guest/m4-build.qcow2`。首次遷移先正常關機、qemu-img check，備份所有即將覆寫的 SD ranges，再離線 commit，最後逐 byte 讀回所有變更區塊。原 overlay 使用 commit -d 保留，遷移後不再用它開機。

WSL `/sdk/nds-guest/direct-sd-migration-20261020/` 保存：

- `original-ranges.bin`：依 map 順序串接的原始 SD 區塊。
- `map.json`：各區塊在 SD 的 offset/length 與原始 SHA256；備份檔 offset 為先前各段 length 的累計。
- `backup-complete`、`commit.log`、`verified`：階段證據。

這是特定卡片的遷移回復材料，不是可獨立開機的完整磁碟備份。原 overlay 保留，但其 backing SD 已更新，不應再使用舊啟動命令。若需回到遷移前，必須先停止所有 SD 使用者並依 map 還原原始區塊；此操作會丟棄遷移後的 SD 修改，應另外備份。

## 實測結果（遷移開始 2026-10-20，完成 2026-10-21）

- 原 overlay qemu-img check PASS；備份原始 SD 待覆寫 ranges 共 14,529,200,128 bytes。
- 離線 qemu-img commit -d 完成，flush 後全部變更區塊逐 byte 讀回相符。
- e2fsck -fn exit 0；Ubuntu 5.15.0-198-generic 直接 SD 開機 PASS，LLVM/SDK/專案保留。
- Guest 新建 /home/dev/nd-data/input.bin，flush/fsync；make nds-file-check 對此新檔重測 exit 0。SD direct read 與 Guest reference 一致，兩輪 is_device_ptr float kernel 各 128 個結果皆 PASS。
- 初次新檔測試 FAIL 是範例越界測試被 EOF 截短，並非讀不到檔案；改用 allocation 內偏移一 byte 的 destination 測試界限後 PASS。初次與重測 log/rc 均保留。
- 正常關閉 Guest 後，以 debugfs 唯讀從 /dev/sdd2 匯出新檔，SHA256 f3478f375703e0512f7096bdd2515eb02e42be09ade9d5633ecebbdc19f4a238 與原內容一致。板端沒有 mmcblk1p2 節點，未用板端 mount 作為成功證據。
- Guest 已重新以 raw SD 模式啟動。證據在 artifacts/direct-sd/manifest.json、direct-sd-newfile-retry.log/.rc。
