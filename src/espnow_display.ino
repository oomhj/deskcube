//**********************************************************************
// ESP-NOW 图片传输 — 基站/接收端
//**********************************************************************
#include "main.h"

#define VERSION "V101"

// 串口协议包：cmd(1) + len(2) + stripIdx(1) + pixels(3840) = 3844
#define SERIAL_STRIP_PKT_BYTES  (1 + 2 + 1 + STRIP_BUFFER_BYTES)

// ===================== 接收端 =====================
#if defined(ESPNOW_MODE_RECEIVER)

void setup()
{
  Serial.begin(115200);
  Serial.println();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  espnowReceiverInit(&tft);
}

void loop()
{
  delay(10);
}

// ===================== 发送端 =====================
#elif defined(ESPNOW_MODE_SENDER)

enum { CMD_IMG_START = 0x01, CMD_STRIP_DATA = 0x02, CMD_IMG_END = 0x03 };

static void drainSerial(uint16_t len) {
  for (uint16_t i = 0; i < len; i++) {
    if (Serial.read() < 0) break;
  }
}

// ---- 双缓冲异步发送 ----

static uint8_t stripBuf[2][STRIP_BUFFER_BYTES];

/**
 * 尝试从串口补全一个被异步接收截断的 strip 包。
 * 返回 true 表示已补全至完整包。
 */
static bool completeAsyncStrip(uint8_t *buf, int *recvPos) {
  if (*recvPos < 3 || buf[0] != CMD_STRIP_DATA) return false;
  uint16_t declLen = buf[1] | (buf[2] << 8);
  int totalNeeded = 3 + declLen;  // cmd + len + payload
  if (totalNeeded > STRIP_BUFFER_BYTES + 4) return false;  // sanity
  unsigned long start = millis();
  while (*recvPos < totalNeeded) {
    if (Serial.available()) {
      buf[(*recvPos)++] = Serial.read();
    } else {
      if (millis() - start > 2000) return false;  // 超时放弃
      delay(1);
    }
  }
  return true;
}

