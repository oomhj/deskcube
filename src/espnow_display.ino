//**********************************************************************
// ESP-NOW JPEG 传输 — 基站
//**********************************************************************
#include "main.h"

#define VERSION "V101"

#if defined(ESPNOW_MODE_RECEIVER)

void setup() {
  Serial.begin(115200); Serial.println();
  tft.init(); tft.setRotation(0); tft.fillScreen(TFT_BLACK);
  espnowReceiverInit(&tft);
}
void loop() { delay(10); }

#elif defined(ESPNOW_MODE_SENDER)

enum { CMD_IMG_START = 0x01, CMD_IMG_END = 0x03,
       CMD_JPG_START = 0x10, CMD_JPG_DATA = 0x11 };

// =====================================================================
// JPEG 解码渲染
// =====================================================================
#include <TJpg_Decoder.h>
#include "jpeg_render.h"

// =====================================================================
// JPEG 接收状态
// =====================================================================
static uint8_t *jpgBuf = NULL;
static int      jpgTotalSize = 0, jpgRecvSize = 0;
static int      jpgChunkRemain = 0;
static unsigned long jpgRecvStart = 0;

static uint16_t g_imgId = 1;
static int      g_totalSent = 0;

void setup() {
  Serial.begin(115200);
  tft.init(); tft.setRotation(0); tft.fillScreen(TFT_BLACK);
  delay(100);

  // ---- 读取 MAC ----
  uint8_t peerMac[6];
  Serial.println("Enter receiver MAC (format: XX:XX:XX:XX:XX:XX):");
  Serial.print("> ");
  int idx = 0, val = 0;
  while (idx < 6) {
    while (!Serial.available()) { delay(10); }
    char c = Serial.read();
    if (c == '\n' || c == '\r') break;
    if (c >= '0' && c <= '9')       val = val * 16 + (c - '0');
    else if (c >= 'a' && c <= 'f')  val = val * 16 + (c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')  val = val * 16 + (c - 'A' + 10);
    else if (c == ':' || c == '-') {
      if (idx < 6) { peerMac[idx++] = val; }
      val = 0;
    }
  }
  if (idx < 6) { peerMac[idx++] = val; }
  if (idx < 6) { memset(peerMac, 0xFF, 6); }
  else {
    Serial.printf("Using MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  peerMac[0], peerMac[1], peerMac[2],
                  peerMac[3], peerMac[4], peerMac[5]);
  }

  espnowSenderInit(peerMac, &tft);
  renderTargetTFT = &tft;
  TJpgDec.setCallback(jpegRenderCallback);

  // =================================================================
  // 主循环
  // =================================================================
  g_imgId = 1; g_totalSent = 0; bool inImage = false;
  enum { S_IDLE, S_JPG_RECV } sState = S_IDLE;
  uint8_t sCmd; uint16_t sLen;

  while (1) {
    switch (sState) {
      case S_IDLE:
        if (Serial.available() < 3) { delay(1); break; }
        sCmd = Serial.read();
        sLen = Serial.read() | (Serial.read() << 8);
        switch (sCmd) {
          case CMD_IMG_START:
            sendStartPacket(g_imgId);
            inImage = true; g_totalSent = 0;
            break;
          case CMD_IMG_END:
            if (!inImage) break;
            sendEndPacket(g_imgId, g_totalSent);
            inImage = false; g_imgId++;
            break;
          case CMD_JPG_START: {
            if (jpgBuf) free(jpgBuf);
            if (sLen < 2) break;
            jpgTotalSize = Serial.read() | (Serial.read() << 8);
            if (jpgTotalSize < 64 || jpgTotalSize > 32768) break;
            jpgBuf = (uint8_t *)malloc(jpgTotalSize);
            if (!jpgBuf) break;
            jpgRecvSize = 0; jpgChunkRemain = 0; jpgRecvStart = millis();
            Serial.printf("jpgsize:%d\n", jpgTotalSize);
            if (sendStartPacket(g_imgId)) {
              inImage = true; g_totalSent = 0; sState = S_JPG_RECV;
            } else { free(jpgBuf); jpgBuf = NULL; }
            break;
          }
          default:
            for (uint16_t i = 0; i < sLen; i++) Serial.read();
            break;
        }
        break;

      case S_JPG_RECV:
        if (jpgChunkRemain == 0) {
          if (Serial.available() < 3) { delay(1); break; }
          uint8_t c = Serial.read();
          uint16_t clen = Serial.read() | (Serial.read() << 8);
          if (c != CMD_JPG_DATA) { free(jpgBuf); jpgBuf = NULL; sState = S_IDLE; break; }
          int need = jpgTotalSize - jpgRecvSize;
          jpgChunkRemain = (clen < need) ? clen : need;
        }
        while (jpgChunkRemain > 0 && Serial.available() && jpgRecvSize < jpgTotalSize) {
          jpgBuf[jpgRecvSize++] = Serial.read();
          jpgChunkRemain--;
        }
        if (jpgRecvSize >= jpgTotalSize) {
          Serial.printf("jpgreceived:%d\n", jpgRecvSize);
          if (jpgBuf) {
            tft.setSwapBytes(true);
            TJpgDec.drawJpg(0, 0, jpgBuf, jpgTotalSize);
            tft.setSwapBytes(false);
            if (sendJpegFile(g_imgId, jpgBuf, jpgTotalSize)) {
              Serial.println("success");
            } else {
              Serial.println("espsendfail");
            }
          }
          free(jpgBuf); jpgBuf = NULL;
          for (int i = 0; i < 30; i++) Serial.write(0x06);
          sState = S_IDLE;
        }
        if (millis() - jpgRecvStart > 30000) {
          free(jpgBuf); jpgBuf = NULL; sState = S_IDLE;
        }
        break;
    }
    yield();
  }
}

void loop() { delay(10); }

#else
void setup() { Serial.begin(115200); }
void loop() { delay(10); }
#endif
