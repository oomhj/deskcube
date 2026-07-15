#ifndef __MAIN_H__
#define __MAIN_H__

#include <ESP8266WiFi.h>
#include <TFT_eSPI.h>
#include "espnow_img_proto.h"

// ===== 共享变量 =====
TFT_eSPI tft = TFT_eSPI();

// ===== 发送端 =====
#ifdef ESPNOW_MODE_SENDER

void espnowSenderInit(const uint8_t *peerMac, TFT_eSPI *tft, uint8_t channel = 1);

// 自测模式（生成渐变彩条并发送，需编译时加 -DESPNOW_SELF_TEST）
#ifdef ESPNOW_SELF_TEST
void sendImage(uint16_t imageId, int waitMs = 5);
#endif

// 宿主机串口传图模式
bool sendStartPacket(uint16_t imageId);
bool sendEndPacket(uint16_t imageId, int sent);
int  sendStripFromHost(uint16_t imageId, int stripIdx, const uint8_t *pixels);

#endif

// ===== 接收端 =====
#ifndef ESPNOW_MODE_SENDER

void espnowReceiverInit(TFT_eSPI *tft, uint8_t channel = 1);
bool isReceiving();
bool isTransferComplete();
int  getReceiveProgress();

#endif

#endif
