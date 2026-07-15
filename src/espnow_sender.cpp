/**
 * ESP-NOW 图片发送端示例
 *
 * 将一张 240×240 RGB565 图片切分为 8×8 块，
 * 通过 ESP-NOW 逐包发送给接收端。
 *
 * 使用方式：
 *   1. 准备图片数据 (uint16_t pixels[240][240])
 *   2. 调用 sendImage(pixels, peerMac) 发送
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include "espnow_img_proto.h"

// ---------- 回调函数 ----------
static volatile bool sendDone = false;
static volatile bool sendSuccess = false;

static void onDataSent(uint8_t *mac_addr, uint8_t status) {
    sendDone = true;
    sendSuccess = (status == 0);
}

// ---------- 初始化 ESP-NOW ----------
void espnowSenderInit(const uint8_t *peerMac, uint8_t channel = 1) {
    WiFi.mode(WIFI_STA);
    wifi_set_channel(channel);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
    esp_now_register_send_cb(onDataSent);

    // 添加对端
    esp_now_add_peer((uint8_t *)peerMac, ESP_NOW_ROLE_SLAVE, channel, NULL, 0);
}

// ---------- 发送一包并等待结果 ----------
static bool sendPacket(uint8_t *data, int len) {
    sendDone = false;
    esp_now_send(NULL, data, len);  // NULL = 发给所有已添加的 peer

    // 等待发送完成（超时 100ms）
    unsigned long start = millis();
    while (!sendDone) {
        if (millis() - start > 100) {
            Serial.println("[Sender] Send timeout!");
            return false;
        }
        yield();
    }
    return sendSuccess;
}

// ---------- 发送完整图片 ----------
void sendImage(uint16_t pixels[IMG_HEIGHT][IMG_WIDTH],
               uint16_t imageId = 1,
               int waitMs = 5)   // 包间延时
{
    Serial.println("\n========== Sending Image ==========");
    Serial.printf("Image ID: %u, Total packets: %d\n", imageId, BLOCK_TOTAL);

    // --- 发送开始包 ---
    EspnowCtrlPacket startPkt;
    memset(&startPkt, 0, sizeof(startPkt));
    startPkt.header.type    = PKT_IMAGE_START;
    startPkt.header.imageId = imageId;
    startPkt.header.total   = BLOCK_TOTAL;
    startPkt.param          = (IMG_WIDTH << 16) | IMG_HEIGHT;

    if (!sendPacket((uint8_t *)&startPkt, sizeof(startPkt))) {
        Serial.println("[Sender] Failed to send START packet!");
        return;
    }
    Serial.println("[Sender] START sent OK");

    delay(10);

    // --- 发送数据包 ---
    EspnowImagePacket pkt;
    int sentCount = 0;
    int retryCount = 0;

    for (int row = 0; row < BLOCK_ROWS; row++) {
        for (int col = 0; col < BLOCK_COLS; col++) {
            // 填充包头
            memset(&pkt, 0, sizeof(pkt));
            pkt.header.type    = PKT_IMAGE_DATA;
            pkt.header.imageId = imageId;
            pkt.header.seq     = row * BLOCK_COLS + col;
            pkt.header.total   = BLOCK_TOTAL;
            pkt.header.x       = col;
            pkt.header.y       = row;
            pkt.header.w       = BLOCK_SIZE;
            pkt.header.h       = BLOCK_SIZE;

            // 拷贝 8×8 像素数据 (RGB565)
            int idx = 0;
            for (int py = 0; py < BLOCK_SIZE; py++) {
                for (int px = 0; px < BLOCK_SIZE; px++) {
                    int imgX = col * BLOCK_SIZE + px;
                    int imgY = row * BLOCK_SIZE + py;
                    uint16_t c = pixels[imgY][imgX];
                    pkt.data[idx++] = c & 0xFF;
                    pkt.data[idx++] = (c >> 8) & 0xFF;
                }
            }

            // 发送（带重试）
            bool ok = false;
            for (int retry = 0; retry < 3; retry++) {
                if (sendPacket((uint8_t *)&pkt, sizeof(pkt))) {
                    ok = true;
                    break;
                }
                retryCount++;
                delay(2);
            }

            if (!ok) {
                Serial.printf("[Sender] FAILED seq=%d (x=%d,y=%d)\n",
                              pkt.header.seq, col, row);
            } else {
                sentCount++;
            }

            delay(waitMs);  // 包间延时，避免接收端缓冲溢出
        }

        // 每行进度输出
        Serial.printf("[Sender] Row %d/30: %d blocks sent\n",
                      row + 1, (row + 1) * BLOCK_COLS);
    }

    // --- 发送结束包 ---
    EspnowCtrlPacket endPkt;
    memset(&endPkt, 0, sizeof(endPkt));
    endPkt.header.type    = PKT_IMAGE_END;
    endPkt.header.imageId = imageId;
    endPkt.header.total   = BLOCK_TOTAL;
    endPkt.param          = sentCount;

    if (!sendPacket((uint8_t *)&endPkt, sizeof(endPkt))) {
        Serial.println("[Sender] Failed to send END packet!");
    } else {
        Serial.println("[Sender] END sent OK");
    }

    // --- 完成 ---
    Serial.println("========== Send Complete ==========");
    Serial.printf("Sent: %d/%d, Retries: %d\n",
                  sentCount, BLOCK_TOTAL, retryCount);
}
