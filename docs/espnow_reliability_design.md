# ESP-NOW 图片传输 — 可靠性优化设计方案

## 现状分析

当前协议已实现的基础功能：
- ✅ 8×8 分块传输（每包 138 字节，含 128 像素数据 + 10 字节头）
- ✅ 发送端逐行生成 + LCD 显示 + ESP-NOW 发送
- ✅ 接收端逐块接收 + 即时 LCD 刷新
- ✅ 连续刷帧（帧间无停顿）

存在的问题：
- ❌ 使用广播 MAC → 底层无硬件 ACK，`onDataSent` 回调的 `sendSuccess` **不可靠**
- ❌ 接收端无去重机制 → 重传导致重复块覆盖
- ❌ 接收端 MAC 地址硬编码发现流程不完善

---

## 一、MAC 地址发现与配置

### 方案：手动配置 + 串口提示

接收端上电后在串口打印自身 MAC。用户将其填入发送端代码。

```
接收端串口输出:
[Receiver] ESP-NOW ready (instant block push)
  MAC: 5C:CF:7F:XX:XX:XX        ← 复制这个地址
```

发送端宏定义：
```cpp
// 接收端 MAC 地址（从接收端串口获取后填入）
#define RECEIVER_MAC {0x5C, 0xCF, 0x7F, 0xXX, 0xXX, 0xXX}
```

### 变更文件
| 文件 | 变更 |
|------|------|
| `src/espnow_display.ino` | 替换 `peerMac[]` 的广播地址为 `RECEIVER_MAC` |
| `include/espnow_img_proto.h` 或 `src/main.h` | 添加 MAC 宏定义 |

---

## 二、单播发送 + 硬件 ACK

### 原理

广播包：`esp_now_send(NULL, data, len)` → ESP-NOW **不等待 ACK**，`onDataSent` 回调中的 `status` 始终为 `ESP_NOW_SEND_SUCCESS`。

单播包：`esp_now_send(peerMac, data, len)` → ESP-NOW 等待接收方硬件 ACK，`onDataSent` 返回真实状态。

### 变更：发送端

`sendPacket()` 使用实际 MAC：

```cpp
static bool sendPacket(uint8_t *data, int len) {
    sendDone = false;
    esp_now_send(peerMac, data, len);    // 改为单播
    unsigned long start = millis();
    while (!sendDone) {
        if (millis() - start > 200) {   // 超时增加到 200ms
            sendSuccess = false;
            break;
        }
        yield();
    }
    return sendSuccess;
}
```

重传参数调整：

```cpp
// 当前（广播）
for (int r = 0; r < 3; r++) {
    if (sendPacket(...)) break;
    delay(2);
}

// 改为（单播 + 合理重试间隔）
static const int MAX_RETRIES = 5;
static const int RETRY_DELAY_MS = 8;

for (int r = 0; r < MAX_RETRIES; r++) {
    if (sendPacket(...)) break;
    retries++;
    delay(RETRY_DELAY_MS);
}
```

### 变更文件
| 文件 | 变更 |
|------|------|
| `src/espnow_sender.cpp` | `sendPacket()` 目标改为 `peerMac`；`sendImage()` 内重试参数调整 |

---

## 三、包序列号与接收端去重

### 当前状态

协议头中已有 `seq` 字段：
```cpp
typedef struct {
    uint8_t  type;       // 包类型
    uint16_t imageId;    // 图片 ID
    uint16_t seq;        // 自增序列号
    uint16_t total;      // 总包数
    uint8_t  stripIdx;   // 行索引
    uint8_t  blockIdx;   // 块索引
    uint8_t  w, h;       // 块宽高
} EspnowPacketHeader;
```

`seq = stripIdx * BLOCKS_PER_STRIP + blockIdx` 天然唯一有序。

### 接收端去重逻辑

每帧开始（PKT_IMAGE_START）重置去重表：

```cpp
// 新增：去重状态
static uint16_t lastSeq[TOTAL_STRIPS];  // 每行最后收到的 blockIdx
static uint16_t currentImageId = 0;

// PKT_IMAGE_START 处理中
memset(lastSeq, 0xFF, sizeof(lastSeq));  // 初始化为 0xFFFF（无包状态）
currentImageId = hdr->imageId;

// PKT_IMAGE_DATA 处理中
uint8_t si = pkt->header.stripIdx;
uint8_t bi = pkt->header.blockIdx;

if (bi <= lastSeq[si]) {
    // 重复包，丢弃
    return;
}
lastSeq[si] = bi;

// 继续正常绘制...
```

### 跨帧处理

帧结束时（PKT_IMAGE_END）不去重置，由下一帧的 PKT_IMAGE_START 负责。

如果出现丢帧（START 丢失），接收端自动初始化分支中也要重置去重表：

```cpp
// DATA 自动初始化（START 丢失时）
if (!rxState.receiving) {
    memset(lastSeq, 0xFF, sizeof(lastSeq));
    currentImageId = hdr->imageId;
    // ...
}
```

### 变更文件
| 文件 | 变更 |
|------|------|
| `include/espnow_img_proto.h` | 无需变更（`seq` 已存在） |
| `src/espnow_receiver.cpp` | 新增 `lastSeq[]` 数组及去重逻辑 |

---

## 四、射频参数确认

### 当前状态
```cpp
// 发送端 espnowSenderInit() 中
WiFi.mode(WIFI_STA);            // ✓ 正确
wifi_set_channel(channel);      // ✓ 已锁定

// 接收端 espnowReceiverInit() 中
WiFi.mode(WIFI_STA);            // ✓ 正确
wifi_set_channel(channel);      // ✓ 已锁定
```

### 无需变更
信道锁定和 Wi-Fi 模式已经符合要求。

---

## 五、预期效果对比

| 指标 | 当前（广播） | 优化后（单播+ARQ） |
|------|-------------|-------------------|
| ACK 真实性 | ❌ 始终返回成功 | ✅ 真实硬件 ACK |
| 丢包恢复 | ❌ 盲目重试 3 次 | ✅ 基于 ACK 的可靠重试 |
| 重复包处理 | ❌ 可能多次写入 | ✅ 序列号去重 |
| 重试次数 | 3 | 5 |
| 重试间隔 | 2ms | 8ms |
| 预期画面完整性 | 可能有残留黑块 | 基本无残留 |

---

## 六、实现步骤

```
Step 1: 获取接收端 MAC
  → 烧录接收端 → 读串口输出的 MAC
  → 填入发送端 #define RECEIVER_MAC

Step 2: 发送端改为单播
  → sendPacket() 目标改为 peerMac
  → 调整重试参数（5次, 8ms间隔）

Step 3: 接收端去重
  → 新增 lastSeq[] 数组
  → DATA 处理中判断重复
  → START/AUTO-INIT 时重置

Step 4: 验证
  → 烧录两端 → 观察画面完整性
  → 对比 serial 输出的丢包率
```
