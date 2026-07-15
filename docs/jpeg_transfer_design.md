# JPEG 串口传图 + ESP-NOW 投屏设计

## 架构

```
宿主机                              基站
  │                                    │
  │  ── 串口 (XOR 校验) ──            │
  ├─ CMD_JPG_START                    │ malloc(jpgBuf)
  ├─ CMD_JPG_DATA ×N                  │ 逐字节收齐
  │                                    │
  │  ── 解码 + 显示 ──                │
  │                                    │ drawJpg(jpgBuf) → LCD
  │                                    │   ├─ setSwapBytes(true)
  │                                    │   └─ tft.pushImage 逐 tile
  │                                    │
  │  ── ESP-NOW 转发 ──               │
  │                                    │ sendJpegFile(jpgBuf)
  │                                    │   ├─ PKT_JPG_START
  │                                    │   ├─ PKT_JPG_DATA ×N
  │                                    │   │  每包 3 次重试
  │                                    │   └─ PKT_JPG_END
  │                                    │
  │ <────────────────────────── 30 ACK │
  │                                    │
  ├─ CMD_IMG_END ────────────────────> │ sendEndPacket()
  │                                    │ → PKT_IMAGE_END
```

## 串口协议

| 字段 | 大小 | 说明 |
|------|------|------|
| cmd | 1B | 命令字 |
| len | 2B | payload 长度（小端） |
| payload | len B | 数据 |
| xor | 1B | XOR 校验和 |

XOR = cmd ^ len_lo ^ len_hi ^ payload[0] ^ payload[1] ^ ...

## ESP-NOW 协议

| 包类型 | 值 | 数据 | 说明 |
|--------|-----|------|------|
| `PKT_JPG_START` | 0x10 | header + param=总字节数 | JPEG 开始 |
| `PKT_JPG_DATA` | 0x11 | header + 239B JPEG 数据 | JPEG 分片 |
| `PKT_JPG_END` | 0x12 | header | JPEG 结束 |

## 安全措施

- JPEG 大小限制 64~32768 字节
- 串口 XOR 校验和，校验失败丢弃包
- S_JPG_RECV 30 秒超时保护
- ESP-NOW 每包 3 次重试
- 接收机 JPEG 分片去重（jpgChunkSeen 位图）
- malloc 失败回退 S_IDLE

## 内存布局

```
基站 RAM:
  jpgBuf (heap)       ~8000   JPEG 文件 (malloc/free)
  Serial RX           =4096   硬件串口缓冲
  TJpg work (heap)    ~3100   解码器工作区
  TFT + 其他          ~28000

接收机 RAM:
  jpgRecvBuf (heap)   ~8000   JPEG 文件 (malloc/free)
  Serial RX           =4096   硬件串口缓冲
  TJpg work (heap)    ~3100   解码器工作区
  TFT + 其他          ~28000
```

## 文件

| 文件 | 职责 |
|------|------|
| `src/espnow_display.ino` | 基站串口状态机 + JPEG 渲染 |
| `src/espnow_sender.cpp` | ESP-NOW 发送（控制包 + JPEG） |
| `src/espnow_receiver.cpp` | 接收端 JPEG 重组 + 渲染 |
| `include/espnow_img_proto.h` | 协议常量和数据结构 |
| `include/jpeg_render.h` | 共享渲染回调 |
| `include/main.h` | 函数声明 |
| `tools/send.py` | 宿主机工具 |
