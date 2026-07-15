# JPEG 串口传图 + 逐块 ESP-NOW 方案

## 设计目标

宿主机发送 JPEG 压缩图片 → 基站串口完整写入内存 → 解码一个 8×8 块→ ESP-NOW 发一个 → 接收端 LCD 显示

**核心思路：** 串口只负责 JPEG 数据传到内存，不通 ESP-NOW。完整接收后，解码与发送交替进行。

---

## 分层架构

```
┌─────────────────────────────────────────────────────────┐
│                    串口层 (UART 115200)                   │
│  只负责：CMD_JPG_START → 收字节 → jpgBuf 收满            │
│  不涉及：ESP-NOW、LCD、解码                               │
└───────────────────────┬─────────────────────────────────┘
                        │ jpgBuf 收齐
                        ▼
┌─────────────────────────────────────────────────────────┐
│                    解码发送层                              │
│  TJpg_Decoder 解码 → 输出回调                             │
│  每收到一个 tile → 拆 8×8 子块                              │
│  sendImageBlock() 逐个 ESP-NOW 发送                       │
│  strip 30 块收齐 → displayStrip() + ACK                   │
└─────────────────────────────────────────────────────────┘
```

---

## 数据流

```
宿主机                               基站
  │                                    │
  │  (已处理为 240×240 JPEG)           │
  │                                    │
  ├─ CMD_JPG_START ──────────────────> │ ── 串口层 ──
  │   payload: [totalSize(2B)]        │
  │                                    │ malloc(totalSize)
  │                                    │ 校验: 64~32768
  │                                    │ qHead=0, qTail=0
  │                                    │ sState = S_JPG_RECV
  │                                    │
  ├─ CMD_JPG_DATA (chunk 0) ─────────> │ while (Serial.available())
  ├─ CMD_JPG_DATA (chunk 1) ─────────> │   jpgBuf[pos++] = read()
  │  ...                              │
  ├─ CMD_JPG_DATA (chunk N) ─────────> │ pos >= totalSize
  │                                    │ ── 收齐 ──
  │                                    │
  │                                    │ ── 解码发送层 ──
  │                                    │ drawJpg(jpgBuf)
  │                                    │   ↓ 输出回调 ×N
  │                                    │   ├─ 拆 8×8 block
  │                                    │   ├─ sendImageBlock()
  │                                    │   ├─ 累积到 accum[2]
  │                                    │   └─ strip 30块 → ACK
  │ <───────────────────────────── ACK │ (每 strip 一次, ×30)
  │  ...                               │
  │                                    │ free(jpgBuf)
  │                                    │ sState = S_IDLE
  │                                    │
  ├─ CMD_IMG_END ────────────────────> │ sendEndPacket()
  │                                    │ → ESP-NOW END
```

---

## 串口层实现（只负责传到内存）

### 状态机

```
S_IDLE ──CMD_JPG_START──> S_JPG_RECV
                                │
                    30秒超时? → free + S_IDLE
                                │
                    收字节 → jpgBuf
                                │
                    全量收齐 → 跳转解码
```

### CMD_JPG_START 处理

```cpp
case CMD_JPG_START: {
    // 1. 读取总大小
    jpgTotalSize = Serial.read() | (Serial.read() << 8);

    // 2. 范围校验（240×240 JPEG Q100 ≈ 30KB）
    if (jpgTotalSize < 64 || jpgTotalSize > 32768) break;

    // 3. 分配堆内存
    jpgBuf = (uint8_t *)malloc(jpgTotalSize);
    if (!jpgBuf) break;

    // 4. 清空可能残留的 RGB565 队列
    qHead = 0; qTail = 0;

    // 5. 准备接收
    jpgRecvSize = 0;
    jpgRecvStart = millis();
    sState = S_JPG_RECV;

    // 6. ESP-NOW START 通知接收端
    sendStartPacket(g_imgId);
}
```

### S_JPG_RECV 状态

```cpp
case S_JPG_RECV: {
    // 超时保护：30 秒没收齐则放弃
    if (millis() - jpgRecvStart > 30000) {
        free(jpgBuf);
        sState = S_IDLE;
        break;
    }

    // 非阻塞式读取串口可用字节
    while (jpgRecvSize < jpgTotalSize && Serial.available())
        jpgBuf[jpgRecvSize++] = Serial.read();

    // 收齐 → 跳转到解码发送
    if (jpgRecvSize >= jpgTotalSize) {
        drawJpg_and_send();   // 见解码发送层
        free(jpgBuf);
        sState = S_IDLE;
    }
}
```

