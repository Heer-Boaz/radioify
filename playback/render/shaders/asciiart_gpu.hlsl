
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
StructuredBuffer<uint4> StatsBuffer : register(t2); // y=lumRange
#else
Texture2D<float4> InputTexture : register(t0);
StructuredBuffer<uint4> StatsBuffer : register(t1); // y=lumRange
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
static const float kShadowChromaBoostStart = 12.0f;
static const float kShadowChromaBoostFull = 72.0f;
static const float kShadowChromaPreserveStrength = 160.0f / 256.0f;

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

float ShadowSaturationFromLuma(float y255) {
    if (y255 <= (float)kShadowSatStartLuma) {
        return kShadowMinSaturation;
    }
    if (y255 >= (float)kShadowSatFullLuma) {
        return 1.0f;
    }
    float t = (y255 - (float)kShadowSatStartLuma) /
              max((float)(kShadowSatFullLuma - kShadowSatStartLuma), 1.0f);
    return lerp(kShadowMinSaturation, 1.0f, t);
}

float ShadowSaturationWithChroma(float y255, float chroma255) {
    float keep = ShadowSaturationFromLuma(y255);
    if (keep >= 1.0f) {
        return 1.0f;
    }
    if (chroma255 <= kShadowChromaBoostStart) {
        return keep;
    }
    float chromaBoost = 1.0f;
    if (chroma255 < kShadowChromaBoostFull) {
        chromaBoost = (chroma255 - kShadowChromaBoostStart) /
                      (kShadowChromaBoostFull - kShadowChromaBoostStart);
    }
    float shadowWeight = 1.0f - keep;
    float preserveGain =
        shadowWeight * chromaBoost * kShadowChromaPreserveStrength;
    return min(1.0f, keep + (1.0f - keep) * preserveGain);
}

float GetSourceBlueConfidence(float blueSignal, float chromaSignal) {
    float blue = saturate((blueSignal - kSourceBlueSignalStart) /
                          max(kSourceBlueSignalFull - kSourceBlueSignalStart,
                              1e-5f));
    float chroma = saturate((chromaSignal - kSourceChromaSignalStart) /
                            max(kSourceChromaSignalFull -
                                    kSourceChromaSignalStart,
                                1e-5f));
    return blue * chroma;
}

