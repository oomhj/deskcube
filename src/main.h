#ifndef __MAIN_H__
#define __MAIN_H__

#include <ESP8266WiFi.h>
#include <TFT_eSPI.h>
#include "espnow_img_proto.h"

TFT_eSPI tft = TFT_eSPI();

// ===== 发送端 =====
#ifdef ESPNOW_MODE_SENDER

void espnowSenderInit(const uint8_t *peerMac, TFT_eSPI *tft, uint8_t channel = 1);
bool sendStartPacket(uint16_t imageId);
bool sendEndPacket(uint16_t imageId, int sent);
bool sendJpegFile(uint16_t imageId, const uint8_t *jpgData, int jpgSize);
bool sendCmdPacket(uint16_t imageId, uint8_t cmdId, const uint8_t *params, uint8_t len);

#endif

// ===== 接收端 =====
#ifndef ESPNOW_MODE_SENDER

void espnowReceiverInit(TFT_eSPI *tft, uint8_t channel = 1);
bool isReceiving();
bool isTransferComplete();
int  getReceiveProgress();

#endif

#endif
