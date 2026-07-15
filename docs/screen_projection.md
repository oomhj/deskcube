# ESP-NOW 投屏功能说明

## 概述

两块 ESP8266（NodeMCU）之间通过 ESP-NOW 实时传输 240×240 图片：

```
宿主机 ──USB串口──→ 基站 ──ESP-NOW──→ 接收端
                      (LCD显示)          (LCD显示)
```

- **基站**：连接宿主机 USB，接收图片数据，通过 ESP-NOW 转发
- **接收端**：接收 ESP-NOW 数据包，实时在 LCD 上渲染

---

## 硬件接线

| | 基站 | 接收端 |
|---|---|---|
| 主控 | NodeMCU (ESP8266) | NodeMCU (ESP8266) |
| LCD | ST7789V 240×240 SPI | ST7789V 240×240 SPI |
| USB 串口 | `/dev/cu.usbserial-1140` | `/dev/cu.usbserial-1130` |

LCD 引脚配置见 `User_Setup.h`。

---

## 固件烧录

```bash
# 接收端
pio run -e nodemcu_receiver -t upload

# 基站
pio run -e nodemcu_sender -t upload
```

两个环境共用同一套源码，通过编译宏区分：

| 环境 | 宏 | 编译的文件 |
|---|---|---|
| `nodemcu_receiver` | `ESPNOW_MODE_RECEIVER` | espnow_display.ino + espnow_receiver.cpp |
| `nodemcu_sender` | `ESPNOW_MODE_SENDER` | espnow_display.ino + espnow_sender.cpp |

---

## 首次配置

基站上电后需输入接收端 MAC 地址：

1. 打开接收端串口，查看 MAC：
   ```
   MAC: 8C:4F:00:53:A3:18
   ```

2. 打开基站串口，按提示输入 MAC：
   ```
   Enter receiver MAC (format: XX:XX:XX:XX:XX:XX):
   > 8C:4F:00:53:A3:18
   Using MAC: 8C:4F:00:53:A3:18
   [Base] Ready. Send image data via serial.
   ```

基站自动保存 MAC 到 `tools/receiver_mac.txt`，后续无需重复输入。

---

## 投屏工具

### send.py（推荐）

```bash
# 首次：自动读取接收端 MAC + 配置基站（MAC 自动保存）
python3 tools/send.py /dev/cu.usbserial-1130 /dev/cu.usbserial-1140

# 使用已保存的 MAC 快速启动
python3 tools/send.py /dev/cu.usbserial-1140

# 指定 MAC 并发送图片（自动缩放至 240×240）
python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.jpg

# 幻灯片模式
while true; do
  python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.jpg
  sleep 5
done
```

依赖：`pyserial`、`Pillow`。

```bash
pip3 install pyserial Pillow
```

### test_espnow.py

自动化测试脚本，读取接收端 MAC → 配置基站 → 验证传输。

```bash
python3 tools/test_espnow.py /dev/cu.usbserial-1130 /dev/cu.usbserial-1140
```

---

## 传输协议

### ESP-NOW 协议

| 参数 | 值 |
|---|---|
| 图片尺寸 | 240 × 240 像素 |
| 分块方式 | 30 行 × 30 列 = 900 个 8×8 块 |
| 每包大小 | 138 字节（128 像素数据 + 10 字节头） |
| 传输方式 | 单播 + 硬件 ACK |
| 重试策略 | 最多 5 次，间隔 8ms |
| 去重机制 | 接收端序列号去重 |

#### 数据包格式

```
EspnowPacketHeader (10 字节):
  type      (1B)  — 包类型 (START=0x01, DATA=0x02, END=0x03)
  imageId   (2B)  — 图片序列号
  seq       (2B)  — 包序号 (0~899)
  total     (2B)  — 总包数 (900)
  stripIdx  (1B)  — 行索引 (0~29)
  blockIdx  (1B)  — 块索引 (0~29)
  w, h      (2B)  — 块宽高 (8×8)

EspnowImagePacket (138 字节):
  header    (10B) — 包头
  data      (128B) — 64 像素 × RGB565
```

### 串口协议（宿主机 → 基站）

| 命令 | 说明 |
|---|---|
| `CMD_IMG_START` (0x01) | 开始新图片，基站发送 ESP-NOW START |
| `CMD_STRIP_DATA` (0x02) | 一行像素数据（3840 字节），基站 LCD 显示 + ESP-NOW 发送 |
| `CMD_IMG_END` (0x03) | 图片结束，基站发送 ESP-NOW END |

包格式：`[cmd][len_lo][len_hi][payload]`

每行数据 = 240 × 8 像素 × 2 字节 = **3840 字节 RGB565**。

---

## 接收端渲染

接收端使用 8×240 行缓冲区（3840 字节 RAM）：

1. 收到 `PKT_IMAGE_START` → 重置去重表
2. 收到 `PKT_IMAGE_DATA` → 解码像素到行缓冲区的对应 8×8 位置
3. 一行 30 块收齐 → 一次性 `pushSprite` 刷新 LCD
4. 收到 `PKT_IMAGE_END` → 打印传输统计

去重：每行记录最后收到的 `blockIdx`，重复包直接丢弃。

---

## 传输统计

接收端每帧结束后通过串口输出：

```
=== Image Receive Complete ===
Received: 900/900
Lost:     0
Time:     2.2 s
Speed:    87.4 KB/s
```

典型速度约 **80~100 KB/s**，丢包率约 0%（视距离和信道质量而定）。

---

## 文件结构

```
src/
├── espnow_display.ino        # 主入口，基站/接收端模式选择
├── espnow_sender.cpp          # 基站：ESP-NOW 发送 + 行缓冲
├── espnow_receiver.cpp        # 接收端：ESP-NOW 接收 + LCD 渲染
└── main.h                     # 头文件

include/
└── espnow_img_proto.h         # 协议定义

tools/
├── send.py                    # 投屏工具
├── test_espnow.py             # 测试脚本
└── receiver_mac.txt           # 已保存的 MAC

docs/
├── espnow_optimization.md     # 优化方案
└── espnow_reliability_design.md  # 可靠性设计
```