float3 ApplySaturationAroundLuma(float3 rgb, float y255, float saturation) {
    float gray = y255 / 255.0f;
    return saturate(gray.xxx + (rgb - gray.xxx) * saturation);
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

float3 CompressShadowChroma(float3 rgb, float sourceBlueConfidence) {
    float y = GetLuma(rgb);
    float chroma = (max(max(rgb.r, rgb.g), rgb.b) -
                    min(min(rgb.r, rgb.g), rgb.b)) *
                   255.0f;
    float keep = ShadowSaturationWithChroma(y, chroma);
    if (keep < 1.0f) {
        rgb = ApplySaturationAroundLuma(rgb, y, keep);
    }

    float blueDominance = rgb.b - max(rgb.r, rgb.g);
    float dark = saturate((kShadowBlueGuardStartLuma - y) /
                          max(kShadowBlueGuardRange, 1.0f));
    float dominance = saturate((blueDominance - kShadowBlueDominanceStart) /
                               max(kShadowBlueDominanceFull -
                                       kShadowBlueDominanceStart,
                                   1e-5f));
    float guard =
        dark * dominance * (1.0f - kSourceBlueGuardRelax *
                                          saturate(sourceBlueConfidence));
    if (guard <= 0.0f) {
        return saturate(rgb);
    }
    return ApplySaturationAroundLuma(rgb, y,
                                     lerp(1.0f, kShadowBlueGuardKeep, guard));
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

float PQEotf(float v) {
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 128.0f;
    const float c3 = 2392.0f / 128.0f;
    float vp = pow(max(v, 0.0f), 1.0f / m2);
    float num = max(vp - c1, 0.0f);
    float den = max(c2 - c3 * vp, 1e-6f);
    return pow(num / den, 1.0f / m1);
}

float3 PQEotf(float3 v) {
    return float3(PQEotf(v.x), PQEotf(v.y), PQEotf(v.z));
}

float HlgEotf(float v) {
    const float a = 0.17883277f;
    const float b = 1.0f - 4.0f * a;
    const float c = 0.5f - a * log(4.0f * a);
    v = saturate(v);
    if (v <= 0.5f) {
        return (v * v) / 3.0f;
    }
    return (exp((v - c) / a) + b) / 12.0f;
}

float3 HlgEotf(float3 v) {
    return float3(HlgEotf(v.x), HlgEotf(v.y), HlgEotf(v.z));
}

float ToneMapFilmic(float x) {
    const float A = 0.15f;
    const float B = 0.50f;
    const float C = 0.10f;
    const float D = 0.20f;
    const float E = 0.02f;
    const float F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 ToneMapFilmic(float3 x) {
    return float3(ToneMapFilmic(x.x), ToneMapFilmic(x.y), ToneMapFilmic(x.z));
}

float LinearToSrgb(float v) {
    v = max(v, 0.0f);
    if (v <= 0.0031308f) {
        return v * 12.92f;
    }
    return 1.055f * pow(v, 1.0f / 2.4f) - 0.055f;
}

float3 LinearToSrgb(float3 v) {
    return float3(LinearToSrgb(v.x), LinearToSrgb(v.y), LinearToSrgb(v.z));
}

float ApplyHdrToSdr(float v) {
    v = saturate(v);
    if (yuvTransfer == kTransferPq) {
        v = PQEotf(v);
    } else {
        v = HlgEotf(v);
    }
    v = ToneMapFilmic(v * kHdrScale);
    v = saturate(v);
    return LinearToSrgb(v);
}

float3 ApplyHdrToSdr(float3 v) {
    v = saturate(v);
    if (yuvTransfer == kTransferPq) {
        v = PQEotf(v);
    } else {
        v = HlgEotf(v);
    }
    v = ToneMapFilmic(v * kHdrScale);
    v = saturate(v);
    return LinearToSrgb(v);
}

float2 RotateInputUV(float2 uv) {
    uint turns = rotationQuarterTurns & 3u;
    if (turns == 1u) return float2(uv.y, 1.0f - uv.x);
    if (turns == 2u) return float2(1.0f - uv.x, 1.0f - uv.y);
    if (turns == 3u) return float2(1.0f - uv.y, uv.x);
    return uv;
}

float SampleLumaRaw(float2 uv, SamplerState s) {
    float2 srcUv = RotateInputUV(uv);
#ifdef NV12_INPUT
    float yNorm = TextureY.SampleLevel(s, srcUv, 0);
    return ExpandYNorm(yNorm) * 255.0f;
#else
    return GetLuma(InputTexture.SampleLevel(s, srcUv, 0).rgb);
#endif
}

float SampleLuma(float2 uv) { return SampleLumaRaw(uv, LinearSampler); }
float SampleLumaArea(float2 centerUV, float2 dotSizePx) {
    // 4x4 adaptive Gaussian filter (16 samples)
    // Reverted to 16 samples to fix TDR/Freeze, but added Gaussian weighting for precision
    float2 texSize = float2(width, height);
    float2 step = dotSizePx / 4.0f / texSize;
    // Start at top-left of the dot area
    float2 start = centerUV - (dotSizePx * 0.5f / texSize) + (step * 0.5f);

    float sum = 0;
    float weightSum = 0;
    
    [unroll]
    for (int dy = 0; dy < 4; ++dy) {
        [unroll]
        for (int dx = 0; dx < 4; ++dx) {
            // Center of 4x4 grid is (1.5, 1.5)
            float distX = abs((float)dx - 1.5f);
            float distY = abs((float)dy - 1.5f);
            // Max dist sq = 1.5^2 + 1.5^2 = 4.5
            float normDist = (distX * distX + distY * distY) / 4.5f;
            
            // Weight: 1.0 at center, ~0.4 at corners
            float weight = 1.0f - (normDist * 0.6f);
            
            sum += SampleLumaRaw(start + float2((float)dx, (float)dy) * step, LinearSampler) * weight;
            weightSum += weight;
        }
    }
    float luma = sum / weightSum;
#ifdef NV12_INPUT
    if (yuvTransfer != kTransferSdr) {
        luma = ApplyHdrToSdr(luma / 255.0f) * 255.0f;
    }
#endif
    return luma;
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

struct InputSample {
    float3 rgb;
    float chromaSignal;
    float blueSignal;
};

InputSample SampleInput(float2 uv) {
    float2 srcUv = RotateInputUV(uv);
    InputSample sample;
#ifdef NV12_INPUT
    float y = ExpandYNorm(TextureY.SampleLevel(LinearSampler, srcUv, 0));
    float2 uv_val = ExpandUV(TextureUV.SampleLevel(LinearSampler, srcUv, 0));
    sample.chromaSignal = max(abs(uv_val.x), abs(uv_val.y));
    sample.blueSignal = max(uv_val.x - max(uv_val.y, 0.0f) * 0.75f, 0.0f);

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
    sample.rgb = float3(r, g, b);
#else
    sample.rgb = InputTexture.SampleLevel(LinearSampler, srcUv, 0).rgb;
    sample.chromaSignal = 0.0f;
    sample.blueSignal = 0.0f;
#endif

    float3 rgb = sample.rgb;
#ifdef NV12_INPUT
    if (yuvTransfer != kTransferSdr) {
        rgb = ApplyHdrToSdr(rgb);
    }
#endif
    rgb = saturate(rgb);

    sample.rgb = rgb;
    return sample;
}

InputSample SampleInputArea(float2 centerUV, float2 dotSizePx) {
    float2 texSize = float2(width, height);
    float2 offset = dotSizePx * 0.25f / texSize;
    InputSample s00 = SampleInput(centerUV + float2(-offset.x, -offset.y));
    InputSample s10 = SampleInput(centerUV + float2( offset.x, -offset.y));
    InputSample s01 = SampleInput(centerUV + float2(-offset.x,  offset.y));
    InputSample s11 = SampleInput(centerUV + float2( offset.x,  offset.y));

    InputSample sample;
    sample.rgb = (s00.rgb + s10.rgb + s01.rgb + s11.rgb) * 0.25f;
    sample.blueSignal =
        (s00.blueSignal + s10.blueSignal + s01.blueSignal + s11.blueSignal) *
        0.25f;
    sample.chromaSignal =
        (s00.chromaSignal + s10.chromaSignal + s01.chromaSignal +
         s11.chromaSignal) *
        0.25f;
    return sample;
}

uint DotGridWidth() { return outWidth * 2u; }
uint DotGridHeight() { return outHeight * 4u; }
uint DotSampleIndex(uint x, uint y) { return y * DotGridWidth() + x; }

uint PackByte255(float v) {
    return (uint)(saturate(v / 255.0f) * 255.0f + 0.5f);
}

uint PackDotMetrics(float luma, float edge, float sourceBlueConfidence) {
    return PackByte255(luma) |
           (PackByte255(edge) << 8) |
           ((uint)(saturate(sourceBlueConfidence) * 255.0f + 0.5f) << 16);
}

float DotLuma(AsciiDotSample sample) {
    return (float)(sample.metrics & 0xffu);
}

float DotEdge(AsciiDotSample sample) {
    return (float)((sample.metrics >> 8) & 0xffu);
}

float DotSourceBlueConfidence(AsciiDotSample sample) {
    return (float)((sample.metrics >> 16) & 0xffu) * (1.0f / 255.0f);
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

    float cellW = (float)width / (float)outWidth;
    float cellH = (float)height / (float)outHeight;
    float dotW = cellW * 0.5f;
    float dotH = cellH * 0.25f;
    float2 dotSize = float2(dotW, dotH);
    float2 uv = float2(((float)DTid.x + 0.5f) * dotW / (float)width,
                       ((float)DTid.y + 0.5f) * dotH / (float)height);

    float luma = SampleLumaArea(uv, dotSize);
    InputSample sample = SampleInputArea(uv, dotSize);

    AsciiDotSample outDot;
    outDot.color = PackColor(sample.rgb);
    outDot.metrics = PackDotMetrics(
        luma, 0.0f,
        GetSourceBlueConfidence(sample.blueSignal, sample.chromaSignal));
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
    float l00 = gDotEdgeTileLuma[center - 11u];
    float l01 = gDotEdgeTileLuma[center - 10u];
    float l02 = gDotEdgeTileLuma[center - 9u];
    float l10 = gDotEdgeTileLuma[center - 1u];
    float l12 = gDotEdgeTileLuma[center + 1u];
    float l20 = gDotEdgeTileLuma[center + 9u];
    float l21 = gDotEdgeTileLuma[center + 10u];
    float l22 = gDotEdgeTileLuma[center + 11u];

    float gx = (l02 + 2.0f * l12 + l22) - (l00 + 2.0f * l10 + l20);
    float gy = (l20 + 2.0f * l21 + l22) - (l00 + 2.0f * l01 + l02);
    float edge = sqrt(gx * gx + gy * gy) * 0.25f;

    AsciiDotSample outDot = DotInputBuffer[DTid.y * dotWCount + DTid.x];
    outDot.metrics = (outDot.metrics & 0xffff00ffu) | (PackByte255(edge) << 8);
    DotOutputBuffer[DTid.y * dotWCount + DTid.x] = outDot;
}

struct DotInfo {
    float luma;
    float edge;
    float3 color;
    float sourceBlueConfidence;
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

float EdgeMaskFitScore(uint mask, DotInfo dots[8]) {
    uint dotCount = countbits(mask & 0xffu);
    if (dotCount == 0u || dotCount == 8u) {
        return 3.402823e+38f;
    }

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

    float3 rawBg = sumOff / offCount;
    float3 meanOn = sumOn / onCount;
    float validCount = onCount + offCount;
    float3 meanAll = (sumOn + sumOff) / validCount;
    float3 delta = abs(meanOn - rawBg) * 255.0f;
    float bgBlend = EdgeBackgroundBlendAmount(
        onCount, validCount, max(max(delta.r, delta.g), delta.b));
    float3 bg = lerp(rawBg, meanAll, bgBlend);
    float scale = min((float)kInkCoverageMaxScale / 256.0f,
                      1.0f / max(kInkVisibleDotCoverage, 1e-5f));
    float3 fg = saturate(bg + (meanOn - bg) * scale);
    float3 predOn = lerp(bg, fg, kInkVisibleDotCoverage);
    float bgY = GetLuma(bg);
    float fgY = GetLuma(fg);
    float predOnY = GetLuma(predOn);

    float score = 0.0f;
    [unroll]
    for (int j = 0; j < 8; ++j) {
        uint bit = BrailleBitFromDotIndex((uint)j);
        bool on = ((mask & (1u << bit)) != 0u);
        float3 pred = on ? predOn : bg;
        float3 d = dots[j].color - pred;
        float dy = (GetLuma(dots[j].color) - (on ? predOnY : bgY)) / 255.0f;
        score += dot(d, d) + dy * dy * kPerceptualLumaErrorWeight;
    }
    if (bgY > fgY && kPerceptualBrightBgPenalty > 0.0f) {
        float bgLead = bgY - fgY;
        float visibleInkContrast = bgLead * kInkVisibleDotCoverage;
        float dominanceArea = max(0.0f, 1.0f - kInkVisibleDotCoverage * 2.0f);
        float bgDominance = bgLead * dominanceArea;
        float missing =
            max((float)kPerceptualPreferredBrightBgInkContrast -
                    visibleInkContrast,
                bgDominance);
        if (missing > 0.0f) {
            float missingNorm = missing / 255.0f;
            score += missingNorm * missingNorm * onCount *
                     kPerceptualBrightBgPenalty;
        }
    }
    return score;
}

uint FitEdgeMask(uint initialMask, DotInfo dots[8]) {
    initialMask &= 0xffu;
    uint bestMask = initialMask;
    float baseScore = EdgeMaskFitScore(initialMask, dots);
    float bestScore = baseScore;

    [loop]
    for (uint candidate = 1u; candidate < 255u; ++candidate) {
        float score = EdgeMaskFitScore(candidate, dots);
        if (score < bestScore) {
            bestScore = score;
            bestMask = candidate;
        }
    }

    if (bestMask == initialMask || bestScore >= 3.402823e+37f) {
        return initialMask;
    }
    if (baseScore >= 3.402823e+37f) {
        return bestMask;
    }
    if (bestScore <= baseScore * (1.0f - kEdgeMaskFitMinGain)) {
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
    float sumAllBlueConfidence = 0.0f;

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
        dots[i].sourceBlueConfidence = DotSourceBlueConfidence(sample);

        cellLumMin = min(cellLumMin, luma);
        cellLumMax = max(cellLumMax, luma);
        cellLumSum += luma;
        cellEdgeMax = max(cellEdgeMax, edge);
        sumAll += dots[i].color;
        sumAllBlueConfidence += dots[i].sourceBlueConfidence;
    }

    float cellBgLum = (cellLumSum - cellLumMin - cellLumMax) * (1.0f / 6.0f);

    uint lumRange = max(StatsBuffer[0].y, 1u);
    float cellRange = cellLumMax - cellLumMin;
    float alpha = saturate((90.0f - cellRange) / (90.0f - 40.0f));
    float refLum = cellBgLum;
    float hysteresis = lerp(4.0f, 10.0f, alpha);

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
    float sumInkBlueConfidence = 0.0f;
    float sumBgBlueConfidence = 0.0f;
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
        float coverage = GetInkLevelFromLum(rawLumDiff);
        float coverageU = coverage * 255.0f;
        uint ditherMask = 0;
        [unroll]
        for (int k = 0; k < 8; ++k) {
            uint bit = BrailleBitFromDotIndex((uint)k);
            if (coverageU > (float)kDitherThresholdByBit[bit]) {
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
            sumInkBlueConfidence += dots[k2].sourceBlueConfidence;
            inkCount++;
        } else {
            sumBg += dots[k2].color;
            sumBgBlueConfidence += dots[k2].sourceBlueConfidence;
            bgCount++;
        }
    }

    uint dotCount = countbits(bitmask);

    // 7. Color Processing
    // Determine the initial Foreground (Ink) and Background colors based on the dots identified above.
    // If no dots are set, the cell is effectively all background.
    float3 curFg = (inkCount > 0) ? (sumInk / inkCount) : (sumAll / 8.0f);
    float3 curBg = (bgCount > 0) ? (sumBg / bgCount) : (sumAll / 8.0f);
    float curFgBlueConfidence =
        (inkCount > 0) ? (sumInkBlueConfidence / inkCount)
                       : (sumAllBlueConfidence / 8.0f);
    float curBgBlueConfidence =
        (bgCount > 0) ? (sumBgBlueConfidence / bgCount)
                      : (sumAllBlueConfidence / 8.0f);

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
            curBgBlueConfidence =
                lerp(curBgBlueConfidence, sumAllBlueConfidence / 8.0f,
                     bgBlend);
        }
    }

    float colorBoundaryAmount = 0.0f;
    if (bgCount > 0 && inkCount > 0 && dotCount > 0u && dotCount < 8u) {
        colorBoundaryAmount = ColorBoundarySoftAmount(curFg, curBg);
    }
    float effectiveSignalStrength = AttenuateSignalForColorBoundary(
        signalStrength, colorBoundaryAmount);

    // Intelligent Contrast Management
    // This system dynamically adjusts contrast based on the "Signal Strength" of the cell.
    // Goal:
    // 1. Noise Suppression: If the signal is weak (noise), blend the foreground into the background
    //    to make it look like a subtle texture rather than hard noise.
    // 2. Detail Enhancement: If the signal is strong (real detail), boost the contrast to make it pop.
    
    // Calculate Signal Strength:
    // We look at both Edge magnitude and Luma difference.
    // - Edge Signal: Ramps from 0 to 1 as edge strength goes from 4 to 16.
    // - Luma Signal: Ramps from 0 to 1 as luma difference goes from 4 to 28.
    float blendStrength =
        kSignalStrengthFloor +
        (1.0f - kSignalStrengthFloor) * effectiveSignalStrength;

    // Dampening (Noise Hiding):
    // If signalStrength is low, we interpolate curFg towards curBg.
    // This effectively "fades out" noise into the background.
    curFg = lerp(curBg, curFg, blendStrength);

    // Boosting (Detail Pop):
    // If signalStrength is high (> 0.8), we apply an asymmetrical contrast boost.
    if (effectiveSignalStrength > 0.8f) {
        // Normalize the range [0.8, 1.0] to [0.0, 1.0]
        float boost = (effectiveSignalStrength - 0.8f) * 5.0f;
        
        // Calculate the midpoint (gray point) between FG and BG
        float3 center = (curFg + curBg) * 0.5f;
        
        // 'delta' is the vector from Center to Foreground.
        // Conversely, the vector from Center to Background is -delta.
        float3 delta = curFg - center;
        
        // Asymmetrical Boost Logic:
        // We expand the distance from the center to increase contrast.
        // Formula: NewPos = Center + DirectionVector * ExpansionFactor
        
        // Match the CPU renderer's asymmetric boost: ink moves strongly, the
        // background only a little so edge context stays visible.
        curFg = center + delta * (1.0f + boost * (127.0f / 256.0f));
        curBg = center - delta * (1.0f + boost * (31.0f / 256.0f));
        
        curFg = saturate(curFg);
        curBg = saturate(curBg);
    }

    // Luma Correction (Ink)
    // Ensure the ink isn't too dark to be seen.
    float curY = GetLuma(curFg);
    if (curY < (float)kInkMinLuma) {
        if (curY <= 0.0f) {
            curFg = (float)kInkMinLuma / 255.0f;
        } else {
            float scale = ((float)kInkMinLuma * 256.0f) / curY;
            if (scale > (float)kInkMaxScale) scale = (float)kInkMaxScale;
            curFg = min(1.0f, (curFg * scale) / 256.0f);
        }
    }

    // Adaptive Saturation (Ink)
    // Adjust saturation based on brightness.
    // Dark colors get reduced saturation to prevent blue/purple artifacts in shadows.
    curY = GetLuma(curFg);
    float inkChroma = (max(max(curFg.r, curFg.g), curFg.b) -
                       min(min(curFg.r, curFg.g), curFg.b)) *
                      255.0f;
    float adaptiveSat =
        (float)kColorSaturation * ShadowSaturationWithChroma(curY, inkChroma);
    adaptiveSat *=
        1.0f + kSourceBlueInkBoost * saturate(curFgBlueConfidence) *
        saturate((60.0f - curY) / 60.0f);
    curFg = ApplySaturationAroundLuma(curFg, curY, adaptiveSat / 256.0f);

    if (!useDither && effectiveSignalStrength >= kInkCoverageMinSignal &&
        bgCount > 0 && inkCount > 0 && dotCount > 0u && dotCount < 8u) {
        float scale =
            min((float)kInkCoverageMaxScale / 256.0f,
                1.0f / max(kInkVisibleDotCoverage, 1e-5f));
        if (scale > 1.0f) {
            curFg = saturate(curBg + (curFg - curBg) * scale);
            float coverageY = GetLuma(curFg);
            if (coverageY < (float)kInkCoverageMinLuma) {
                curFg = ScaleColorToLuma(curFg, coverageY,
                                         (float)kInkCoverageMinLuma);
            }
        }
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
    curFg = CompressShadowChroma(curFg, curFgBlueConfidence);
    curBg = CompressShadowChroma(curBg, curBgBlueConfidence);

    bool fullMask = (dotCount == 8u);
    if (fullMask && bgCount == 0) {
        float dir = (cellLumMean < 128.0f) ? 1.0f : -1.0f;
        float minDeltaY = 6.0f;
        float fgY = GetLuma(curFg);
        float bgY = GetLuma(curBg);
        float need = minDeltaY - dir * (bgY - fgY);
        if (need > 0.0f) {
            float shift = dir * need / 255.0f;
            curBg = saturate(curBg + shift);
            curBg = CompressShadowChroma(curBg, curBgBlueConfidence);
        }
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
