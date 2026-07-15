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

// 16 行临时像素缓冲区（2 条 strip，处理 16×16 MCU 跨越）
static uint8_t  jpgRowBuf[IMG_WIDTH * 16 * 2];
static int      jpgBufStartRow = -1;

// TJpg_Decoder 输出回调
static bool jpegOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (jpgBufStartRow < 0) {
    jpgBufStartRow = y;
    memset(jpgRowBuf, 0, sizeof(jpgRowBuf));
  }

  int bufY = y - jpgBufStartRow;
  // 拷贝像素到行缓冲区
  for (int row = 0; row < h && (bufY + row) < 16; row++) {
    memcpy(jpgRowBuf + (bufY + row) * IMG_WIDTH * 2 + x * 2,
           (uint8_t *)(bitmap + row * w), w * 2);
  }

  // 最后一列 → 提取完整 strip 入队
  if (x + w >= IMG_WIDTH) {
    int endRow = jpgBufStartRow + h;
    int startSi = jpgBufStartRow / STRIP_H;
    int numStrips = endRow / STRIP_H - startSi;

    for (int i = 0; i < numStrips; i++) {
      int si = startSi + i;
      int off = (jpgBufStartRow % 16) * IMG_WIDTH * 2 + i * STRIP_BUFFER_BYTES;

      // 从 jpgRowBuf 拷贝到 recvBuf → 入队
      memcpy(recvBuf, jpgRowBuf + off, STRIP_BUFFER_BYTES);
      displayStrip(si, recvBuf);
      qPush(si, recvBuf);
      // JPEG 模式：每完成一个 strip 发 ACK
      Serial.write(0x06);
    }

    jpgBufStartRow = -1;
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
  uint16_t imgId   = 1;
  int      totalSent  = 0;
  bool     inImage    = false;
  bool     endPending = false;

  // 串口接收状态
  enum { S_IDLE, S_HEAD, S_DATA, S_JPG_RECV } sState = S_IDLE;
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
            Serial.printf("\n=== Image #%u START ===\n", imgId);
            if (sendStartPacket(imgId)) {
              inImage = true; totalSent = 0; endPending = false;
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
            sState = S_HEAD;
            break;
          }

          case CMD_IMG_END: {
            if (!inImage) break;
            Serial.printf("=== Image #%u END (queue drain) ===\n", imgId);
            endPending = true;
            inImage = false;
            break;
          }

          // ===== JPEG 模式 =====
          case CMD_JPG_START: {
            Serial.printf("\n=== JPEG Image #%u START ===\n", imgId);
            if (jpgBuf) { free(jpgBuf); jpgBuf = NULL; }

            if (sLen < 2) break;
            jpgTotalSize = Serial.read() | (Serial.read() << 8);
            jpgBuf = (uint8_t *)malloc(jpgTotalSize);
            if (!jpgBuf) {
              Serial.println("  ERROR: malloc failed for JPEG");
              break;
            }
            jpgRecvSize = 0;

            if (sendStartPacket(imgId)) {
              inImage = true; totalSent = 0; endPending = false;
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

      case S_HEAD: {
        if (!Serial.available()) break;
        sState = S_DATA;
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
      beginSendStrip(imgId, si, data);
    }
    if (isSendBusy()) {
      int st = pollSendStrip(imgId);
      if (st == 1) totalSent += getSendCount();
    }

    // ================ 图片结束 ================
    if (endPending && qEmpty() && !isSendBusy()) {
      bool ok = sendEndPacket(imgId, totalSent);
      Serial.printf("=== Image #%u END, sent: %d ===\n", imgId, totalSent);
      if (!ok) Serial.println("  WARNING: END failed!");
      Serial.println();
      endPending = false;
      imgId++;
      totalSent = 0;
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
