//**********************************************************************
// ESP-NOW JPEG 传输 — 基站/接收端
//**********************************************************************
#include "main.h"

#define VERSION "V101"

// ===================== 接收端 =====================
#if defined(ESPNOW_MODE_RECEIVER)

void setup() {
  Serial.begin(115200); Serial.println();
  tft.init(); tft.setRotation(0); tft.fillScreen(TFT_BLACK);
  espnowReceiverInit(&tft);
}
void loop() { delay(10); }

// ===================== 发送端（仅 JPEG 模式）=====================
#elif defined(ESPNOW_MODE_SENDER)

enum { CMD_IMG_START = 0x01, CMD_IMG_END = 0x03,
       CMD_JPG_START = 0x10, CMD_JPG_DATA = 0x11,
       CMD_CMD       = 0x20 };

// 串口协议: [cmd][len_lo][len_hi][payload...][xr]
static void drainSerial(uint16_t len) {
  for (uint16_t i = 0; i < len + 1; i++) Serial.read();
}

// =====================================================================
// JPEG 解码 + 渲染
// =====================================================================
#include <TJpg_Decoder.h>
#include "jpeg_render.h"

// =====================================================================
// JPEG 接收状态
// =====================================================================
static uint8_t *jpgBuf = NULL;
static int      jpgTotalSize = 0;
static int      jpgRecvSize = 0;
static int      jpgChunkRemain = 0;
static uint8_t  jpgChunkXor = 0;    // 当前 CMD_JPG_DATA 帧的 XOR
static unsigned long jpgRecvStart = 0;

static uint16_t g_imgId = 1;
static int      g_totalSent = 0;

