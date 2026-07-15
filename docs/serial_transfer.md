# 串口传图功能说明

## 概述

宿主机（PC/Mac）通过 USB 串口将图片逐行发送给 ESP8266 基站，基站收到后：
1. 在本地 LCD 上显示该行
2. 将该行拆分为 30 个 8×8 块，通过 ESP-NOW 转发给接收端

无需在 ESP8266 上缓存整张图片（115KB），只需 3840 字节的行缓冲区。

---

## 协议定义

### 包格式

所有多字节整数为**小端序**（LSB first）。除特别说明外，每个数据包末尾带 1 字节 XOR 校验和：

```
┌─────────┬──────────┬──────────────────────┬──────────┐
│  cmd    │  len     │  payload             │  xor_sum │
│  1 byte │  2 bytes │  len bytes           │  1 byte  │
└─────────┴──────────┴──────────────────────┴──────────┘
```

`xor_sum = cmd XOR len_lo XOR len_hi XOR payload[0] XOR payload[1] XOR ...`

接收端在校验失败时静默丢弃该包并等待下一个命令头。

### 命令列表

| 命令 | 值 | payload | 说明 |
|------|-----|---------|------|
| `CMD_IMG_START` | `0x01` | 无 | 开始新图片 |
| `CMD_STRIP_DATA` | `0x02` | `[stripIdx(1B)][pixels(3840B)]` | 一行像素数据 |
| `CMD_IMG_END` | `0x03` | 无 | 图片传输结束 |

### 像素数据格式

每行 = 8 像素高 × 240 像素宽 = 1920 像素  
每像素 = RGB565 小端序（2 字节）  
每行大小 = 1920 × 2 = **3840 字节**

```
像素排列：从左到右，从上到下
每个像素：LSB [b4 b3 b2 g5 g4 g3 g2]  MSB [r4 r3 r2 g5 g4 g3]
          byte 0 (低字节)               byte 1 (高字节)
```

### 数据传输流程

基站每处理完一行后回复 `0x06` (ACK) 通知宿主机发下一行。

```
宿主机                               基站
  │                                    │
  ├─ CMD_IMG_START ──────────────────> │ 准备接收
  │                                    ├─ ESP-NOW: PKT_IMAGE_START
  │                                    │
  ├─ CMD_STRIP_DATA (strip=0) ───────> │ LCD 显示行0 + ESP-NOW 发30块
  │ <───────────────────────────── ACK │
  ├─ CMD_STRIP_DATA (strip=1) ───────> │ LCD 显示行1 + ESP-NOW 发30块
  │ <───────────────────────────── ACK │
  │  ...                               │
  ├─ CMD_STRIP_DATA (strip=29) ──────> │ LCD 显示行29 + ESP-NOW 发30块
  │ <───────────────────────────── ACK │
  │                                    │
  ├─ CMD_IMG_END ────────────────────> │ 传输完成
  │                                    ├─ ESP-NOW: PKT_IMAGE_END
```

> **注意：** 串口协议不设 CRC 之外的流控握手（如 XON/XOFF 或 RTS/CTS）。
> 行级 ACK 提供明确的背压（back-pressure）：基站未处理完前一个 strip
> 不会回复 ACK，宿主机不会发送下一个 strip。这避免了 RX 缓冲区（512 字节）
> 在 115200 baud 以上波特率溢出的可能。

---

## 基站端实现

### 关键代码

```cpp
// espnow_display.ino — 基站主循环
static uint8_t stripBuf[3840];  // 全局缓冲区（避免栈溢出）

while (1) {
    if (Serial.available() < 4) { delay(1); continue; }  // 3B header + 1B xor

    uint8_t cmd = Serial.read();
    uint16_t len = Serial.read() | (Serial.read() << 8);

    switch (cmd) {
        case CMD_IMG_START:
            if (sendStartPacket(imgId)) {
                inImage = true;
                totalSent = 0;
            }
            break;

        case CMD_STRIP_DATA:
            if (!inImage) { drainSerial(len); break; }
            stripIdx = Serial.read();
            Serial.readBytes(stripBuf, 3840);
            sendStripFromHost(imgId, stripIdx, stripBuf);  // LCD + ESP-NOW
            Serial.write(0x06);  // ACK
            break;

        case CMD_IMG_END:
            sendEndPacket(imgId, totalSent);
            inImage = false;
            break;
    }
}
```

### 发送函数

