# ESP-NOW 图片传输协议优化方案

## 1. 核心传输逻辑：ARQ 带重传确认机制

### 现状问题
当前使用广播 MAC (`0xFF:FF:FF:FF:FF:FF`) 发送，广播包底层**无硬件 ACK 机制**，发送方无法确认接收方是否收到。

### 优化目标
改为**单播模式**（指定接收方 MAC 地址），利用 ESP-NOW 硬件自动返回的 ACK 状态判断发送成败，实现可靠的 ARQ (Automatic Repeat reQuest)。

### 实现方案

#### 发送端 (Tx)
```
调用 esp_now_send(dstMac, data, len) 发送数据
  ↓
系统触发 SentCb 回调，返回状态：
  - ESP_NOW_SEND_SUCCESS → 发送成功
  - ESP_NOW_SEND_FAIL    → 发送失败
```

#### 重传策略
| 参数 | 建议值 | 说明 |
|------|--------|------|
| 最大重试次数 \(N_{retry}\) | 3 ~ 5 次 | 超过后丢弃该包，继续下一包 |
| 重试间隔 \(T_{wait}\) | 5 ~ 10 ms | 避免信道拥堵 |

#### 代码框架
```cpp
static void onDataSent(uint8_t *mac_addr, uint8_t status) {
    sendDone = true;
    sendSuccess = (status == 0);  // 0 = ESP_NOW_SEND_SUCCESS
}

bool sendPacketReliable(uint8_t *dstMac, uint8_t *data, int len, int maxRetries) {
    for (int r = 0; r < maxRetries; r++) {
        sendDone = false;
        esp_now_send(dstMac, data, len);
        
        unsigned long start = millis();
        while (!sendDone) {
            if (millis() - start > 100) break;  // 超时保护
            yield();
        }
        
        if (sendSuccess) return true;
        delay(5);  // 重试间隔
    }
    return false;  // 全部重试失败
}
```

---

## 2. 核心防错逻辑：包序列号与去重

### 问题
重传机制导致接收端可能收到**重复数据包**。

### 方案
在数据包头中加入**自增序列号 (seq)**，接收端维护历史记录，重复包直接丢弃。

#### 序列号设计
```cpp
typedef struct {
    uint8_t  type;       // 包类型
    uint16_t imageId;    // 图片 ID
    uint16_t seq;        // 自增序列号（0 ~ TOTAL_PACKETS-1）
    uint16_t total;      // 总包数
    uint8_t  stripIdx;   // 行索引
    uint8_t  blockIdx;   // 块索引
    uint8_t  w, h;       // 块宽高
} EspnowPacketHeader;
```

`seq = stripIdx * BLOCKS_PER_STRIP + blockIdx`，天然唯一且有序。

#### 接收端去重
```cpp
static uint16_t lastSeenSeq[TOTAL_STRIPS];  // 每行记录最后收到的块号

bool isDuplicate(uint8_t stripIdx, uint8_t blockIdx) {
    if (blockIdx <= lastSeenSeq[stripIdx]) {
        return true;  // 重复包，丢弃
    }
    lastSeenSeq[stripIdx] = blockIdx;
    return false;
}
```

> 注意：跨图片时需重置 `lastSeenSeq[]`（在 PKT_IMAGE_START 处理中清零）。

---

## 3. 硬件与射频参数

### 信道锁定
通信双方必须工作在**完全相同的 Wi-Fi 信道上**。

| 问题 | 后果 | 解决 |
|------|------|------|
| 双方信道不一致 | ESP-NOW 时断时续 | 发送端/接收端统一调用 `wifi_set_channel(channel)` |
| 自动信道切换 | 连接不稳定 | 手动固定信道，如 channel = 1 |

### Wi-Fi 模式
```
正确：WiFi.mode(WIFI_STA);           // 站点模式（推荐）
      WiFi.mode(WIFI_AP_STA);        // 双模

错误：WiFi.mode(WIFI_AP);            // 纯 AP 模式 → 射频忙于处理连接请求，丢包严重
```

### 当前实现检查
```cpp
// 发送端
WiFi.mode(WIFI_STA);
wifi_set_channel(channel);           // ✓ 已锁定信道

// 接收端
WiFi.mode(WIFI_STA);
wifi_set_channel(channel);           // ✓ 已锁定信道
```

---

## 4. 综合数据流

```
发送端                             接收端
  │                                  │
  ├─ PKT_IMAGE_START ──────────────> │ 重置去重表、初始化状态
  │   (seq=0, 无重传)                │
  │                                  │
  ├─ PKT_IMAGE_DATA[seq=1] ────────> │ 检查 seq > lastSeen → 接受并绘制
  │←────── ACK (硬件自动) ────────── │
  │                                  │
  ├─ PKT_IMAGE_DATA[seq=2] ────┐     │
  │   (发送失败，无 ACK)        │     │
  │                             │     │
  ├─ PKT_IMAGE_DATA[seq=2] ────┘───> │ 重传包，检查 seq = lastSeen+1 → 接受
  │←────── ACK ───────────────────── │
  │                                  │
  │   ...                            │
  │                                  │
  ├─ PKT_IMAGE_END ────────────────> │ 统计、完成
  │   (seq=901, 无重传)              │
```

---

## 5. 后续优化方向

1. **动态调整块大小**：当前 8×8=128 字节数据，ESP-NOW 最大 250 字节，可尝试 8×15=240 字节提升吞吐
2. **管道化传输**：不等前一个包 ACK 再发下一个，允许窗口内连续发送
3. **NACK 反馈**：接收端发送 NACK 包告知发送端缺失的序列号，发送端针对性补发
4. **自适应重试间隔**：根据当前丢包率动态调整 \(T_{wait}\)
