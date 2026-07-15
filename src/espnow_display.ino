//**********************************************************************
// ESP-NOW 图片传输 — 发送端/接收端
//**********************************************************************
#include "main.h"

#define VERSION "V101"

// 接收端 MAC 地址（从接收端串口获取）
#define RECEIVER_MAC  {0x8C, 0x4F, 0x00, 0x53, 0xA3, 0x18}

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

  uint8_t peerMac[] = RECEIVER_MAC;

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
