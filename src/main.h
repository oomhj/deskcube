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

// 同步控制包
bool sendStartPacket(uint16_t imageId);
bool sendEndPacket(uint16_t imageId, int sent);

// LCD 显示（入队时调用）
void displayStrip(int stripIdx, const uint8_t *pixels);

// 直接发送一个 8×8 块（JPEG 解码模式用）
bool sendImageBlock(uint16_t imageId, int stripIdx, int blockIdx, const uint8_t *blockPixels);

// ===== 异步 ESP-NOW 发送（队列模式用） =====
// beginSendStrip: 开始发送一个 strip 的所有块
// pollSendStrip:  轮询进度，返回 0=发送中 1=完成 -1=全丢
void beginSendStrip(uint16_t imageId, int stripIdx, const uint8_t *pixels);
int  pollSendStrip(uint16_t imageId);
bool isSendBusy();      // 异步发送器是否忙
int  getSendCount();    // 最近一个 strip 的成功块数

// 自测模式
#ifdef ESPNOW_SELF_TEST
void sendImage(uint16_t imageId, int waitMs = 5);
#endif

#endif

// ===== 接收端 =====
#ifndef ESPNOW_MODE_SENDER

void espnowReceiverInit(TFT_eSPI *tft, uint8_t channel = 1);
bool isReceiving();
bool isTransferComplete();
int  getReceiveProgress();

#endif

#endif
