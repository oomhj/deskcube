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

// 16 行缓冲：8×8 MCU 攒 2 行再 flush，16×16 MCU 1 行即 flush
static uint8_t jpgRowBuf[IMG_WIDTH * 16 * 2];
static int     jpgBufStartY = -1;   // 缓冲对应图片的第几行
static uint16_t jpgRowDone = 0;     // 位图：哪些行已收齐全部列

static bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    // 新 16 行块开始
    if (jpgBufStartY < 0) {
        jpgBufStartY = y;
        jpgRowDone = 0;
        memset(jpgRowBuf, 0, sizeof(jpgRowBuf));
    }

    // 拷贝 tile 像素到缓冲中的正确行
    int bufY = y - jpgBufStartY;
    for (int row = 0; row < h; row++) {
        memcpy(jpgRowBuf + (bufY + row) * IMG_WIDTH * 2 + x * 2,
               (uint8_t *)bitmap + row * w * 2, w * 2);
    }

    // 当前 tile 列完成 → 标记这些行已收齐
    if (x + w >= IMG_WIDTH) {
        for (int row = 0; row < h; row++)
            jpgRowDone |= (1 << (bufY + row));
    }

    // 16 行全收齐 → 拆成 2 条 8 高 strip 推送
    if (jpgRowDone == 0xFFFF) {
        int baseStrip = jpgBufStartY / 8;
        for (int si = 0; si < 2; si++)
            displayStrip(baseStrip + si, jpgRowBuf + si * STRIP_BUFFER_BYTES);
        jpgBufStartY = -1;
        jpgRowDone = 0;
    }
    return true;
}

// 串口接收缓冲
static uint8_t recvBuf[STRIP_BUFFER_BYTES];
static int     recvPos = 0, recvIdx = 0;
static uint16_t recvLen = 0;

static uint8_t *jpgBuf = NULL;        // malloc'd JPEG 数据缓冲区
static int      jpgTotalSize = 0;     // JPEG 文件总字节数
static int      jpgRecvSize = 0;      // 已接收 JPEG 字节数
static int      jpgChunkRemain = 0;   // 当前 CMD_JPG_DATA 帧剩余 payload 字节
static unsigned long jpgRecvStart = 0; // 接收开始时间（超时用）

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
  TJpgDec.setCallback(tftOutput);

  Serial.println("[Base] Queue+JPEG forward mode (Q_SIZE=4, RX_BUF=4096)");

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
            if (jpgTotalSize < 64 || jpgTotalSize > 32768) {
              Serial.printf("  ERROR: invalid JPEG size %d (64-32768)\n", jpgTotalSize);
              break;
            }
            jpgBuf = (uint8_t *)malloc(jpgTotalSize);
            if (!jpgBuf) {
              Serial.println("  ERROR: malloc failed for JPEG");
              break;
            }
            jpgRecvSize = 0;
            jpgChunkRemain = 0;
            jpgRecvStart = millis();

            if (sendStartPacket(g_imgId)) {
              // 清空可能的 RGB565 队列残留
              qHead = 0; qTail = 0;
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
        // 超时保护：30 秒内没收齐则放弃
        if (millis() - jpgRecvStart > 30000) {
          Serial.printf("  ERROR: JPEG recv timeout (%d/%d bytes)\n",
                        jpgRecvSize, jpgTotalSize);
          if (jpgBuf) { free(jpgBuf); jpgBuf = NULL; }
          sState = S_IDLE;
          break;
        }
        // CMD_JPG_DATA 帧解析：只提取 payload，跳过 3 字节头
        if (jpgChunkRemain == 0) {
          // 等待新帧头
          if (Serial.available() < 3) break;
          uint8_t c = Serial.read();
          uint16_t chunkLen = Serial.read() | (Serial.read() << 8);
          if (c != CMD_JPG_DATA) {
            Serial.printf("  ERROR: expected JPG_DATA, got 0x%02X\n", c);
            if (jpgBuf) { free(jpgBuf); jpgBuf = NULL; }
            sState = S_IDLE;
            break;
          }
          int need = jpgTotalSize - jpgRecvSize;
          jpgChunkRemain = (chunkLen < need) ? chunkLen : need;
        }
        // 消费当前 chunk 的 payload 字节
        while (jpgChunkRemain > 0 && Serial.available() && jpgRecvSize < jpgTotalSize) {
          jpgBuf[jpgRecvSize++] = Serial.read();
          jpgChunkRemain--;
        }
        if (jpgRecvSize >= jpgTotalSize) {
          Serial.printf("  JPEG received (%d bytes)\n", jpgRecvSize);

          if (jpgBuf) {
            // 第一步：基座本地 LCD 解码显示
            TJpgDec.drawJpg(0, 0, jpgBuf, jpgTotalSize);
            Serial.println("  Local display OK");

            // 第二步：ESP-NOW 转发 JPEG 到接收机
            bool ok = sendJpegFile(g_imgId, jpgBuf, jpgTotalSize);
            Serial.printf("  ESP-NOW forward %s\n", ok ? "OK" : "FAILED");
          }
          if (jpgBuf) { free(jpgBuf); jpgBuf = NULL; }

          // 收齐后回 30 个 ACK
          for (int i = 0; i < 30; i++) Serial.write(0x06);

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
