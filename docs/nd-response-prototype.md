# 最小 ND 資料回傳原型（2026-10-20）

目標是先在 Luckfox RV1106 實機回傳 SD 資料。LEO-16 的完整協定仍未完成：目前單裝置、單 client、一次一個請求；request id、重連恢復、多 client 及版本協商留待後續。

## 線上格式

所有整數 little-endian。保留既有 16-byte request：u16 magic=0x4e44、u16 opcode=1、u32 length（bytes）、u64 sector（512-byte sectors）。目前接受 1..4096 bytes。

新增 16-byte response：u16 magic=0x4e44、u16 version=1、s32 status（0 或負 Linux errno）、u32 length、u32 reserved=0；後接 length bytes。錯誤時 length=0。IN transfer 使用 short packet/ZLP 結束；序列埠後端依固定 header 與 length framing。沒有 request id，因此同時僅能有一個未完成交易。

## 實作

- f_nd 新增 Bulk IN，OUT 完成後交給 workqueue，唯讀 kernel_read /dev/mmcblk1，再送 IN response。IN 完成後才重新排入下一個 OUT。reset 發生於 block read 時可能需重新枚舉；此原型尚未完整驗證 reset 矩陣。
- host driver 在完整命令後接收 response，提供 partial read 與 poll；回傳處理錯誤不再被 consumed byte count 掩蓋。disconnect 與資料操作共用 mutex 保護生命週期。
- libnd 依 header 接收資料、回填 buffer、限制 EOF，單次 wire chunk 最多 4 KiB。仍只支援第一 extent、讀取從檔案起點開始；跨 extent 回 ENOTSUP，LEO-18 另行實作。
- ND_TRANSPORT_PATH 預設 /dev/nd0；Guest 設 /dev/ttyS0，使用 raw 115200。ND_PARTITION_START_SECTOR 預設本卡已核對的 4096，可覆寫；不能將此預設用於任意磁碟。
- 同一時間停用其他 ND client。USB test 會暫時 detach interface 1 的 host driver，結束後自動 reattach；執行前先停止 QEMU。

## 本次實測

- Luckfox kernel 5.10 gadget object 與 SDK 完整 kernel/boot image 建置成功。
- 已備份板端 boot 分區（LBA 0x800，0x2000 sectors），備份前綴與 SDK 舊 image 完全相同。
- 新映像只寫 boot 分區，讀回逐 byte 比對成功；板端正常開機。
- libusb ND Bulk IN 讀 sector 0 的 512 bytes 與 UMS 一致。
- /dev/nd0 分段寫入命令（5+11 bytes）、每次 37-byte 部分讀取、poll drain：sector 0/512 bytes、sector 4096/4096 bytes、sector 4097/123 bytes 全部比對一致。
- libnd 經 /dev/nd0 讀原有 sample.bin（9 bytes）與 libnd.cpp（6258 bytes），含 count=0、1 byte、4 KiB、跨傳輸 chunk、EOF 與 buffer sentinel 檢查，全通過。
- Guest serial ttyS0 端到端測試已通過：sample.bin（9 bytes）與 libnd.cpp（6258 bytes），同樣驗證 count=0、1 byte、4 KiB、跨傳輸 chunk、EOF 與 sentinel。Guest ttyS1 另作管理 console；不混用 ND binary stream。
- LEO-17 的非零 offset API 仍未實作；不把此項全數標為 Done。

## 重跑

```sh
make check
sudo python3 tests/hardware/usb_read_smoke.py --disk /dev/sdd --sector 0 --length 512
# 檔案必須位於輸出的 SD filesystem；先確認實際 partition start。
sudo python3 tests/hardware/libnd_read_smoke.py --lib ./build/libnd.so --file /path/on/sd/sample.bin
# Guest: ND 在 ttyS0；管理 console 使用獨立 ttyS1。
sudo python3 /mnt/near_data/libnd_read_smoke.py --lib /mnt/near_data/libnd.so --file /home/dev/nd_test/sample.bin --transport /dev/ttyS0
```

Guest 此輪使用 snapshot=on，僅測試 SD 原有、未修改的 fixtures。任何新建/修改檔案若落在 overlay，其 FIEMAP 不可當作原始 SD 資料映射。共享目錄只有測試腳本與 libnd.so，且為唯讀。

## 燒錄產物（不提交 binary）

- artifacts/nd-response-boot.img
- artifacts/board-boot-before-response.img：實機完整 boot 分區備份
- artifacts/board-boot-after-response.img：更新後讀回
- artifacts/nd-response-kernel-build.log、nd-response-flash.log、nd-response-readback.log

SDK 另保留 boot.img.before-nd-response 與 gadget 原始碼 .before-response 備份。需還原時使用已核對的同一 boot 分區，不整碟重刷。