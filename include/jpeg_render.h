#ifndef JPEG_RENDER_H
#define JPEG_RENDER_H

#include <TFT_eSPI.h>

// TJpg_Decoder 输出回调：逐 tile 直写 TFT
// 调用前需设置 renderTargetTFT 和 setSwapBytes(true)
static TFT_eSPI *renderTargetTFT = NULL;

static bool jpegRenderCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    if (!renderTargetTFT) return false;
    renderTargetTFT->pushImage(x, y, w, h, bitmap);
    return true;
}

#endif