```cpp
// espnow_sender.cpp
bool sendStartPacket(uint16_t imageId);
void sendEndPacket(uint16_t imageId, int sent);
int  sendStripFromHost(uint16_t imageId, int stripIdx, const uint8_t *pixels);
```

`sendStripFromHost()` 完成：
1. 将像素数据写入 strip sprite（`drawPixel` 逐像素填入 8×240 缓冲区）
2. `pushSprite` 推送到基站 LCD
3. 从缓冲区读取 8×8 块，通过 `sendPacket()` 单播发送（硬件 ACK + 最多 5 次重试）

---

## 宿主机工具

### send.py

```bash
# 安装依赖
pip3 install pyserial Pillow

# 发送图片（自动缩放至 240×240）
python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.jpg
```

环境变量：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `STRIP_ACK_TIMEOUT` | `5.0` | 等待基站 ACK 超时（秒） |

核心函数：

```python
def load_image_strips(path):
    """加载图片 → 缩放到 240×240 → RGB565 → 30 行数据"""
    img = Image.open(path).convert('RGB')
    img = img.resize((240, 240), Image.LANCZOS)
    # 返回 30 个 bytes，每个 3840 字节

def send_image_via_serial(ser, strips):
    """发送完整图片（START + 30 strips + END），每行等待 ACK"""
    send_serial_packet(ser, CMD_IMG_START)
    for idx, strip_data in enumerate(strips):
        payload = bytes([idx]) + strip_data
        send_serial_packet(ser, CMD_STRIP_DATA, payload)
        if not wait_strip_ack(ser):
            raise RuntimeError('ACK timeout')
    send_serial_packet(ser, CMD_IMG_END)
```

### 直接串口发送（无 Python 依赖）

```bash
# 手动发送一行红色测试数据（Python 单行命令）
python3 -c "
import serial, struct, time
ser = serial.Serial('/dev/cu.usbserial-1140', 115200)
# START
ser.write(b'\x01\x00\x00\x01')  # 最后字节 = XOR 校验和
time.sleep(0.1)
# 一行红色 (3840 字节)
strip = bytes([0]) + b'\x00\xF8' * 1920
payload = strip
xor = 0x02 ^ 0x01 ^ 0x0F  # cmd ^ len_lo ^ len_hi 等
ser.write(b'\x02' + struct.pack('<H', len(payload)) + payload + bytes([xor]))
time.sleep(5)  # 等待处理
# END
ser.write(b'\x03\x00\x00\x03')
ser.close()
"
```

> 手动发送时校验和不可省略，否则基站会因校验失败丢弃包。

---

## 性能

| 环节 | 速度 | 说明 |
|------|------|------|
| 串口传输 (115200) | ~11 KB/s | 每行 3840 字节 ≈ 330ms |
| 串口传输 (460800) | ~44 KB/s | 每行 ≈ 83ms |
| 串口传输 (921600) | ~88 KB/s | 每行 ≈ 42ms |
| ESP-NOW (单播 ACK) | ~90 KB/s | 900 包约 1.3 秒 |
| 整图传输 (115200) | ~11 秒 | 30 行 × 333ms + ESP-NOW 1.3s + ACK 等待 |
| 整图传输 (921600) | ~2.0 秒 | 30 行 × 42ms + ESP-NOW 1.3s + ACK 等待 |

> 串口是瓶颈。ESP8266 硬件支持最高 921600 baud。
> ESP8266 串口 RX 缓冲区已增大至 512 字节（`-DSERIAL_RX_BUFFER_SIZE=512`），
> 配合行级 ACK 背压机制，有效防止 FIFO 溢出。

---

## 已知限制

- **接收端去重策略丢弃乱序包：** ESP-NOW 是无序传输协议。当前接收端要求每行内的 `blockIdx`
  单调递增，先到达的较大 `blockIdx` 会使稍后到达的较小 `blockIdx` 被丢弃，导致图像空洞。
  这是有意取舍（避免重复绘制消耗 LCD 带宽），而非 bug。
- **无校验重传：** 串口协议校验失败时静默丢弃，不会请求重传。
  这在高波特率（921600）下可能增加图像噪声。

---

## 文件位置

| 文件 | 说明 |
|------|------|
| `src/espnow_display.ino` | 基站主循环，串口命令解析 |
| `src/espnow_sender.cpp` | `sendStartPacket()` / `sendEndPacket()` / `sendStripFromHost()` |
| `tools/send.py` | 宿主机投屏工具 |
