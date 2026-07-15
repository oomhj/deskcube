# 串口传图模块审查报告 v2

审查日期：2026-07-16
审查范围：当前 `lcd` 分支的全量代码

---

## 🔴 关键问题

### 1. 队列条目指针悬空（数据竞争）

**位置：** `espnow_sender.cpp:145-151`（`beginSendStrip`） / `espnow_display.ino:220-224`（消费者出队）

**问题：** `qPop()` 返回指向 `stripQ[qTail]` 的指针，`beginSendStrip` 将其保存为 `as.pixels`。
后续 `qPush` 在环形队列回绕时会写入同一内存地址，覆盖正在异步发送的像素数据。

**发生条件：** ESP-NOW 发送较慢时（重试多），`qPush` 能把队列写满并回绕到 `qTail` 位置。

**影响：** ESP-NOW 发送已损坏的像素数据 → 接收端显示颜色错乱/花屏。

**修复：** 出队时将像素数据复制到专用发送缓冲区，而非仅保存指针。

### 2. `sendDone` ISR 竞态条件

**位置：** `espnow_sender.cpp:18-19, 161-205`（`pollSendStrip`）

**问题：** `sendDone` 和 `sendSuccess` 在 ISR（`onDataSent`）和主循环（`pollSendStrip`）之间共享。
读取结果和重置标志之间有时间窗口，ISR 可在此时再次触发，导致状态丢失或误判。

**影响：** 间歇性超时，一个 strip 内的块可能被错误重试或跳过，最终表现为 ACK 超时。

**修复：** 改用事件计数器（`sendEvents++`），避免读写竞争。

---

## 🟡 中等问题

### 3. 环形队列实际容量为 Q_SIZE-1

**位置：** `espnow_display.ino:39`

`qFull()` 使用 `(qHead + 1) % Q_SIZE == qTail` 检测，浪费一个槽位。
Q_SIZE=8 实际只能存 7 条 strip。不影响正确性，但降低缓冲效率。

### 4. 新图片开始时不重置 recvLen

**位置：** `espnow_display.ino:129-136`

`CMD_IMG_START` 处理中重置了 `inImage` 和 `totalSent`，但没有重置 `recvLen` 和 `recvPos`。
如果上一张图因背压残留了 `recvLen > 0`，新图片的第一条 strip 会因 `recvLen` 误判而跳过。

---

## 🟢 设计评价

### 优点

- **队列解耦**：串口接收和 ESP-NOW 发送完全异步，ACK 不再等待 ESP-NOW
- **背压机制**：队列满时自动停止串口数据，无需额外流控协议
- **RX 缓冲区 4096**：可容纳一整条 strip，提供充足缓冲余量
- **代码结构清晰**：`S_IDLE`/`S_HEAD`/`S_DATA` 状态机 + 队列生产者/消费者分离

### 待优化

- Python 端仍使用 `time.sleep(0.5)` 等待 START，可改为等待串口提示
- `CMD_IMG_END` 需等队列排空，增加了传图结束延迟（队列深时明显）
