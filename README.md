# ESP-NOW JPEG 投屏系统

宿主机通过串口发送 JPEG 图片 → 基站解码显示 → ESP-NOW 转发 → 接收端解码显示。支持亮度指令远程控制。

```
宿主机 ──串口 JPEG──> 基站 ──ESP-NOW──> 接收机
                       │                 │
                    LCD 显示           LCD 显示
                       │                 │
                   指令转发 ←──ESP-NOW── 亮度 PWM
```

## 硬件

| 设备 | 功能 | 串口 | 接收机 MAC |
|------|------|------|-----------|
| **基站** | 串口收图 + 本地显示 + ESP-NOW 转发 | `/dev/cu.usbserial-130` | — |
| **接收机 1** | ESP-NOW 收图 + 本地显示 | `/dev/cu.usbserial-110` | `8C:4F:00:53:A3:18` |
| **接收机 2** | ESP-NOW 收图 + 本地显示 | — | `EC:64:C9:C9:37:0C` |

- 背光控制：GPIO5（NPN 三极管），active LOW
- PWM 范围：80%~100%（亮度 1~10 档，校准值）

## 快速开始

```bash
# 刷基站
pio run -e nodemcu_sender -t upload --upload-port /dev/cu.usbserial-130

# 刷接收机
pio run -e nodemcu_receiver -t upload --upload-port /dev/cu.usbserial-110

# 传图
python3 tools/send.py /dev/cu.usbserial-130 EC:64:C9:C9:37:0C --raw-jpeg photo.jpg

# 亮度调节 (1-10)
python3 tools/send.py /dev/cu.usbserial-130 EC:64:C9:C9:37:0C --brightness=5
```

## 传输链路

```
串口 (115200, XOR 校验, readBytes 批量接收)
  └─ JPEG 文件 (~6-8KB, Q70, ~13 chunks)
       ├─ 基站: drawJpg → tft.pushImage (setSwapBytes)
       └─ ESP-NOW: 分片 239B/包 × 3 重试 → 接收机 drawJpg → LCD
```

性能：整图约 **2 秒**（115200 baud）。

## 功能

| 功能 | 命令 | 说明 |
|------|------|------|
| JPEG 传图 | `--raw-jpeg <file>` | 直传 240×240 JPEG |
| JPEG 编码传图 | `photo.jpg` | PIL 自动缩放 + 编码 |
| 亮度调节 | `--brightness=N` | 1~10 档，保存到 EEPROM |

## 项目结构

```
src/
  espnow_display.ino     基站状态机 + JPEG 渲染
  espnow_sender.cpp      ESP-NOW 发送（JPEG + 指令）
  espnow_receiver.cpp    接收端 JPEG 重组 + 指令处理
include/
  espnow_img_proto.h     协议常量 + PKT_CMD 指令帧
  jpeg_render.h          共享渲染回调
  main.h                 函数声明
tools/
  send.py                宿主机工具（传图 + 亮度）
docs/
  serial_transfer.md     串口协议文档
  jpeg_transfer_design.md  架构设计文档
```

## 环境

- PlatformIO + Arduino
- ESP8266 Arduino Core 3.1.2
- TFT_eSPI 2.5.43
- TJpg_Decoder 1.1.0
