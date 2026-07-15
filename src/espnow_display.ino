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

// 2 个 strip 累积器（16×16 tile 跨越 2 个 strip 时需要同时追踪）
static uint8_t  jpgAccum[2][STRIP_BUFFER_BYTES];
static int      jpgAccumIdx[2] = {-1, -1};      // 对应的 strip 序号
static uint32_t jpgAccumMap[2] = {0};            // 块位图（每 bit 一个 block）

// 获取累积器 slot（自动替换已满的 strip）
static int jpgGetSlot(int stripIdx) {
  for (int i = 0; i < 2; i++) {
    if (jpgAccumIdx[i] == stripIdx) return i;         // 匹配
    if (jpgAccumIdx[i] < 0) { jpgAccumIdx[i] = stripIdx; return i; }  // 空
  }
  // 都占用了 → 找一个满的去刷新
  for (int i = 0; i < 2; i++) {
    if (jpgAccumMap[i] == 0x3FFFFFFF) {  // 30 blocks all received
      displayStrip(jpgAccumIdx[i], jpgAccum[i]);
      Serial.write(0x06);
      g_totalSent += 30;
      jpgAccumIdx[i] = stripIdx;
      jpgAccumMap[i] = 0;
      memset(jpgAccum[i], 0, STRIP_BUFFER_BYTES);
      return i;
    }
  }
  return -1;  // 不应发生
}

// TJpg_Decoder 输出回调
static bool jpegOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  // 将 tile 拆成 8×8 子块，逐个发送
  for (int by = 0; by < h; by += BLOCK_H) {
    for (int bx = 0; bx < w; bx += BLOCK_W) {
      int si = (y + by) / STRIP_H;
      int bi = (x + bx) / BLOCK_H;

      // 提取 8×8 像素块（RGB565 格式）
      uint8_t block[BLOCK_DATA_BYTES];
      for (int py = 0; py < BLOCK_H; py++) {
        int srcOff = ((by + py) * w + bx) * 2;
        int dstOff = py * BLOCK_H * 2;
        memcpy(block + dstOff, (uint8_t *)bitmap + srcOff, BLOCK_H * 2);
      }

      // 发送
      sendImageBlock(g_imgId, si, bi, block);

      // 累积到对应的 strip 缓冲区
      int slot = jpgGetSlot(si);
      if (slot >= 0) {
        jpgAccumMap[slot] |= (1UL << bi);
        // 写入像素行（按绝对坐标定位到 accum 中正确位置）
        for (int py = 0; py < BLOCK_H; py++) {
          int absY   = y + by + py;
          int relY   = absY % STRIP_H;
          int srcOff = (py * w + bx) * 2;
          memcpy(jpgAccum[slot] + relY * IMG_WIDTH * 2 + (x + bx) * 2,
                 (uint8_t *)bitmap + ((by + py) * w + bx) * 2, BLOCK_H * 2);
        }
      }
    }
  }

  // tile 行结束 → 尝试 flush 涉及的 strip
  if (x + w >= IMG_WIDTH) {
    int firstSi = y / STRIP_H;
    int lastSi  = (y + h - 1) / STRIP_H;
    for (int si = firstSi; si <= lastSi; si++) {
      for (int i = 0; i < 2; i++) {
        if (jpgAccumIdx[i] == si && jpgAccumMap[i] == 0x3FFFFFFF) {
          displayStrip(si, jpgAccum[i]);
          Serial.write(0x06);
          g_totalSent += 30;
          jpgAccumIdx[i] = -1;
          jpgAccumMap[i] = 0;
        }
      }
    }
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
