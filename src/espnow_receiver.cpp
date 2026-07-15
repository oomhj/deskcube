/**
 * ESP-NOW 图片接收端 (8×8块 + 8×240行缓冲区)
 *
 * 维护 8×240 行缓冲区 Sprite (3840 字节)
 * 每收到一个 8×8 块，写入行缓冲区对应位置
 * 一行 30 块收齐后 pushSprite 到 LCD
 * 无需全图缓冲区
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <TFT_eSPI.h>
#include "espnow_img_proto.h"

typedef struct {
    uint16_t    imageId;
    uint16_t    totalExpected;
    uint16_t    totalReceived;
    uint16_t    currentStrip;
    uint8_t     stripBitmap[BLOCKS_PER_STRIP];  // 当前行收块位图
    unsigned long startTime;
    bool        receiving;
    bool        complete;
} ReceiverState;

static ReceiverState rxState;
static TFT_eSPI *lcd = NULL;
static TFT_eSprite *strip = NULL;   // 8×240 行缓冲区

static void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len);
static void flushStrip(int stripY);

void espnowReceiverInit(TFT_eSPI *tft, uint8_t channel = 1) {
    lcd = tft;

    strip = new TFT_eSprite(lcd);
    strip->createSprite(IMG_WIDTH, STRIP_H);  // 240 × 8

    memset(&rxState, 0, sizeof(rxState));

    WiFi.mode(WIFI_STA);
    wifi_set_channel(channel);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
    esp_now_register_recv_cb(onDataRecv);

    Serial.println("[Receiver] ESP-NOW ready (8x8 blocks, strip buffer)");
    Serial.printf("  Strip: %dx%d (%d bytes)\n",
                  IMG_WIDTH, STRIP_H, STRIP_BUFFER_BYTES);
    Serial.printf("  Total: %d pkts\n", TOTAL_PACKETS);
}

static void flushStrip(int stripY) {
    strip->pushSprite(0, stripY);
    strip->fillSprite(TFT_BLACK);  // 清空缓冲区
}

static void handleDataPacket(EspnowImagePacket *pkt) {
    int stripIdx = pkt->header.stripIdx;
    int blockIdx = pkt->header.blockIdx;

    // 新的一行 → 推送上一行
    if (stripIdx != rxState.currentStrip) {
        if (rxState.currentStrip < TOTAL_STRIPS) {
            flushStrip(rxState.currentStrip * STRIP_H);
        }
        rxState.currentStrip = stripIdx;
        memset(rxState.stripBitmap, 0, sizeof(rxState.stripBitmap));
    }

    rxState.totalReceived++;
    rxState.stripBitmap[blockIdx] = 1;

    // 将 8×8 像素写入行缓冲区
    int destX = blockIdx * BLOCK_H;  // blockIdx × 8

    int idx = 0;
    for (int py = 0; py < BLOCK_W; py++) {
        for (int px = 0; px < BLOCK_H; px++) {
            uint16_t color = pkt->data[idx] | (pkt->data[idx + 1] << 8);
            idx += 2;
            strip->drawPixel(destX + px, py, color);
        }
    }

    // 检查当前行是否收齐
    bool done = true;
    for (int i = 0; i < BLOCKS_PER_STRIP; i++) {
        if (!rxState.stripBitmap[i]) { done = false; break; }
    }
    if (done) {
        flushStrip(stripIdx * STRIP_H);
        int pct = (stripIdx + 1) * 100 / TOTAL_STRIPS;
        Serial.printf("[Receiver] Strip %d/%d (%d%%)\n",
                      stripIdx + 1, TOTAL_STRIPS, pct);
    }
}

static void printStats() {
    float elapsed = (millis() - rxState.startTime) / 1000.0;
    int lost = rxState.totalExpected - rxState.totalReceived;

    Serial.println("\n=== Image Receive Complete ===");
    Serial.printf("Image ID: %u\n", rxState.imageId);
    Serial.printf("Received: %d/%d\n", rxState.totalReceived, rxState.totalExpected);
    Serial.printf("Lost:     %d\n", lost > 0 ? lost : 0);
    Serial.printf("Time:     %.1f s\n", elapsed);
    if (elapsed > 0) {
        float kBps = rxState.totalReceived * BLOCK_DATA_BYTES / elapsed / 1024;
        Serial.printf("Speed:    %.1f KB/s\n", kBps);
    }
}

static void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
    if (len < sizeof(EspnowPacketHeader)) return;
    EspnowPacketHeader *hdr = (EspnowPacketHeader *)data;

    switch (hdr->type) {
        case PKT_IMAGE_START: {
            Serial.println("\n[Receiver] <<< IMAGE START >>>");
            lcd->fillScreen(TFT_BLACK);
            strip->fillSprite(TFT_BLACK);

            rxState.imageId       = hdr->imageId;
            rxState.totalExpected = hdr->total;
            rxState.totalReceived = 0;
            rxState.currentStrip  = 0;
            rxState.receiving     = true;
            rxState.complete      = false;
            rxState.startTime     = millis();
            memset(rxState.stripBitmap, 0, sizeof(rxState.stripBitmap));
            break;
        }

        case PKT_IMAGE_DATA: {
            if (!rxState.receiving) return;
            handleDataPacket((EspnowImagePacket *)data);
            break;
        }

        case PKT_IMAGE_END: {
            if (!rxState.receiving) return;

            // 推送最后一行
            if (rxState.currentStrip < TOTAL_STRIPS) {
                flushStrip(rxState.currentStrip * STRIP_H);
            }

            Serial.println("\n[Receiver] <<< IMAGE END >>>");
            rxState.complete = true;
            rxState.receiving = false;
            printStats();

            lcd->setTextColor(TFT_WHITE, TFT_BLACK);
            lcd->drawString("Transfer Complete!", 4, 2, 2);
            break;
        }

        default:
            Serial.printf("[Receiver] Unknown type: 0x%02X\n", hdr->type);
            break;
    }
}

bool isReceiving()        { return rxState.receiving; }
bool isTransferComplete() { return rxState.complete; }
int  getReceiveProgress() {
    return rxState.totalExpected > 0
        ? (rxState.totalReceived * 100 / rxState.totalExpected)
        : 0;
}
