# JPEG 流式解码 + ESP-NOW 投屏方案

## 目标

宿主机将图片压缩为 JPEG，通过串口流式发送给基站，
基站用 TJpg_Decoder 边收边解，解出的像素入队后异步 ESP-NOW 转发。

## 数据量对比

| 方式 | 240×240 数据量 | 115200 传输 | 921600 传输 |
|------|---------------|-------------|-------------|
| RGB565 裸传 | 115 KB | ~10 s | ~2 s |
| JPEG Q70 | ~8 KB | ~0.7 s | ~0.15 s |
| JPEG Q90 | ~15 KB | ~1.3 s | ~0.25 s |

## 性能收益

```
当前(115200):   串口 10s  ESP-NOW 1.3s  →  总 ~11s
JPEG(115200):   串口 0.7s  解码 1s  ESP-NOW 1.3s  →  总 ~2.4s
                              ↑  串口与解码重叠
```

串口传输不再是瓶颈，瓶颈转移到 TJpg_Decoder 的解码速度（~1s/帧）。

## 内存分析

当前 RAM 使用 63.8KB / 82KB（77.9%），剩余约 18KB。

### 存储策略选型

| 策略 | 额外 RAM | 优点 | 缺点 |
|------|---------|------|------|
| **流式解码** | ~2KB | RAM 友好 | 实现复杂 |
| 整图缓冲再解 | +8~15KB | 实现简单 | RAM 余量仅 3~10KB，太紧 |

**结论：采用流式解码。**

TJpg_Decoder 工作区通过 `tjd_set_workarea()` 分配，推荐 2KB 足够。

## 架构设计

### 数据流

```
宿主机                              基站
  │                                    │
  │  PIL: RGB→JPEG(Q70)               │
  │  分片: 512B/pkt                    │
  │                                    │
  ├─ CMD_JPG_START ──────────────────> │ 准备 JPEG 接收
  │   payload: [totalLen(2B)]          │
  │                                    │
  ├─ CMD_JPG_DATA ────────────────────> │ 写入字节 → TJpg_Decoder 输入回调
  │   payload: [chunk(512B)]           │  → 输出回调 → 像素填 stripBuf
  │  ...                               │  → strip 满 → LCD → 入队 → ACK
  │ <───────────────────────────── ACK │
  │                                    │
  └─ CMD_IMG_END ────────────────────> │ 等队列排空 → ESP-NOW END
```

### 协议扩展

新命令（复用现有协议栈）：

| 命令 | 值 | payload | 说明 |
|------|-----|---------|------|
| `CMD_JPG_START` | `0x10` | `[totalLen(2B)]` | 开始 JPEG 传输，总字节数 |
| `CMD_JPG_DATA` | `0x11` | `[chunk(≤512B)]` | JPEG 数据分片 |

### 解码回调

```c
// 输入回调：TJpg_Decoder 请求数据时从串口缓冲区读取
UINT jpegIn(JDEC *jd, UINT pos, UINT len) {
    // pos 可忽略（流式），len 为请求字节数
    // 从 serialBuf 拷贝 len 字节返回
}

// 输出回调：TJpg_Decoder 解出一个 MCU（8×8 像素块）
UINT jpegOut(JDEC *jd, void *bitmap, JRECT *rect) {
    // rect->left, top, right, bottom 为 MCU 坐标
    // bitmap 为 8×8 RGB888 像素（或 RGB565，取决于配置）
    // 填入 stripBuf → strip 满 → LCD → 入队
}
```

## 改动清单

| 文件 | 改动 |
|------|------|
| `platformio.ini` | sender 添加 `TJpg_Decoder` 依赖 |
| `include/espnow_img_proto.h` | 添加 `CMD_JPG_START(0x10)` / `CMD_JPG_DATA(0x11)` |
| `src/espnow_display.ino` | 新增 JPEG 接收状态机（可复用电量 S_DATA 模式） |
| `src/espnow_sender.cpp` | 基本不变 |
| `tools/send.py` | 新增 JPEG 发送路径 |
| `docs/serial_transfer.md` | 补充 JPEG 协议说明 |
| `docs/jpeg_streaming.md` | 本方案文档 |

## 风险

- TJpg_Decoder 解码耗时 ~1s，期间主循环被阻塞，ESP-NOW 和队列消费暂停
  → **缓解**：串口输入回调在 yield() 中持续服务；解码完成后再恢复队列消费
- JPEG Q70 质量可能有可见压缩伪影
  → **缓解**：可通过环境变量选择质量（`JPEG_QUALITY=80`）
