# ESP-NOW 投屏系统  
**版本 V101**

通过双 ESP8266（NodeMCU）实现实时无线投屏：宿主机通过串口发送图片到基站，基站通过 ESP-NOW 转发给接收端，两端 LCD 同步显示。

```
宿主机 ──USB串口──→ 基站 ──ESP-NOW──→ 接收端
                     (LCD 同步显示)      (LCD 同步显示)
```

---

## 硬件

| 设备 | 功能 | LCD | USB 串口 |
|------|------|-----|----------|
| **基站** | 接收宿主机图片，ESP-NOW 发送 | ST7789V 240×240 SPI | `/dev/cu.usbserial-1140` |
| **接收端** | 接收 ESP-NOW，LCD 渲染 | ST7789V 240×240 SPI | `/dev/cu.usbserial-1130` |

---

## 快速开始

### 1. 烧录

```bash
# 接收端
pio run -e nodemcu_receiver -t upload

# 基站
pio run -e nodemcu_sender -t upload
```

### 2. 投屏

```bash
python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.jpg
```

支持 PNG / JPG / BMP，自动缩放至 240×240。

### 3. 快捷操作

```bash
# 首次（自动读取接收端 MAC 并保存）
python3 tools/send.py /dev/cu.usbserial-1130 /dev/cu.usbserial-1140

# MAC 已保存后，一行搞定
python3 tools/send.py /dev/cu.usbserial-1140 photo.jpg
```

---

## 数据传输

### 串口协议（宿主机 → 基站）

宿主机通过 USB 串口（115200 baud）将图片按行发送：

| 命令 | 值 | 说明 |
|------|-----|------|
| `CMD_IMG_START` | `0x01` | 通知基站准备接收新图片 |
| `CMD_STRIP_DATA` | `0x02` | 一行像素数据（3840 字节 = 240×8 像素 × RGB565） |
| `CMD_IMG_END` | `0x03` | 图片传输结束 |

包格式：`[cmd(1B)][len(2B LE)][payload(len B)]`

详见 [`docs/serial_transfer.md`](docs/serial_transfer.md)。

### ESP-NOW 协议（基站 → 接收端）

基站将每行拆分为 30 个 8×8 块，单播发送。

| 参数 | 值 |
|------|------|
| 分块 | 30 行 × 30 列 = 900 个 8×8 块 |
| 每包 | 138 字节（128 像素 + 10 字节头） |
| 传输 | 单播 + 硬件 ACK |
| 重传 | 5 次 × 8ms（硬件 ACK 驱动） |
| 去重 | 接收端 blockIdx 单调递增 + imageId 跨帧校验 |

详见 [`docs/screen_projection.md`](docs/screen_projection.md)。

---

## 项目结构

```
├── src/
│   ├── espnow_display.ino       # 主入口，区分基站/接收端
│   ├── espnow_sender.cpp         # 基站：串口命令解析 + ESP-NOW 发送
│   ├── espnow_receiver.cpp       # 接收端：ESP-NOW 接收 + LCD 渲染
│   └── main.h                    # 公共头文件
├── include/
│   └── espnow_img_proto.h        # ESP-NOW 协议定义
├── tools/
│   ├── send.py                   # 宿主机投屏工具（推荐）
│   ├── test_espnow.py            # 自动化测试脚本
│   └── receiver_mac.txt          # 已保存的接收端 MAC
├── docs/
│   ├── screen_projection.md      # 投屏功能完整说明
│   ├── serial_transfer.md        # 串口传图协议说明
│   ├── espnow_optimization.md    # ESP-NOW 优化方案
│   └── espnow_reliability_design.md  # 可靠性设计
├── platformio.ini                # 编译配置
└── User_Setup.h                  # TFT LCD 引脚配置
```

---

## 文档索引

| 文档 | 内容 |
|------|------|
| [`docs/serial_transfer.md`](docs/serial_transfer.md) | 串口协议格式、基站解析逻辑、send.py 实现 |
| [`docs/screen_projection.md`](docs/screen_projection.md) | ESP-NOW 传输、接收端渲染、传输统计 |
| [`docs/espnow_optimization.md`](docs/espnow_optimization.md) | 性能瓶颈分析、优化方案 |
| [`docs/espnow_reliability_design.md`](docs/espnow_reliability_design.md) | ARQ 重传、去重、时序设计 |

---

## 版本历史

| 版本 | 日期 | 变更 |
|------|------|------|
| V101 | 2026-07-16 | 单播+硬件ACK；接收端去重+imageId跨帧校验；串口缓冲增大至512B；死代码条件编译隔离；文档与代码同步 |
| V100 | — | 初始版本：广播传输，基本投屏功能 |
