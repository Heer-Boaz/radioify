
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
StructuredBuffer<uint4> StatsBuffer : register(t2); // x=bgLum, y=lumRange
#else
Texture2D<float4> InputTexture : register(t0);
StructuredBuffer<uint4> StatsBuffer : register(t1); // x=bgLum, y=lumRange
#endif

SamplerState LinearSampler : register(s0);

struct AsciiCell {
    uint ch;
    uint fg;
    uint bg;
    uint hasBg;
};

RWStructuredBuffer<AsciiCell> OutputBuffer : register(u0);
RWStructuredBuffer<uint2> HistoryBuffer : register(u1); // x=Fg, y=Bg (packed)

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
    uint r = (uint)(saturate(c.r) * 255.0f);
    uint g = (uint)(saturate(c.g) * 255.0f);
    uint b = (uint)(saturate(c.b) * 255.0f);
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

float GetShadowSaturation(float y255) {
    float t = saturate((y255 - kShadowSatStartLuma) /
                       max(kShadowSatFullLuma - kShadowSatStartLuma, 1.0f));
    return lerp(kShadowMinSaturation, 1.0f, sqrt(t));
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

float3 CompressShadowChroma(float3 rgb, float sourceBlueConfidence) {
    float y = GetLuma(rgb);
    float keep = GetShadowSaturation(y);
    rgb = ApplySaturationAroundLuma(rgb, y, keep);

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
    float yCode = yNorm * maxCode;
    uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
    float yMin = (isFullRange != 0) ? 0.0f : (float)(16u << shift);
    float yMax = (isFullRange != 0) ? maxCode : (float)(235u << shift);
    float denom = max(yMax - yMin, 1.0f);
    return saturate((yCode - yMin) / denom);
}

float2 ExpandUV(float2 uvNorm) {
    float maxCode = GetMaxCode();
    float2 uvCode = uvNorm * maxCode;
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
    float maxC = max(max(sample.rgb.r, sample.rgb.g), sample.rgb.b);
    float minC = min(min(sample.rgb.r, sample.rgb.g), sample.rgb.b);
    sample.chromaSignal = maxC - minC;
    sample.blueSignal = max(sample.rgb.b - max(sample.rgb.r, sample.rgb.g), 0.0f);
#endif

    float3 rgb = sample.rgb;
#ifdef NV12_INPUT
    if (yuvTransfer != kTransferSdr) {
        rgb = ApplyHdrToSdr(rgb);
    } else
#endif
    {
        // Slight contrast boost for SDR sources.
        rgb = (rgb - 0.5f) * 1.05f + 0.5f;
        rgb = saturate(rgb);
    }

    sample.rgb = rgb;
    return sample;
}

struct DotInfo {
    int idx;
    float luma;
    float edge;
    float3 color;
    float sourceBlueConfidence;
};

float EdgeMaskFitScore(uint mask, DotInfo dots[8], int bitMap[8]) {
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
        uint bit = (uint)bitMap[dots[i].idx];
        if ((mask & (1u << bit)) != 0u) {
            sumOn += dots[i].color;
            onCount += 1.0f;
        } else {
            sumOff += dots[i].color;
            offCount += 1.0f;
        }
    }

    float3 bg = sumOff / offCount;
    float3 meanOn = sumOn / onCount;
    float scale = min((float)kInkCoverageMaxScale / 256.0f,
                      1.0f / max(kInkVisibleDotCoverage, 1e-5f));
    float3 fg = saturate(bg + (meanOn - bg) * scale);
    float3 predOn = lerp(bg, fg, kInkVisibleDotCoverage);

    float score = 0.0f;
    [unroll]
    for (int j = 0; j < 8; ++j) {
        uint bit = (uint)bitMap[dots[j].idx];
        float3 pred = ((mask & (1u << bit)) != 0u) ? predOn : bg;
        float3 d = dots[j].color - pred;
        score += dot(d, d);
    }
    if (offCount < onCount) {
        float3 surfaceDelta = meanOn - bg;
        score += dot(surfaceDelta, surfaceDelta) *
                 (onCount - offCount) * kEdgeMaskBgSurfaceWeight;
    }
    return score;
}

