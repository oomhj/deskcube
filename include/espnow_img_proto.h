#ifndef __ESPNOW_IMG_PROTO_H__
#define __ESPNOW_IMG_PROTO_H__

#include <stdint.h>

// =====================================================================
// ESP-NOW 图片传输协议
//
// 图片尺寸:   240 × 240 像素, RGB565 格式
// 行缓冲区:   8 × 240 Sprite (3840 字节), 接收端 RAM
// 传输单元:   8 × 8 像素块 (128 字节/包)
// 渲染对齐:   每块 8×8 正好对应一次 LCD 写入操作
//
// 数据流:
//   strip0: [block0..block29] → flushStrip(y=0)  →
//   strip1: [block0..block29] → flushStrip(y=8)  →
//   ...
//   strip29: ...              → flushStrip(y=232) →
//   总计 30 × 30 = 900 包
// =====================================================================

#define IMG_WIDTH       240
#define IMG_HEIGHT      240

// --- 行缓冲区 (接收端) ---
#define STRIP_H         8
#define STRIP_BUFFER_BYTES (STRIP_H * IMG_WIDTH * 2)  // 3840 字节

// --- 传输块 ---
#define BLOCK_W         8           // 块高 (和 strip 等高)
#define BLOCK_H         8           // 块宽
#define BLOCK_PIXELS    (BLOCK_W * BLOCK_H)        // 64
#define BLOCK_DATA_BYTES (BLOCK_PIXELS * 2)        // 128 字节

#define BLOCKS_PER_STRIP (IMG_WIDTH / BLOCK_H)     // 30
#define TOTAL_STRIPS     (IMG_HEIGHT / BLOCK_W)    // 30
#define TOTAL_PACKETS    (BLOCKS_PER_STRIP * TOTAL_STRIPS)  // 900

// --- ESP-NOW ---
#define ESPNOW_MAX_DATA  250
#define PACKET_HEADER_SIZE 10

// ---------- 包类型 ----------
enum PacketType : uint8_t {
    PKT_IMAGE_START = 0x01,
    PKT_IMAGE_DATA  = 0x02,
    PKT_IMAGE_END   = 0x03,
    PKT_ACK         = 0x04,
    PKT_NACK        = 0x05,
    PKT_JPG_START   = 0x10,
    PKT_JPG_DATA    = 0x11,
    PKT_JPG_END     = 0x12,
};

// JPEG chunk size for ESP-NOW (max 250B per packet, minus 10B header)
#define JPG_CHUNK_DATA_BYTES  239  // 250 - 11B header

// ---------- 数据结构 ----------
#pragma pack(push, 1)

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

// 图片数据包: 10B 包头 + 128B 像素 = 138B
struct EspnowImagePacket {
    EspnowPacketHeader header;
    uint8_t  data[BLOCK_DATA_BYTES];
};

// JPEG 数据包: 10B 包头 + 240B JPEG 数据 = 250B
struct EspnowJpgPacket {
    EspnowPacketHeader header;
    uint8_t  data[JPG_CHUNK_DATA_BYTES];
};

// 控制包
struct EspnowCtrlPacket {
    EspnowPacketHeader header;
    uint16_t param;
};

#pragma pack(pop)

// ---------- 静态校验 ----------
static_assert(sizeof(EspnowImagePacket) <= ESPNOW_MAX_DATA,
              "Packet exceeds ESP-NOW max!");
static_assert(sizeof(EspnowJpgPacket) <= ESPNOW_MAX_DATA,
              "JPG packet exceeds ESP-NOW max!");
static_assert(BLOCK_DATA_BYTES == 128,
              "8x8 block = 128 bytes pixel data");
static_assert(TOTAL_PACKETS == 900,
              "30 strips x 30 blocks = 900 packets");

#endif // __ESPNOW_IMG_PROTO_H__
