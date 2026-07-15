#ifndef __ESPNOW_IMG_PROTO_H__
#define __ESPNOW_IMG_PROTO_H__

#include <stdint.h>

// =====================================================================
// ESP-NOW 图片传输协议
//
// 图片尺寸: 240 × 240 像素, RGB565 格式
// 传输单元: 8 × 8 像素块 (128 字节像素数据)
// 单包大小: 138 字节 (10 字节包头 + 128 字节数据) < 250 字节上限
// 总包数:   (240/8) × (240/8) = 900 包
// 接收策略: 收到一块立即写 LCD，无需全图缓冲区
// =====================================================================

// ---------- 协议常量 ----------
#define IMG_WIDTH       240
#define IMG_HEIGHT      240

#define BLOCK_SIZE      8                     // 像素块边长
#define BLOCK_COLS      (IMG_WIDTH / BLOCK_SIZE)   // 30
#define BLOCK_ROWS      (IMG_HEIGHT / BLOCK_SIZE)  // 30
#define BLOCK_TOTAL     (BLOCK_COLS * BLOCK_ROWS)  // 900

// 每个块像素数据字节数: 8 × 8 × 2 (RGB565) = 128
#define BLOCK_DATA_BYTES  (BLOCK_SIZE * BLOCK_SIZE * 2)

// ESP-NOW 单包最大有效数据长度
#define ESPNOW_MAX_DATA  250

// 包头大小: imageId(2) + seq(2) + total(2) + x(1) + y(1) + w(1) + h(1) = 10
#define PACKET_HEADER_SIZE  10

// 每包携带的像素数据 = 250 - 10 = 240，但我们的块数据固定 128
#define PACKET_DATA_SIZE    BLOCK_DATA_BYTES  // 128

// ---------- 包类型 ----------
enum PacketType : uint8_t {
    PKT_IMAGE_START = 0x01,   // 图片传输开始
    PKT_IMAGE_DATA  = 0x02,   // 图片数据块
    PKT_IMAGE_END   = 0x03,   // 图片传输结束
    PKT_ACK         = 0x04,   // 接收确认
    PKT_NACK        = 0x05,   // 重传请求
};

// ---------- 数据包头（所有包共用前 10 字节）----------
#pragma pack(push, 1)  // 字节对齐，无填充

struct EspnowPacketHeader {
    uint8_t  type;          // PacketType
    uint16_t imageId;       // 图片 ID，区分不同图片
    uint16_t seq;           // 包序号 (0 ~ total-1)
    uint16_t total;         // 总包数
    uint8_t  x;             // 块 X 坐标 (块单位, 0~29)
    uint8_t  y;             // 块 Y 坐标 (块单位, 0~29)
    uint8_t  w;             // 块宽度 (块单位)
    uint8_t  h;             // 块高度 (块单位)
};

// ---------- 图片数据包 ----------
// 包头(10B) + 像素数据(128B) = 138B < 250B ✓
struct EspnowImagePacket {
    EspnowPacketHeader header;
    uint8_t  data[BLOCK_DATA_BYTES];  // 8×8 RGB565 像素数据
};

// ---------- 控制包（开始/结束/ACK/NACK）----------
struct EspnowCtrlPacket {
    EspnowPacketHeader header;
    uint16_t param;         // 附加参数
};

#pragma pack(pop)

// ---------- 校验：确保不超过 ESP-NOW 上限 ----------
static_assert(sizeof(EspnowImagePacket) <= ESPNOW_MAX_DATA,
              "EspnowImagePacket too large!");

#endif // __ESPNOW_IMG_PROTO_H__
