
cbuffer Constants : register(b0) {
    uint width;
    uint height;
    uint outWidth;
    uint outHeight;
    float time;
    uint frameCount;
    uint isFullRange;
    uint bitDepth;
    uint yuvMatrix;
    uint yuvTransfer;
    uint rotationQuarterTurns;
    uint padding;
};

#ifdef NV12_INPUT
Texture2D<float> TextureY : register(t0);
Texture2D<float2> TextureUV : register(t1);
StructuredBuffer<uint> LumaRangeBuffer : register(t2);
#else
Texture2D<float4> InputTexture : register(t0);
StructuredBuffer<uint> LumaRangeBuffer : register(t1);
#endif

SamplerState LinearSampler : register(s0);

struct AsciiCell {
    uint ch;
    uint fg;
    uint bg;
    uint hasBg;
};

struct AsciiDotSample {
    uint color;    // packed RGB
    uint metrics;  // luma, edge, source-blue confidence, unused
};

RWStructuredBuffer<AsciiCell> OutputBuffer : register(u0);
RWStructuredBuffer<uint2> HistoryBuffer : register(u1); // x=Fg, y=Bg (packed)
RWStructuredBuffer<AsciiDotSample> DotOutputBuffer : register(u2);
StructuredBuffer<AsciiCell> RefineInput : register(t3);
StructuredBuffer<AsciiDotSample> DotInputBuffer : register(t4);

groupshared float gDotEdgeTileLuma[100];

static const uint kBrailleBase = 0x2800;
static const float3 kLumaCoeff = float3(0.2126f, 0.7152f, 0.0722f);
static const uint kMatrixBt709 = 0;
static const uint kMatrixBt601 = 1;
static const uint kMatrixBt2020 = 2;
static const uint kTransferSdr = 0;
static const uint kTransferPq = 1;
static const uint kTransferHlg = 2;

#define RADIOIFY_ASCII_BOOL(name, value) static const bool name = value;
#define RADIOIFY_ASCII_FLOAT(name, value) static const float name = value;
#define RADIOIFY_ASCII_COUNT(name, value) static const int name = value;
#define RADIOIFY_ASCII_LUMA_U8(name, value) static const int name = value;
#define RADIOIFY_ASCII_SIGNAL_U8(name, value) \
    static const float name = (float)(value) / 255.0f;
#define RADIOIFY_ASCII_SCALE_256(name, value) \
    static const float name = (float)(value) / 256.0f;
#include "../../video/ascii/asciiart_constants.inc"
#undef RADIOIFY_ASCII_BOOL
#undef RADIOIFY_ASCII_FLOAT
#undef RADIOIFY_ASCII_COUNT
#undef RADIOIFY_ASCII_LUMA_U8
#undef RADIOIFY_ASCII_SIGNAL_U8
#undef RADIOIFY_ASCII_SCALE_256

#include "video_hdr_to_sdr.hlsli"

// Dithering thresholds (optimized for 2x4 braille)
// Ranks: 0, 4, 2, 6, 1, 5, 3, 7
static const int kDitherThresholdByBit[8] = { 
    (0 * 255 + 4) / 8, // 0
    (4 * 255 + 4) / 8, // 1
    (2 * 255 + 4) / 8, // 2
    (6 * 255 + 4) / 8, // 3
    (1 * 255 + 4) / 8, // 4
    (5 * 255 + 4) / 8, // 5
    (3 * 255 + 4) / 8, // 6
    (7 * 255 + 4) / 8  // 7
};

uint PackColor(float3 c) {
    uint r = (uint)(saturate(c.r) * 255.0f + 0.5f);
    uint g = (uint)(saturate(c.g) * 255.0f + 0.5f);
    uint b = (uint)(saturate(c.b) * 255.0f + 0.5f);
    return (r) | (g << 8) | (b << 16) | 0xFF000000;
}

float3 UnpackColor(uint c) {
    float r = (float)(c & 0xFF) / 255.0f;
    float g = (float)((c >> 8) & 0xFF) / 255.0f;
    float b = (float)((c >> 16) & 0xFF) / 255.0f;
    return float3(r, g, b);
}

float GetLuma(float3 c) {
    return dot(c, kLumaCoeff) * 255.0f;
}

uint ExtractBrailleMask(AsciiCell cell) {
    if (cell.ch < kBrailleBase || cell.ch > (kBrailleBase + 0xFFu)) {
        return 0u;
    }
    return cell.ch - kBrailleBase;
}

uint BrailleBitAt(uint col, uint row) {
    return (col == 0u) ? ((row == 3u) ? 6u : row)
                       : ((row == 3u) ? 7u : row + 3u);
}

uint BrailleBitFromDotIndex(uint index) {
    return (index < 4u) ? ((index == 3u) ? 6u : index)
                        : ((index == 7u) ? 7u : index - 1u);
}

bool MaskHasBit(uint mask, uint bit) {
    return ((mask >> bit) & 1u) != 0u;
}

uint ContinuityCandidateBit(uint index) {
    if (index == 0u) return 0u;
    if (index == 1u) return 3u;
    if (index == 2u) return 6u;
    return 7u;
}

bool ContinuityCandidateHasShapeSupport(uint baseMask, uint bit,
                                        uint leftMask, uint rightMask,
                                        uint topMask, uint bottomMask,
                                        uint topLeftMask, uint topRightMask,
                                        uint bottomLeftMask,
                                        uint bottomRightMask) {
    if (bit == 0u) {
        bool local = MaskHasBit(baseMask, 1u) || MaskHasBit(baseMask, 3u);
        bool neighbor = MaskHasBit(topLeftMask, 7u) &&
                        (MaskHasBit(topMask, 6u) ||
                         MaskHasBit(leftMask, 3u));
        return local && neighbor;
    }
    if (bit == 3u) {
        bool local = MaskHasBit(baseMask, 0u) || MaskHasBit(baseMask, 4u);
        bool neighbor = MaskHasBit(topRightMask, 6u) &&
                        (MaskHasBit(topMask, 7u) ||
                         MaskHasBit(rightMask, 0u));
        return local && neighbor;
    }
    if (bit == 6u) {
        bool local = MaskHasBit(baseMask, 2u) || MaskHasBit(baseMask, 7u);
        bool neighbor = MaskHasBit(bottomLeftMask, 3u) &&
                        (MaskHasBit(bottomMask, 0u) ||
                         MaskHasBit(leftMask, 7u));
        return local && neighbor;
    }
    bool local = MaskHasBit(baseMask, 5u) || MaskHasBit(baseMask, 6u);
    bool neighbor = MaskHasBit(bottomRightMask, 0u) &&
                    (MaskHasBit(bottomMask, 3u) ||
                     MaskHasBit(rightMask, 6u));
    return local && neighbor;
}

int PairContinuityScore(bool a, bool b, int matchReward,
                        int mismatchPenalty) {
    if (a && b) {
        return -matchReward;
    }
    if (a != b) {
        return mismatchPenalty;
    }
    return 0;
}

