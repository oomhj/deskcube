/**
 * ESP-NOW 图片接收端（仅 JPEG 模式）
 *
 * PKT_JPG_START → malloc → PKT_JPG_DATA ×N → 重组 → drawJpg → LCD
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include "espnow_img_proto.h"
#include "jpeg_render.h"

// =====================================================================
// 接收状态
// =====================================================================

typedef struct {
    uint16_t    imageId;
    uint16_t    totalChunks;
    unsigned long startTime;
    bool        receiving;
    bool        complete;
} ReceiverState;

static ReceiverState rxState;
static TFT_eSPI *lcd = NULL;

// JPEG 接收缓冲
static uint8_t *jpgRecvBuf = NULL;
static int      jpgRecvTotal = 0;
static int      jpgRecvChunks = 0;

// 分片去重位图（最大 138 片，5×32=160bit 足够）
static uint32_t jpgChunkSeen[5] = {0};

// =====================================================================
// 前向声明
// =====================================================================

static void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len);
static void printStats();

// =====================================================================
// 初始化
// =====================================================================

void espnowReceiverInit(TFT_eSPI *tft, uint8_t channel) {
    lcd = tft;
    renderTargetTFT = lcd;
    TJpgDec.setCallback(jpegRenderCallback);

    memset(&rxState, 0, sizeof(rxState));

    WiFi.mode(WIFI_STA);
    wifi_set_channel(channel);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
    esp_now_register_recv_cb(onDataRecv);

    Serial.println("[Receiver] ESP-NOW ready (JPEG mode)");
    Serial.printf("  MAC: %s\n", WiFi.macAddress().c_str());
}

// =====================================================================
// JPEG 分片接收
// =====================================================================

static void handleJpgData(EspnowJpgPacket *pkt) {
    if (!rxState.receiving) return;
    if (pkt->header.imageId != rxState.imageId) return;

    uint16_t seq = pkt->header.seq;

    // 去重
    int w = seq / 32, b = seq % 32;
    if (w < 5) {
        if (jpgChunkSeen[w] & (1UL << b)) return;
        jpgChunkSeen[w] |= (1UL << b);
    }

    int offset = seq * JPG_CHUNK_DATA_BYTES;
    int chunkLen = jpgRecvTotal - offset;
    if (chunkLen > JPG_CHUNK_DATA_BYTES) chunkLen = JPG_CHUNK_DATA_BYTES;

    if (jpgRecvBuf && offset + chunkLen <= jpgRecvTotal)
        memcpy(jpgRecvBuf + offset, pkt->data, chunkLen);

    jpgRecvChunks++;

    // 全部分片收齐 → 解码显示
    if (jpgRecvChunks >= rxState.totalChunks && jpgRecvBuf) {
        Serial.printf("\n[JPEG] All %d chunks, decoding %d bytes...\n",
                      jpgRecvChunks, jpgRecvTotal);
        lcd->setSwapBytes(true);
        TJpgDec.drawJpg(0, 0, jpgRecvBuf, jpgRecvTotal);
        lcd->setSwapBytes(false);

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
    Serial.printf("Chunks:   %d\n", jpgRecvChunks);
    Serial.printf("Time:     %.1f s\n", elapsed);
}

// =====================================================================
// ESP-NOW 接收回调
// =====================================================================

static void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
    if (len < sizeof(EspnowPacketHeader)) return;
    EspnowPacketHeader *hdr = (EspnowPacketHeader *)data;

    switch (hdr->type) {

        case PKT_JPG_START: {
            Serial.println("\n[Receiver] <<< JPEG START >>>");
            rxState.imageId     = hdr->imageId;
            rxState.totalChunks = hdr->total;
            rxState.receiving   = true;
            rxState.complete    = false;
            rxState.startTime   = millis();

            EspnowCtrlPacket *cpkt = (EspnowCtrlPacket *)data;
            jpgRecvTotal = cpkt->param;
            jpgRecvChunks = 0;
            memset(jpgChunkSeen, 0, sizeof(jpgChunkSeen));

            if (jpgRecvBuf) free(jpgRecvBuf);
            jpgRecvBuf = (uint8_t *)malloc(jpgRecvTotal);
            if (!jpgRecvBuf) {
                Serial.println("[JPEG] malloc failed!");
                rxState.receiving = false;
            }
            Serial.printf("[JPEG] %d chunks, %d bytes\n",
                          hdr->total, jpgRecvTotal);
            break;
        }

        case PKT_JPG_DATA: {
            handleJpgData((EspnowJpgPacket *)data);
            break;
        }

        case PKT_JPG_END: {
            if (!rxState.receiving) return;
            if (hdr->imageId != rxState.imageId) return;
            Serial.printf("\n[JPEG] END (got %d/%d chunks)\n",
                          jpgRecvChunks, hdr->total);

            if (jpgRecvChunks >= hdr->total && jpgRecvBuf) {
                Serial.printf("[JPEG] Decoding %d bytes...\n", jpgRecvTotal);
                lcd->setSwapBytes(true);
                TJpgDec.drawJpg(0, 0, jpgRecvBuf, jpgRecvTotal);
                lcd->setSwapBytes(false);
            }

            rxState.complete = true;
            rxState.receiving = false;
            if (jpgRecvBuf) { free(jpgRecvBuf); jpgRecvBuf = NULL; }
            printStats();
            break;
        }

        case PKT_CMD: {
            if (len < sizeof(EspnowCmdPacket)) return;
            EspnowCmdPacket *cpkt = (EspnowCmdPacket *)data;
            switch (cpkt->cmd) {
                case CMD_SET_BRIGHTNESS: {
                    if (cpkt->len < 1) break;
                    uint8_t b = cpkt->params[0];
                    if (b > 100) b = 100;
                    // BL pin active LOW: 1→75% ON, 10→100% ON
                    if (b < 1) b = 1; if (b > 10) b = 10;
                    int pwm = (10 - b) * 204 / 9;
                    analogWrite(TFT_BL, pwm);
                    Serial.printf("[CMD] brightness=%d (PWM=%d)\n", b, pwm);
                    break;
                }
                default:
                    Serial.printf("[CMD] unknown cmd: 0x%02X\n", cpkt->cmd);
                    break;
            }
            break;
        }

        default:
            Serial.printf("[Receiver] Unknown type: 0x%02X\n", hdr->type);
            break;
    }
}

// =====================================================================
// 查询接口（保留兼容）
// =====================================================================

bool isReceiving()        { return rxState.receiving; }
bool isTransferComplete() { return rxState.complete; }
int  getReceiveProgress() {
    return rxState.totalChunks > 0
        ? (jpgRecvChunks * 100 / rxState.totalChunks)
        : 0;
}
