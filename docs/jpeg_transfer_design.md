# JPEG 串口传图 + ESP-NOW 投屏方案

## 设计目标

宿主机发送 JPEG 压缩图片 → 基站串口完整写入内存 → 本地解码显示 → ESP-NOW 分片转发 → 接收端解码显示

**核心思路：** 串口只负责 JPEG 数据传到内存。收齐后基站解码本地显示，同时 ESP-NOW 转发原始 JPEG 到接收机。

---

## 架构

```
宿主机                               基站
  │                                    │
  │  ── 串口接收 ──                    │
  ├─ CMD_JPG_START ──────────────────> │ malloc(totalSize)
  │   payload: [totalSize(2B)]        │ 校验: 64~32768
  │                                    │ sState = S_JPG_RECV
  ├─ CMD_JPG_DATA (chunk 0..N) ──────> │ jpgBuf[pos++] = read()
  │                                    │ CMD_JPG_DATA 帧解析
  │                                    │ 跳过 3 字节协议头
  │                                    │
  │  ── 收齐后 ──                      │
  │                                    │ setSwapBytes(true)
  │                                    │ drawJpg(jpgBuf)       → LCD 显示
  │                                    │ setSwapBytes(false)
  │                                    │
  │                                    │ sendJpegFile(jpgBuf)  → ESP-NOW
  │                                    │   分片 239B/pkt ×N
  │                                    │
  │                                    │ free(jpgBuf)
  │ <───────────────────────────── ACK │ ×30
  │                                    │
  ├─ CMD_IMG_END ────────────────────> │ sendEndPacket()
  │                                    │ → ESP-NOW PKT_JPG_END
```

## 基站实现

### 串口接收状态机

```
S_IDLE ──CMD_JPG_START──> S_JPG_RECV
                                │
                         30s 超时? → free + S_IDLE
                                │
                    解析 CMD_JPG_DATA 帧
                      ├─ 读 3 字节头 (cmd + len)
                      └─ 读 payload → jpgBuf
                                │
                        jpgRecvSize ≥ totalSize
                                │
                        drawJpg + sendJpegFile
                                │
                        30 × ACK + S_IDLE
```

### JPEG 解码渲染

```
drawJpg(jpgBuf)
  └─ tftOutput(x, y, w, h, bitmap)   ← TJpg_Decoder 回调
       │ bitmap 是大端 RGB565
       │
       └─ memcpy → jpgRowBuf[16×240]
            │ 适应 8×8 / 16×16 MCU
            │ 8×8 MCU → 攒 2 行 tile 行
            │ 16×16 MCU → 1 行即满
            │
            jpgRowDone 位图追踪 16 行
            │
            16 行全收齐 (jpgRowDone == 0xFFFF)
              └─ displayStrip ×2
                   └─ tft.pushImage(x, y, 240, 8, pixels)
                        ↑ setSwapBytes(true) 修正字节序
```

### ESP-NOW 转发

```
sendJpegFile(imageId, jpgBuf, size)
  ├─ PKT_JPG_START   (totalChunks, totalSize)
  ├─ PKT_JPG_DATA ×N (seq, 239B payload)
  │   每包同步发送，3 次重试
  └─ PKT_JPG_END     (totalChunks, totalSize)
```

每个 ESP-NOW 包固定 250B：11B 头 + 239B JPEG 数据。

## 接收端实现

```
onDataRecv(mac, data, len)
  ├─ PKT_JPG_START → malloc(jpgRecvBuf), 重置位图
  ├─ PKT_JPG_DATA  → memcpy 到 jpgRecvBuf[seq × 239]
  │                   去重 (jpgChunkSeen 位图)
  │                   所有分片收齐 → drawJpg → LCD
  └─ PKT_JPG_END   → 收尾统计
```

## 通信协议

### 串口命令

| 命令 | 值 | payload | 说明 |
|------|-----|---------|------|
| `CMD_JPG_START` | `0x10` | `[totalSize(2B)]` | JPEG 总字节数 |
| `CMD_JPG_DATA` | `0x11` | `[chunk(≤512B)]` | JPEG 数据分片 |

### ESP-NOW 包

| 包类型 | 值 | 说明 |
|--------|-----|------|
| `PKT_IMAGE_START` | `0x01` | 兼容控制包 |
| `PKT_IMAGE_END` | `0x03` | 兼容控制包 |
| `PKT_JPG_START` | `0x10` | JPEG 开始，param=totalSize |
| `PKT_JPG_DATA` | `0x11` | seq=分片序号，payload 239B |
| `PKT_JPG_END` | `0x12` | JPEG 结束 |

## 内存布局

```
基站 RAM (≈ 49KB / 82KB = 60%):

  jpgRowBuf[16×240]  = 7680   JPEG 解码行缓冲
  jpgBuf (heap)      ≈ 8000   JPEG 文件 (malloc/free)
  Serial RX (heap)   = 4096   硬件串口缓冲
  TJpg work (heap)   ≈ 3100   解码器工作区
  TFT + 其他         ≈ 28000
```

## 安全性

- JPEG 大小限制 64~32768 字节
- S_JPG_RECV 30 秒超时保护
- CMD_JPG_DATA 帧解析校验
- malloc 失败回退 S_IDLE

## 文件位置

| 文件 | 说明 |
|------|------|
| `src/espnow_display.ino` | 基站主循环、串口状态机、JPEG 回调 |
| `src/espnow_sender.cpp` | `sendJpegFile()`、`displayStrip()` |
| `src/espnow_receiver.cpp` | 接收端 JPEG 重组 + 解码 |
| `tools/send.py` | 宿主机 `--raw-jpeg` 直传 |
| `docs/serial_transfer.md` | 协议文档 |
