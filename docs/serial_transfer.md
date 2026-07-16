# 串口协议说明

## 概述

宿主机通过 USB 串口（115200 baud）与基站通信。支持 JPED 图片传输和远程指令下发。

---

## 包格式

所有多字节整数为**小端序**。每个包末尾带 1 字节 XOR 校验和。

```
┌─────────┬──────────┬──────────────────────┬──────────┐
│  cmd    │  len     │  payload             │  xor_sum │
│  1 byte │  2 bytes │  len bytes           │  1 byte  │
└─────────┴──────────┴──────────────────────┴──────────┘
```

`xor_sum = cmd ^ len_lo ^ len_hi ^ payload[0] ^ payload[1] ^ ...`

校验失败时固件丢弃该包回到空闲状态，不请求重传。

---

## 命令列表

| 命令 | 值 | payload | 说明 |
|------|-----|---------|------|
| `CMD_JPG_START` | `0x10` | `[totalSize(2B)]` | 开始 JPEG 传输 |
| `CMD_JPG_DATA` | `0x11` | `[chunk(≤512B)]` | JPEG 数据分片 |
| `CMD_IMG_START` | `0x01` | 无 | ESP-NOW 控制包 |
| `CMD_IMG_END` | `0x03` | 无 | ESP-NOW 控制包 |
| `CMD_CMD` | `0x20` | `[cmdId(1B)][len(1B)][params...]` | 指令转发 |

---

## JPEG 传输流程

```
宿主机                              基站
  │                                    │
  ├─ CMD_JPG_START ──────────────────> │ malloc(totalSize)
  │                                    │ sState = S_JPG_RECV
  ├─ CMD_JPG_DATA (chunk ×N) ────────> │ readBytes 批量接收 + XOR 校验
  │                                    │
  │                                    │ ── 收齐 ──
  │                                    │ drawJpg(jpgBuf) → LCD
  │                                    │ sendJpegFile() → ESP-NOW
  │ <───────────────────────────── ACK │ ×30
  ├─ CMD_IMG_END ────────────────────> │ sendEndPacket()
```

数据优化：`Serial.readBytes()` 批量接收，每 chunk 一次读取、一次 XOR 计算。

---

## 指令转发流程

```
宿主机                              基站                    接收机
  │                                    │                        │
  ├─ CMD_CMD ────────────────────────> │                        │
  │   payload: [cmdId][len][params]   │ sendCmdPacket()         │
  │                                    ├── ESP-NOW PKT_CMD ───> │
  │ <───────────────────────────── ACK │                        │
  │                                    │                    analogWrite(PWM)
  │                                    │                    EEPROM.save()
```

支持指令：

| cmdId | 命令 | params | 说明 |
|-------|------|--------|------|
| `0x01` | 设置亮度 | `[value(1B)]` | 1~10，80%~100% PWM |

亮度值保存到 EEPROM，重启后自动恢复。

---

## 基站状态机

```
S_IDLE ──CMD_JPG_START──> S_JPG_RECV
                                │
                   readBytes → jpgBuf + XOR 校验
                                │
                        收齐 + 校验通过
                                │
                    ├─ drawJpg → LCD
                    └─ sendJpegFile → ESP-NOW
                                │
                         30×ACK + S_IDLE
                                │
                        30s 超时 → S_IDLE
```

---

## 性能

| 环节 | 耗时 | 说明 |
|------|------|------|
| 串口传输 6.5KB | ~0.6s | 115200, 13 chunks |
| 基站解码 + 显示 | ~0.5s | TJpg_Decoder |
| ESP-NOW 转发 27 包 | ~0.3s | 239B/包, 3 次重试 |
| 接收机解码 + 显示 | ~0.5s | TJpg_Decoder |
| **整图** | **~2s** | |

---

## 宿主机工具

```bash
# 直传 JPEG
python3 tools/send.py /dev/cu.usbserial-130 8C:4F:00:53:A3:18 --raw-jpeg image.jpg

# 亮度调节
python3 tools/send.py /dev/cu.usbserial-130 8C:4F:00:53:A3:18 --brightness=5
```

环境变量：`STRIP_ACK_TIMEOUT`（默认 30s）。