uint FitEdgeMask(uint initialMask, DotInfo dots[8], int bitMap[8]) {
    initialMask &= 0xffu;
    uint bestMask = initialMask;
    float baseScore = EdgeMaskFitScore(initialMask, dots, bitMap);
    float bestScore = baseScore;

    [loop]
    for (uint candidate = 1u; candidate < 255u; ++candidate) {
        float score = EdgeMaskFitScore(candidate, dots, bitMap);
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
    
    float cellW = (float)width / (float)outWidth;
    float cellH = (float)height / (float)outHeight;
    float dotW = cellW / 2.0f;
    float dotH = cellH / 4.0f;

    DotInfo dots[8];
    float cellLumMin = 255.0f;
    float cellLumMax = 0.0f;
    float cellLumSum = 0.0f;
    float cellEdgeMax = 0.0f;

    float3 sumAll = float3(0,0,0);
    float sumAllBlueConfidence = 0.0f;

    // 1. Gather Data
    for (int i = 0; i < 8; ++i) {
        uint ui = (uint)i;
        uint col = ui >> 2;
        uint row = ui & 3u;
        
        float u = (DTid.x * cellW + col * dotW + dotW * 0.5f) / width;
        float v = (DTid.y * cellH + row * dotH + dotH * 0.5f) / height;

        // Area sampling for stable luma/color
        float2 dotSize = float2(dotW, dotH);
        float luma = SampleLumaArea(float2(u, v), dotSize);
        
        // For color, we still use the center sample (or could implement SampleInputArea)
        // Using center sample for color is usually fine as luma drives the structure
        InputSample sample = SampleInput(float2(u, v)); 
        
        // 3x3 Sobel Edge Detection (using area samples for stability)
        // Use dot stride to match CPU behavior (detect edges between dots, not pixels)
        float texDx = dotW / (float)width;
        float texDy = dotH / (float)height;
        
        float l00 = SampleLumaArea(float2(u - texDx, v - texDy), dotSize);
        float l01 = SampleLumaArea(float2(u, v - texDy), dotSize);
        float l02 = SampleLumaArea(float2(u + texDx, v - texDy), dotSize);
        
        float l10 = SampleLumaArea(float2(u - texDx, v), dotSize);
        // l11 is center (luma)
        float l12 = SampleLumaArea(float2(u + texDx, v), dotSize);
        
        float l20 = SampleLumaArea(float2(u - texDx, v + texDy), dotSize);
        float l21 = SampleLumaArea(float2(u, v + texDy), dotSize);
        float l22 = SampleLumaArea(float2(u + texDx, v + texDy), dotSize);

        float gx = (l02 + 2.0f*l12 + l22) - (l00 + 2.0f*l10 + l20);
        float gy = (l20 + 2.0f*l21 + l22) - (l00 + 2.0f*l01 + l02);
        float edge = sqrt(gx*gx + gy*gy) / 4.0f; // Normalize to approx 0-255 range
        
        dots[i].idx = i;
        dots[i].luma = luma;
        dots[i].edge = edge;
        dots[i].color = sample.rgb;
        dots[i].sourceBlueConfidence =
            GetSourceBlueConfidence(sample.blueSignal, sample.chromaSignal);

        cellLumMin = min(cellLumMin, luma);
        cellLumMax = max(cellLumMax, luma);
        cellLumSum += luma;
        cellEdgeMax = max(cellEdgeMax, edge);
        sumAll += sample.rgb;
        sumAllBlueConfidence += dots[i].sourceBlueConfidence;
    }

    float cellBgLum = (cellLumSum - cellLumMin - cellLumMax) * (1.0f / 6.0f);

    uint bgLum = StatsBuffer[0].x;
    float bgNorm = (float)bgLum / 255.0f;
    float effectiveBgLum = ExpandYNorm(bgNorm);
    if (yuvTransfer != kTransferSdr) {
        effectiveBgLum = ApplyHdrToSdr(effectiveBgLum);
    }
    effectiveBgLum *= 255.0f;
    float cellRange = cellLumMax - cellLumMin;
    float alpha = saturate((90.0f - cellRange) / (90.0f - 40.0f));
    float refLum = lerp(effectiveBgLum, cellBgLum, alpha);
    float hysteresis = lerp(4.0f, 10.0f, alpha);

    // 2. Adaptive Thresholding (Per-Sub-Pixel Logic)
    // This section determines which Braille dots should be active based on the input image.
    // We use a "Per-Sub-Pixel" approach inspired by the reference implementation (asciiart.ts).
    // Instead of trying to match a predefined pattern ("Target Dots"), we evaluate each of the 8 sub-pixels
    // independently to see if it differs significantly from the background. This preserves fine details
    // like single-pixel stars or thin lines that might otherwise be lost in noise reduction.

    // Braille bit mapping:
    // The standard Braille pattern is 2 columns x 4 rows.
    // The bits are mapped as follows:
    // Col 0: 0=(0,0), 1=(0,1), 2=(0,2), 6=(0,3)
    // Col 1: 3=(1,0), 4=(1,1), 5=(1,2), 7=(1,3)
    int bitMap[8] = {0, 1, 2, 6, 3, 4, 5, 7};

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

        // Check against hybrid background luminance:
        // Mix global and local background based on local detail.
        float lumDiff = abs(dots[j].luma - refLum);
        
        // Decision: Is this sub-pixel a dot?
        // If the luma difference exceeds the threshold, it's considered a foreground dot.
        uint bit = (uint)bitMap[dots[j].idx];
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
    float avgLumDiff = abs(cellLumMean - effectiveBgLum);
    float edgeSig = saturate((cellEdgeMax - 4.0f) / 12.0f);
    float lumSig = saturate((avgLumDiff - 4.0f) / 24.0f);
    float signalStrength = max(edgeSig, lumSig);

    bool useDither = (cellRange <= 20.0f && cellEdgeMax <= kDitherMaxEdge);
    if (useDither) {
        float coverage = GetInkLevelFromLum(avgLumDiff);
        float coverageU = coverage * 255.0f;
        uint ditherMask = 0;
        [unroll]
        for (int k = 0; k < 8; ++k) {
            uint bit = (uint)bitMap[dots[k].idx];
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
        bitmask = FitEdgeMask(bitmask, dots, bitMap);
    }

    [unroll]
    for (int k2 = 0; k2 < 8; ++k2) {
        uint bit = (uint)bitMap[dots[k2].idx];
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
    float blendStrength = kSignalStrengthFloor +
                          (1.0f - kSignalStrengthFloor) * signalStrength;

    if (!useDither && bgCount > 0 && inkCount > 0) {
        float fgY = GetLuma(curFg);
        float bgY = GetLuma(curBg);
        bool bgIsBrightFeature =
            bgCount <= inkCount && bgCount <= kBrightBgSwapMaxDots &&
            signalStrength >= kBrightBgSwapMinSignal &&
            bgY >= fgY + kBrightBgSwapDelta;
        bool swapBeatsEdgeFit = true;
        if (bgIsBrightFeature && edgeMaskFitEligible) {
            uint swappedMask = (~bitmask) & 0xffu;
            float currentScore = EdgeMaskFitScore(bitmask, dots, bitMap);
            float swappedScore = EdgeMaskFitScore(swappedMask, dots, bitMap);
            swapBeatsEdgeFit =
                swappedScore < 3.402823e+37f &&
                (currentScore >= 3.402823e+37f ||
                 swappedScore <=
                     currentScore * (1.0f - kBrightBgSwapScoreMinGain));
        }
        if (bgIsBrightFeature && swapBeatsEdgeFit) {
            float3 swapColor = curFg;
            curFg = curBg;
            curBg = swapColor;

            float swapBlueConfidence = curFgBlueConfidence;
            curFgBlueConfidence = curBgBlueConfidence;
            curBgBlueConfidence = swapBlueConfidence;

            int swapCount = inkCount;
            inkCount = bgCount;
            bgCount = swapCount;

            bitmask = (~bitmask) & 0xffu;
            dotCount = 8u - dotCount;
        }
    }

    // Dampening (Noise Hiding):
    // If signalStrength is low, we interpolate curFg towards curBg.
    // This effectively "fades out" noise into the background.
    curFg = lerp(curBg, curFg, blendStrength);

    // Boosting (Detail Pop):
    // If signalStrength is high (> 0.8), we apply an asymmetrical contrast boost.
    if (signalStrength > 0.8f) {
        // Normalize the range [0.8, 1.0] to [0.0, 1.0]
        float boost = (signalStrength - 0.8f) * 5.0f; 
        
        // Calculate the midpoint (gray point) between FG and BG
        float3 center = (curFg + curBg) * 0.5f;
        
        // 'delta' is the vector from Center to Foreground.
        // Conversely, the vector from Center to Background is -delta.
        float3 delta = curFg - center;
        
        // Asymmetrical Boost Logic:
        // We expand the distance from the center to increase contrast.
        // Formula: NewPos = Center + DirectionVector * ExpansionFactor
        
        // 1.0f is the base scale (original position).
        // We add 'boost * 0.5f' to push the Foreground 50% further out.
        curFg = center + delta * (1.0f + boost * 1.5f);
        
        // We subtract 'delta' (add -delta) to push the Background in the opposite direction.
        // We only push it 10% further out (boost * 0.1f) to avoid crushing it to black.
        // curBg = center - delta * (1.0f + boost * 0.1f); 
        
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
    float adaptiveSat = (float)kColorSaturation;
    if (curY < 60.0f) {
        adaptiveSat = adaptiveSat * (curY / 60.0f);
    }
    adaptiveSat *=
        1.0f + kSourceBlueInkBoost * saturate(curFgBlueConfidence) *
        saturate((60.0f - curY) / 60.0f);
    curFg = ApplySaturationAroundLuma(curFg, curY, adaptiveSat / 256.0f);

    if (!useDither && signalStrength >= kInkCoverageMinSignal &&
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

    // Temporal Stability (Ghosting Reduction)
    // We blend the current frame's color with the previous frame's color to reduce flickering.
    // However, too much blending causes "ghosting" (trails behind moving objects).
    float3 prevFg = UnpackColor(history.x);
    float3 prevBg = UnpackColor(history.y);

    // Luma Correction (Bg)
    // Ensure background isn't too dark.
    float bgY = GetLuma(curBg);
    if (bgY < (float)kBgMinLuma) {
        if (bgY <= 0.0f) {
            curBg = (float)kBgMinLuma / 255.0f;
        } else {
            float scale = ((float)kBgMinLuma * 256.0f) / bgY;
            curBg = min(1.0f, (curBg * scale) / 256.0f);
        }
    }
    
    // Calculate perceptual distance to decide if we should reset (cut) or blend.
    float diffFg = dot(abs(curFg - prevFg), kLumaCoeff);
    float diffBg = dot(abs(curBg - prevBg), kLumaCoeff);
    
    float resetThresh = (float)kTemporalResetDelta / 255.0f;

    // Alpha Blending:
    // We use a high alpha (230/255 ~= 0.9) to favor the new frame.
    // This significantly reduces ghosting compared to the old value (0.7).
    // If the change is large (> resetThresh), we snap instantly (alpha=1.0 implicitly).
    if (diffFg < resetThresh) {
        curFg = lerp(prevFg, curFg, 230.0f/255.0f);
    }
    float bgBlendAlpha = 230.0f/255.0f;
    if (diffBg < resetThresh) {
        curBg = lerp(prevBg, curBg, bgBlendAlpha);
    }
    curFg = CompressShadowChroma(curFg, curFgBlueConfidence);
    curBg = CompressShadowChroma(curBg, curBgBlueConfidence);

    bool fullMask = (dotCount == 8u);
    if (fullMask && bgCount == 0) {
        float dir = (cellLumMean < effectiveBgLum) ? 1.0f : -1.0f;
        float minDeltaY = lerp(8.0f, 4.0f, signalStrength);
        float fgY = GetLuma(curFg);
        float bgY = GetLuma(curBg);
        float need = minDeltaY - dir * (bgY - fgY);
        if (need > 0.0f) {
            float shift = dir * need / 255.0f;
            curBg = saturate(curBg + shift);
        }
    }

    // Write back history for the next frame
    uint fg24 = PackColor(curFg) & 0x00FFFFFF;
    HistoryBuffer[cellIndex] = uint2(fg24 | (bitmask << 24), PackColor(curBg));

    AsciiCell cell;
    cell.ch = kBrailleBase + bitmask;
    cell.fg = PackColor(curFg);
    cell.bg = PackColor(curBg);
    
    cell.hasBg = 1u;

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
