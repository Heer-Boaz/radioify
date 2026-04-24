#pragma once

#include <cstddef>
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
    float subtitleTop;
    float subtitleHeight;

    float subtitleLeft;
    float subtitleWidth;
    float subtitleAlpha;
    uint32_t outputColorSpace;

    float outputSdrWhiteNits;
    float outputPeakNits;
    float outputFullFrameNits;
    float asciiGlyphPeakNits;
};

static_assert((sizeof(ShaderConstants) % 16) == 0,
              "ShaderConstants size must be 16-byte aligned");
static_assert(offsetof(ShaderConstants, outputColorSpace) % 16 == 12,
              "outputColorSpace must occupy the final cbuffer lane");
static_assert(offsetof(ShaderConstants, outputSdrWhiteNits) ==
                  offsetof(ShaderConstants, outputColorSpace) + 4,
              "outputSdrWhiteNits must follow outputColorSpace");
static_assert(offsetof(ShaderConstants, outputPeakNits) ==
                  offsetof(ShaderConstants, outputColorSpace) + 8,
              "outputPeakNits must follow outputColorSpace");
static_assert(offsetof(ShaderConstants, outputFullFrameNits) ==
                  offsetof(ShaderConstants, outputColorSpace) + 12,
              "outputFullFrameNits must follow outputColorSpace");
static_assert(offsetof(ShaderConstants, asciiGlyphPeakNits) ==
                  offsetof(ShaderConstants, outputColorSpace) + 16,
              "asciiGlyphPeakNits must share the HDR constants cbuffer row");