int NeighborContinuityScore(uint mask, uint baseMask, uint leftMask,
                            uint rightMask, uint topMask, uint bottomMask,
                            uint topLeftMask, uint topRightMask,
                            uint bottomLeftMask, uint bottomRightMask,
                            int strength) {
    int matchReward = strength;
    int mismatchPenalty = strength >> 1;
    int diagonalReward = strength * 2;
    int diagonalMismatch = strength;
    int score = (int)countbits(mask & ~baseMask) * 256 +
                (int)countbits(baseMask & ~mask) * 1024;

    [unroll]
    for (uint row = 0u; row < 4u; ++row) {
        score += PairContinuityScore(
            MaskHasBit(mask, BrailleBitAt(0u, row)),
            MaskHasBit(leftMask, BrailleBitAt(1u, row)), matchReward,
            mismatchPenalty);
        score += PairContinuityScore(
            MaskHasBit(mask, BrailleBitAt(1u, row)),
            MaskHasBit(rightMask, BrailleBitAt(0u, row)), matchReward,
            mismatchPenalty);
    }
    [unroll]
    for (uint col = 0u; col < 2u; ++col) {
        score += PairContinuityScore(
            MaskHasBit(mask, BrailleBitAt(col, 0u)),
            MaskHasBit(topMask, BrailleBitAt(col, 3u)), matchReward,
            mismatchPenalty);
        score += PairContinuityScore(
            MaskHasBit(mask, BrailleBitAt(col, 3u)),
            MaskHasBit(bottomMask, BrailleBitAt(col, 0u)), matchReward,
            mismatchPenalty);
    }

    score += PairContinuityScore(MaskHasBit(mask, BrailleBitAt(0u, 0u)),
                                 MaskHasBit(topLeftMask, BrailleBitAt(1u, 3u)),
                                 diagonalReward, diagonalMismatch);
    score += PairContinuityScore(MaskHasBit(mask, BrailleBitAt(1u, 0u)),
                                 MaskHasBit(topRightMask, BrailleBitAt(0u, 3u)),
                                 diagonalReward, diagonalMismatch);
    score += PairContinuityScore(MaskHasBit(mask, BrailleBitAt(0u, 3u)),
                                 MaskHasBit(bottomLeftMask, BrailleBitAt(1u, 0u)),
                                 diagonalReward, diagonalMismatch);
    score += PairContinuityScore(MaskHasBit(mask, BrailleBitAt(1u, 3u)),
                                 MaskHasBit(bottomRightMask, BrailleBitAt(0u, 0u)),
                                 diagonalReward, diagonalMismatch);
    return score;
}

uint RefineNeighborContinuityMask(uint baseMask, uint leftMask, uint rightMask,
                                  uint topMask, uint bottomMask,
                                  uint topLeftMask, uint topRightMask,
                                  uint bottomLeftMask, uint bottomRightMask,
                                  float contrast) {
    baseMask &= 0xffu;
    int strength = (int)(kNeighborContinuityStrength * 256.0f + 0.5f);
    int minGain = (int)(kNeighborContinuityMinGain * 256.0f + 0.5f);
    if (baseMask == 0u || baseMask == 0xffu || strength <= 0 ||
        contrast < (float)kNeighborContinuityMinContrast) {
        return baseMask;
    }

    uint bestMask = baseMask;
    int baseScore =
        NeighborContinuityScore(baseMask, baseMask, leftMask, rightMask,
                                topMask, bottomMask, topLeftMask,
                                topRightMask, bottomLeftMask,
                                bottomRightMask, strength);
    int bestScore = baseScore;
    [unroll]
    for (uint i = 0u; i < 4u; ++i) {
        uint bit = ContinuityCandidateBit(i);
        uint bitFlag = 1u << bit;
        if ((baseMask & bitFlag) != 0u) {
            continue;
        }
        if (!ContinuityCandidateHasShapeSupport(
                baseMask, bit, leftMask, rightMask, topMask, bottomMask,
                topLeftMask, topRightMask, bottomLeftMask, bottomRightMask)) {
            continue;
        }
        uint candidate = baseMask | bitFlag;
        int score = NeighborContinuityScore(
            candidate, baseMask, leftMask, rightMask, topMask, bottomMask,
            topLeftMask, topRightMask, bottomLeftMask, bottomRightMask,
            strength);
        if (score < bestScore) {
            bestScore = score;
            bestMask = candidate;
        }
    }

    return (baseScore - bestScore >= minGain) ? bestMask : baseMask;
}

void AccumulateNeighborMeans(AsciiCell cell, uint mask, float weight,
                             float bgAmount, inout float3 bgSum,
                             inout float bgSumWeight, float fgAmount,
                             inout float3 fgSum, inout float fgSumWeight) {
    if (weight <= 0.0f) {
        return;
    }
    bool useBg = bgAmount > 0.0f && cell.hasBg != 0u;
    bool useFg = fgAmount > 0.0f && mask != 0u;
    if (!useBg && !useFg) {
        return;
    }

    float3 fg = UnpackColor(cell.fg);
    if (useBg) {
        float coverage =
            saturate(((float)countbits(mask) * kInkVisibleDotCoverage) *
                     (1.0f / 8.0f));
        bgSum += lerp(UnpackColor(cell.bg), fg, coverage) * weight;
        bgSumWeight += weight;
    }
    if (useFg) {
        fgSum += fg * weight;
        fgSumWeight += weight;
    }
}

float CodeFromNorm(float norm) {
    norm = saturate(norm);
    if (bitDepth > 8u) {
        uint shift = 16u - bitDepth;
        return norm * 65535.0f / (float)(1u << shift);
    }
    return norm * 255.0f;
}

