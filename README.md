# ESP-NOW 图片投屏

通过 ESP-NOW 在两个 ESP8266（NodeMCU）之间实时传输 240×240 图片，支持 LCD 显示。

## 硬件

| 设备 | 功能 | LCD | 串口 |
|------|------|-----|------|
| 基站 | 接收宿主机图片，通过 ESP-NOW 发送 | ST7789V 240×240 | `/dev/cu.usbserial-1140` |
| 接收端 | 接收 ESP-NOW 并实时显示 | ST7789V 240×240 | `/dev/cu.usbserial-1130` |

## 快速开始

### 1. 烧录固件

```bash
# 接收端
pio run -e nodemcu_receiver -t upload

# 基站
pio run -e nodemcu_sender -t upload
```

首次运行基站需在串口输入接收端 MAC 地址。

### 2. 一键投屏

```bash
# 自动读取 MAC + 配置基站 + 监控（推荐）
python3 tools/send.py /dev/cu.usbserial-1130 /dev/cu.usbserial-1140

# MAC 已保存后，快速启动
python3 tools/send.py /dev/cu.usbserial-1140

# 发送本地图片
python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.jpg
```

## 传输协议

| 参数 | 值 |
|------|-----|
| 图片尺寸 | 240 × 240 像素 |
| 每包大小 | 8×8 块 = 138 字节（128 像素 + 10 头） |
| 总包数 | 900 包/帧 |
| 传输方式 | 单播 + 硬件 ACK |
| 重传策略 | 5 次 × 8ms 间隔 |
| 去重 | 接收端序列号去重 |

## 项目结构

```
├── src/
│   ├── espnow_display.ino      # 主程序（基站/接收端）
│   ├── espnow_sender.cpp        # 基站实现
│   ├── espnow_receiver.cpp      # 接收端实现
│   └── main.h                   # 头文件
├── include/
│   └── espnow_img_proto.h       # ESP-NOW 传输协议
├── tools/
│   └── send.py                  # 宿主机投屏工具
├── docs/
│   ├── espnow_optimization.md   # 优化方案
│   └── espnow_reliability_design.md  # 可靠性设计
├── platformio.ini               # 编译配置
└── User_Setup.h                 # TFT 引脚配置
```