### 安全措施

| 措施 | 说明 |
|------|------|
| 尺寸上下限 | `64 ≤ totalSize ≤ 32768`，防异常值 |
| malloc 失败检查 | NULL 时回退 S_IDLE |
| 接收超时 | 30 秒无进展自动放弃 |
| 队列清空 | JPEG 开始前 qHead=qTail=0 |
| 重复 START 保护 | 已有 jpgBuf 时先 free 再分配 |

---

## 解码发送层

### 入口

```cpp
void drawJpg_and_send() {
    jpgDecoding = true;
    JRESULT jr = TJpgDec.drawJpg(0, 0, jpgBuf, jpgTotalSize);
    jpgDecoding = false;

    if (jr != JDR_OK)
        Serial.printf("JPEG decode ERROR: %d\n", jr);
}
```

### 输出回调：tile → 8×8 块 → ESP-NOW

```cpp
static bool jpegOutput(x, y, w, h, bitmap) {
    // bitmap = RGB565 tile (16×16 或 8×8)
    for (by in 0..h step 8)
        for (bx in 0..w step 8)
            si = (y+by)/8;          // strip 索引
            bi = (x+bx)/8;          // block 索引

            // 提取 8×8 子块 → ESP-NOW 发送
            extract_block(block, bitmap, by, bx, w);
            sendImageBlock(imgId, si, bi, block);

            // 累积像素到 accum[slot]（供 LCD 显示）
            slot = getSlot(si);        // 双 accum 自动替换已满的
            accumMap[slot] |= (1<<bi);
            copy_pixels(accum[slot], bitmap, x, y, by, bx, w);

    // tile 行结束 → 刷出完整 strip
    if (x + w >= IMG_WIDTH)
        for each accum slot
            if (accumMap[slot] == 0x3FFFFFFF)   // 30 块全齐
                displayStrip(si, accum[slot]);
                Serial.write(0x06);             // ACK ×30
}
```

### sendImageBlock

```cpp
bool sendImageBlock(imageId, stripIdx, blockIdx, blockPixels) {
    EspnowImagePacket pkt;
    pkt.header = { PKT_IMAGE_DATA, imageId, seq,
                   total, stripIdx, blockIdx, 8, 8 };
    memcpy(pkt.data, blockPixels, 128);

    for (int r = 0; r < 3; r++)
        if (sendPacket(&pkt, sizeof(pkt))) return true;
    return false;
}
```

### 双 strip 累积器

16×16 MCU tile 跨越 2 个 strip，用 2 个累积器同时追踪：

```
jpgAccum[0] → strip N     (rows 0-7)
jpgAccum[1] → strip N+1   (rows 8-15)
```

每个累积器带 30bit 位图，bit n 为 1 表示第 n 个 block 已收到。
`0x3FFFFFFF`（30 位全 1）= strip 完整 → display + ACK。

---

## 内存布局

```
静态 RAM (64912/81920 = 79.2%)

  stripQ[4][3840]     = 15360   环形队列（RGB565 模式）
  jpgAccum[2][3840]   = 7680    双 strip 累积器
  recvBuf[3840]       = 3840    串口接收缓冲
  as.pixels[3840]     = 3840    ESP-NOW 异步发送副本
  其他 (TFT, etc)     ≈ 34000

堆 (heap) ≈ 17000

  jpgBuf              ≈ 8000     JPEG 数据 (malloc)
  TJpg work           ≈ 3100     解码器工作区 (malloc in library)
  Serial RX ring      ≈ 4096     硬件串口缓冲
```

> JPEG 解码期间峰值堆需求 ≈ 8KB+3KB+4KB ≈ 15KB < 17KB ✓

---

## 文件

| 文件 | 说明 |
|------|------|
| `src/espnow_display.ino` | 串口状态机 + JPEG 回调 |
| `src/espnow_sender.cpp` | `sendImageBlock()`、`sendPacket()` |
| `tools/send.py` | `--raw-jpeg` 直传；`--jpeg` PIL 处理 |
| `docs/serial_transfer.md` | 完整协议文档 |
