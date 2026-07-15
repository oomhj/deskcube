//**********************************************************************
// ESP-NOW 图片传输 — 基站/接收端
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

// ===================== 发送端 =====================
#elif defined(ESPNOW_MODE_SENDER)

// 串口命令
enum {
  CMD_IMG_START = 0x01, CMD_STRIP_DATA = 0x02, CMD_IMG_END = 0x03,
  CMD_JPG_START = 0x10, CMD_JPG_DATA  = 0x11,
};

static void drainSerial(uint16_t len) {
  for (uint16_t i = 0; i < len; i++) {
    if (Serial.read() < 0) break;
  }
}

// =====================================================================
// 环形队列
// =====================================================================
#define Q_SIZE  4   // 4 × 3840 = 15 KB; JPEG 解码时堆空间给 JPEG buffer
static uint8_t stripQ[Q_SIZE][STRIP_BUFFER_BYTES];
static int qStripIdx[Q_SIZE];
static volatile int qHead = 0, qTail = 0;

static bool qFull()  { return ((qHead + 1) % Q_SIZE) == qTail; }
static bool qEmpty() { return qHead == qTail; }

static bool qPush(int stripIdx, const uint8_t *data) {
  if (qFull()) return false;
  memcpy(stripQ[qHead], data, STRIP_BUFFER_BYTES);
  qStripIdx[qHead] = stripIdx;
  qHead = (qHead + 1) % Q_SIZE;
  return true;
}

static bool qPop(int *stripIdx, const uint8_t **data) {
  if (qEmpty()) return false;
  if (data)     *data = stripQ[qTail];
  if (stripIdx) *stripIdx = qStripIdx[qTail];
  qTail = (qTail + 1) % Q_SIZE;
  return true;
}

// =====================================================================
// JPEG 解码
// =====================================================================
#include <TJpg_Decoder.h>

// 串口接收缓冲（JPEG 回调也会用到，故提前声明）
static uint8_t recvBuf[STRIP_BUFFER_BYTES];
static int     recvPos = 0, recvIdx = 0;
static uint16_t recvLen = 0;

static uint8_t *jpgBuf = NULL;        // malloc'd JPEG 数据缓冲区
static int      jpgTotalSize = 0;     // JPEG 文件总字节数
static int      jpgRecvSize = 0;      // 已接收字节数
static bool     jpgDecoding = false;  // 正在解码

// JPEG 全局状态
static uint16_t g_imgId = 1;
static int      g_totalSent = 0;

// 当前 strip 的 8×8 块发送进度
static uint8_t jpgStripBuf[STRIP_BUFFER_BYTES];
static int     jpgCurStrip = -1;   // 正在累积的 strip
static int     jpgCurBlocks = 0;   // 已发送块数

static void flushJpgStrip() {
  if (jpgCurStrip < 0) return;
  displayStrip(jpgCurStrip, jpgStripBuf);
  Serial.write(0x06);       // strip 完成 → ACK
  g_totalSent += jpgCurBlocks;
  jpgCurStrip = -1;
  jpgCurBlocks = 0;
}

