# ESP-NOW 图片传输 — 可靠性设计（已实现）

## 概述

本文档记录已实现的可靠性机制。所有设计均已在 V101 代码中落地。

---

## 一、MAC 地址配置

**当前实现**：基站上电后通过串口交互输入接收端 MAC，或通过 `send.py` 自动配置。

```text
接收端串口输出:
[Receiver] ESP-NOW ready (instant block push)
  MAC: 8C:4F:00:53:A3:18        ← 复制这个地址

基站串口提示:
Enter receiver MAC (format: XX:XX:XX:XX:XX:XX):
> 8C:4F:00:53:A3:18
Using MAC: 8C:4F:00:53:A3:18
```

**自动化流程**（`tools/send.py`）：
1. 复位接收端 → 读取串口输出的 MAC
2. 写入 `tools/receiver_mac.txt` 持久化
3. 通过串口下发 MAC 到基站

---

## 二、单播发送 + 硬件 ACK

**核心函数**：`sendPacket()` in `espnow_sender.cpp`

```cpp
static bool sendPacket(uint8_t *data, int len) {
    sendDone = false;
    esp_now_send(peerAddr, data, len);    // 单播，等待硬件 ACK
    unsigned long start = millis();
    while (!sendDone) {
        if (millis() - start > 200) {     // 超时保护 200ms
            sendSuccess = false;
            break;
        }
        yield();
    }
    return sendSuccess;
}
```

**原理**：
- 广播发送（`esp_now_send(NULL, ...)`）：`onDataSent` 始终返回成功，不可靠
- 单播发送（`esp_now_send(peerMac, ...)`）：等待接收端 802.11 ACK，`onDataSent` 返回真实状态

**重试策略**：

| 参数 | 当前值 |
|------|--------|
| 最大重试次数 | 5 次 |
| 重试间隔 | 8ms |
| 单次超时 | 200ms |
| 每包最坏耗时 | 1 秒（5 次 × 200ms） |

---

## 三、包序列号与接收端去重

### 序列号

```cpp
// espnow_img_proto.h
struct EspnowPacketHeader {
    uint8_t  type;          // PacketType
    uint16_t imageId;       // 图片 ID
    uint16_t seq;           // 包序号 (0~899)
    uint16_t total;         // 总包数 (900)
    uint8_t  stripIdx;      // 行索引 (0~29), y = stripIdx * 8
    uint8_t  blockIdx;      // 块在行内的序号 (0~29), x = blockIdx * 8
    uint8_t  w;             // = BLOCK_W = 8
    uint8_t  h;             // = BLOCK_H = 8
};
```

`seq = stripIdx * BLOCKS_PER_STRIP + blockIdx`，天然唯一有序。

### 接收端去重

```cpp
// espnow_receiver.cpp
static uint16_t lastSeq[TOTAL_STRIPS];  // 每行最后收到的 blockIdx

// PKT_IMAGE_START 时重置
memset(lastSeq, 0xFF, sizeof(lastSeq));

// PKT_IMAGE_DATA 处理中
if (bi <= lastSeq[si]) return;  // 重复/乱序包，丢弃
lastSeq[si] = bi;
```

**跨帧防护**：除去重外，还检查 `pkt->header.imageId == rxState.imageId`，防止前一张图的延迟包污染新帧。

---

## 四、射频参数

| 参数 | 当前值 | 说明 |
|------|--------|------|
| Wi-Fi 模式 | `WIFI_STA` | 站点模式，射频开销最小 |
| 信道 | 1（默认，可配置） | 双方 `wifi_set_channel(channel)` 锁定 |
| ESP-NOW 角色 | Controller / Slave | 发送端为 CONTROLLER，接收端为 SLAVE |

---

## 五、当前可靠性效果

| 指标 | 当前状态 |
|------|----------|
| ACK 真实性 | ✅ 真实硬件 ACK |
| 丢包恢复 | ✅ 基于 ACK 的 5 次重试 |
| 重复包处理 | ✅ 序列号单调递增去重 |
| 跨帧污染 | ✅ imageId 匹配校验 |
| 重试间隔 | 8ms |
| 预期画面完整性 | 基本完整，偶有单个块丢失 |

---

## 六、流程总结

```
发送端（基站）                        接收端
  │                                      │
  ├─ PKT_IMAGE_START ────────────────>   │ 重置去重表，记录 imageId
  │  单播 + 硬件 ACK                    │
  │                                      │
  ├─ PKT_IMAGE_DATA[seq=n] ──────────>   │ 校验 imageId + 去重 → 绘制
  │←──────── 802.11 ACK ──────────────   │
  │  若 ACK 超时，重试最多 5 次          │
  │                                      │
  ├─ PKT_IMAGE_END ──────────────────>   │ 校验 imageId → 统计输出
  │  单播 + 硬件 ACK                    │
```
