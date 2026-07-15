/**
 * ESP-NOW 图片发送端 (8×8块 + LCD 本地显示)
 *
 * 提供同步（sendPacket）和异步（beginPollBlock/pollSendBlock）
 * 两种 ESP-NOW 发送方式。异步方式用于串口传图队列模式。
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <TFT_eSPI.h>
#include "espnow_img_proto.h"

static TFT_eSPI *lcd = NULL;
static TFT_eSprite *strip = NULL;   // 8×240 行缓冲区

static uint8_t peerAddr[6];

static volatile bool sendDone = false;
static volatile bool sendSuccess = false;
static volatile int  sendEvents = 0;   // ISR 事件计数器（防竞态）

static void onDataSent(uint8_t *mac_addr, uint8_t status) {
    sendDone = true;
    sendSuccess = (status == 0);
    sendEvents++;
}

void espnowSenderInit(const uint8_t *peerMac, TFT_eSPI *tft, uint8_t channel) {
    lcd = tft;

    strip = new TFT_eSprite(lcd);
    strip->createSprite(IMG_WIDTH, STRIP_H);

    WiFi.mode(WIFI_STA);
    wifi_set_channel(channel);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
    esp_now_register_send_cb(onDataSent);
    esp_now_add_peer((uint8_t *)peerMac, ESP_NOW_ROLE_SLAVE, channel, NULL, 0);

    memcpy(peerAddr, peerMac, 6);

    Serial.println("[Sender] ESP-NOW initialized");
    Serial.printf("  MAC: %s, Peer: %02x:%02x:%02x:%02x:%02x:%02x\n",
                  WiFi.macAddress().c_str(),
                  peerMac[0], peerMac[1], peerMac[2],
                  peerMac[3], peerMac[4], peerMac[5]);
    Serial.printf("  Block: %dx%d (%d B/pkt), Total: %d pkts\n",
                  BLOCK_W, BLOCK_H, BLOCK_DATA_BYTES, TOTAL_PACKETS);
}

// =====================================================================
// 同步发送（用于 START/END 控制包 & 自测模式）
// =====================================================================

static bool sendPacket(uint8_t *data, int len) {
    sendDone = false;
    esp_now_send(peerAddr, data, len);
    unsigned long start = millis();
    while (!sendDone) {
        if (millis() - start > 200) {
            sendSuccess = false;
            break;
        }
        yield();
    }
    return sendSuccess;
}

bool sendStartPacket(uint16_t imageId) {
    EspnowCtrlPacket startPkt;
    memset(&startPkt, 0, sizeof(startPkt));
    startPkt.header.type    = PKT_IMAGE_START;
    startPkt.header.imageId = imageId;
    startPkt.header.total   = TOTAL_PACKETS;
    return sendPacket((uint8_t *)&startPkt, sizeof(startPkt));
}

bool sendEndPacket(uint16_t imageId, int sent) {
    EspnowCtrlPacket endPkt;
    memset(&endPkt, 0, sizeof(endPkt));
    endPkt.header.type    = PKT_IMAGE_END;
    endPkt.header.imageId = imageId;
    endPkt.header.total   = TOTAL_PACKETS;
    endPkt.param          = sent;
    return sendPacket((uint8_t *)&endPkt, sizeof(endPkt));
}

// =====================================================================
// 直接发送一个 8×8 块（JPEG 解码模式用）
// =====================================================================

bool sendImageBlock(uint16_t imageId, int stripIdx, int blockIdx, const uint8_t *blockPixels) {
    EspnowImagePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type     = PKT_IMAGE_DATA;
    pkt.header.imageId  = imageId;
    pkt.header.seq      = stripIdx * BLOCKS_PER_STRIP + blockIdx;
    pkt.header.total    = TOTAL_PACKETS;
    pkt.header.stripIdx = stripIdx;
    pkt.header.blockIdx = blockIdx;
    pkt.header.w        = BLOCK_W;
    pkt.header.h        = BLOCK_H;

    memcpy(pkt.data, blockPixels, BLOCK_DATA_BYTES);

    for (int r = 0; r < 3; r++) {
        if (sendPacket((uint8_t *)&pkt, sizeof(pkt))) return true;
        delay(5);
    }
    return false;
}

// =====================================================================
// ESP-NOW 发送 JPEG 文件（分片发送，接收机负责解码）
// =====================================================================

bool sendJpegFile(uint16_t imageId, const uint8_t *jpgData, int jpgSize) {
    int totalChunks = (jpgSize + JPG_CHUNK_DATA_BYTES - 1) / JPG_CHUNK_DATA_BYTES;

    // START
    EspnowCtrlPacket startPkt;
    memset(&startPkt, 0, sizeof(startPkt));
    startPkt.header.type    = PKT_JPG_START;
    startPkt.header.imageId = imageId;
    startPkt.header.total   = totalChunks;
    startPkt.param          = jpgSize;
    if (!sendPacket((uint8_t *)&startPkt, sizeof(startPkt))) return false;

    // DATA chunks
    EspnowJpgPacket pkt;
    for (int i = 0; i < totalChunks; i++) {
        int offset = i * JPG_CHUNK_DATA_BYTES;
        int chunkLen = jpgSize - offset;
        if (chunkLen > JPG_CHUNK_DATA_BYTES) chunkLen = JPG_CHUNK_DATA_BYTES;

        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type    = PKT_JPG_DATA;
        pkt.header.imageId = imageId;
        pkt.header.seq     = i;
        pkt.header.total   = totalChunks;
        memcpy(pkt.data, jpgData + offset, chunkLen);

        bool ok = false;
        for (int r = 0; r < 3; r++) {
            if (sendPacket((uint8_t *)&pkt, sizeof(pkt))) { ok = true; break; }
            delay(5);
        }
        if (!ok) return false;
    }

    // END
    EspnowCtrlPacket endPkt;
    memset(&endPkt, 0, sizeof(endPkt));
    endPkt.header.type    = PKT_JPG_END;
    endPkt.header.imageId = imageId;
    endPkt.header.total   = totalChunks;
    endPkt.param          = jpgSize;
    return sendPacket((uint8_t *)&endPkt, sizeof(endPkt));
}

// =====================================================================
// LCD 显示（用于串口传图队列模式）
// =====================================================================

void displayStrip(int stripIdx, const uint8_t *pixels) {
    strip->pushImage(0, 0, IMG_WIDTH, STRIP_H, (uint16_t *)pixels);
    strip->pushSprite(0, stripIdx * STRIP_H);
}

// =====================================================================
// 异步 ESP-NOW 块发送（队列模式用）
// =====================================================================

static void buildBlock(uint16_t imageId, int stripIdx, int blockIdx,
                        const uint8_t *pixels, EspnowImagePacket *pkt) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->header.type     = PKT_IMAGE_DATA;
    pkt->header.imageId  = imageId;
    pkt->header.seq      = stripIdx * BLOCKS_PER_STRIP + blockIdx;
    pkt->header.total    = TOTAL_PACKETS;
    pkt->header.stripIdx = stripIdx;
    pkt->header.blockIdx = blockIdx;
    pkt->header.w        = BLOCK_W;
    pkt->header.h        = BLOCK_H;

    int baseOff = blockIdx * BLOCK_H * 2;
    int outIdx = 0;
    for (int py = 0; py < BLOCK_W; py++) {
        int rowOff = baseOff + py * IMG_WIDTH * 2;
        for (int px = 0; px < BLOCK_H; px++) {
            int off = rowOff + px * 2;
            pkt->data[outIdx++] = pixels[off];
            pkt->data[outIdx++] = pixels[off + 1];
        }
    }
}

// 异步发送状态
typedef struct {
    uint8_t  pixels[STRIP_BUFFER_BYTES]; // 像素数据副本（避免队列回绕覆写）
    int      stripIdx;
    int      blockIdx;              // 0~29
    int      retries;               // 当前块已重试次数
    int      sent;                  // 成功发送的块数
    unsigned long blockStart;       // 当前块的开始时间
    bool     blockBusy;             // 是否有块发送正在进行
} AsyncSendState;

static AsyncSendState as = {0};
static int lastSendEvents = 0;     // 上次处理的事件计数

void beginSendStrip(uint16_t imageId, int stripIdx, const uint8_t *pixels) {
    memcpy(as.pixels, pixels, STRIP_BUFFER_BYTES);  // 关键：复制而非保存指针
    as.stripIdx  = stripIdx;
    as.blockIdx  = 0;
    as.retries   = 0;
    as.sent      = 0;
    as.blockBusy = false;
}

bool isSendBusy()   { return as.blockBusy; }
int  getSendCount() { return as.sent; }

/**
 * 轮询异步发送进度。
 * @return  0 = 发送中, 1 = 完成, -1 = 失败（所有块都丢了）
 */
