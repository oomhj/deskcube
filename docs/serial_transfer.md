# 串口传图协议说明

## 概述

宿主机通过 USB 串口将 JPEG 图片发送给 ESP8266 基站，基站本地解码显示后，通过 ESP-NOW 分片转发给接收端。

---

## 串口协议

### 包格式

所有多字节整数为**小端序**（LSB first）。每个包末尾带 1 字节 XOR 校验和。

```
┌─────────┬──────────┬──────────────────────┬──────────┐
│  cmd    │  len     │  payload             │  xor_sum │
│  1 byte │  2 bytes │  len bytes           │  1 byte  │
└─────────┴──────────┴──────────────────────┴──────────┘
```

`xor_sum = cmd XOR len_lo XOR len_hi XOR payload[0] XOR payload[1] XOR ...`

校验失败时固件丢弃该包回到空闲状态，不请求重传（USB 串口误码率极低）。

### 命令列表

| 命令 | 值 | payload | 说明 |
|------|-----|---------|------|
| `CMD_JPG_START` | `0x10` | `[totalSize(2B)]` | 开始 JPEG 传输 |
| `CMD_JPG_DATA` | `0x11` | `[chunk(≤512B)]` | JPEG 数据分片 |
| `CMD_IMG_START` | `0x01` | 无 | ESP-NOW START 控制包 |
| `CMD_IMG_END` | `0x03` | 无 | ESP-NOW END 控制包 |

### JPEG 传输流程

```
宿主机                              基站
  │                                    │
  ├─ CMD_JPG_START ──────────────────> │ malloc(totalSize)
  │   payload: [totalSize(2B)]        │ 校验 64~32768
  │                                    │ sState = S_JPG_RECV
  │                                    │
  ├─ CMD_JPG_DATA (chunk 0) ─────────> │ jpgBuf[pos++] = read()
  ├─ CMD_JPG_DATA (chunk 1) ─────────> │ 实时计算 XOR
  │  ...                              │
  ├─ CMD_JPG_DATA (chunk N) ─────────> │ 收齐 → 解码 + 显示
  │                                    │
  │                                    │ sendJpegFile() → ESP-NOW
  │ <───────────────────────────── ACK │ ×30 strip ACK
  │                                    │
  ├─ CMD_IMG_END ────────────────────> │ sendEndPacket()
```

---

## 基站实现

### 串口状态机

```
S_IDLE ──CMD_JPG_START──> S_JPG_RECV
                                │
                          串口字节 → jpgBuf
                          实时计算 XOR
                                │
                          30s 超时? → S_IDLE
                                │
                        收齐 + XOR 校验通过
                                │
                    ├─ drawJpg(jpgBuf) → LCD
                    └─ sendJpegFile() → ESP-NOW
                                │
                         30 × ACK + S_IDLE
```

### JPEG 解码渲染

```
drawJpg(jpgBuf)
  └─ jpegRenderCallback(x, y, w, h, bitmap)
       └─ tft.pushImage(x, y, w, h, bitmap)
            ↑ setSwapBytes(true) 修正字节序
```

### ESP-NOW 转发

```
sendJpegFile(imageId, jpgBuf, size)
  ├─ PKT_JPG_START      (totalChunks, totalSize)
  ├─ PKT_JPG_DATA ×N    (seq, 239B payload, 每包 3 次重试)
  └─ PKT_JPG_END        (totalChunks, totalSize)
```

---

## 接收端实现

```
onDataRecv(mac, data, len)
  ├─ PKT_JPG_START → malloc, 重置位图
  ├─ PKT_JPG_DATA  → memcpy 到 jpgRecvBuf[seq*239]
  │                   去重 (jpgChunkSeen 位图)
  │                   全部分片收齐 → drawJpg → LCD
  └─ PKT_JPG_END   → 收尾
```

---

## 性能

| 环节 | 耗时 | 说明 |
|------|------|------|
| 串口传输 6.5KB (115200) | ~0.6s | JPEG Q70 |
| 基站解码 + 显示 | ~0.5s | TJpg_Decoder |
| ESP-NOW 转发 27 包 | ~0.3s | 239B/包 × 3 重试 |
| 接收机解码 + 显示 | ~0.5s | TJpg_Decoder |
| **整图总耗时** | **~2s** | |

---

## 宿主机工具

```bash
# 直传 JPEG（需已是 240×240）
python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 --raw-jpeg image.jpg

# PIL 缩放 + JPEG 编码
python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.png
```

环境变量：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `STRIP_ACK_TIMEOUT` | `30.0` | 等待基站 ACK 超时（秒） |

---

## 文件位置

| 文件 | 说明 |
|------|------|
| `src/espnow_display.ino` | 基站主循环、串口状态机、JPEG 渲染 |
| `src/espnow_sender.cpp` | `sendJpegFile()` / `sendPacket()` |
| `src/espnow_receiver.cpp` | 接收端 JPEG 重组 + 解码 |
| `tools/send.py` | 宿主机串口发送工具 |
| `include/jpeg_render.h` | 16 行缓冲渲染器（两端共用） |
| `include/espnow_img_proto.h` | ESP-NOW 协议头 |
