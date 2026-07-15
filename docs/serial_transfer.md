# 串口传图功能说明

## 概述

宿主机（PC/Mac）通过 USB 串口将图片逐行发送给 ESP8266 基站，基站收到后：
1. 在本地 LCD 上显示该行
2. 将该行拆分为 30 个 8×8 块，通过 ESP-NOW 转发给接收端

无需在 ESP8266 上缓存整张图片（115KB），只需 3840 字节的行缓冲区。

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
| `CMD_IMG_START` | `0x01` | 无 | 开始 RGB565 图片传输 |
| `CMD_STRIP_DATA` | `0x02` | `[stripIdx(1B)][pixels(3840B)]` | 一行 RGB565 像素数据 |
| `CMD_IMG_END` | `0x03` | 无 | 图片传输结束 |
| `CMD_JPG_START` | `0x10` | `[totalLen(2B)]` | 开始 JPEG 传输，总字节数 |
| `CMD_JPG_DATA` | `0x11` | `[chunk(≤512B)]` | JPEG 数据分片 |

### JPEG 传输模式

宿主机将图片压缩为 JPEG（推荐 Q70，约 8KB），基站用 TJpg_Decoder 解码后走相同队列路径。

**JPEG 传输流程：**

```
宿主机                              基站
  │                                    │
  ├─ CMD_JPG_START ──────────────────> │ malloc JPEG 缓冲区
  │                                    │
  ├─ CMD_JPG_DATA (分片 0..N) ───────> │ 收满 → TJpg_Decoder 解码
  │                                    │ → 输出回调 → strip 入队 → ACK
  │ <──────────────────────── ACK ×30  │ (每解完一条 strip 发 ACK)
  │                                    │
  ├─ CMD_IMG_END ────────────────────> │ 等队列排空 → ESP-NOW: END
```

JPEG 解码期间，ESP-NOW 异步发送可同时运行（队列中的 strip 会被按序发出）。

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

```
宿主机                              基站
  │                                    │
  ├─ CMD_IMG_START ──────────────────> │ 准备接收
  │                                    ├─ ESP-NOW: PKT_IMAGE_START
  │                                    │
  ├─ CMD_STRIP_DATA (strip=0) ───────> │ LCD 显示 → 入队 → ACK ╮
  │ <───────────────────────────── ACK │                        │
  ├─ CMD_STRIP_DATA (strip=1) ───────> │ LCD 显示 → 入队 → ACK │
  │ <───────────────────────────── ACK │              ┌─────────┤
  │  ...                               │   环形队列   │ ESP-NOW │
  ├─ CMD_STRIP_DATA (strip=29) ──────> │ LCD 显示 → 入 → 出队   │
  │ <───────────────────────────── ACK │   (Q=3)     └─────────┤
  │                                    │                        │
  ├─ CMD_IMG_END ────────────────────> │ 等队列排空 → ESP-NOW: END
```

---

## 架构

### 环形队列 + 异步 ESP-NOW

基站内部采用**生产者-消费者**模式：

```
串口(115200)                         ESP-NOW(~90KB/s)
    │                                    ▲
    ▼                                    │
┌──────────┐  入队   ┌────────────┐  出队  ┌──────────┐
│ S_IDLE   │ ──────> │ stripQ[7]  │ ─────> │ begin    │
│ S_HEAD   │         │ 环形队列    │        │ SendStrip│
│ S_DATA   │         │ Q_SIZE=4   │        │ + poll   │
└──────────┘         │ 有效容量=7 │        └──────────┘
     │               └────────────┘
     ▼
   LCD 显示
   + ACK(0x06)
```

| 角色 | 说明 |
|------|------|
| **生产者（串口状态机）** | `S_IDLE` → 读命令头 → `S_HEAD` → `S_DATA` → 收完 3840 像素 |
| **入队 + ACK** | `displayStrip()` 推 LCD → `qPush()` 入队 → `Serial.write(0x06)` |
| **背压** | 队列满时 `qPush` 失败 → 不回 ACK → Python 暂停发送 → 无数据溢出 |
| **消费者（异步 ESP-NOW）** | `beginSendStrip()` 开始 → `pollSendStrip()` 逐块轮询 |
| **出队时机** | 上一个 strip 的 30 块全部发完（或跳过失败块）后，`qPop` 取下一个 |