int pollSendStrip(uint16_t imageId) {
    // --- 等待当前块完成（基于事件计数器，防 ISR 竞态）---
    if (as.blockBusy) {
        if (sendEvents != lastSendEvents) {
            // 有新的事件发生
            lastSendEvents = sendEvents;
            if (sendSuccess) {
                as.sent++;
                as.blockIdx++;
                as.retries = 0;
            } else {
                as.retries++;
                if (as.retries >= 3) {
                    as.blockIdx++;  // 跳过此块
                    as.retries = 0;
                }
            }
            as.blockBusy = false;
        } else {
            // 尚未完成，检查超时
            if (millis() - as.blockStart > 200) {
                lastSendEvents = sendEvents;  // 消费可能延迟的旧事件
                as.retries++;
                if (as.retries >= 3) {
                    as.blockIdx++;
                    as.retries = 0;
                }
                as.blockBusy = false;
            } else {
                return 0;  // 仍在等待
            }
        }
    }

    // --- 检查是否全部完成 ---
    if (as.blockIdx >= BLOCKS_PER_STRIP) {
        return (as.sent > 0) ? 1 : -1;
    }

    // --- 发送下一块 ---
    EspnowImagePacket pkt;
    buildBlock(imageId, as.stripIdx, as.blockIdx, as.pixels, &pkt);

    esp_now_send(peerAddr, (uint8_t *)&pkt, sizeof(pkt));
    as.blockStart = millis();
    as.blockBusy  = true;

    return 0;  // 发送中
}

