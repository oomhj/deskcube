/**
 * ESP-NOW 图片发送端 (8×8块 + LCD 本地显示)
 *
 * 生成渐变彩条，一边在本地 LCD 显示，一边通过 ESP-NOW 发送。
 * 无需全图缓冲区，仅用 8×240 行缓冲区 (3840 字节)。
 *
 * 异步双缓冲模式：
 *   LCD 推完后立即 ACK，ESP-NOW 发送期间通过 yield 循环
 *   同时接收下一行串口数据，串口传输被覆盖在 ESP-NOW 延迟中。
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <TFT_eSPI.h>
#include "espnow_img_proto.h"

static TFT_eSPI *lcd = NULL;
static TFT_eSprite *strip = NULL;   // 8×240 行缓冲区，同时用于显示和发送

// 对端 MAC（单播用）
static uint8_t peerAddr[6];

static volatile bool sendDone = false;
static volatile bool sendSuccess = false;

// ----- 异步串口接收缓冲（ESP-NOW 等待期间回填）-----
static uint8_t *g_recvBuf = NULL;
static int *g_recvPos = NULL;
static int g_recvMax = 0;

void setRecvBuffer(uint8_t *buf, int *pos, int max) {
    g_recvBuf = buf; g_recvPos = pos; g_recvMax = max;
}

void clearRecvBuffer() {
    g_recvBuf = NULL; g_recvPos = NULL; g_recvMax = 0;
}

static void onDataSent(uint8_t *mac_addr, uint8_t status) {
    sendDone = true;
    sendSuccess = (status == 0);
}

void espnowSenderInit(const uint8_t *peerMac, TFT_eSPI *tft, uint8_t channel) {
    lcd = tft;

    // 创建 8×240 行缓冲区
    strip = new TFT_eSprite(lcd);
    strip->createSprite(IMG_WIDTH, STRIP_H);  // 240 × 8

    WiFi.mode(WIFI_STA);
    wifi_set_channel(channel);

    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
    esp_now_register_send_cb(onDataSent);
    esp_now_add_peer((uint8_t *)peerMac, ESP_NOW_ROLE_SLAVE, channel, NULL, 0);

    // 保存对端 MAC 供 sendPacket 单播使用
    memcpy(peerAddr, peerMac, 6);

    Serial.println("[Sender] ESP-NOW initialized (async mode)");
    Serial.printf("  MAC: %s\n", WiFi.macAddress().c_str());
    Serial.printf("  Peer: %02x:%02x:%02x:%02x:%02x:%02x\n",
                  peerMac[0], peerMac[1], peerMac[2],
                  peerMac[3], peerMac[4], peerMac[5]);
    Serial.printf("  Block: %dx%d (%d B/pkt), Total: %d pkts\n",
                  BLOCK_W, BLOCK_H, BLOCK_DATA_BYTES, TOTAL_PACKETS);
}

// ----- sendPacket：发送并等待 ESP-NOW ACK，同时读取串口到接收缓冲 -----
static bool sendPacket(uint8_t *data, int len) {
    sendDone = false;
    esp_now_send(peerAddr, data, len);
    unsigned long start = millis();
    while (!sendDone) {
        if (millis() - start > 200) {
            sendSuccess = false;
            break;
        }
        // 在等待 ESP-NOW ACK 期间，把串口到达的数据收进异步缓冲区
        if (g_recvBuf && g_recvPos && g_recvMax &&
            *g_recvPos < g_recvMax && Serial.available()) {
            g_recvBuf[(*g_recvPos)++] = Serial.read();
        }
        yield();
    }
    return sendSuccess;
}

// =====================================================================
// 自测模式：生成渐变彩条并发送（仅启用 ESPNOW_SELF_TEST 时编译）
// =====================================================================
#ifdef ESPNOW_SELF_TEST

static void fillStrip(uint16_t imageId, int stripIdx) {
    int baseY = stripIdx * STRIP_H;
    for (int py = 0; py < STRIP_H; py++) {
        for (int px = 0; px < IMG_WIDTH; px++) {
            int y = baseY + py;
            int x = px;
            // 水平 R 渐变, 垂直 G 渐变, 对角 B 渐变
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

    for (int stripIdx = 0; stripIdx < TOTAL_STRIPS; stripIdx++) {
        // 生成这一行的渐变像素
        fillStrip(imageId, stripIdx);

        // 推送到本地 LCD 显示
        strip->pushSprite(0, stripIdx * STRIP_H);

        // 逐块发送
        for (int blockIdx = 0; blockIdx < BLOCKS_PER_STRIP; blockIdx++) {
            memset(&pkt, 0, sizeof(pkt));
            pkt.header.type    = PKT_IMAGE_DATA;
            pkt.header.imageId = imageId;
            pkt.header.seq     = stripIdx * BLOCKS_PER_STRIP + blockIdx;
            pkt.header.total   = TOTAL_PACKETS;
            pkt.header.stripIdx = stripIdx;
            pkt.header.blockIdx = blockIdx;
            pkt.header.w       = BLOCK_W;
            pkt.header.h       = BLOCK_H;

            // 从 strip 缓冲区中拷贝 8×8 像素
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

#endif // ESPNOW_SELF_TEST

// =====================================================================
// 宿主机模式 — 异步双缓冲
// =====================================================================

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

/**
 * 异步发送一个 strip：
 *   1. 将像素填入 sprite → pushSprite 到本地 LCD
 *   2. 立即回复 ACK（宿主机同时开始发下一行）
 *   3. ESP-NOW 逐块发送，同时通过 setRecvBuffer 接收下一行串口数据
 *
 * @param recvBuf  非空时，ESP-NOW 等待期间将串口数据填至此缓冲区
 * @param recvPos  输入时置 0，返回时填入已接收的字节数
 * @param recvMax  缓冲区容量
 * @return 成功发送的块数
 */
