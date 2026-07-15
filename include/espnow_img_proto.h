#ifndef __ESPNOW_IMG_PROTO_H__
#define __ESPNOW_IMG_PROTO_H__

#include <stdint.h>

// =====================================================================
// ESP-NOW JPEG 传输协议
//
// 图片尺寸:   240 × 240 像素
// 基站:       串口接收 JPEG → ESP-NOW 分片转发
// 接收机:     重组 JPEG → TJpg_Decoder 解码 → LCD 显示
// ESP-NOW 分片: 239 字节数据 + 11 字节头 = 250 字节/包
//
// 包类型:
//   PKT_JPG_START  — JPEG 文件开始, param=总字节数
//   PKT_JPG_DATA   — JPEG 数据分片, seq=分片序号
//   PKT_JPG_END    — JPEG 传输结束
//   PKT_IMAGE_START/END — 兼容控制包 (seq=总包数)
// =====================================================================

#define IMG_WIDTH       240
#define IMG_HEIGHT      240
#define TOTAL_STRIPS    30   // 每张图 30 个 strip ACK

// --- ESP-NOW ---
#define ESPNOW_MAX_DATA  250

// JPEG chunk size for ESP-NOW: 250B - 11B header
#define JPG_CHUNK_DATA_BYTES  239

// ---------- 包类型 ----------
enum PacketType : uint8_t {
    PKT_IMAGE_START = 0x01,
    PKT_IMAGE_DATA  = 0x02,
    PKT_IMAGE_END   = 0x03,
    PKT_JPG_START   = 0x10,
    PKT_JPG_DATA    = 0x11,
    PKT_JPG_END     = 0x12,
};

// ---------- 数据结构 ----------
#pragma pack(push, 1)

struct EspnowPacketHeader {
    uint8_t  type;
    uint16_t imageId;
    uint16_t seq;
    uint16_t total;
    uint8_t  stripIdx;
    uint8_t  blockIdx;
    uint8_t  w;
    uint8_t  h;
};

// JPEG 数据包: 11B 头 + 239B JPEG 数据 = 250B
struct EspnowJpgPacket {
    EspnowPacketHeader header;
    uint8_t  data[JPG_CHUNK_DATA_BYTES];
};

// 控制包: 11B 头 + 2B 参数
struct EspnowCtrlPacket {
    EspnowPacketHeader header;
    uint16_t param;
};

#pragma pack(pop)

// ---------- 静态校验 ----------
static_assert(sizeof(EspnowJpgPacket) <= ESPNOW_MAX_DATA,
              "JPG packet exceeds ESP-NOW max!");

#endif // __ESPNOW_IMG_PROTO_H__
