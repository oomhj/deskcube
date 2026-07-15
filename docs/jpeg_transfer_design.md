# JPEG 串口传图 + 逐块 ESP-NOW 方案

## 设计目标

宿主机发送 JPEG 压缩图片 → 基站串口接收 → 解码 → 逐 8×8 块 ESP-NOW 发送 → 接收端 LCD 显示

**核心思路：** 传图与投屏完全解耦。串口只负责把 JPEG 文件完整写入内存；写入完成后，解码一个 8×8 块，ESP-NOW 发一个，依次完成。

## 数据流

```
宿主机                               基站
  │                                    │
  │  PIL: 缩放 240×240 → JPEG Q70     │
  │                                    │
  ├─ CMD_JPG_START ──────────────────> │ malloc(totalSize)
  │   payload: [totalSize(2B)]        │
  │                                    │
  ├─ CMD_JPG_DATA (chunk 0) ─────────> │ jpgBuf[pos++] = read()
  ├─ CMD_JPG_DATA (chunk 1) ─────────> │ jpgBuf[pos++] = read()
  │  ...                              │
  ├─ CMD_JPG_DATA (chunk N) ─────────> │ 所有数据收齐
  │                                    │
  │                                    │ ── jd_prepare + jd_decomp ──
  │                                    │     │
  │                                    │     └─ 输出回调（每 tile 一次）
  │                                    │         │
  │                                    │         ├─ 拆成 8×8 子块
  │                                    │         ├─ sendImageBlock() 逐个发
  │                                    │         ├─ 累积到 strip 缓冲区
  │                                    │         └─ strip 30 块满 → ACK
  │ <───────────────────────────── ACK │         (每解完一个 strip 发一次)
  │  ... ×30                           │
  │                                    │
  ├─ CMD_IMG_END ────────────────────> │ sendEndPacket() → ESP-NOW END
```

## 两种模式对比

| | RGB565 裸传 | JPEG 压缩传 |
|---|---|---|
| 串口数据量 | 115 KB | ~6-8 KB (Q70) |
| 串口传输 (115200) | ~10 s | ~0.6 s |
| 解码 | 不需要 | TJpg_Decoder ~0.5-1s |
| ESP-NOW 发送 | 队列异步 ~1.3s | 回调同步逐块 ~1-5s |
| 总耗时 | ~11 s | ~2-3 s (解码+发送重叠) |
| 内存开销 | stripQ 15KB + recvBuf 4KB | jpgBuf ~8KB + accum 8KB |
| 基站 LCD | 实时显示 | 逐 strip 显示 |

## 协议扩展

| 命令 | 值 | payload | 说明 |
|------|-----|---------|------|
| `CMD_JPG_START` | `0x10` | `[totalSize(2B)]` | 开始 JPEG 传输 |
| `CMD_JPG_DATA` | `0x11` | `[chunk(≤512B)]` | JPEG 数据分片 |

复用 `CMD_IMG_START(0x01)` / `CMD_IMG_END(0x03)` 作为 ESP-NOW 的 START/END 控制包。

## 基站实现

### 接收阶段

```
S_IDLE ──CMD_JPG_START──> S_JPG_RECV
                                │
                          [串口收数据 → jpgBuf]
                                │
                          [全量收齐]
                                │
                          jd_prepare + jd_decomp
                                │
                          S_IDLE
```

```cpp
case CMD_JPG_START: {
    jpgTotalSize = read16();
    jpgBuf = (uint8_t *)malloc(jpgTotalSize);
    jpgRecvSize = 0;
    sState = S_JPG_RECV;
    sendStartPacket(g_imgId);   // ESP-NOW START
}

case S_JPG_RECV: {
    while (jpgRecvSize < jpgTotalSize && Serial.available())
        jpgBuf[jpgRecvSize++] = Serial.read();
    if (jpgRecvSize >= jpgTotalSize) {
        TJpgDec.drawJpg(0, 0, jpgBuf, jpgTotalSize);
        free(jpgBuf);
        sState = S_IDLE;
    }
}
```

### 解码 + 发送阶段

TJpg_Decoder 输出回调拆解解码后的 tile，按 8×8 块逐个 ESP-NOW 发送：

```cpp
static bool jpegOutput(x, y, w, h, bitmap) {
    // bitmap = RGB565 tile (16×16 或 8×8)
    for (by in 0..h step 8)
        for (bx in 0..w step 8)
            si = (y+by)/8;          // strip 序号
            bi = (x+bx)/8;          // block 序号

            // 提取 8×8 子块 → block[]
            sendImageBlock(imgId, si, bi, block);

            // 累积到 strip 缓冲区
            slot = getSlot(si);
            accumMap[slot] |= (1 << bi);
            memcpy(accum[slot] + position, pixels, 16 bytes);

    // tile 行结束 → flush 完成的 strip
    if (x + w >= IMG_WIDTH)
        for each slot
            if (accumMap[slot] == 0x3FFFFFFF)  // 30 bits all 1
                displayStrip(si, accum);
                Serial.write(0x06);    // ACK
```

### sendImageBlock

直接发送一个 8×8 块的 ESP-NOW 数据包（同步，带 3 次重试）：

```cpp
bool sendImageBlock(imageId, stripIdx, blockIdx, blockPixels) {
    EspnowImagePacket pkt;
    pkt.header = { PKT_IMAGE_DATA, imageId, seq, total,
                   stripIdx, blockIdx, 8, 8 };
    memcpy(pkt.data, blockPixels, 128);

    for (int r = 0; r < 3; r++)
        if (sendPacket(&pkt, sizeof(pkt))) return true;
    return false;
}
```

### 双 strip 累积器

16×16 MCU tile 跨越 2 个 strip（top 8 行 → strip N, bot 8 行 → strip N+1）。
用 2 个累积器 `jpgAccum[2]` + 30bit 位图追踪每个 strip 收到的块：

| 字段 | 说明 |
|------|------|
| `jpgAccum[i][3840]` | strip 的像素数据 |
| `jpgAccumIdx[i]` | 对应的 strip 序号（-1=空） |
| `jpgAccumMap[i]` | 30bit 位图，bit n=1 表示 block n 已收到 |

当 `jpgAccumMap[i] == 0x3FFFFFFF`（30 bit 全 1），该 strip 完整 → display + ACK。

## 内存布局

```
静态 RAM (64912/81920 = 79.2%):

  stripQ[4][3840]      = 15360  环形队列（仅 RGB565 模式用）
  jpgAccum[2][3840]    = 7680   双 strip 累积器
  recvBuf[3840]        = 3840   串口接收缓冲
  as.pixels[3840]      = 3840   ESP-NOW 异步发送副本
  jpgBuf (heap)        ≈ 8000   JPEG 数据缓冲 (malloc/free)
  Serial RX (heap)     = 4096   硬件串口环形缓冲
  TJpg work (heap)     ≈ 3100   解码器工作区 (malloc in library)
  TFT_eSPI + 其他      ≈ 18000
```

> JPEG 解码期间总堆需求 ≈ 8KB(jpgBuf) + 3KB(TJpg) + 4KB(Serial) ≈ 15KB
> 当前剩余堆 ≈ 17KB，足够

## 文件位置

| 文件 | 说明 |
|------|------|
| `src/espnow_display.ino` | 主循环、串口状态机、JPEG 接收、解码回调 |
| `src/espnow_sender.cpp` | `sendImageBlock()` / `sendPacket()` 等发送函数 |
| `tools/send.py` | 宿主机：自动缩放 → JPEG 编码 → 串口发送 |
| `docs/jpeg_streaming.md` | 原始可行性分析 |
