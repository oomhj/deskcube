/**
 * ESP-NOW 图片接收端
 *
 * RGB565 模式：每收到一个 8×8 块立即刷新 LCD
 * JPEG 模式：  接收分片 → 重组 → TJpg_Decoder 解码 → 全屏显示
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include "espnow_img_proto.h"

// =====================================================================
// 接收状态
// =====================================================================

typedef struct {
    uint16_t    imageId;
    uint16_t    totalExpected;   // RGB565: 总包数 / JPEG: 总块数
    uint16_t    totalReceived;
    unsigned long startTime;
    bool        receiving;
    bool        complete;
    uint8_t     mode;  // 0=RGB565, 1=JPEG
} ReceiverState;

static ReceiverState rxState;
static TFT_eSPI *lcd = NULL;
static TFT_eSprite *block = NULL;  // 8×8 块缓冲区（RGB565 模式）

// JPEG 接收
static uint8_t *jpgRecvBuf = NULL;
static int      jpgRecvTotal = 0;
static int      jpgRecvChunks = 0;
static int      jpgRecvSize = 0;

// 去重（RGB565 模式）
static uint16_t lastSeq[TOTAL_STRIPS];

// =====================================================================
// 前向声明
// =====================================================================

static void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len);
static void printStats();

// =====================================================================
// TJpg_Decoder 输出回调（JPEG 解码后显示到 LCD）
// =====================================================================

static bool jpgDisplay(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    lcd->pushImage(x, y, w, h, bitmap);
    return true;
}

// =====================================================================
// 初始化
// =====================================================================

void espnowReceiverInit(TFT_eSPI *tft, uint8_t channel) {
    lcd = tft;

    // 8×8 块 Sprite (RGB565 模式)
    block = new TFT_eSprite(lcd);
    block->createSprite(BLOCK_W, BLOCK_H);

    // TJpg_Decoder 输出回调
    TJpgDec.setCallback(jpgDisplay);

    memset(&rxState, 0, sizeof(rxState));

    WiFi.mode(WIFI_STA);
    wifi_set_channel(channel);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
    esp_now_register_recv_cb(onDataRecv);

    Serial.println("[Receiver] ESP-NOW ready (RGB565 + JPEG)");
    Serial.printf("  MAC: %s\n", WiFi.macAddress().c_str());
}

// =====================================================================
// RGB565 块渲染
// =====================================================================

static void handleDataPacket(EspnowImagePacket *pkt) {
    int x = pkt->header.blockIdx * BLOCK_H;
    int y = pkt->header.stripIdx * BLOCK_W;

    rxState.totalReceived++;

    int idx = 0;
    for (int py = 0; py < BLOCK_W; py++) {
        for (int px = 0; px < BLOCK_H; px++) {
            uint16_t color = pkt->data[idx] | (pkt->data[idx + 1] << 8);
            idx += 2;
            block->drawPixel(px, py, color);
        }
    }
    block->pushSprite(x, y);
}

// =====================================================================
// JPEG 块接收
// =====================================================================

static void handleJpgData(EspnowJpgPacket *pkt) {
    if (!rxState.receiving || rxState.mode != 1) return;
    if (pkt->header.imageId != rxState.imageId) return;

    int offset = pkt->header.seq * JPG_CHUNK_DATA_BYTES;
    int chunkLen = jpgRecvTotal - offset;
    if (chunkLen > JPG_CHUNK_DATA_BYTES) chunkLen = JPG_CHUNK_DATA_BYTES;

    if (jpgRecvBuf && offset + chunkLen <= jpgRecvTotal) {
        memcpy(jpgRecvBuf + offset, pkt->data, chunkLen);
        jpgRecvSize += chunkLen;
    }
    jpgRecvChunks++;

    // 所有分片收齐 → 解码显示
    if (jpgRecvChunks >= pkt->header.total && jpgRecvBuf) {
        Serial.printf("\n[JPEG] All chunks received (%d), decoding %d bytes...\n",
                      jpgRecvChunks, jpgRecvTotal);
        TJpgDec.drawJpg(0, 0, jpgRecvBuf, jpgRecvTotal);

        rxState.complete = true;
        rxState.receiving = false;
        free(jpgRecvBuf); jpgRecvBuf = NULL;
        printStats();
    }
}

// =====================================================================
// 统计输出
// =====================================================================

static void printStats() {
    float elapsed = (millis() - rxState.startTime) / 1000.0;

    Serial.println("\n=== Transfer Complete ===");
    Serial.printf("Image ID: %u\n", rxState.imageId);
    Serial.printf("Received: %d\n", rxState.totalReceived);
    Serial.printf("Time:     %.1f s\n", elapsed);
}

// =====================================================================
// ESP-NOW 接收回调
// =====================================================================

static void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
    if (len < sizeof(EspnowPacketHeader)) return;
    EspnowPacketHeader *hdr = (EspnowPacketHeader *)data;

    switch (hdr->type) {

        // ---- RGB565 模式 ----
        case PKT_IMAGE_START: {
            Serial.println("\n[Receiver] <<< RGB565 START >>>");
            rxState.imageId       = hdr->imageId;
            rxState.totalExpected = hdr->total;
            rxState.totalReceived = 0;
            rxState.receiving     = true;
            rxState.complete      = false;
            rxState.mode          = 0;
            rxState.startTime     = millis();
            memset(lastSeq, 0xFF, sizeof(lastSeq));

            if (jpgRecvBuf) { free(jpgRecvBuf); jpgRecvBuf = NULL; }
            break;
        }

        case PKT_IMAGE_DATA: {
            if (!rxState.receiving || rxState.mode != 0) return;
            EspnowImagePacket *pkt = (EspnowImagePacket *)data;
            if (pkt->header.imageId != rxState.imageId) return;

            uint8_t si = pkt->header.stripIdx;
            uint8_t bi = pkt->header.blockIdx;

            if (lastSeq[si] != 0xFFFF && bi <= lastSeq[si]) return;
            lastSeq[si] = bi;

            handleDataPacket(pkt);
            break;
        }

        case PKT_IMAGE_END: {
            if (!rxState.receiving || rxState.mode != 0) return;
            if (hdr->imageId != rxState.imageId) return;
            Serial.println("\n[Receiver] <<< RGB565 END >>>");
            rxState.complete = true;
            rxState.receiving = false;
            printStats();
            break;
        }

        // ---- JPEG 模式 ----
        case PKT_JPG_START: {
            Serial.println("\n[Receiver] <<< JPEG START >>>");
            rxState.imageId       = hdr->imageId;
            rxState.totalExpected = hdr->total;
            rxState.totalReceived = 0;
            rxState.receiving     = true;
            rxState.complete      = false;
            rxState.mode          = 1;
            rxState.startTime     = millis();

            EspnowCtrlPacket *cpkt = (EspnowCtrlPacket *)data;
            jpgRecvTotal = cpkt->param;
            jpgRecvChunks = 0;
            jpgRecvSize = 0;

            if (jpgRecvBuf) free(jpgRecvBuf);
            jpgRecvBuf = (uint8_t *)malloc(jpgRecvTotal);
            if (!jpgRecvBuf) {
                Serial.println("[JPEG] malloc failed!");
                rxState.receiving = false;
            }
            Serial.printf("[JPEG] Expecting %d chunks, %d bytes\n",
                          hdr->total, jpgRecvTotal);
            break;
        }

        case PKT_JPG_DATA: {
            handleJpgData((EspnowJpgPacket *)data);
            break;
        }

        case PKT_JPG_END: {
            if (!rxState.receiving || rxState.mode != 1) return;
            if (hdr->imageId != rxState.imageId) return;
            Serial.printf("\n[JPEG] END (got %d/%d chunks)\n",
                          jpgRecvChunks, hdr->total);

            if (jpgRecvChunks >= hdr->total && jpgRecvBuf) {
                Serial.printf("[JPEG] Decoding %d bytes...\n", jpgRecvTotal);
                TJpgDec.drawJpg(0, 0, jpgRecvBuf, jpgRecvTotal);
            }

            rxState.complete = true;
            rxState.receiving = false;
            if (jpgRecvBuf) { free(jpgRecvBuf); jpgRecvBuf = NULL; }
            printStats();
            break;
        }

        default:
            Serial.printf("[Receiver] Unknown type: 0x%02X\n", hdr->type);
            break;
    }
}

// =====================================================================
// 查询接口
// =====================================================================

bool isReceiving()        { return rxState.receiving; }
bool isTransferComplete() { return rxState.complete; }
int  getReceiveProgress() {
    return rxState.totalExpected > 0
        ? (rxState.totalReceived * 100 / rxState.totalExpected)
        : 0;
}