int sendStripFromHost(uint16_t imageId, int stripIdx, const uint8_t *pixels,
                      uint8_t *recvBuf, int *recvPos, int recvMax) {
    // ---- Phase 1: 液晶显示（立即）----
    int idx = 0;
    for (int py = 0; py < STRIP_H; py++) {
        for (int px = 0; px < IMG_WIDTH; px++) {
            uint16_t c = pixels[idx] | (pixels[idx + 1] << 8);
            idx += 2;
            strip->drawPixel(px, py, c);
        }
    }
    strip->pushSprite(0, stripIdx * STRIP_H);
    // 提前 ACK：宿主机收到后开始发下一行
    Serial.write(0x06);

    // ---- Phase 2: ESP-NOW 发送（与串口接收并行）----
    setRecvBuffer(recvBuf, recvPos, recvMax);

    EspnowImagePacket pkt;
    int sent = 0, retries = 0;

    for (int blockIdx = 0; blockIdx < BLOCKS_PER_STRIP; blockIdx++) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type    = PKT_IMAGE_DATA;
        pkt.header.imageId = imageId;
        pkt.header.seq     = stripIdx * BLOCKS_PER_STRIP + blockIdx;
        pkt.header.total   = TOTAL_PACKETS;
        pkt.header.stripIdx = stripIdx;
        pkt.header.blockIdx = blockIdx;
        pkt.header.w       = BLOCK_W;
        pkt.header.h       = BLOCK_H;

        // 从像素缓冲区读取 8×8 块
        int baseOff = blockIdx * BLOCK_H * 2;
        int outIdx = 0;
        for (int py = 0; py < BLOCK_W; py++) {
            int rowOff = baseOff + py * IMG_WIDTH * 2;
            for (int px = 0; px < BLOCK_H; px++) {
                int off = rowOff + px * 2;
                pkt.data[outIdx++] = pixels[off];
                pkt.data[outIdx++] = pixels[off + 1];
            }
        }

        bool ok = false;
        for (int r = 0; r < 3; r++) {
            if (sendPacket((uint8_t *)&pkt, sizeof(pkt))) { ok = true; break; }
            retries++;
            delay(5);
        }
        if (ok) sent++;
    }

    clearRecvBuffer();
    return sent;
}

// 向后兼容：无异步缓冲版本（供自测模式等调用）
int sendStripFromHostSync(uint16_t imageId, int stripIdx, const uint8_t *pixels) {
    int dummyPos = 0;
    return sendStripFromHost(imageId, stripIdx, pixels, NULL, &dummyPos, 0);
}
