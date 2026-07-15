# ESP-NOW 图片投屏

通过 ESP-NOW 在两块 ESP8266（NodeMCU）之间实时传输 240×240 图片，支持 LCD 显示。  
宿主机通过串口发送图片给基站，基站通过 ESP-NOW 转发给接收端。

## 硬件

| 设备 | 功能 | LCD | 串口 |
|------|------|-----|------|
| **基站** | 接收宿主机图片，ESP-NOW 发送 | ST7789V 240×240 | `/dev/cu.usbserial-1140` |
| **接收端** | 接收 ESP-NOW 并实时 LCD 显示 | ST7789V 240×240 | `/dev/cu.usbserial-1130` |

## 快速开始

### 1. 烧录固件

```bash
# 接收端
pio run -e nodemcu_receiver -t upload

# 基站
pio run -e nodemcu_sender -t upload
```

首次运行基站会提示在串口输入接收端 MAC 地址。查看接收端串口获取 MAC。

### 2. 投屏

```bash
# 首次：自动读取接收端 MAC + 配置基站（MAC 会自动保存）
python3 tools/send.py /dev/cu.usbserial-1130 /dev/cu.usbserial-1140

# 之后：使用已保存的 MAC 快速启动
python3 tools/send.py /dev/cu.usbserial-1140

# 指定 MAC 并发送图片（自动缩放至 240×240）
python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.jpg

# 循环发送（幻灯片模式，按 Ctrl+C 停止）
while true; do
  python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.jpg
  sleep 5
done
```

## 传输协议

| 参数 | 值 |
|------|-----|
| 图片尺寸 | 240 × 240 像素 |
| 分块 | 30 行 × 30 列 = 900 个 8×8 块 |
| 每包 | 138 字节（128 像素数据 + 10 字节头） |
| 传输 | 单播 + 硬件 ACK（非广播） |
| 重传 | 失败重试 5 次，间隔 8ms |
| 去重 | 接收端序列号去重，防止重复包 |

### 串口协议（宿主机 ↔ 基站）

宿主机通过 USB 串口逐行发送图片数据：

```
CMD_IMG_START  (0x01)  →  基站发送 ESP-NOW START
CMD_STRIP_DATA (0x02)  →  基站接收一行 → LCD 显示 → ESP-NOW 分包发送
CMD_IMG_END    (0x03)  →  基站发送 ESP-NOW END
```

每行数据：3840 字节（240×8 像素 × RGB565）。

## 项目结构

```
├── src/
│   ├── espnow_display.ino      # 主程序（基站/接收端）
│   ├── espnow_sender.cpp        # 基站：ESP-NOW 发送 + 梯形缓冲
│   ├── espnow_receiver.cpp      # 接收端：ESP-NOW 接收 + LCD 刷新
│   └── main.h                   # 头文件
├── include/
│   └── espnow_img_proto.h       # ESP-NOW 传输协议定义
├── tools/
│   ├── send.py                  # 宿主机投屏工具（推荐）
│   ├── test_espnow.py           # 自动测试脚本
│   └── receiver_mac.txt         # 已保存的接收端 MAC
├── docs/
│   ├── espnow_optimization.md   # 优化方案
│   └── espnow_reliability_design.md  # 可靠性设计
├── platformio.ini               # 编译配置
└── User_Setup.h                 # TFT LCD 引脚配置
```
