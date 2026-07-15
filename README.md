# ESP-NOW JPEG 投屏系统

宿主机通过串口发送 JPEG 图片 → 基站解码显示 → ESP-NOW 转发 → 接收端解码显示。

```
宿主机 ──串口 JPEG──> 基站 ──ESP-NOW──> 接收机
                       │                 │
                    LCD 显示           LCD 显示
```

## 硬件

| 设备 | 功能 | 串口 |
|------|------|------|
| **基站** | 串口收图 + 本地显示 + ESP-NOW 转发 | `/dev/cu.usbserial-1140` |
| **接收端** | ESP-NOW 收图 + 本地显示 | `/dev/cu.usbserial-1130` |

## 快速开始

```bash
# 刷基站
pio run -e nodemcu_sender -t upload --upload-port /dev/cu.usbserial-1140

# 刷接收机
pio run -e nodemcu_receiver -t upload --upload-port /dev/cu.usbserial-1130

# 传图（PIL 缩放 + JPEG 编码）
python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.jpg

# 直传 JPEG（已缩放为 240×240）
python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 --raw-jpeg image.jpg
```

## 传输链路

```
串口 (115200, XOR 校验)
  └─ JPEG 文件 (~6-8KB, Q70)
       └─ TJpg_Decoder 解码
            ├─ 基站 LCD: tft.pushImage 逐 tile
            └─ ESP-NOW: 分片 239B/包 ×N, 3 次重试
                 └─ 接收机: 重组 → TJpg_Decoder → LCD
```

性能：整图约 **2 秒**（115200 baud）。

## 项目结构

```
src/
  espnow_display.ino     基站串口状态机 + JPEG 渲染
  espnow_sender.cpp      ESP-NOW 发送（控制包 + JPEG 分片）
  espnow_receiver.cpp    接收端 JPEG 重组 + 显示
include/
  espnow_img_proto.h     协议常量和数据结构
  jpeg_render.h          共享渲染回调（两端共用）
  main.h                 函数声明
tools/
  send.py                宿主机串口发送工具
docs/
  serial_transfer.md     串口协议文档
  jpeg_transfer_design.md  架构设计文档
```

## 文档

| 文档 | 内容 |
|------|------|
| [`docs/serial_transfer.md`](docs/serial_transfer.md) | 串口协议、XOR 校验、状态机 |
| [`docs/jpeg_transfer_design.md`](docs/jpeg_transfer_design.md) | 架构设计、内存布局、安全措施 |

## 环境

- PlatformIO + Arduino
- ESP8266 Arduino Core 3.1.2
- TFT_eSPI 2.5.43
- TJpg_Decoder 1.1.0
