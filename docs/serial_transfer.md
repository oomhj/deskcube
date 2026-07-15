# 串口传图功能说明

## 概述

宿主机（PC/Mac）通过 USB 串口将 JPEG 文件发送给 ESP8266 基站，基站：
1. 本地 LCD 解码显示
2. 通过 ESP-NOW 将 JPEG 文件分片转发给接收端
3. 接收端重组后解码显示

全链路 JPEG 压缩传输，无需 RGB565 裸数据。

---

## 协议定义

### 包格式

所有多字节整数为**小端序**（LSB first）。

```
┌─────────┬──────────┬──────────────────────┐
│  cmd    │  len     │  payload             │
│  1 byte │  2 bytes │  len bytes           │
└─────────┴──────────┴──────────────────────┘
```

### 命令列表

| 命令 | 值 | payload | 说明 |
|------|-----|---------|------|
| `CMD_IMG_START` | `0x01` | 无 | ESP-NOW START 控制包 |
| `CMD_IMG_END` | `0x03` | 无 | ESP-NOW END 控制包 |
| `CMD_JPG_START` | `0x10` | `[totalSize(2B)]` | 开始 JPEG 传输 |
| `CMD_JPG_DATA` | `0x11` | `[chunk(≤512B)]` | JPEG 数据分片 |

### 数据传输流程

```
宿主机                              基站
  │                                    │
  │  ── 串口接收 ──                    │
  ├─ CMD_JPG_START ──────────────────> │ malloc(totalSize)
  │   payload: [totalSize(2B)]        │ 校验 64~32768
  │                                    │ sState = S_JPG_RECV
  ├─ CMD_JPG_DATA (chunk 0..N) ──────> │ jpgBuf[pos++] = read()
  │                                    │
  │  ── 收齐后 ──                      │
  │                                    │ drawJpg(jpgBuf) → LCD 显示
  │                                    │ sendJpegFile() → ESP-NOW 分片
  │                                    │
  │ <───────────────────────────── ACK │ ×30 strip ACK
  │                                    │ free(jpgBuf)
  ├─ CMD_IMG_END ────────────────────> │ sendEndPacket()
  │                                    │ → ESP-NOW PKT_JPG_END
```

---

## 架构

### 基站端

```
串口 ──CMD_JPG_DATA──> jpgBuf (heap)
                            │
                       drawJpg()
                        ├─ tftOutput 回调
                        │    └─ memcpy → jpgRowBuf[16×240]
                        │       → 16 行收齐 → displayStrip ×2
                        │          → tft.pushImage (setSwapBytes=true)
                        │
                        └─ sendJpegFile()
                             └─ ESP-NOW PKT_JPG_START
                                → PKT_JPG_DATA ×N (239B/chunk)
                                → PKT_JPG_END
```

### 接收端

```
ESP-NOW PKT_JPG_START → malloc(jpgRecvBuf)
PKT_JPG_DATA ×N       → memcpy 到 jpgRecvBuf[seq×239]
PKT_JPG_END            → drawJpg(jpgRecvBuf) → LCD 显示
```

---

## 性能

| 环节 | 耗时 | 说明 |
|------|------|------|
| 串口传输 6.5KB (115200) | ~0.6s | JPEG Q70 约 6-8KB |
| 基站解码 + 显示 | ~0.5s | TJpg_Decoder drawJpg |
| ESP-NOW 转发 27 包 | ~0.3s | 239B/包 × 3 重试 |
| 接收机解码 + 显示 | ~0.5s | TJpg_Decoder drawJpg |
| **整图总耗时** | **~2s** | 串口瓶颈 |

---

## 宿主机工具

### send.py

```bash
pip3 install pyserial Pillow

# JPEG 直传（文件必须是 240×240）
python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 --raw-jpeg image.jpg

# PIL 缩放+编码后传
python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.jpg
# 或: --jpeg 强制 JPEG 模式
```

环境变量：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `STRIP_ACK_TIMEOUT` | `30.0` | 等待基站 ACK 超时（秒） |

---

## 文件位置

| 文件 | 说明 |
|------|------|
| `src/espnow_display.ino` | 基站主循环、串口状态机、JPEG 解码回调 |
| `src/espnow_sender.cpp` | `sendJpegFile()`/`sendPacket()` 等发送函数 |
| `src/espnow_receiver.cpp` | 接收端 JPEG 重组 + 解码显示 |
| `tools/send.py` | 宿主机串口发送工具 |
| `platformio.ini` | 编译配置 |
| `docs/jpeg_transfer_design.md` | 详细架构设计 |