float3 ScaleColorToLuma(float3 rgb, float y255, float targetY255) {
    targetY255 = clamp(targetY255, 0.0f, 255.0f);
    if (y255 <= 0.0f) {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return saturate(rgb * (targetY255 / y255));
}

float3 SoftenRgbTowardMean(float3 rgb, float3 mean, float amount) {
    return saturate(lerp(rgb, mean, amount));
}

float EdgeBackgroundBlendAmount(float dotCount, float validCount,
                                float colorDelta255) {
    if (dotCount <= 0.0f || dotCount >= validCount ||
        kEdgeBackgroundBlendStrength <= 0.0f) {
        return 0.0f;
    }
    float deltaAmount =
        saturate((colorDelta255 - (float)kEdgeBackgroundBlendMinDelta) /
                 max((float)(kEdgeBackgroundBlendFullDelta -
                             kEdgeBackgroundBlendMinDelta),
                     1.0f));
    float split = min(dotCount, validCount - dotCount);
    float balance = saturate((split * 2.0f) / validCount);
    return deltaAmount * balance * kEdgeBackgroundBlendStrength;
}

float ChromaHueSimilarity(float3 a, float3 b) {
    float ay = GetLuma(a);
    float by = GetLuma(b);
    float3 ac = a * 255.0f - ay.xxx;
    float3 bc = b * 255.0f - by.xxx;
    float a2 = dot(ac, ac);
    float b2 = dot(bc, bc);
    if (a2 < 64.0f || b2 < 64.0f) return -1.0f;
    float dotv = dot(ac, bc);
    if (dotv <= 0.0f) return -1.0f;
    return dotv * rsqrt(max(a2 * b2, 1.0f));
}

float ColorBoundarySoftAmount(float3 fg, float3 bg) {
    if (ChromaHueSimilarity(fg, bg) < kColorBoundaryHueSimilarity) {
        return 0.0f;
    }
    float3 d = abs(fg - bg) * 255.0f;
    float colorDelta = max(max(d.r, d.g), d.b);
    return saturate((colorDelta - (float)kColorBoundarySoftMinDelta) /
                    max((float)(kColorBoundarySoftFullDelta -
                                kColorBoundarySoftMinDelta),
                        1.0f));
}

float AttenuateSignalForColorBoundary(float signalStrength,
                                      float boundaryAmount) {
    float attenuation = min(boundaryAmount * kColorBoundarySignalAttenuation,
                            240.0f / 256.0f);
    return saturate(signalStrength * (1.0f - attenuation));
}

float GetMaxCode() {
    return (float)((1u << bitDepth) - 1u);
}

float ExpandYNorm(float yNorm) {
    float maxCode = GetMaxCode();
    float yCode = min(CodeFromNorm(yNorm), maxCode);
    uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
    float yMin = (isFullRange != 0) ? 0.0f : (float)(16u << shift);
    float yMax = (isFullRange != 0) ? maxCode : (float)(235u << shift);
    float denom = max(yMax - yMin, 1.0f);
    return saturate((yCode - yMin) / denom);
}

float2 ExpandUV(float2 uvNorm) {
    float maxCode = GetMaxCode();
    float2 uvCode =
        min(float2(CodeFromNorm(uvNorm.x), CodeFromNorm(uvNorm.y)),
            float2(maxCode, maxCode));
    uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
    float cMin = (isFullRange != 0) ? 0.0f : (float)(16u << shift);
    float cMax = (isFullRange != 0) ? maxCode : (float)(240u << shift);
    float cMid = (float)(128u << shift);
    float denom = max(cMax - cMin, 1.0f);
    return (uvCode - cMid) / denom;
}

float ApplyHdrToSdr(float v) {
    return RadioifyHdrTransferToSdr(v, yuvTransfer, kHdrScale, kTransferPq,
                                    kTransferHlg);
}

float3 ApplyHdrToSdr(float3 v) {
    return RadioifyHdrTransferToSdr(v, yuvTransfer, kHdrScale, kTransferPq,
                                    kTransferHlg);
}

float2 RotateInputUV(float2 uv) {
    uint turns = rotationQuarterTurns & 3u;
    if (turns == 1u) return float2(uv.y, 1.0f - uv.x);
    if (turns == 2u) return float2(1.0f - uv.x, 1.0f - uv.y);
    if (turns == 3u) return float2(1.0f - uv.y, uv.x);
    return uv;
}

float GetInkLevelFromLum(float lum) {
    float norm = lum / 255.0f;
    float x = kInkUseBright ? norm : (1.0f - norm);
    float coverage = pow(x, kInkGamma);
    if (coverage > 0.001f) {
        coverage = coverage * kCoverageGain + kCoverageBias;
    }
    if (coverage < kCoverageZeroCutoff) coverage = 0.0f;
    return saturate(coverage);
}

uint GetInkLevelByteFromLum(float lum) {
    return (uint)(GetInkLevelFromLum(lum) * 255.0f + 0.5f);
}

struct InputSample {
    float3 rgb;
};

static const uint kYCoeffR = 13933u;
static const uint kYCoeffG = 46871u;
static const uint kYCoeffB = 4732u;

uint RgbToYByte(uint r, uint g, uint b) {
    uint y = (r * kYCoeffR + g * kYCoeffG + b * kYCoeffB) >> 16;
    return min(y, 255u);
}

#ifndef NV12_INPUT
uint4 LoadRgba8(uint2 p) {
    float4 c = saturate(InputTexture.Load(int3((int)p.x, (int)p.y, 0)));
    return uint4((uint)(c.r * 255.0f + 0.5f),
                 (uint)(c.g * 255.0f + 0.5f),
                 (uint)(c.b * 255.0f + 0.5f),
                 (uint)(c.a * 255.0f + 0.5f));
}
#endif

void ClampSourceRange(inout uint start, inout uint end, uint extent) {
    start = min(start, extent);
    end = min(end, extent);
    if (end <= start && extent > 0u) {
        end = min(start + 1u, extent);
        if (end <= start) {
            start = end - 1u;
        }
    }
}

void ForwardSourceRange(uint index, uint count, uint extent,
                        out uint start, out uint end) {
    start = (index * extent) / count;
    end = ((index + 1u) * extent) / count;
    ClampSourceRange(start, end, extent);
}

void ReverseSourceRange(uint index, uint count, uint extent,
                        out uint start, out uint end) {
    start = ((count - index - 1u) * extent) / count;
    end = ((count - index) * extent) / count;
    ClampSourceRange(start, end, extent);
}

void SourceBoundsForDot(uint dotX, uint dotY, uint dotWCount, uint dotHCount,
                        out uint2 start, out uint2 end) {
    uint turns = rotationQuarterTurns & 3u;
    if (turns == 1u) {
        ForwardSourceRange(dotY, dotHCount, width, start.x, end.x);
        ReverseSourceRange(dotX, dotWCount, height, start.y, end.y);
    } else if (turns == 2u) {
        ReverseSourceRange(dotX, dotWCount, width, start.x, end.x);
        ReverseSourceRange(dotY, dotHCount, height, start.y, end.y);
    } else if (turns == 3u) {
        ReverseSourceRange(dotY, dotHCount, width, start.x, end.x);
        ForwardSourceRange(dotX, dotWCount, height, start.y, end.y);
    } else {
        ForwardSourceRange(dotX, dotWCount, width, start.x, end.x);
        ForwardSourceRange(dotY, dotHCount, height, start.y, end.y);
    }
}

float ExpandYCode(float yCode) {
    float maxCode = GetMaxCode();
    yCode = min(max(yCode, 0.0f), maxCode);
    uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
    float yMin = (isFullRange != 0) ? 0.0f : (float)(16u << shift);
    float yMax = (isFullRange != 0) ? maxCode : (float)(235u << shift);
    float denom = max(yMax - yMin, 1.0f);
    return saturate((yCode - yMin) / denom);
}

float2 ExpandUVCode(float2 uvCode) {
    float maxCode = GetMaxCode();
    uvCode = min(max(uvCode, float2(0.0f, 0.0f)),
                 float2(maxCode, maxCode));
    uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
    float cMin = (isFullRange != 0) ? 0.0f : (float)(16u << shift);
    float cMax = (isFullRange != 0) ? maxCode : (float)(240u << shift);
    float cMid = (float)(128u << shift);
    float denom = max(cMax - cMin, 1.0f);
    return (uvCode - cMid) / denom;
}

uint CodeFromNormRounded(float norm) {
    uint maxCode = (1u << bitDepth) - 1u;
    return min((uint)(CodeFromNorm(norm) + 0.5f), maxCode);
}

float3 YuvCodeToRgb(uint yCode, uint uCode, uint vCode) {
    float y = ExpandYCode((float)yCode);
    float2 uv_val = ExpandUVCode(float2((float)uCode, (float)vCode));

    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (yuvMatrix == kMatrixBt2020) {
        r = y + 1.4746 * uv_val.y;
        g = y - 0.16455 * uv_val.x - 0.57135 * uv_val.y;
        b = y + 1.8814 * uv_val.x;
    } else if (yuvMatrix == kMatrixBt601) {
        r = y + 1.4020 * uv_val.y;
        g = y - 0.3441 * uv_val.x - 0.7141 * uv_val.y;
        b = y + 1.7720 * uv_val.x;
    } else {
        r = y + 1.5748 * uv_val.y;
        g = y - 0.1873 * uv_val.x - 0.4681 * uv_val.y;
        b = y + 1.8556 * uv_val.x;
    }

    float3 rgb = float3(r, g, b);
#ifdef NV12_INPUT
    if (yuvTransfer != kTransferSdr) {
        rgb = ApplyHdrToSdr(rgb);
    }
#endif
    return saturate(rgb);
}

uint NormalizeYCodeToByte(uint yCode) {
    float y = ExpandYCode((float)yCode);
#ifdef NV12_INPUT
    if (yuvTransfer != kTransferSdr) {
        y = ApplyHdrToSdr(y);
    }
#endif
    return (uint)(saturate(y) * 255.0f + 0.5f);
}

InputSample SampleSourceArea(uint dotX, uint dotY, uint dotWCount,
                             uint dotHCount, out float luma) {
    // Match the CPU renderer's scaled-dot prefilter: each braille dot owns a
    // discrete source-pixel rectangle and averages all pixels in that rectangle.
    uint2 start;
    uint2 end;
    SourceBoundsForDot(dotX, dotY, dotWCount, dotHCount, start, end);

    uint count = 0u;
    InputSample sample;
#ifdef NV12_INPUT
    uint sumY = 0u;
    uint sumU = 0u;
    uint sumV = 0u;
    [loop]
    for (uint sy = start.y; sy < end.y; ++sy) {
        [loop]
        for (uint sx = start.x; sx < end.x; ++sx) {
            sumY += CodeFromNormRounded(TextureY.Load(int3((int)sx, (int)sy, 0)));
            uint2 uvPos = uint2(sx >> 1, sy >> 1);
            float2 uvNorm = TextureUV.Load(int3((int)uvPos.x, (int)uvPos.y, 0));
            sumU += CodeFromNormRounded(uvNorm.x);
            sumV += CodeFromNormRounded(uvNorm.y);
            ++count;
        }
    }
    uint half = count >> 1;
    uint yAvg = (sumY + half) / max(count, 1u);
    uint uAvg = (sumU + half) / max(count, 1u);
    uint vAvg = (sumV + half) / max(count, 1u);
    luma = (float)NormalizeYCodeToByte(yAvg);
    sample.rgb = YuvCodeToRgb(yAvg, uAvg, vAvg);
#else
    uint sumR = 0u;
    uint sumG = 0u;
    uint sumB = 0u;
    uint sumA = 0u;
    uint sumLuma = 0u;
    [loop]
    for (uint sy = start.y; sy < end.y; ++sy) {
        [loop]
        for (uint sx = start.x; sx < end.x; ++sx) {
            uint4 rgba = LoadRgba8(uint2(sx, sy));
            sumR += rgba.r;
            sumG += rgba.g;
            sumB += rgba.b;
            sumA += rgba.a;
            sumLuma += RgbToYByte(rgba.r, rgba.g, rgba.b);
            ++count;
        }
    }
    uint safeCount = max(count, 1u);
    uint avgR = sumR / safeCount;
    uint avgG = sumG / safeCount;
    uint avgB = sumB / safeCount;
    uint avgA = sumA / safeCount;
    uint avgLuma = sumLuma / safeCount;
    if (avgA == 0u) {
        avgLuma = 255u;
    }
    luma = (float)avgLuma;
    sample.rgb = float3((float)avgR, (float)avgG, (float)avgB) * (1.0f / 255.0f);
#endif
    return sample;
}

uint DotGridWidth() { return outWidth * 2u; }
uint DotGridHeight() { return outHeight * 4u; }
uint DotSampleIndex(uint x, uint y) { return y * DotGridWidth() + x; }

uint PackByte255(float v) {
    return (uint)(saturate(v / 255.0f) * 255.0f + 0.5f);
}

uint PackDotMetrics(float luma, float edge) {
    return PackByte255(luma) | (PackByte255(edge) << 8);
}

float DotLuma(AsciiDotSample sample) {
    return (float)(sample.metrics & 0xffu);
}

float DotEdge(AsciiDotSample sample) {
    return (float)((sample.metrics >> 8) & 0xffu);
}

uint FastIntSqrt(uint x) {
    if (x == 0u) {
        return 0u;
    }
    uint r = x;
    uint q = 1u;
    while (q <= r && q <= 0x40000000u) {
        q <<= 2;
    }
    while (q > 1u) {
        q >>= 2;
        bool tNonNegative = (r >= q);
        r >>= 1;
        if (tNonNegative) {
            r += q;
        }
    }
    return r;
}

AsciiDotSample DotSampleAtClamped(int x, int y, uint dotWCount,
                                  uint dotHCount) {
    uint maxX = max(dotWCount, 1u) - 1u;
    uint maxY = max(dotHCount, 1u) - 1u;
    uint cx = (uint)clamp(x, 0, (int)maxX);
    uint cy = (uint)clamp(y, 0, (int)maxY);
    return DotInputBuffer[cy * dotWCount + cx];
}

[numthreads(8, 8, 1)]
void CSDotPrefilter(uint3 DTid : SV_DispatchThreadID) {
    uint dotWCount = DotGridWidth();
    uint dotHCount = DotGridHeight();
    if (DTid.x >= dotWCount || DTid.y >= dotHCount) {
        return;
    }

    float luma = 255.0f;
    InputSample sample =
        SampleSourceArea(DTid.x, DTid.y, dotWCount, dotHCount, luma);

    AsciiDotSample outDot;
    outDot.color = PackColor(sample.rgb);
    outDot.metrics = PackDotMetrics(luma, 0.0f);
    DotOutputBuffer[DTid.y * dotWCount + DTid.x] = outDot;
}

[numthreads(8, 8, 1)]
void CSDotEdge(uint3 DTid : SV_DispatchThreadID,
               uint3 GTid : SV_GroupThreadID,
               uint3 Gid : SV_GroupID) {
    uint dotWCount = DotGridWidth();
    uint dotHCount = DotGridHeight();
    uint localLinear = GTid.y * 8u + GTid.x;
    for (uint tileIndex = localLinear; tileIndex < 100u; tileIndex += 64u) {
        int tileX = (int)(tileIndex % 10u);
        int tileY = (int)(tileIndex / 10u);
        int srcX = (int)(Gid.x * 8u) + tileX - 1;
        int srcY = (int)(Gid.y * 8u) + tileY - 1;
        gDotEdgeTileLuma[tileIndex] =
            DotLuma(DotSampleAtClamped(srcX, srcY, dotWCount, dotHCount));
    }
    GroupMemoryBarrierWithGroupSync();

    if (DTid.x >= dotWCount || DTid.y >= dotHCount) {
        return;
    }

    uint center = (GTid.y + 1u) * 10u + GTid.x + 1u;
    int l00 = (int)gDotEdgeTileLuma[center - 11u];
    int l01 = (int)gDotEdgeTileLuma[center - 10u];
    int l02 = (int)gDotEdgeTileLuma[center - 9u];
    int l10 = (int)gDotEdgeTileLuma[center - 1u];
    int l12 = (int)gDotEdgeTileLuma[center + 1u];
    int l20 = (int)gDotEdgeTileLuma[center + 9u];
    int l21 = (int)gDotEdgeTileLuma[center + 10u];
    int l22 = (int)gDotEdgeTileLuma[center + 11u];

    int gx = (l02 + 2 * l12 + l22) - (l00 + 2 * l10 + l20);
    int gy = (l20 + 2 * l21 + l22) - (l00 + 2 * l01 + l02);
    uint magSq = (uint)(gx * gx + gy * gy);
    uint edge = min(FastIntSqrt(magSq) >> 2, 255u);

    AsciiDotSample outDot = DotInputBuffer[DTid.y * dotWCount + DTid.x];
    outDot.metrics = (outDot.metrics & 0xffff00ffu) | (edge << 8);
    DotOutputBuffer[DTid.y * dotWCount + DTid.x] = outDot;
}

struct DotInfo {
    float luma;
    float edge;
    float3 color;
};

float MaskColorBoundarySoftAmount(uint mask, DotInfo dots[8]) {
    float3 sumOn = float3(0.0f, 0.0f, 0.0f);
    float3 sumOff = float3(0.0f, 0.0f, 0.0f);
    float onCount = 0.0f;
    float offCount = 0.0f;

    [unroll]
    for (int i = 0; i < 8; ++i) {
        uint bit = BrailleBitFromDotIndex((uint)i);
        if ((mask & (1u << bit)) != 0u) {
            sumOn += dots[i].color;
            onCount += 1.0f;
        } else {
            sumOff += dots[i].color;
            offCount += 1.0f;
        }
    }

    if (onCount <= 0.0f || offCount <= 0.0f) {
        return 0.0f;
    }
    return ColorBoundarySoftAmount(sumOn / onCount, sumOff / offCount);
}

int Scale256(float value) {
    return (int)(value * 256.0f + 0.5f);
}

int ClampByteInt(int v) {
    return clamp(v, 0, 255);
}

int DivRoundPositiveInt(int numerator, int denominator) {
    uint denom = (uint)max(1, denominator);
    return (int)(((uint)max(0, numerator) + (denom >> 1)) / denom);
}

int ScaleDelta256(int delta, int scale) {
    int magnitude = (abs(delta * scale) + 128) >> 8;
    return (delta >= 0) ? magnitude : -magnitude;
}

int ByteRangeTo255(int value, int start, int full) {
    if (value <= start) {
        return 0;
    }
    if (value >= full) {
        return 255;
    }
    return DivRoundPositiveInt((value - start) * 255, full - start);
}

int EdgeBackgroundBlendAmountInt(int dotCount, int validCount,
                                 int colorDelta255) {
    int strength = Scale256(kEdgeBackgroundBlendStrength);
    if (dotCount <= 0 || dotCount >= validCount || strength <= 0) {
        return 0;
    }
    int deltaAmount = ByteRangeTo255(colorDelta255,
                                     kEdgeBackgroundBlendMinDelta,
                                     kEdgeBackgroundBlendFullDelta);
    if (deltaAmount <= 0) {
        return 0;
    }
    int split = min(dotCount, validCount - dotCount);
    int balance = clamp(DivRoundPositiveInt(split * 510, validCount), 0, 255);
    return (int)(((uint)deltaAmount * (uint)balance * (uint)strength +
                  255u * 128u) /
                 (255u * 256u));
}

int BlendChannelToMeanInt(int value, int mean, int amount256) {
    return ClampByteInt(value + ScaleDelta256(mean - value, amount256));
}

int MaxRgbDeltaInt(int ar, int ag, int ab, int br, int bg, int bb) {
    return max(max(abs(ar - br), abs(ag - bg)), abs(ab - bb));
}

uint3 DotColorBytes(DotInfo dot) {
    float3 c = saturate(dot.color);
    return uint3((uint)(c.r * 255.0f + 0.5f),
                 (uint)(c.g * 255.0f + 0.5f),
                 (uint)(c.b * 255.0f + 0.5f));
}

int PerceptualDisplayErrorInt(int srcR, int srcG, int srcB, int predR,
                              int predG, int predB, int predY) {
    int dr = srcR - predR;
    int dg = srcG - predG;
    int db = srcB - predB;
    int dy = (int)RgbToYByte((uint)srcR, (uint)srcG, (uint)srcB) - predY;
    int lumaWeight = Scale256(kPerceptualLumaErrorWeight);
    int lumaError = (dy * dy * lumaWeight + 128) >> 8;
    return dr * dr + dg * dg + db * db + lumaError;
}

static const int kEdgeMaskFitInvalidScoreGpu = 0x3fffffff;

int EdgeMaskFitScore(uint mask, DotInfo dots[8]) {
    uint dotCount = countbits(mask & 0xffu);
    if (dotCount == 0u || dotCount == 8u) {
        return kEdgeMaskFitInvalidScoreGpu;
    }

    int sumOnR = 0;
    int sumOnG = 0;
    int sumOnB = 0;
    int sumOffR = 0;
    int sumOffG = 0;
    int sumOffB = 0;
    int onCount = 0;
    int offCount = 0;

    [unroll]
    for (int i = 0; i < 8; ++i) {
        uint bit = BrailleBitFromDotIndex((uint)i);
        uint3 rgb = DotColorBytes(dots[i]);
        if ((mask & (1u << bit)) != 0u) {
            sumOnR += (int)rgb.r;
            sumOnG += (int)rgb.g;
            sumOnB += (int)rgb.b;
            ++onCount;
        } else {
            sumOffR += (int)rgb.r;
            sumOffG += (int)rgb.g;
            sumOffB += (int)rgb.b;
            ++offCount;
        }
    }

    if (onCount == 0 || offCount == 0) {
        return kEdgeMaskFitInvalidScoreGpu;
    }

    int rawBgR = DivRoundPositiveInt(sumOffR, offCount);
    int rawBgG = DivRoundPositiveInt(sumOffG, offCount);
    int rawBgB = DivRoundPositiveInt(sumOffB, offCount);
    int meanOnR = DivRoundPositiveInt(sumOnR, onCount);
    int meanOnG = DivRoundPositiveInt(sumOnG, onCount);
    int meanOnB = DivRoundPositiveInt(sumOnB, onCount);
    int validCount = onCount + offCount;
    int meanAllR = DivRoundPositiveInt(sumOnR + sumOffR, validCount);
    int meanAllG = DivRoundPositiveInt(sumOnG + sumOffG, validCount);
    int meanAllB = DivRoundPositiveInt(sumOnB + sumOffB, validCount);
    int bgBlend = EdgeBackgroundBlendAmountInt(
        onCount, validCount,
        MaxRgbDeltaInt(meanOnR, meanOnG, meanOnB, rawBgR, rawBgG, rawBgB));
    int bgR = BlendChannelToMeanInt(rawBgR, meanAllR, bgBlend);
    int bgG = BlendChannelToMeanInt(rawBgG, meanAllG, bgBlend);
    int bgB = BlendChannelToMeanInt(rawBgB, meanAllB, bgBlend);

    int coverage = max(1, Scale256(kInkVisibleDotCoverage));
    int scale = min(kInkCoverageMaxScale, (256 * 256 + coverage / 2) / coverage);
    int fgR = ClampByteInt(bgR + ScaleDelta256(meanOnR - bgR, scale));
    int fgG = ClampByteInt(bgG + ScaleDelta256(meanOnG - bgG, scale));
    int fgB = ClampByteInt(bgB + ScaleDelta256(meanOnB - bgB, scale));
    int predOnR = (fgR * coverage + bgR * (256 - coverage) + 128) >> 8;
    int predOnG = (fgG * coverage + bgG * (256 - coverage) + 128) >> 8;
    int predOnB = (fgB * coverage + bgB * (256 - coverage) + 128) >> 8;
    int bgY = (int)RgbToYByte((uint)bgR, (uint)bgG, (uint)bgB);
    int fgY = (int)RgbToYByte((uint)fgR, (uint)fgG, (uint)fgB);
    int predOnY = (int)RgbToYByte((uint)predOnR, (uint)predOnG,
                                  (uint)predOnB);

    int score = 0;
    [unroll]
    for (int j = 0; j < 8; ++j) {
        uint bit = BrailleBitFromDotIndex((uint)j);
        bool on = ((mask & (1u << bit)) != 0u);
        uint3 rgb = DotColorBytes(dots[j]);
        score += PerceptualDisplayErrorInt(
            (int)rgb.r, (int)rgb.g, (int)rgb.b, on ? predOnR : bgR,
            on ? predOnG : bgG, on ? predOnB : bgB, on ? predOnY : bgY);
    }

    int brightBgPenalty = Scale256(kPerceptualBrightBgPenalty);
    if (bgY > fgY && brightBgPenalty > 0) {
        int bgLead = bgY - fgY;
        int visibleInkContrast = (bgLead * coverage + 128) >> 8;
        int dominanceArea = max(0, 256 - coverage * 2);
        int bgDominance = (bgLead * dominanceArea + 128) >> 8;
        int missing = max(kPerceptualPreferredBrightBgInkContrast -
                              visibleInkContrast,
                          bgDominance);
        if (missing > 0) {
            score += (missing * missing * onCount * brightBgPenalty + 128) >> 8;
            score = min(score, kEdgeMaskFitInvalidScoreGpu - 1);
        }
    }
    return score;
}

uint FitEdgeMask(uint initialMask, DotInfo dots[8]) {
    // GPU candidate pruning was measured with D3D timestamp queries and only
    // saved about 0-5% while causing visible drift. Keep the GPU path as the
    // simple full-sweep implementation.
    initialMask &= 0xffu;
    uint bestMask = initialMask;
    int baseScore = EdgeMaskFitScore(initialMask, dots);
    int bestScore = baseScore;

    [loop]
    for (uint candidate = 1u; candidate < 255u; ++candidate) {
        int score = EdgeMaskFitScore(candidate, dots);
        if (score < bestScore) {
            bestScore = score;
            bestMask = candidate;
        }
    }

    if (bestMask == initialMask || bestScore >= kEdgeMaskFitInvalidScoreGpu) {
        return initialMask;
    }
    if (baseScore >= kEdgeMaskFitInvalidScoreGpu) {
        return bestMask;
    }
    int requiredGain = 256 - Scale256(kEdgeMaskFitMinGain);
    if (bestScore * 256 <= baseScore * requiredGain) {
        return bestMask;
    }
    return initialMask;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= outWidth || DTid.y >= outHeight)
        return;

    uint cellIndex = DTid.y * outWidth + DTid.x;
    
    DotInfo dots[8];
    float cellLumMin = 255.0f;
    float cellLumMax = 0.0f;
    float cellLumSum = 0.0f;
    float cellEdgeMax = 0.0f;

    float3 sumAll = float3(0,0,0);

    uint dotBaseX = DTid.x * 2u;
    uint dotBaseY = DTid.y * 4u;
    uint dotStride = DotGridWidth();
    uint dotBaseIndex = dotBaseY * dotStride + dotBaseX;

    // 1. Gather prefiltered dot data
    for (int i = 0; i < 8; ++i) {
        uint ui = (uint)i;
        uint col = ui >> 2;
        uint row = ui & 3u;
        AsciiDotSample sample = DotInputBuffer[dotBaseIndex + row * dotStride +
                                               col];
        float luma = DotLuma(sample);
        float edge = DotEdge(sample);
        
        dots[i].luma = luma;
        dots[i].edge = edge;
        dots[i].color = UnpackColor(sample.color);

        cellLumMin = min(cellLumMin, luma);
        cellLumMax = max(cellLumMax, luma);
        cellLumSum += luma;
        cellEdgeMax = max(cellEdgeMax, edge);
        sumAll += dots[i].color;
    }

    float cellBgLum = (cellLumSum - cellLumMin - cellLumMax) * (1.0f / 6.0f);

    uint lumRange = max(LumaRangeBuffer[0], 1u);
    float cellRange = cellLumMax - cellLumMin;
    float flatness = saturate((90.0f - cellRange) / (90.0f - 40.0f));
    float refLum = cellBgLum;
    float hysteresis = lerp(4.0f, 10.0f, flatness);

    // 2. Adaptive Thresholding (Per-Sub-Pixel Logic)
    // This section determines which Braille dots should be active based on the input image.
    // We use a "Per-Sub-Pixel" approach inspired by the reference implementation (asciiart.ts).
    // Instead of trying to match a predefined pattern ("Target Dots"), we evaluate each of the 8 sub-pixels
    // independently to see if it differs significantly from the background. This preserves fine details
    // like single-pixel stars or thin lines that might otherwise be lost in noise reduction.

    uint2 history = HistoryBuffer[cellIndex];
    uint prevMask = history.x >> 24;

    uint bitmask = 0;
    float3 sumInk = float3(0,0,0);
    float3 sumBg = float3(0,0,0);
    int inkCount = 0;
    int bgCount = 0;

    // Base threshold (DELTA in ts)
    // This is the minimum luma difference required for a pixel to be considered "foreground".
    // A value of 26.0f (out of 255) increases sensitivity without much extra noise.
    float baseThreshold = 26.0f; 

    for (int j = 0; j < 8; ++j) {
        // Edge-aware threshold modulation:
        // In areas with strong edges (high detail), we lower the threshold to capture more subtle features.
        // In flat areas (low edge), we keep the threshold high to suppress noise.
        // Formula: threshold = max(10, base - 0.3 * edge)
        // If edge is strong (e.g., 100), threshold drops to 10, making it very sensitive.
        float threshold =
            max(kEdgeThresholdFloor, baseThreshold - dots[j].edge * 0.3f);

        // Check against local background luminance.
        float lumDiff = abs(dots[j].luma - refLum);
        
        // Decision: Is this sub-pixel a dot?
        // If the luma difference exceeds the threshold, it's considered a foreground dot.
        uint bit = BrailleBitFromDotIndex((uint)j);
        bool wasOn = ((prevMask >> bit) & 1) != 0;
        float onThreshold = threshold;
        float offThreshold = max(6.0f, threshold - hysteresis);
        bool isDot = wasOn ? (lumDiff >= offThreshold) :
                              (lumDiff >= onThreshold);

        if (isDot) {
            bitmask |= (1 << bit);
        }
    }

    // Calculate stats for contrast logic
    float cellLumMean = cellLumSum / 8.0f;
    float rawLumDiff = abs(cellLumMean - cellBgLum);
    float avgLumDiff = min(255.0f, rawLumDiff * 255.0f / (float)lumRange);
    float edgeSig = saturate((cellEdgeMax - 4.0f) / 12.0f);
    float lumSig = saturate((avgLumDiff - 4.0f) / 24.0f);
    float signalStrength = max(edgeSig, lumSig);

    bool useDither = (cellRange <= 20.0f && cellEdgeMax <= kDitherMaxEdge);
    if (useDither) {
        float ditherMean = floor(cellLumSum / 8.0f);
        float ditherBg = floor((cellLumSum - cellLumMin - cellLumMax) *
                               (1.0f / 6.0f));
        uint coverage = GetInkLevelByteFromLum(abs(ditherMean - ditherBg));
        uint ditherMask = 0;
        [unroll]
        for (int k = 0; k < 8; ++k) {
            uint bit = BrailleBitFromDotIndex((uint)k);
            if (coverage > (uint)kDitherThresholdByBit[bit]) {
                ditherMask |= (1u << bit);
            }
        }
        bitmask = ditherMask;
    }

    bool edgeMaskFitEligible =
        !useDither && cellRange >= (float)kEdgeMaskFitMinRange &&
        signalStrength >= kEdgeMaskFitMinSignal;
    if (edgeMaskFitEligible) {
        float edgeFitSignalStrength = AttenuateSignalForColorBoundary(
            signalStrength, MaskColorBoundarySoftAmount(bitmask, dots));
        edgeMaskFitEligible = edgeFitSignalStrength >= kEdgeMaskFitMinSignal;
    }
    if (edgeMaskFitEligible) {
        bitmask = FitEdgeMask(bitmask, dots);
    }

    [unroll]
    for (int k2 = 0; k2 < 8; ++k2) {
        uint bit = BrailleBitFromDotIndex((uint)k2);
        if ((bitmask & (1u << bit)) != 0u) {
            sumInk += dots[k2].color;
            inkCount++;
        } else {
            sumBg += dots[k2].color;
            bgCount++;
        }
    }

    uint dotCount = countbits(bitmask);

    // 7. Color Processing
    // Determine the initial Foreground (Ink) and Background colors based on the dots identified above.
    // If no dots are set, the cell is effectively all background.
    float3 curFg = (inkCount > 0) ? (sumInk / inkCount) : (sumAll / 8.0f);
    float3 curBg = (bgCount > 0) ? (sumBg / bgCount) : (sumAll / 8.0f);

    // Color Lift: Ensure colors aren't completely black to maintain some visibility.
    // Currently set to 0 to allow true black (zwart-zwart).
    // If you want to lift the shadows, increase kColorLift.
    curFg = max(curFg, (float)kColorLift / 255.0f);
    curBg = max(curBg, (float)kColorLift / 255.0f);

    if (bgCount > 0 && inkCount > 0 && dotCount > 0u && dotCount < 8u) {
        float3 mean = max(sumAll / 8.0f,
                          float3((float)kColorLift / 255.0f,
                                 (float)kColorLift / 255.0f,
                                 (float)kColorLift / 255.0f));
        float3 delta = abs(curFg - curBg) * 255.0f;
        float bgBlend = EdgeBackgroundBlendAmount(
            (float)dotCount, 8.0f, max(max(delta.r, delta.g), delta.b));
        if (bgBlend > 0.0f) {
            curBg = lerp(curBg, mean, bgBlend);
        }
    }

    float colorBoundaryAmount = 0.0f;
    if (bgCount > 0 && inkCount > 0 && dotCount > 0u && dotCount < 8u) {
        colorBoundaryAmount = ColorBoundarySoftAmount(curFg, curBg);
    }

    if (colorBoundaryAmount > 0.0f &&
        kColorBoundarySoftStrength > 0.0f) {
        float amount = kColorBoundarySoftStrength * colorBoundaryAmount;
        if (amount > 0.0f) {
            float3 mean = sumAll / 8.0f;
            curFg = SoftenRgbTowardMean(curFg, mean, amount);
            curBg = SoftenRgbTowardMean(curBg, mean, amount);
        }
    }

    // Temporal Stability (Ghosting Reduction)
    // We blend the current frame's color with the previous frame's color to reduce flickering.
    // However, too much blending causes "ghosting" (trails behind moving objects).
    float3 prevFg = UnpackColor(history.x);
    float3 prevBg = UnpackColor(history.y);

    // Calculate perceptual distance to decide if we should reset (cut) or blend.
    float diffFg = dot(abs(curFg - prevFg), float3(1.0f, 1.0f, 1.0f)) *
                   255.0f;
    float diffBg = dot(abs(curBg - prevBg), float3(1.0f, 1.0f, 1.0f)) *
                   255.0f;
    
    float resetThresh = (float)kTemporalResetDelta;

    // Alpha Blending:
    // We use a high alpha (230/256 ~= 0.9) to favor the new frame.
    // This significantly reduces ghosting compared to the old value (0.7).
    // If the change is large (> resetThresh), we snap instantly (alpha=1.0 implicitly).
    if (diffFg < resetThresh) {
        curFg = lerp(prevFg, curFg, 230.0f / 256.0f);
    }
    float bgBlendAlpha = 230.0f / 256.0f;
    if (diffBg < resetThresh) {
        curBg = lerp(prevBg, curBg, bgBlendAlpha);
    }

    AsciiCell cell;
    cell.ch = kBrailleBase + bitmask;
    cell.fg = PackColor(curFg);
    cell.bg = PackColor(curBg);
    
    cell.hasBg = 1u;

    OutputBuffer[cellIndex] = cell;
}

