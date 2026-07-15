#ifndef JPEG_RENDER_H
#define JPEG_RENDER_H

#include <TFT_eSPI.h>
#include "espnow_img_proto.h"

// 16 行缓冲 + strip 切分渲染
// 使用时需在外部定义 renderTargetTFT 指针并设置 setSwapBytes
static TFT_eSPI *renderTargetTFT = NULL;

static uint8_t  _jpgRowBuf[IMG_WIDTH * 16 * 2];
static int      _jpgBufStartY = -1;
static uint16_t _jpgRowDone = 0;

static bool jpegRenderCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    if (!renderTargetTFT) return false;

    if (_jpgBufStartY < 0) {
        _jpgBufStartY = y; _jpgRowDone = 0;
        memset(_jpgRowBuf, 0, sizeof(_jpgRowBuf));
    }
    int bufY = y - _jpgBufStartY;
    for (int row = 0; row < h; row++)
        memcpy(_jpgRowBuf + (bufY + row) * IMG_WIDTH * 2 + x * 2,
               (uint8_t *)bitmap + row * w * 2, w * 2);
    if (x + w >= IMG_WIDTH)
        for (int row = 0; row < h; row++) _jpgRowDone |= (1 << (bufY + row));
    if (_jpgRowDone == 0xFFFF) {
        int base = _jpgBufStartY / 8;
        for (int si = 0; si < 2; si++)
            renderTargetTFT->pushImage(0, (base + si) * 8, IMG_WIDTH, 8,
                                       (uint16_t *)(_jpgRowBuf + si * 3840));
        _jpgBufStartY = -1; _jpgRowDone = 0;
    }
    return true;
}

#endif