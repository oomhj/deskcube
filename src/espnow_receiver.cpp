/**
 * ESP-NOW 图片接收端 (每收到一块立即刷新 LCD)
 *
 * 每收到一个 8×8 块，立即用 Sprite 推送到 LCD 对应位置。
 * 无需行缓冲区，仅用 8×8 Sprite (128 字节)。
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <TFT_eSPI.h>
#include "espnow_img_proto.h"

typedef struct {
    uint16_t    imageId;
    uint16_t    totalExpected;
    uint16_t    totalReceived;
    unsigned long startTime;
    bool        receiving;
    bool        complete;
} ReceiverState;

static ReceiverState rxState;
static TFT_eSPI *lcd = NULL;
static TFT_eSprite *block = NULL;   // 8×8 块缓冲区

// 去重：每行最后收到的 blockIdx
static uint16_t lastSeq[TOTAL_STRIPS];

static void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len);

void espnowReceiverInit(TFT_eSPI *tft, uint8_t channel) {
    lcd = tft;

    // 创建 8×8 块 Sprite (128 字节)
    block = new TFT_eSprite(lcd);
    block->createSprite(BLOCK_W, BLOCK_H);

    memset(&rxState, 0, sizeof(rxState));

    WiFi.mode(WIFI_STA);
    wifi_set_channel(channel);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
    esp_now_register_recv_cb(onDataRecv);

    Serial.println("[Receiver] ESP-NOW ready (instant block push)");
    Serial.printf("  MAC: %s\n", WiFi.macAddress().c_str());
    Serial.printf("  Block: %dx%d, Total: %d pkts\n",
                  BLOCK_W, BLOCK_H, TOTAL_PACKETS);
}

static void handleDataPacket(EspnowImagePacket *pkt) {
    int x = pkt->header.blockIdx * BLOCK_H;
    int y = pkt->header.stripIdx * BLOCK_W;

    rxState.totalReceived++;

    // 解码像素到 8×8 Sprite
    int idx = 0;
    for (int py = 0; py < BLOCK_W; py++) {
        for (int px = 0; px < BLOCK_H; px++) {
            uint16_t color = pkt->data[idx] | (pkt->data[idx + 1] << 8);
            idx += 2;
            block->drawPixel(px, py, color);
        }
    }

    // 立即推送到 LCD
    block->pushSprite(x, y);
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
            rxState.imageId       = hdr->imageId;
            rxState.totalExpected = hdr->total;
            rxState.totalReceived = 0;
            rxState.receiving     = true;
            rxState.complete      = false;
            rxState.startTime     = millis();

            // 重置去重表
            memset(lastSeq, 0xFF, sizeof(lastSeq));
            break;
        }

        case PKT_IMAGE_DATA: {
            if (!rxState.receiving) return;

            EspnowImagePacket *pkt = (EspnowImagePacket *)data;

            // 跨帧防护：丢弃不属于当前帧的延迟包
            if (pkt->header.imageId != rxState.imageId) return;

            uint8_t si = pkt->header.stripIdx;
            uint8_t bi = pkt->header.blockIdx;

            // 去重：同一 strip 内 blockIdx 必须单调递增，重复/乱序包丢弃
            // lastSeq 初始化为 0xFFFF（见 PKT_IMAGE_START memset），
            // 所以首个包（bi=0）满足 lastSeq==0xFFFF 时放行
            if (lastSeq[si] != 0xFFFF && bi <= lastSeq[si]) return;
            lastSeq[si] = bi;

            handleDataPacket(pkt);
            break;
        }

        case PKT_IMAGE_END: {
            if (!rxState.receiving) return;

            // 跨帧防护：丢弃不属于当前帧的延迟 END 包
            if (hdr->imageId != rxState.imageId) return;

            Serial.println("\n[Receiver] <<< IMAGE END >>>");
            rxState.complete = true;
            rxState.receiving = false;
            printStats();
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
