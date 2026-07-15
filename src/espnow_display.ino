//**********************************************************************
// ESP-NOW 图片传输 — 基站/接收端
//**********************************************************************
#include "main.h"

#define VERSION "V101"

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
      peerMac[idx++] = val;
      val = 0;
    }
  }
  // 保存最后一个字节（换行前累积的）
  peerMac[idx++] = val;

  if (idx < 6) {
    // 输入不完整或超时，使用默认广播地址
    Serial.println("\nInvalid MAC, using broadcast");
    memset(peerMac, 0xFF, 6);
  } else {
    Serial.print("\nUsing MAC: ");
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\n",
                  peerMac[0], peerMac[1], peerMac[2],
                  peerMac[3], peerMac[4], peerMac[5]);
  }

  espnowSenderInit(peerMac, &tft);

  Serial.println("[Base] Ready. Send image data via serial or press Enter for self-test.");

  // 行缓冲区（全局避免栈溢出）
  static uint8_t stripBuf[STRIP_BUFFER_BYTES];
  uint16_t imgId = 1;
  int totalSent = 0;
  bool inImage = false;

  // 串口命令定义
  enum { CMD_IMG_START = 0x01, CMD_STRIP_DATA = 0x02, CMD_IMG_END = 0x03 };

  while (1) {
    if (Serial.available() < 3) {
      // 无串口命令时自生成测试图
      if (!inImage) {
        sendImage(imgId++, 0);
      } else {
        delay(1);
      }
      continue;
    }

    // 读命令头: [cmd] [len_lo] [len_hi]
    uint8_t cmd = Serial.read();
    uint16_t len = Serial.read() | (Serial.read() << 8);

    switch (cmd) {
      case CMD_IMG_START: {
        Serial.printf("\n=== Image #%u START from host ===\n", imgId);
        bool ok = sendStartPacket(imgId);
        inImage = true;
        totalSent = 0;
        if (ok) Serial.println("  ESP-NOW START sent");
        break;
      }

      case CMD_STRIP_DATA: {
        if (!inImage) {
          for (int i = 0; i < len; i++) Serial.read();
          break;
        }
        if (len < 1) break;
        int stripIdx = Serial.read();
        int dataLen = len - 1;

        if (dataLen != STRIP_BUFFER_BYTES) {
          for (int i = 0; i < dataLen; i++) Serial.read();
          break;
        }

        int readBytes = 0;
        while (readBytes < STRIP_BUFFER_BYTES) {
          int n = Serial.readBytes(stripBuf + readBytes, STRIP_BUFFER_BYTES - readBytes);
          if (n <= 0) break;
          readBytes += n;
        }

        if (readBytes == STRIP_BUFFER_BYTES) {
          int sent = sendStripFromHost(imgId, stripIdx, stripBuf);
          totalSent += sent;
        }
        break;
      }

      case CMD_IMG_END: {
        if (!inImage) break;
        sendEndPacket(imgId, totalSent);
        Serial.printf("=== Image #%u END, total sent: %d ===\n\n", imgId, totalSent);
        inImage = false;
        imgId++;
        break;
      }

      default:
        for (int i = 0; i < len; i++) Serial.read();
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