### 串口接收状态机

```
S_IDLE ──[3B header]──> S_HEAD
                          │
                    [字节到达] 
                          │
                          ▼
                       S_DATA ──[3840B读完]──> displayStrip() → qPush() → ACK
                                                  │                   │
                                                  └── 失败(队列满) ──┘
                                                        → 不 ACK（背压）
                                                        → 下次循环重试入队
```

接收到的 strip 数据暂存于 `recvBuf[3840]`，入队时 `memcpy` 到 `stripQ[qHead]`。

### 环形队列

```
#define Q_SIZE  8
static uint8_t stripQ[Q_SIZE][3840];  // 8 × 3840 = 30 KB

qPush()           : 写入 stripQ[qHead], (qHead+1)%8
qPop()            : 读取 stripQ[qTail], (qTail+1)%8
qFull()/qEmpty()  : (qHead+1)%8 == qTail → full; qHead == qTail → empty
```

一个槽位用于 full/empty 区分，有效容量 = 7 条 strip。
入队时 `memcpy` 3840 字节，ESP-NOW 发送使用**独立缓冲区** `as.pixels[]`，不受队列回绕影响。

### 异步 ESP-NOW 发送

队列消费者的 ESP-NOW 发送通过 **事件计数器** 与 ISR 通信，避免竞态：

```cpp
// ISR 上下文
void onDataSent(...) {
    sendSuccess = (status == 0);
    sendEvents++;           // 原子递增
}

// 主循环
int pollSendStrip() {
    if (sendEvents != lastSendEvents) {
        lastSendEvents = sendEvents;  // 消费事件
        // 处理结果（成功/失败/重试）
    }
    if (超时) {
        // 强制超时，跳过此块
    }
    // 发送下一块或返回完成
}
```

每块超时 200ms，最多 3 次重试后跳过。
一个 strip 发完（至少发了 1 块）返回 `1`，全丢返回 `-1`。

### 图片结束

```
宿主机                    基站
  │                        │
  ├─CMD_IMG_END ─────────> │ endPending = true
  │                        │ 等队列排空
  │                        │ 等 async send 空闲
  │                        │ → sendEndPacket()  → ESP-NOW END
```

`CMD_IMG_END` 立即回复，实际 ESP-NOW END 包在队列全部排空后发送。

---

## 关键代码

### espnow_display.ino — 基站主循环

```cpp
// 环形队列
#define Q_SIZE  8
static uint8_t stripQ[Q_SIZE][STRIP_BUFFER_BYTES];
static volatile int qHead = 0, qTail = 0;

// 串口接收状态机
switch (sState) {
  case S_IDLE:
    if (Serial.available() >= 3) {
      cmd = Serial.read();
      len = Serial.read() | (Serial.read() << 8);
      // CMD_IMG_START / CMD_STRIP_DATA / CMD_IMG_END
    }
    break;
  case S_HEAD:
    if (Serial.available()) sState = S_DATA;
    break;
  case S_DATA:
    while (recvPos < recvLen && Serial.available())
      recvBuf[recvPos++] = Serial.read();
    if (recvPos >= recvLen) {
      displayStrip(recvIdx, recvBuf);        // LCD
      if (qPush(recvIdx, recvBuf))
        Serial.write(0x06);                  // ACK
      sState = S_IDLE;
    }
    break;
}

// 消费者
if (!isSendBusy() && !qEmpty()) {
  qPop(&si, &data);
  beginSendStrip(imgId, si, data);  // 内部 memcpy
}
if (isSendBusy()) {
  int st = pollSendStrip(imgId);
  if (st == 1) totalSent += getSendCount();
}
```

