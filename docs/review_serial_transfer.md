# 串口传图模块审查报告

审查日期：2026-07-16
审查范围：`docs/serial_transfer.md` · `src/espnow_display.ino` · `src/espnow_sender.cpp` · `src/espnow_receiver.cpp` · `tools/send.py` · `include/espnow_img_proto.h`

---

## 🔴 高严重度

### 1. 无流控机制 → 串口 RX 缓冲区溢出风险

**文件：** `tools/send.py:137` / `src/espnow_display.ino:110-136`

Python 脚本以固定 10ms 间隔发送每个 strip（`SERIAL_STRIP_DELAY=0.01`），不等待基站处理完毕。
`sendStripFromHost()` 需处理本地 LCD PushSprite + 30 个 ESP-NOW 块发送（每块最多 200ms 超时 + 5 次重试）。
最坏情况下一个 strip 耗时可达 ~6 秒。

ESP8266 串口 RX 缓冲区仅 512 字节（`-DSERIAL_RX_BUFFER_SIZE=512`）。3840 字节的 strip 数据以 115200 baud
到达耗时 ~333ms，若固件忙于 ESP-NOW 发送，512 字节溢出后数据损坏，导致协议状态机永久错位。

**建议：** 可选方案 A — 基站每完成一个 strip 后向串口发 ACK，Python 端 wait；
可选方案 B — 默认 strip 延迟提高到 ~200ms 并写入文档说明。

### 2. CMD_IMG_START 失败时仍继续传输

**文件：** `src/espnow_display.ino:101-108`

```cpp
bool ok = sendStartPacket(imgId);
inImage = true;        // ← 不管 sendStartPacket 是否成功
totalSent = 0;
```

`sendStartPacket()` 失败后接收端未进入接收状态，后续所有 strip 数据在接收端被丢弃。
但基站仍会处理全部数据（LCD 显示 + ESP-NOW 发送），白耗 CPU 和串口带宽。

**建议：** `if (!ok) break;` 等待下一个 `CMD_IMG_START`。

---

## 🟡 中严重度

### 3. `len` 字段边界安全

**文件：** `src/espnow_display.ino:110-122`

```cpp
int dataLen = len - 1;  // len 是 uint16_t
if (dataLen != STRIP_BUFFER_BYTES) {
    for (int i = 0; i < dataLen; i++) Serial.read();
    break;
}
```

- `len = 0` 时 `dataLen = 65535`，drain 循环会尝试读取 65535 字节，可能无限阻塞。
- 无合理上限检查，一个损坏的包即可让循环挂起。

**建议：** 在 drain 循环前检查 `dataLen` 是否在合理范围内（如 ≤ 4096）。

### 4. `sendPacket()` 静态 volatile 变量竞态（理论）

**文件：** `src/espnow_sender.cpp:19-25, 54-66`

`sendDone = false` 赋值与 `esp_now_send()` 调用之间有微小时间窗口。如果前一次发送的回调延迟触发，
当前发送会误认为已完成。实际风险极低，因为 `esp_now_send` 内部会重新注册回调上下文。

**建议：** 使用事件计数器替代 bool 标志位。

### 5. 串口协议无校验

**文件：** `docs/serial_transfer.md` 协议定义

无 CRC / checksum。高波特率（921600）下 bit 错误概率上升，错误的 `len` 或 `cmd` 可导致状态机损坏。

**建议：** 在 payload 末尾加 1 字节 XOR 校验和。

### 6. `stripIdx` 在长度校验前已消耗

**文件：** `src/espnow_display.ino:116`

`Serial.read()` 取出 `stripIdx` 后如果 `dataLen` 校验失败，`stripIdx` 已不可恢复。虽然当前未用其做日志，
但违反"先校验再消耗"原则。

**建议：** 先用 `Serial.peek()` 试探，确认长度后再 `read()`。

---

## 🟢 低严重度

### 7. `sendStripFromHost` 逐像素绘制冗余

`drawPixel` 逐像素将字节数组写入 sprite，然后 `pushSprite`。如传图频率高（>1Hz），可考虑 `pushImage` 替代。

### 8. `imageId` 溢出

`uint16_t` 自增，65536 次后归零。接收端用 `imageId` 做跨帧防护，归零时延迟包可能被误接收。

### 9. Python 工具无 strip 计数验证

`send_image_via_serial` 不验证 strip 数是否 = 30。若未来支持非 240 高图片会出问题。

### 10. 接收端不处理重排序包

ESP-NOW 是无序传输，当前去重逻辑（`bi <= lastSeq[si]`）会丢弃晚到但序号更小的包，导致图像空洞。
此为有意取舍（避免重复绘制），但未在文档中说明。

---

## ✅ 好的实践

- **8×240 行缓冲区设计** 仅需 3840 字节而非 115KB，适合 ESP8266 有限 RAM
- `Serial.readBytes` 安全读取（while 循环处理 partial read）
- 接收端同时做了去重 + 跨帧防护（imageId 校验）
- Python 端 RGB565 转换与 TFT_eSPI `color565()` 一致，字节序正确
- ESP-NOW 每块最多 5 次重试，200ms 超时断点避免死等
- `static_assert` 确保包不超过 250 字节 ESP-NOW 限制
- 工具链完整：MAC 发现 → 自动配置 → 发送 → 监测

---

## 📋 总结

| 严重度 | 数量 | 修复建议 |
|--------|------|----------|
| 🔴 高 | 2 | ① 加 strip 级 ACK 流控 ② START 失败中止传图 |
| 🟡 中 | 4 | ① len 边界检查 ② 校验和 ③ stripIdx 先 peek ④ volatile 优化 |
| 🟢 低 | 4 | 按需可改，短期可不处理 |
