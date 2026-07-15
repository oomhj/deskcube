/**
 * ESP-NOW 图片接收端
 *
 * 通过 ESP-NOW 接收图片数据包，每收到一个 8×8 块，
 * 立即用 Sprite 缓存区推送到 LCD，无需全图缓冲区。
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <TFT_eSPI.h>
#include "espnow_img_proto.h"

// ---------- 接收状态 ----------
typedef struct {
    uint16_t imageId;           // 当前接收的图片 ID
    uint16_t totalExpected;     // 期望总包数
    uint16_t totalReceived;     // 已接收包数
    uint16_t lastSeq;           // 最后收到的序号（用于丢包检测）
    unsigned long startTime;    // 接收开始时间
    bool receiving;             // 是否正在接收
    bool complete;              // 是否接收完成
    uint8_t bitmap[BLOCK_ROWS][BLOCK_COLS];  // 接收位图（跟踪哪些块已收到）
} ReceiverState;

static ReceiverState rxState;
static TFT_eSPI *lcd = NULL;
static TFT_eSprite *blockSprite = NULL;

// ---------- 回调函数声明 ----------
static void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len);

// ---------- 初始化 ----------
void espnowReceiverInit(TFT_eSPI *tft, uint8_t channel = 1) {
    lcd = tft;

    // 创建 8×8 Sprite 缓存区
    blockSprite = new TFT_eSprite(lcd);
    blockSprite->createSprite(BLOCK_SIZE, BLOCK_SIZE);

    // 清空接收状态
    memset(&rxState, 0, sizeof(rxState));

    // 初始化 ESP-NOW
    WiFi.mode(WIFI_STA);
    wifi_set_channel(channel);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
    esp_now_register_recv_cb(onDataRecv);

    Serial.println("[Receiver] ESP-NOW initialized, waiting for image...");
}

// ---------- 处理图片数据包 ----------
static void handleDataPacket(EspnowImagePacket *pkt) {
    int x = pkt->header.x * BLOCK_SIZE;
    int y = pkt->header.y * BLOCK_SIZE;
    int seq = pkt->header.seq;

    // 标记该块已接收
    if (pkt->header.y < BLOCK_ROWS && pkt->header.x < BLOCK_COLS) {
        rxState.bitmap[pkt->header.y][pkt->header.x] = 1;
    }

    rxState.totalReceived++;
    rxState.lastSeq = seq;

    // 将接收到的像素数据填入 Sprite
    int idx = 0;
    for (int py = 0; py < BLOCK_SIZE; py++) {
        for (int px = 0; px < BLOCK_SIZE; px++) {
            uint16_t color = pkt->data[idx] | (pkt->data[idx + 1] << 8);
            idx += 2;
            blockSprite->drawPixel(px, py, color);
        }
    }

    // 推送到 LCD
    blockSprite->pushSprite(x, y);

    // 可选：画块边框（调试用）
    // lcd->drawRect(x, y, BLOCK_SIZE, BLOCK_SIZE, TFT_WHITE);
}

// ---------- 打印接收统计 ----------
static void printStats() {
    float elapsed = (millis() - rxState.startTime) / 1000.0;
    int lost = rxState.totalExpected - rxState.totalReceived;

    Serial.println("\n=== Image Receive Complete ===");
    Serial.printf("Image ID: %u\n", rxState.imageId);
    Serial.printf("Received: %d/%d\n", rxState.totalReceived, rxState.totalExpected);
    Serial.printf("Lost:     %d\n", lost > 0 ? lost : 0);
    Serial.printf("Time:     %.1f s\n", elapsed);

    if (elapsed > 0) {
        float pps = rxState.totalReceived / elapsed;
        Serial.printf("Speed:    %.0f packets/s\n", pps);
    }

    // 打印未收到的块
    if (lost > 0) {
        Serial.println("Missing blocks:");
        for (int r = 0; r < BLOCK_ROWS; r++) {
            for (int c = 0; c < BLOCK_COLS; c++) {
                if (!rxState.bitmap[r][c]) {
                    Serial.printf("  [%d,%d] seq=%d\n", c, r, r * BLOCK_COLS + c);
                }
            }
        }
    }
}

// ---------- 重置接收状态 ----------
static void resetState() {
    rxState.receiving = false;
    rxState.complete = false;
    rxState.totalReceived = 0;
    rxState.lastSeq = 0;
    memset(rxState.bitmap, 0, sizeof(rxState.bitmap));
}

// ---------- ESP-NOW 接收回调 ----------
static void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
    if (len < sizeof(EspnowPacketHeader)) return;

    EspnowPacketHeader *hdr = (EspnowPacketHeader *)data;

    switch (hdr->type) {
        case PKT_IMAGE_START: {
            EspnowCtrlPacket *pkt = (EspnowCtrlPacket *)data;
            Serial.println("\n[Receiver] <<< IMAGE START >>>");

            // 清屏
            lcd->fillScreen(TFT_BLACK);

            // 初始化接收状态
            rxState.imageId      = pkt->header.imageId;
            rxState.totalExpected = pkt->header.total;
            rxState.totalReceived = 0;
            rxState.receiving     = true;
            rxState.complete      = false;
            rxState.startTime     = millis();
            memset(rxState.bitmap, 0, sizeof(rxState.bitmap));

            Serial.printf("  Image ID: %u, Total packets: %d\n",
                          rxState.imageId, rxState.totalExpected);
            break;
        }

        case PKT_IMAGE_DATA: {
            if (!rxState.receiving) {
                Serial.println("[Receiver] WARN: data packet ignored (not receiving)");
                return;
            }
            EspnowImagePacket *pkt = (EspnowImagePacket *)data;
            handleDataPacket(pkt);
            break;
        }

        case PKT_IMAGE_END: {
            if (!rxState.receiving) return;

            EspnowCtrlPacket *pkt = (EspnowCtrlPacket *)data;
            Serial.println("\n[Receiver] <<< IMAGE END >>>");

            rxState.complete = true;
            rxState.receiving = false;

            printStats();

            // 显示完成信息在 LCD 上（短暂显示）
            lcd->setTextColor(TFT_WHITE, TFT_BLACK);
            lcd->setTextSize(1);
            lcd->drawString("Transfer Complete!", 4, 2, 2);

            break;
        }

        default:
            Serial.printf("[Receiver] Unknown packet type: 0x%02X\n", hdr->type);
            break;
    }
}

// ---------- 查询状态 ----------
bool isReceiving()       { return rxState.receiving; }
bool isTransferComplete(){ return rxState.complete; }
int  getReceiveProgress(){ return rxState.totalExpected > 0 ?
                                  (rxState.totalReceived * 100 / rxState.totalExpected) : 0; }