void setup()
{
  Serial.begin(115200);
  Serial.println();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  delay(100);

  // 从串口读取接收端 MAC 地址
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
      if (idx < 6) {
        peerMac[idx++] = val;
      }
      val = 0;
    }
  }
  if (idx < 6) {
    peerMac[idx++] = val;
  }

  if (idx < 6) {
    Serial.println("\nInvalid MAC, using broadcast");
    memset(peerMac, 0xFF, 6);
  } else {
    Serial.print("\nUsing MAC: ");
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\n",
                  peerMac[0], peerMac[1], peerMac[2],
                  peerMac[3], peerMac[4], peerMac[5]);
  }

  espnowSenderInit(peerMac, &tft);

  Serial.println("[Base] Ready. Async dual-buffer mode.");
  Serial.printf("  %d bytes/pkt (cmd+len+idx+pixels)\n", SERIAL_STRIP_PKT_BYTES);

  // ---- 主循环 ----
  uint16_t imgId = 1;
  int totalSent = 0;
  bool inImage = false;
  int bufIdx = 0;           // 当前正在填充/使用的缓冲区索引
  int asyncRecvPos = 0;     // 异步接收字节计数

  while (1) {
    // ====== 阶段 1：补全 + 消耗异步接收的 strip ======
    if (asyncRecvPos > 0) {
      // 如果是不完整的 strip，尝试从串口补齐
      if (asyncRecvPos >= 3 && asyncRecvPos < SERIAL_STRIP_PKT_BYTES &&
          stripBuf[bufIdx][0] == CMD_STRIP_DATA) {
        completeAsyncStrip(stripBuf[bufIdx], &asyncRecvPos);
      }
      // 补全后检查是否完整
      if (asyncRecvPos >= SERIAL_STRIP_PKT_BYTES && stripBuf[bufIdx][0] == CMD_STRIP_DATA) {
        // 处理一个异步 strip，同时继续收下一批
        int aIdx = stripBuf[bufIdx][3];
        int nextBuf = 1 - bufIdx;
        asyncRecvPos = 0;
        int sent = sendStripFromHost(imgId, aIdx, stripBuf[bufIdx] + 4,
                                      stripBuf[nextBuf], &asyncRecvPos, STRIP_BUFFER_BYTES);
        totalSent += sent;
        bufIdx = nextBuf;
        continue;
      }
      // 无法处理，丢弃残余
      asyncRecvPos = 0;
    }

    // ====== 阶段 2：从串口读取一个 strip ======
    if (Serial.available() < 3) {
      delay(1);
      continue;
    }

    // 读命令头
    uint8_t cmd = Serial.read();
    uint16_t len = Serial.read() | (Serial.read() << 8);

    switch (cmd) {
      case CMD_IMG_START: {
        Serial.printf("\n=== Image #%u START from host ===\n", imgId);
        if (sendStartPacket(imgId)) {
          inImage = true;
          totalSent = 0;
          asyncRecvPos = 0;
          Serial.println("  ESP-NOW START sent");
        } else {
          Serial.println("  ERROR: START send failed - aborting image");
        }
        break;
      }

      case CMD_STRIP_DATA: {
        if (!inImage) {
          drainSerial(len);
          break;
        }
        if (len < 1) { drainSerial(0); break; }
        if (len > STRIP_BUFFER_BYTES + 1) {
          Serial.printf("  ERROR: invalid len=%u (max=%u)\n", len, STRIP_BUFFER_BYTES + 1);
          drainSerial(len);
          break;
        }

        int stripIdx = Serial.peek();
        if (stripIdx < 0 || stripIdx >= TOTAL_STRIPS) {
          drainSerial(len);
          break;
        }
        stripIdx = Serial.read();

        // 读 3840 字节像素到当前缓冲区
        int readBytes = 0;
        while (readBytes < STRIP_BUFFER_BYTES) {
          int n = Serial.readBytes(stripBuf[bufIdx] + readBytes, STRIP_BUFFER_BYTES - readBytes);
          if (n <= 0) break;
          readBytes += n;
        }
        if (readBytes < STRIP_BUFFER_BYTES) break;

        // 异步发送（LCD + ACK + ESP-NOW），同时收下一行到另一个缓冲区
        int nextBuf = 1 - bufIdx;
        asyncRecvPos = 0;
        int sent = sendStripFromHost(imgId, stripIdx, stripBuf[bufIdx],
                                      stripBuf[nextBuf], &asyncRecvPos, STRIP_BUFFER_BYTES);
        totalSent += sent;
        bufIdx = nextBuf;

        // 补全不完整的异步收据
        if (asyncRecvPos >= 3 && asyncRecvPos < SERIAL_STRIP_PKT_BYTES &&
            stripBuf[bufIdx][0] == CMD_STRIP_DATA) {
          completeAsyncStrip(stripBuf[bufIdx], &asyncRecvPos);
        }

        // 循环消化完整的异步 strip
        while (asyncRecvPos >= SERIAL_STRIP_PKT_BYTES && stripBuf[bufIdx][0] == CMD_STRIP_DATA) {
          uint16_t rlen = stripBuf[bufIdx][1] | (stripBuf[bufIdx][2] << 8);
          if (rlen != STRIP_BUFFER_BYTES + 1) break;

          int aIdx = stripBuf[bufIdx][3];
          int prev = asyncRecvPos;
          asyncRecvPos = 0;
          int sent2 = sendStripFromHost(imgId, aIdx, stripBuf[bufIdx] + 4,
                                         stripBuf[1 - bufIdx], &asyncRecvPos, STRIP_BUFFER_BYTES);
          totalSent += sent2;
          bufIdx = 1 - bufIdx;

          // 贴心地补全下一批不完整数据
          if (asyncRecvPos >= 3 && asyncRecvPos < SERIAL_STRIP_PKT_BYTES &&
              stripBuf[bufIdx][0] == CMD_STRIP_DATA) {
            completeAsyncStrip(stripBuf[bufIdx], &asyncRecvPos);
          }

          Serial.printf("  [async] Strip %d (%d raw)\n", aIdx, prev);
        }

        break;
      }

      case CMD_IMG_END: {
        if (!inImage) break;
        bool endOk = sendEndPacket(imgId, totalSent);
        Serial.printf("=== Image #%u END, total sent: %d ===\n", imgId, totalSent);
        if (!endOk) Serial.println("  WARNING: END packet send failed!");
        Serial.println();
        inImage = false;
        asyncRecvPos = 0;
        imgId++;
        break;
      }

      default:
        drainSerial(len);
        Serial.printf("Unknown cmd: 0x%02X\n", cmd);
        break;
    }
  }
}

void loop()
{
  delay(10);
}

// ===================== 原始时钟模式（已废弃）=====================
#else

void setup()
{
  Serial.begin(115200);
  Serial.println();
  Serial.println("This build has no ESP-NOW mode defined.");
  Serial.println("Use ESPNOW_MODE_RECEIVER or ESPNOW_MODE_SENDER.");
  while (1) { delay(1000); }
}

void loop()
{
  delay(10);
}

#endif
