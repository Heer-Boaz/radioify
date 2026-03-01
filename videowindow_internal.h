#pragma once

#include <cstdint>

struct ShaderConstants {
    uint32_t isFullRange;
    uint32_t yuvMatrix;
    uint32_t yuvTransfer;
    uint32_t bitDepth;

    float progress;
    float overlayAlpha;
    uint32_t isPaused;
    uint32_t hasRGBA;

    uint32_t volPct;
    uint32_t rotationQuarterTurns;
    float textTop;
    float textHeight;

    float textLeft;
    float textWidth;
    uint32_t pad1;
    uint32_t pad2;

    float subtitleTop;
    float subtitleHeight;
    float subtitleLeft;
    float subtitleWidth;

    float subtitleAlpha;
    float pad3;
    float pad4;
    float pad5;
};

static_assert((sizeof(ShaderConstants) % 16) == 0,
              "ShaderConstants size must be 16-byte aligned");