void setup() {
  Serial.begin(115200); Serial.println();
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

  if (idx < 6) {
    Serial.println("\nInvalid MAC, using broadcast");
    memset(peerMac, 0xFF, 6);
  } else {
    Serial.printf("\nUsing MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  peerMac[0], peerMac[1], peerMac[2],
                  peerMac[3], peerMac[4], peerMac[5]);
  }

  espnowSenderInit(peerMac, &tft);
  renderTargetTFT = &tft;
  TJpgDec.setCallback(jpegRenderCallback);

  Serial.println("[Base] JPEG only mode");

  // =================================================================
  // 主循环
  // =================================================================
  g_imgId = 1;
  g_totalSent = 0;
  bool inImage = false;

  enum { S_IDLE, S_JPG_RECV } sState = S_IDLE;
  uint8_t sCmd; uint16_t sLen;

  while (1) {
    switch (sState) {
      case S_IDLE:
        if (Serial.available() < 4) { delay(1); break; }  // 3B header + 1B xr
        sCmd = Serial.read();
        sLen = Serial.read() | (Serial.read() << 8);

        switch (sCmd) {
          case CMD_IMG_START: {
            uint8_t xr = Serial.read();
            if (xr != (sCmd ^ (sLen & 0xFF) ^ ((sLen >> 8) & 0xFF))) break;
            Serial.printf("\n=== Image #%u START ===\n", g_imgId);
            if (sendStartPacket(g_imgId))
              { inImage = true; g_totalSent = 0; }
            else Serial.println("  ERROR: START failed");
            break;
          }

          case CMD_IMG_END: {
            uint8_t xr = Serial.read();
            if (xr != (sCmd ^ (sLen & 0xFF) ^ ((sLen >> 8) & 0xFF))) break;
            if (!inImage) break;
            sendEndPacket(g_imgId, g_totalSent);
            Serial.printf("=== Image #%u END, sent: %d ===\n", g_imgId, g_totalSent);
            inImage = false; g_imgId++;
            break;
          }

          case CMD_JPG_START: {
            if (sLen < 2) { drainSerial(sLen); break; }
            jpgTotalSize = Serial.read() | (Serial.read() << 8);
            uint8_t xr = Serial.read();
            uint8_t calc = sCmd ^ (sLen & 0xFF) ^ ((sLen >> 8) & 0xFF)
                           ^ (jpgTotalSize & 0xFF) ^ ((jpgTotalSize >> 8) & 0xFF);
            if (xr != calc) { drainSerial(0); break; }
            if (jpgTotalSize < 64 || jpgTotalSize > 32768) break;
            if (jpgBuf) free(jpgBuf);
            jpgBuf = (uint8_t *)malloc(jpgTotalSize);
            if (!jpgBuf) break;
            jpgRecvSize = 0; jpgChunkRemain = 0; jpgRecvStart = millis();
            if (sendStartPacket(g_imgId)) {
              inImage = true; g_totalSent = 0; sState = S_JPG_RECV;
              Serial.printf("  Receiving %d bytes JPEG...\n", jpgTotalSize);
            } else { free(jpgBuf); jpgBuf = NULL; }
            break;
          }

          case CMD_CMD: {
            if (sLen < 2) { drainSerial(sLen); break; }
            uint8_t cmdId = Serial.read();
            uint8_t cmdLen = Serial.read();
            uint8_t cmdParams[CMD_MAX_PARAMS];
            uint8_t plen = (cmdLen < CMD_MAX_PARAMS) ? cmdLen : CMD_MAX_PARAMS;
            for (int i = 0; i < plen; i++) cmdParams[i] = Serial.read();
            // drain remaining params + xor byte
            for (int i = plen; i < cmdLen + 1; i++) Serial.read();
            sendCmdPacket(g_imgId, cmdId, cmdParams, plen);
            Serial.println("  CMD sent");
            break;
          }

          default:
            drainSerial(sLen);
            Serial.printf("Unknown cmd: 0x%02X\n", sCmd);
            break;
        }
        break;

      case S_JPG_RECV:
        if (millis() - jpgRecvStart > 30000) {
          Serial.printf("  ERROR: recv timeout (%d/%d)\n", jpgRecvSize, jpgTotalSize);
          if (jpgBuf) { free(jpgBuf); jpgBuf = NULL; }
          sState = S_IDLE; break;
        }
        // --- 帧解析 ---
        if (jpgChunkRemain == 0) {
          if (Serial.available() < 4) break;  // 3B header + at least 1B payload/xr
          uint8_t c = Serial.read();
          uint16_t clen = Serial.read() | (Serial.read() << 8);
          if (c != CMD_JPG_DATA) {
            Serial.printf("  ERROR: expected JPG_DATA, got 0x%02X\n", c);
            free(jpgBuf); jpgBuf = NULL; sState = S_IDLE; break;
          }
          int need = jpgTotalSize - jpgRecvSize;
          jpgChunkRemain = (clen < need) ? clen : need;
          jpgChunkXor = c ^ (clen & 0xFF) ^ ((clen >> 8) & 0xFF);
        }
        // --- 读 payload 字节 ---
        while (jpgChunkRemain > 0 && Serial.available() > 1 && jpgRecvSize < jpgTotalSize) {
          uint8_t b = Serial.read();
          jpgBuf[jpgRecvSize++] = b;
          jpgChunkXor ^= b;
          jpgChunkRemain--;
        }
        // --- 校验 XOR（chunk 收完时 serial 还剩 1 字节）---
        if (jpgChunkRemain == 0 && jpgRecvSize < jpgTotalSize && Serial.available() > 0) {
          uint8_t rxXor = Serial.read();
          if (rxXor != jpgChunkXor) {
            Serial.println("  ERROR: JPG_DATA xr mismatch, aborting");
            free(jpgBuf); jpgBuf = NULL; sState = S_IDLE; break;
          }
        }
        // --- 全部收齐 ---
        if (jpgRecvSize >= jpgTotalSize) {
          // 还有未读的 xr 字节（最后一个 chunk 读完时 xr 可能没读）
          if (jpgChunkRemain == 0) Serial.read();  // drain trailing xr
          Serial.printf("  JPEG received (%d bytes)\n", jpgRecvSize);
          if (jpgBuf) {
            tft.setSwapBytes(true);
            TJpgDec.drawJpg(0, 0, jpgBuf, jpgTotalSize);
            tft.setSwapBytes(false);
            bool ok = sendJpegFile(g_imgId, jpgBuf, jpgTotalSize);
            Serial.printf("  ESP-NOW %s\n", ok ? "OK" : "FAILED");
          }
          free(jpgBuf); jpgBuf = NULL;
          for (int i = 0; i < 30; i++) Serial.write(0x06);
          sState = S_IDLE;
        }
        break;
    }
    yield();
  }
}

void loop() { delay(10); }

// ===================== 原始时钟模式 =====================
#else
void setup() {
  Serial.begin(115200); Serial.println();
  Serial.println("No ESP-NOW mode defined.");
  while (1) delay(1000);
}
void loop() { delay(10); }
#endif