// TJpg_Decoder 输出回调：收到一个解码后的 tile → 拆成 8×8 块直接 ESP-NOW 发送
static bool jpegOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  // tile 拆成 8×8 的子块
  for (int by = 0; by < h; by += BLOCK_H) {
    for (int bx = 0; bx < w; bx += BLOCK_W) {
      int stripIdx = (y + by) / STRIP_H;
      int blockIdx = (x + bx) / BLOCK_H;

      // 如果换 strip 了，完成上一个
      if (stripIdx != jpgCurStrip && jpgCurStrip >= 0) {
        flushJpgStrip();
      }

      // 初始化新 strip
      if (jpgCurStrip < 0) {
        jpgCurStrip = stripIdx;
        jpgCurBlocks = 0;
        memset(jpgStripBuf, 0, sizeof(jpgStripBuf));
      }

      // 从 bitmap 中提取 8×8 像素块
      uint8_t block[BLOCK_DATA_BYTES];
      int outIdx = 0;
      for (int py = 0; py < BLOCK_H; py++) {
        int tileRow = by + py;        // 在 tile 内的行
        int imgX = x + bx;            // 块在图片中的 X
        // bitmap 的 row = tileRow * w, 每像素 2 字节
        int srcOff = (tileRow * w + bx) * 2;
        memcpy(block + outIdx, (uint8_t *)bitmap + srcOff, BLOCK_H * 2);
        outIdx += BLOCK_H * 2;

        // 同时写入 jpgStripBuf 供 LCD 显示用
        int absY = y + tileRow;
        int stripRelY = absY % STRIP_H;
        memcpy(jpgStripBuf + stripRelY * IMG_WIDTH * 2 + imgX * 2,
               (uint8_t *)bitmap + srcOff, BLOCK_H * 2);
      }

      // 直接发送这个 8×8 块
      sendImageBlock(g_imgId, stripIdx, blockIdx, block);
      jpgCurBlocks++;
    }
  }

  // 最后一列 → 刷出当前 strip
  if (x + w >= IMG_WIDTH) {
    flushJpgStrip();
  }
  return true;
}

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
  TJpgDec.setCallback(jpegOutput);

  Serial.println("[Base] Queue+JPEG mode (Q_SIZE=4, RX_BUF=4096)");

  // =================================================================
  // 主循环
  // =================================================================
  g_imgId = 1;
  g_totalSent = 0;
  bool     inImage    = false;
  bool     endPending = false;

  // 串口接收状态
  enum { S_IDLE, S_DATA, S_JPG_RECV } sState = S_IDLE;
  uint8_t sCmd  = 0;
  uint16_t sLen = 0;

  while (1) {
    // ================ 生产者：串口 → 队列 ================
    switch (sState) {

      case S_IDLE: {
        // 有背压残留数据时不再读串口
        if (recvPos >= (int)recvLen && recvLen > 0) break;

        if (Serial.available() < 3) { delay(1); break; }
        sCmd = Serial.read();
        sLen = Serial.read() | (Serial.read() << 8);

        switch (sCmd) {
          case CMD_IMG_START: {
            Serial.printf("\n=== Image #%u START ===\n", g_imgId);
            if (sendStartPacket(g_imgId)) {
              inImage = true; g_totalSent = 0; endPending = false;
              recvLen = 0; recvPos = 0;
              Serial.println("  ESP-NOW START sent");
            } else {
              Serial.println("  ERROR: START failed");
            }
            break;
          }

          case CMD_STRIP_DATA: {
            if (!inImage) { drainSerial(sLen); break; }
            if (sLen < 1) break;
            if (sLen > STRIP_BUFFER_BYTES + 1) { drainSerial(sLen); break; }
            if (!Serial.available()) break;
            int peek = Serial.peek();
            if (peek < 0 || peek >= TOTAL_STRIPS) { drainSerial(sLen); break; }
            recvIdx = Serial.read();
            recvLen = sLen - 1;
            recvPos = 0;
            sState = S_DATA;
            break;
          }

          case CMD_IMG_END: {
            if (!inImage) break;
            Serial.printf("=== Image #%u END (queue drain) ===\n", g_imgId);
            endPending = true;
            inImage = false;
            break;
          }

          // ===== JPEG 模式 =====
          case CMD_JPG_START: {
            Serial.printf("\n=== JPEG Image #%u START ===\n", g_imgId);
            if (jpgBuf) { free(jpgBuf); jpgBuf = NULL; }

            if (sLen < 2) break;
            jpgTotalSize = Serial.read() | (Serial.read() << 8);
            jpgBuf = (uint8_t *)malloc(jpgTotalSize);
            if (!jpgBuf) {
              Serial.println("  ERROR: malloc failed for JPEG");
              break;
            }
            jpgRecvSize = 0;

            if (sendStartPacket(g_imgId)) {
              inImage = true; g_totalSent = 0; endPending = false;
              sState = S_JPG_RECV;
              Serial.printf("  Receiving %d bytes JPEG...\n", jpgTotalSize);
            } else {
              Serial.println("  ERROR: START failed");
              free(jpgBuf); jpgBuf = NULL;
            }
            break;
          }

          default:
            drainSerial(sLen);
            Serial.printf("Unknown cmd: 0x%02X\n", sCmd);
            break;
        }
        break;
      }

case S_DATA: {
        while (recvPos < (int)recvLen && Serial.available()) {
          recvBuf[recvPos++] = Serial.read();
        }
        if (recvPos >= (int)recvLen) {
          displayStrip(recvIdx, recvBuf);
          if (qPush(recvIdx, recvBuf)) {
            Serial.write(0x06);
          }
          sState = S_IDLE;
        }
        break;
      }

      // ===== JPEG 数据接收 =====
      case S_JPG_RECV: {
        while (jpgRecvSize < jpgTotalSize && Serial.available()) {
          jpgBuf[jpgRecvSize++] = Serial.read();
        }
        if (jpgRecvSize >= jpgTotalSize) {
          Serial.printf("  JPEG received (%d bytes), decoding...\n", jpgRecvSize);

          // 解码（输出回调填充 strip 并入队）
          jpgDecoding = true;
          if (jpgBuf) {
            JRESULT jr = TJpgDec.drawJpg(0, 0, jpgBuf, jpgTotalSize);
            if (jr != JDR_OK) {
              Serial.printf("  JPEG decode ERROR: %d\n", jr);
            } else {
              Serial.println("  JPEG decode OK");
            }
          } else {
            Serial.println("  JPEG ERROR: jpgBuf is NULL (malloc failed?)");
          }
          jpgDecoding = false;

          if (jpgBuf) { free(jpgBuf); jpgBuf = NULL; }
          sState = S_IDLE;
        }
        break;
      }
    }

    // ================ 背压恢复 ================
    if (recvPos >= (int)recvLen && recvLen > 0 && sState == S_IDLE) {
      if (qPush(recvIdx, recvBuf)) {
        Serial.write(0x06);
        recvLen = 0; recvPos = 0;
      }
    }

    // ================ 消费者：队列 → ESP-NOW ================
    if (!isSendBusy() && !qEmpty()) {
      int si; const uint8_t *data;
      qPop(&si, &data);
      beginSendStrip(g_imgId, si, data);
    }
    if (isSendBusy()) {
      int st = pollSendStrip(g_imgId);
      if (st == 1) g_totalSent += getSendCount();
    }

    // ================ 图片结束 ================
    if (endPending && qEmpty() && !isSendBusy()) {
      bool ok = sendEndPacket(g_imgId, g_totalSent);
      Serial.printf("=== Image #%u END, sent: %d ===\n", g_imgId, g_totalSent);
      if (!ok) Serial.println("  WARNING: END failed!");
      Serial.println();
      endPending = false;
      g_imgId++;
      g_totalSent = 0;
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