uint RefineInputMaskAt(int x, int y) {
    if (x < 0 || y < 0 || x >= (int)outWidth || y >= (int)outHeight) {
        return 0u;
    }
    return ExtractBrailleMask(RefineInput[(uint)y * outWidth + (uint)x]);
}

AsciiCell RefineInputCellAt(int x, int y) {
    if (x < 0 || y < 0 || x >= (int)outWidth || y >= (int)outHeight) {
        AsciiCell emptyCell;
        emptyCell.ch = kBrailleBase;
        emptyCell.fg = 0u;
        emptyCell.bg = 0u;
        emptyCell.hasBg = 0u;
        return emptyCell;
    }
    return RefineInput[(uint)y * outWidth + (uint)x];
}

[numthreads(8, 8, 1)]
void CSRefineContinuity(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= outWidth || DTid.y >= outHeight) {
        return;
    }

    uint cellIndex = DTid.y * outWidth + DTid.x;
    AsciiCell cell = RefineInput[cellIndex];
    uint baseMask = ExtractBrailleMask(cell);
    float contrast = 0.0f;
    if (cell.hasBg != 0u) {
        contrast = abs(GetLuma(UnpackColor(cell.fg)) -
                       GetLuma(UnpackColor(cell.bg)));
    }

    uint dotCount = countbits(baseMask);
    bool needsTopology =
        kNeighborContinuityStrength > 0.0f &&
        baseMask != 0u && baseMask != 0xffu &&
        contrast >= (float)kNeighborContinuityMinContrast;
    bool needsColorAa =
        cell.hasBg != 0u && dotCount > 0u && dotCount < 8u &&
        (kNeighborColorAaStrength > 0.0f ||
         kNeighborFgColorAaStrength > 0.0f) &&
        contrast >= (float)kNeighborColorAaMinContrast;
    if (!needsTopology && !needsColorAa) {
        OutputBuffer[cellIndex] = cell;
        return;
    }

    int x = (int)DTid.x;
    int y = (int)DTid.y;
    uint leftMask = 0u;
    uint rightMask = 0u;
    uint topMask = 0u;
    uint bottomMask = 0u;
    uint topLeftMask = 0u;
    uint topRightMask = 0u;
    uint bottomLeftMask = 0u;
    uint bottomRightMask = 0u;
    if (needsTopology) {
        leftMask = RefineInputMaskAt(x - 1, y);
        rightMask = RefineInputMaskAt(x + 1, y);
        topMask = RefineInputMaskAt(x, y - 1);
        bottomMask = RefineInputMaskAt(x, y + 1);
        topLeftMask = RefineInputMaskAt(x - 1, y - 1);
        topRightMask = RefineInputMaskAt(x + 1, y - 1);
        bottomLeftMask = RefineInputMaskAt(x - 1, y + 1);
        bottomRightMask = RefineInputMaskAt(x + 1, y + 1);
    }
    uint refinedMask = baseMask;
    if (needsTopology) {
        refinedMask = RefineNeighborContinuityMask(
            baseMask, leftMask, rightMask, topMask, bottomMask, topLeftMask,
            topRightMask, bottomLeftMask, bottomRightMask, contrast);
        if (refinedMask != baseMask) {
            cell.ch = kBrailleBase + refinedMask;
        }
    }

    dotCount = countbits(refinedMask);
    if (needsColorAa && dotCount > 0u && dotCount < 8u) {
        float3 curBg = UnpackColor(cell.bg);
        float3 curFg = UnpackColor(cell.fg);

        float partialness = (float)min(dotCount, 8u - dotCount) * 0.25f;
        float contrastAmount =
            saturate((contrast - (float)kNeighborColorAaMinContrast) / 48.0f);
        float bgAmount =
            kNeighborColorAaStrength * partialness * contrastAmount;
        float fgAmount =
            kNeighborFgColorAaStrength * partialness * contrastAmount;
        if (bgAmount > 0.0f || fgAmount > 0.0f) {
            AsciiCell leftCell = RefineInputCellAt(x - 1, y);
            AsciiCell rightCell = RefineInputCellAt(x + 1, y);
            AsciiCell topCell = RefineInputCellAt(x, y - 1);
            AsciiCell bottomCell = RefineInputCellAt(x, y + 1);
            AsciiCell topLeftCell = RefineInputCellAt(x - 1, y - 1);
            AsciiCell topRightCell = RefineInputCellAt(x + 1, y - 1);
            AsciiCell bottomLeftCell = RefineInputCellAt(x - 1, y + 1);
            AsciiCell bottomRightCell = RefineInputCellAt(x + 1, y + 1);
            if (!needsTopology) {
                leftMask = ExtractBrailleMask(leftCell);
                rightMask = ExtractBrailleMask(rightCell);
                topMask = ExtractBrailleMask(topCell);
                bottomMask = ExtractBrailleMask(bottomCell);
                topLeftMask = ExtractBrailleMask(topLeftCell);
                topRightMask = ExtractBrailleMask(topRightCell);
                bottomLeftMask = ExtractBrailleMask(bottomLeftCell);
                bottomRightMask = ExtractBrailleMask(bottomRightCell);
            }

            float3 bgSum = curBg * 4.0f;
            float bgSumWeight = 4.0f;
            float3 fgSum = curFg * 6.0f;
            float fgSumWeight = 6.0f;
            AccumulateNeighborMeans(leftCell, leftMask, 2.0f, bgAmount, bgSum,
                                    bgSumWeight, fgAmount, fgSum, fgSumWeight);
            AccumulateNeighborMeans(rightCell, rightMask, 2.0f, bgAmount, bgSum,
                                    bgSumWeight, fgAmount, fgSum, fgSumWeight);
            AccumulateNeighborMeans(topCell, topMask, 2.0f, bgAmount, bgSum,
                                    bgSumWeight, fgAmount, fgSum, fgSumWeight);
            AccumulateNeighborMeans(bottomCell, bottomMask, 2.0f, bgAmount,
                                    bgSum, bgSumWeight, fgAmount, fgSum,
                                    fgSumWeight);
            AccumulateNeighborMeans(topLeftCell, topLeftMask, 1.0f, bgAmount,
                                    bgSum, bgSumWeight, fgAmount, fgSum,
                                    fgSumWeight);
            AccumulateNeighborMeans(topRightCell, topRightMask, 1.0f, bgAmount,
                                    bgSum, bgSumWeight, fgAmount, fgSum,
                                    fgSumWeight);
            AccumulateNeighborMeans(bottomLeftCell, bottomLeftMask, 1.0f,
                                    bgAmount, bgSum, bgSumWeight, fgAmount,
                                    fgSum, fgSumWeight);
            AccumulateNeighborMeans(bottomRightCell, bottomRightMask, 1.0f,
                                    bgAmount, bgSum, bgSumWeight, fgAmount,
                                    fgSum, fgSumWeight);
            if (bgAmount > 0.0f) {
                curBg = lerp(curBg, bgSum / bgSumWeight, bgAmount);
            }
            if (fgAmount > 0.0f) {
                float3 oldFg = curFg;
                curFg = lerp(curFg, fgSum / fgSumWeight, fgAmount);

                float bgY = GetLuma(curBg);
                float fgY = GetLuma(curFg);
                float oldFgY = GetLuma(oldFg);
                float minDelta =
                    min(abs(oldFgY - bgY), (float)kNeighborColorAaMinContrast);
                if (minDelta > 0.0f && abs(fgY - bgY) < minDelta) {
                    float targetY = clamp(bgY + ((oldFgY >= bgY) ? minDelta
                                                                 : -minDelta),
                                          0.0f, 255.0f);
                    curFg = ScaleColorToLuma(curFg, max(fgY, 1.0f), targetY);
                }
                cell.fg = PackColor(curFg);
            }
            if (bgAmount > 0.0f) {
                cell.bg = PackColor(curBg);
            }
        }
    }
    OutputBuffer[cellIndex] = cell;
}

[numthreads(8, 8, 1)]
void CSSyncHistory(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= outWidth || DTid.y >= outHeight) {
        return;
    }

    uint cellIndex = DTid.y * outWidth + DTid.x;
    AsciiCell cell = OutputBuffer[cellIndex];
    uint bitmask = 0u;
    if (cell.ch >= kBrailleBase && cell.ch <= (kBrailleBase + 0xFFu)) {
        bitmask = cell.ch - kBrailleBase;
    }
    uint fg24 = cell.fg & 0x00FFFFFFu;
    HistoryBuffer[cellIndex] = uint2(fg24 | (bitmask << 24), cell.bg);
}