### espnow_sender.cpp — 发送函数

```cpp
void displayStrip(int stripIdx, const uint8_t *pixels);  // LCD
void beginSendStrip(uint16_t imageId, int stripIdx,
                    const uint8_t *pixels);  // 异步开始（内部复制数据）
int  pollSendStrip(uint16_t imageId);        // 轮询，0/1/-1
bool isSendBusy();
int  getSendCount();
bool sendStartPacket(uint16_t imageId);      // 同步（START/END 控制包）
bool sendEndPacket(uint16_t imageId, int sent);
```

---

## 宿主机工具

### send.py

```bash
pip3 install pyserial Pillow

# 发送图片（自动缩放至 240×240）
python3 tools/send.py /dev/cu.usbserial-1140 8C:4F:00:53:A3:18 photo.jpg
```

环境变量：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `STRIP_ACK_TIMEOUT` | `30.0` | 等待基站 ACK 超时（秒） |

核心流程：

```python
send_serial_packet(ser, CMD_IMG_START)   # START
for idx, strip_data in enumerate(strips):
    send_serial_packet(ser, CMD_STRIP_DATA, payload)
    wait_strip_ack(ser)                   # 每行等 ACK
send_serial_packet(ser, CMD_IMG_END)     # END
```

ACK 超时通过环境变量 `STRIP_ACK_TIMEOUT` 调整。
基站在队列满时不回复 ACK → Python 等待 → 天然背压。

### 直接串口发送（无 Python 依赖）

```bash
python3 -c "
import serial, struct, time
ser = serial.Serial('/dev/cu.usbserial-1140', 115200)
ser.write(b'\x01\x00\x00'); time.sleep(0.5)        # START
strip = bytes([0]) + b'\x00\xF8' * 1920             # 红色
ser.write(b'\x02' + struct.pack('<H', len(strip)) + strip)
time.sleep(5)
ser.write(b'\x03\x00\x00')                           # END
ser.close()
"
```

---

## 性能

| 环节 | 速度 | 说明 |
|------|------|------|
| 串口传输 (115200) | ~11 KB/s | 每行 3840 字节 ≈ 330ms |
| 串口传输 (460800) | ~44 KB/s | 每行 ≈ 83ms |
| 串口传输 (921600) | ~88 KB/s | 每行 ≈ 42ms |
| ESP-NOW (单播 ACK) | ~90 KB/s | 900 包约 1.3 秒 |
| 整图传输 (115200) | ~10-11 秒 | 串口瓶颈，ACK 即时响应不增加延迟 |
| 整图传输 (921600) | ~1.5-2 秒 | ESP-NOW 时间与串口时间重叠 |

> **瓶颈分析：** 队列架构下 ACK 在 LCD 显示后立即回复（不等待 ESP-NOW）。
> 串口传输是唯一瓶颈（115200 下 ~11s），ESP-NOW 时间被重叠覆盖。
> 串口 RX 缓冲区 4096 字节，可容纳一整条 strip 数据。

---

## 已知限制

- **接收端去重丢弃乱序包：** ESP-NOW 是无序协议。接收端要求每行 `blockIdx` 单调递增，
  晚到的较小 `blockIdx` 被丢弃，导致图像空洞。此为有意取舍（避免重复绘制）。
- **队列满降速：** ESP-NOW 严重阻塞时（多频段干扰），队列填满后 ACK 暂停，
  整图时间 = 串口传输时间 + ESP-NOW 额外延迟。
- **无校验重传：** 串口协议无校验和，损坏包静默丢弃。

---

## 文件位置

| 文件 | 说明 |
|------|------|
| `src/espnow_display.ino` | 基站主循环，串口状态机，环形队列 |
| `src/espnow_sender.cpp` | LCD 显示、异步/同步 ESP-NOW 发送 |
| `tools/send.py` | 宿主机投屏工具 |
| `platformio.ini` | `-DSERIAL_RX_BUFFER_SIZE=4096` 串口缓冲 |
