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
  if (idx < 6) {
    // 输入不完整或超时，使用默认广播地址
    Serial.println("\nInvalid MAC, using broadcast");
    memset(peerMac, 0xFF, 6);
  } else {
    peerMac[idx] = val;
    Serial.print("\nUsing MAC: ");
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\n",
                  peerMac[0], peerMac[1], peerMac[2],
                  peerMac[3], peerMac[4], peerMac[5]);
  }

  espnowSenderInit(peerMac, &tft);

  // 循环发送（图片在 sendImage 内部逐行生成，无需大数组）
  uint16_t imgId = 1;
  while (1) {
    sendImage(imgId++, 0);
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
