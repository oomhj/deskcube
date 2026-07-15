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

enum { CMD_IMG_START = 0x01, CMD_STRIP_DATA = 0x02, CMD_IMG_END = 0x03 };

static void drainSerial(uint16_t len) {
  for (uint16_t i = 0; i < len; i++) {
    if (Serial.read() < 0) break;
  }
}

// =====================================================================
// 环形队列：串口写入 → ESP-NOW 读出
// =====================================================================
#define Q_SIZE  8      // 8 × 3840 = 30 KB
static uint8_t stripQ[Q_SIZE][STRIP_BUFFER_BYTES];
static int qStripIdx[Q_SIZE];
static volatile int qHead  = 0;  // 写指针
static volatile int qTail  = 0;  // 读指针

static bool qFull()  { return ((qHead + 1) % Q_SIZE) == qTail; }
static bool qEmpty() { return qHead == qTail; }

// 入队（在 ISR 上下文中安全——仅操作 qHead）
static bool qPush(int stripIdx, const uint8_t *data) {
  if (qFull()) return false;
  memcpy(stripQ[qHead], data, STRIP_BUFFER_BYTES);
  qStripIdx[qHead] = stripIdx;
  qHead = (qHead + 1) % Q_SIZE;
  return true;
}

// 出队（在 ISR 上下文中安全——仅操作 qTail）
static bool qPop(int *stripIdx, const uint8_t **data) {
  if (qEmpty()) return false;
  if (data)     *data = stripQ[qTail];
  if (stripIdx) *stripIdx = qStripIdx[qTail];
  qTail = (qTail + 1) % Q_SIZE;
  return true;
}

// =====================================================================
// 串口接收缓冲 + 状态
// =====================================================================
static uint8_t recvBuf[STRIP_BUFFER_BYTES];
static int     recvPos  = 0;
static int     recvIdx  = 0;
static uint16_t recvLen = 0;

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
  Serial.println("[Base] Queue mode ready (Q_SIZE=8, RX_BUF=4096)");
  Serial.printf("  Strip: %dB, Q: %d entries\n", STRIP_BUFFER_BYTES, Q_SIZE);

  // =================================================================
  // 主循环
  // =================================================================
  uint16_t imgId   = 1;
  int      totalSent  = 0;
  bool     inImage    = false;
  bool     endPending = false;  // CMD_IMG_END 收到，等队列排空

  // 串口接收状态
  enum { S_IDLE, S_HEAD, S_DATA } sState = S_IDLE;
  uint8_t sCmd  = 0;
  uint16_t sLen = 0;

  while (1) {
    // ================ 生产者：串口 → 队列 ================
    switch (sState) {

      case S_IDLE: {
        if (Serial.available() < 3) { delay(1); break; }
        sCmd = Serial.read();
        sLen = Serial.read() | (Serial.read() << 8);

        switch (sCmd) {
          case CMD_IMG_START: {
            Serial.printf("\n=== Image #%u START ===\n", imgId);
            if (sendStartPacket(imgId)) {
              inImage = true; totalSent = 0; endPending = false;
              Serial.println("  ESP-NOW START sent");
            } else {
              Serial.println("  ERROR: START failed");
            }
            break;
          }

          case CMD_STRIP_DATA: {
            if (!inImage) { drainSerial(sLen); break; }
            if (sLen < 1) break;
            if (sLen > STRIP_BUFFER_BYTES + 1) {
              drainSerial(sLen); break;
            }
            // 读 stripIdx（先 peek 校验）
            if (!Serial.available()) break;
            int peek = Serial.peek();
            if (peek < 0 || peek >= TOTAL_STRIPS) {
              drainSerial(sLen); break;
            }
            recvIdx = Serial.read();
            recvLen = sLen - 1;  // 像素字节数（不含 stripIdx）
            recvPos = 0;
            sState = S_HEAD;  // 等待像素第一个字节
            break;
          }

          case CMD_IMG_END: {
            if (!inImage) break;
            Serial.printf("=== Image #%u END (pending queue drain) ===\n", imgId);
            endPending = true;
            inImage = false;
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
        // 等候第一个像素字节到达
        if (!Serial.available()) break;
        // 第一条数据 = stripIdx（已在 S_IDLE 中读完），切到数据接收
        sState = S_DATA;
        break;
      }

      case S_DATA: {
        // 非阻塞地收像素
        while (recvPos < (int)recvLen && Serial.available()) {
          recvBuf[recvPos++] = Serial.read();
        }
        if (recvPos >= (int)recvLen) {
          // ---- 一个完整的 strip 到手 ----
          // ① LCD 显示
          displayStrip(recvIdx, recvBuf);

          // ② 入队
          if (qPush(recvIdx, recvBuf)) {
            Serial.write(0x06);  // ACK → 宿主机可发下一行
          }
          // 入队失败 = 队列满 → 不回 ACK = 背压

          sState = S_IDLE;
        }
        break;
      }
    }

    // ================ 背压恢复：队列有空位时补 ACK ================
    // 如果 recvBuf 中有未入队的数据（之前队列满），重新尝试入队
    // （通过状态机：S_DATA 完成后 recvPos = recvLen，若 qPush 失败则
    //   数据停留在 recvBuf，下次入队在上层重试，但这里状态已回 S_IDLE。
    //   需要额外处理：用一个标志记住有"待入队"数据）
    //
    // 简化：如果 recvPos == recvLen 且 recvLen > 0，表示发完了但没入队，
    //       立刻重试入队
    if (recvPos >= (int)recvLen && recvLen > 0 && sState == S_IDLE) {
      if (qPush(recvIdx, recvBuf)) {
        Serial.write(0x06);  // 补 ACK
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
      if (st == 1) {
        totalSent += getSendCount();
      } else if (st == -1) {
        // 全丢，跳过
      }
    }

    // ================ 图片结束 ================
    if (endPending && qEmpty() && !isSendBusy()) {
      bool ok = sendEndPacket(imgId, totalSent);
      Serial.printf("=== Image #%u END, total sent: %d ===\n", imgId, totalSent);
      if (!ok) Serial.println("  WARNING: END packet failed!");
      Serial.println();
      endPending = false;
      imgId++;
      totalSent = 0;
    }

    yield();
  }
}

void loop() { delay(10); }

// ===================== 原始时钟模式（已废弃）=====================
#else
void setup() {
  Serial.begin(115200); Serial.println();
  Serial.println("This build has no ESP-NOW mode defined.");
  Serial.println("Use ESPNOW_MODE_RECEIVER or ESPNOW_MODE_SENDER.");
  while (1) delay(1000);
}
void loop() { delay(10); }
#endif
