/**
 * ESP-NOW 图片发送端
 *
 * 提供同步 sendPacket 用于 START/END 控制包和 JPEG 分片发送。
 * JPEG 模式下 displayStrip 直写 TFT，不经过 Sprite 中转。
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <TFT_eSPI.h>
#include "espnow_img_proto.h"

// 控制包声明需要的常量（兼容保留）
#define TOTAL_PACKETS  900

static uint8_t peerAddr[6];

static volatile bool sendDone = false;
static volatile bool sendSuccess = false;

static void onDataSent(uint8_t *mac_addr, uint8_t status) {
    sendDone = true;
    sendSuccess = (status == 0);
}

void espnowSenderInit(const uint8_t *peerMac, TFT_eSPI *tft, uint8_t channel) {
    (void)tft;
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
}

// =====================================================================
// 同步发送
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
    EspnowCtrlPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type    = PKT_IMAGE_START;
    pkt.header.imageId = imageId;
    pkt.header.total   = TOTAL_PACKETS;
    return sendPacket((uint8_t *)&pkt, sizeof(pkt));
}

bool sendEndPacket(uint16_t imageId, int sent) {
    EspnowCtrlPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type    = PKT_IMAGE_END;
    pkt.header.imageId = imageId;
    pkt.header.total   = TOTAL_PACKETS;
    pkt.param          = sent;
    return sendPacket((uint8_t *)&pkt, sizeof(pkt));
}

// =====================================================================
// ESP-NOW JPEG 文件发送
// =====================================================================

bool sendJpegFile(uint16_t imageId, const uint8_t *jpgData, int jpgSize) {
    int totalChunks = (jpgSize + JPG_CHUNK_DATA_BYTES - 1) / JPG_CHUNK_DATA_BYTES;

    EspnowCtrlPacket startPkt;
    memset(&startPkt, 0, sizeof(startPkt));
    startPkt.header.type    = PKT_JPG_START;
    startPkt.header.imageId = imageId;
    startPkt.header.total   = totalChunks;
    startPkt.param          = jpgSize;
    if (!sendPacket((uint8_t *)&startPkt, sizeof(startPkt))) return false;

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

    EspnowCtrlPacket endPkt;
    memset(&endPkt, 0, sizeof(endPkt));
    endPkt.header.type    = PKT_JPG_END;
    endPkt.header.imageId = imageId;
    endPkt.header.total   = totalChunks;
    endPkt.param          = jpgSize;
    return sendPacket((uint8_t *)&endPkt, sizeof(endPkt));
}