// =====================================================================
// 自测模式（仅 ESPNOW_SELF_TEST）
// =====================================================================
#ifdef ESPNOW_SELF_TEST

static void fillStrip(uint16_t imageId, int stripIdx) {
    int baseY = stripIdx * STRIP_H;
    for (int py = 0; py < STRIP_H; py++) {
        for (int px = 0; px < IMG_WIDTH; px++) {
            int y = baseY + py;
            int x = px;
            uint8_t r = (x * 256 / IMG_WIDTH) & 0xF8;
            uint8_t g = (y * 256 / IMG_HEIGHT) & 0xFC;
            uint8_t b = ((x + y) * 128 / (IMG_WIDTH + IMG_HEIGHT)) & 0xF8;
            uint16_t c = (r << 8) | (g << 3) | (b >> 3);
            strip->drawPixel(x, py, c);
        }
    }
}

void sendImage(uint16_t imageId, int waitMs) {
    Serial.printf("\n========== Sending Image #%u ==========\n", imageId);

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

    int sent = 0, retries = 0;
    unsigned long t0 = millis();

    for (int stripIdx = 0; stripIdx < TOTAL_STRIPS; stripIdx++) {
        fillStrip(imageId, stripIdx);
        strip->pushSprite(0, stripIdx * STRIP_H);

        EspnowImagePacket pkt;
        for (int blockIdx = 0; blockIdx < BLOCKS_PER_STRIP; blockIdx++) {
            memset(&pkt, 0, sizeof(pkt));
            pkt.header.type     = PKT_IMAGE_DATA;
            pkt.header.imageId  = imageId;
            pkt.header.seq      = stripIdx * BLOCKS_PER_STRIP + blockIdx;
            pkt.header.total    = TOTAL_PACKETS;
            pkt.header.stripIdx = stripIdx;
            pkt.header.blockIdx = blockIdx;
            pkt.header.w        = BLOCK_W;
            pkt.header.h        = BLOCK_H;
            // 从 strip sprite 读像素
            int idx = 0;
            for (int py = 0; py < BLOCK_W; py++) {
                for (int px = 0; px < BLOCK_H; px++) {
                    uint16_t c = strip->readPixel(blockIdx * BLOCK_H + px, py);
                    pkt.data[idx++] = c & 0xFF;
                    pkt.data[idx++] = (c >> 8) & 0xFF;
                }
            }

            bool ok = false;
            for (int r = 0; r < 5; r++) {
                if (sendPacket((uint8_t *)&pkt, sizeof(pkt))) { ok = true; break; }
                retries++;
                delay(8);
            }
            if (ok) sent++;
            delay(waitMs);
        }
        Serial.printf("[Sender] Strip %d/%d (%d%%)\n",
                      stripIdx + 1, TOTAL_STRIPS,
                      (stripIdx + 1) * 100 / TOTAL_STRIPS);
    }

    unsigned long elapsed = millis() - t0;

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

#endif // ESPNOW_SELF_TEST
