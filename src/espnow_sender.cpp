/**
 * ESP-NOW 图片发送端 (8×8 块)
 *
 * 将 240×240 RGB565 图片切分为 8×8 块，
 * 按行发送：每行 30 个块，共 30 行 = 900 包
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include "espnow_img_proto.h"

static volatile bool sendDone = false;
static volatile bool sendSuccess = false;

static void onDataSent(uint8_t *mac_addr, uint8_t status) {
    sendDone = true;
    sendSuccess = (status == 0);
}

void espnowSenderInit(const uint8_t *peerMac, uint8_t channel = 1) {
    WiFi.mode(WIFI_STA);
    wifi_set_channel(channel);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
    esp_now_register_send_cb(onDataSent);
    esp_now_add_peer((uint8_t *)peerMac, ESP_NOW_ROLE_SLAVE, channel, NULL, 0);

    Serial.println("[Sender] ESP-NOW initialized");
    Serial.printf("  Block: %dx%d (%d B/pkt), Total: %d pkts\n",
                  BLOCK_W, BLOCK_H, BLOCK_DATA_BYTES, TOTAL_PACKETS);
}

static bool sendPacket(uint8_t *data, int len) {
    sendDone = false;
    esp_now_send(NULL, data, len);

    unsigned long start = millis();
    while (!sendDone) {
        if (millis() - start > 100) return false;
        yield();
    }
    return sendSuccess;
}

void sendImage(uint16_t pixels[IMG_HEIGHT][IMG_WIDTH],
               uint16_t imageId = 1,
               int waitMs = 5)
{
    Serial.println("\n========== Sending Image ==========");
    Serial.printf("Image ID: %u, %d strips x %d blocks = %d pkts\n",
                  imageId, TOTAL_STRIPS, BLOCKS_PER_STRIP, TOTAL_PACKETS);

    // --- START ---
    EspnowCtrlPacket startPkt;
    memset(&startPkt, 0, sizeof(startPkt));
    startPkt.header.type    = PKT_IMAGE_START;
    startPkt.header.imageId = imageId;
    startPkt.header.total   = TOTAL_PACKETS;

    if (!sendPacket((uint8_t *)&startPkt, sizeof(startPkt))) {
        Serial.println("[Sender] START failed!");
        return;
    }
    Serial.println("[Sender] START sent OK");
    delay(10);

    // --- DATA ---
    EspnowImagePacket pkt;
    int sent = 0, retries = 0;
    unsigned long t0 = millis();

    for (int strip = 0; strip < TOTAL_STRIPS; strip++) {
        for (int block = 0; block < BLOCKS_PER_STRIP; block++) {
            memset(&pkt, 0, sizeof(pkt));
            pkt.header.type    = PKT_IMAGE_DATA;
            pkt.header.imageId = imageId;
            pkt.header.seq     = strip * BLOCKS_PER_STRIP + block;
            pkt.header.total   = TOTAL_PACKETS;
            pkt.header.stripIdx = strip;
            pkt.header.blockIdx = block;
            pkt.header.w       = BLOCK_W;
            pkt.header.h       = BLOCK_H;

            // 拷贝 8×8 像素
            int idx = 0;
            for (int py = 0; py < BLOCK_W; py++) {
                for (int px = 0; px < BLOCK_H; px++) {
                    uint16_t c = pixels[strip * BLOCK_W + py]
                                        [block * BLOCK_H + px];
                    pkt.data[idx++] = c & 0xFF;
                    pkt.data[idx++] = (c >> 8) & 0xFF;
                }
            }

            bool ok = false;
            for (int r = 0; r < 3; r++) {
                if (sendPacket((uint8_t *)&pkt, sizeof(pkt))) { ok = true; break; }
                retries++;
                delay(2);
            }
            if (ok) sent++;
            delay(waitMs);
        }
        Serial.printf("[Sender] Strip %d/%d (%d%%)\n",
                      strip + 1, TOTAL_STRIPS,
                      (strip + 1) * 100 / TOTAL_STRIPS);
    }

    unsigned long elapsed = millis() - t0;

    // --- END ---
    EspnowCtrlPacket endPkt;
    memset(&endPkt, 0, sizeof(endPkt));
    endPkt.header.type    = PKT_IMAGE_END;
    endPkt.header.imageId = imageId;
    endPkt.header.total   = TOTAL_PACKETS;
    endPkt.param          = sent;
    sendPacket((uint8_t *)&endPkt, sizeof(endPkt));

    Serial.println("========== Send Complete ==========");
    Serial.printf("Sent: %d/%d, Retries: %d\n", sent, TOTAL_PACKETS, retries);
    Serial.printf("Time: %lu ms (%.1f s)\n", elapsed, elapsed / 1000.0);
    if (elapsed > 0) {
        float kBps = sent * BLOCK_DATA_BYTES / (float)elapsed * 1000 / 1024;
        Serial.printf("Data rate: %.1f KB/s\n", kBps);
    }
}
