#include "asciiart_gpu.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <iostream>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {
    // Embedded HLSL shader source
    const char* kComputeShaderSource = R"(
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
    uint padding[2];
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
SamplerState PointSampler : register(s1);

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
static const float kHdrScale = 10000.0f / 100.0f;
static const uint kMatrixBt709 = 0;
static const uint kMatrixBt601 = 1;
static const uint kMatrixBt2020 = 2;
static const uint kTransferSdr = 0;
static const uint kTransferPq = 1;
static const uint kTransferHlg = 2;

// Constants from CPU version
static const int kMinContrastForBraille = 8; // Reduced from 15 to allow finer details
static const int kEdgeShift = 3;
static const int kColorLift = 0;
static const int kColorSaturation = 340;
static const int kTemporalResetDelta = 48;
static const int kColorAlpha = 180; // Reduced from 220 for more stability
static const int kBgAlpha = 140;    // Reduced from 180 for more stability
static const int kInkMinLuma = 40;  // Reduced from 110 to allow dark details
static const int kBgMinLuma = 10;   // Reduced from 20
static const int kInkMaxScale = 1280;
static const float kSignalStrengthFloor = 0.2f;
static const int kEdgeMin = 4; // Reverted to 4 for sensitivity, noise handled by logic
static const int kEdgeBoost = 245;
static const int kDitherBias = 48;
static const int kAAScoreBandMin = 12;
static const int kAAScoreBandMax = 144;

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

float SampleLumaRaw(float2 uv, SamplerState s) {
#ifdef NV12_INPUT
    float yNorm = TextureY.SampleLevel(s, uv, 0);
    return ExpandYNorm(yNorm) * 255.0f;
#else
    return GetLuma(InputTexture.SampleLevel(s, uv, 0).rgb);
#endif
}

float SampleLuma(float2 uv) { return SampleLumaRaw(uv, LinearSampler); }
float SampleLumaPoint(float2 uv) { return SampleLumaRaw(uv, PointSampler); }

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
    float x = norm; // kInkUseBright = true
    // Gamma 0.65: Slightly steeper to delay onset of dots
    float coverage = pow(x, 0.65f); 
    if (coverage > 0.001f) {
        // Reduced gain (1.30) to prevent early saturation to 6-dots
        coverage = coverage * 1.30f; 
    }
    // Cutoff 0.03: Filter very faint noise
    if (coverage < 0.03f) coverage = 0.0f; 
    return saturate(coverage);
}

float GetEdgeBoostFromMag(float edge) {
    float range = 255.0f - (float)kEdgeMin;
    if (edge <= (float)kEdgeMin || range <= 0.0f) return 0.0f;
    float boost = (edge - (float)kEdgeMin) * (float)kEdgeBoost / range;
    return min(boost, (float)kEdgeBoost);
}

float4 SampleInput(float2 uv) {
    float4 color;
#ifdef NV12_INPUT
    float y = ExpandYNorm(TextureY.SampleLevel(LinearSampler, uv, 0));
    float2 uv_val = ExpandUV(TextureUV.SampleLevel(LinearSampler, uv, 0));

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
    color = float4(r, g, b, 1.0);
#else
    color = InputTexture.SampleLevel(LinearSampler, uv, 0);
#endif

    float3 rgb = color.rgb;
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

    return float4(rgb, color.a);
}
)"
R"(
struct DotInfo {
    int idx;
    int score;
    float luma;
    float edge;
    float contrast;
    float3 color;
};

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
    float cellContrastMax = 0.0f;

    float3 sumAll = float3(0,0,0);

    // 1. Gather Data
    for (int i = 0; i < 8; ++i) {
        int col = i / 4;
        int row = i % 4;
        
        float u = (DTid.x * cellW + col * dotW + dotW * 0.5f) / width;
        float v = (DTid.y * cellH + row * dotH + dotH * 0.5f) / height;

        // Area sampling for stable luma/color
        float2 dotSize = float2(dotW, dotH);
        float luma = SampleLumaArea(float2(u, v), dotSize);
        
        // For color, we still use the center sample (or could implement SampleInputArea)
        // Using center sample for color is usually fine as luma drives the structure
        float4 color = SampleInput(float2(u, v)); 
        
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
        
        // Local Contrast
        float avgNeighbor = (l00 + l01 + l02 + l10 + l12 + l20 + l21 + l22) * 0.125f;
        float contrast = abs(luma - avgNeighbor);

        dots[i].idx = i;
        dots[i].luma = luma;
        dots[i].edge = edge;
        dots[i].contrast = contrast;
        dots[i].color = color.rgb;

        cellLumMin = min(cellLumMin, luma);
        cellLumMax = max(cellLumMax, luma);
        cellLumSum += luma;
        cellEdgeMax = max(cellEdgeMax, edge);
        cellContrastMax = max(cellContrastMax, contrast);
        sumAll += color.rgb;
    }

    float cellBgLum = (cellLumSum - cellLumMin - cellLumMax) * (1.0f / 6.0f);

    // Adjust bgLum and lumRange for Limited Range if needed
    uint bgLum = StatsBuffer[0].x;
    uint lumRange = StatsBuffer[0].y;

    float effectiveLumRange = max((float)lumRange, 80.0f); // Clamp to prevent noise amplification
    if (isFullRange == 0) {
        uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
        float yRange = (float)(219u << shift);
        float scale = GetMaxCode() / max(yRange, 1.0f);
        effectiveLumRange = effectiveLumRange * scale;
    }
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
    int inkCount = 0;
    int bgCount = 0;

    // Base threshold (DELTA in ts)
    // This is the minimum luma difference required for a pixel to be considered "foreground".
    // A value of 30.0f (out of 255) provides a good balance between sensitivity and noise rejection.
    float baseThreshold = 30.0f; 

    for (int j = 0; j < 8; ++j) {
        // Edge-aware threshold modulation:
        // In areas with strong edges (high detail), we lower the threshold to capture more subtle features.
        // In flat areas (low edge), we keep the threshold high to suppress noise.
        // Formula: threshold = max(10, base - 0.2 * edge)
        // If edge is strong (e.g., 100), threshold drops to 10, making it very sensitive.
        float threshold = max(10.0f, baseThreshold - dots[j].edge * 0.2f);

        // Check against hybrid background luminance:
        // Mix global and local background based on local detail.
        float lumDiff = abs(dots[j].luma - refLum);
        
        // Decision: Is this sub-pixel a dot?
        // If the luma difference exceeds the threshold, it's considered a foreground dot.
        // Note: We use luma difference as a proxy for "color distance" for performance.
        uint bit = (uint)bitMap[dots[j].idx];
        bool wasOn = ((prevMask >> bit) & 1) != 0;
        float onThreshold = threshold;
        float offThreshold = max(6.0f, threshold - hysteresis);
        bool isDot = wasOn ? (lumDiff >= offThreshold) : (lumDiff >= onThreshold);

        // Accumulate colors for Foreground (Ink) and Background calculation later.
        if (isDot) {
            bitmask |= (1 << bit);
            sumInk += dots[j].color;
            inkCount++;
        } else {
            sumBg += dots[j].color;
            bgCount++;
        }
    }

    // Calculate stats for contrast logic
    float cellLumMean = cellLumSum / 8.0f;
    float avgLumDiff = abs(cellLumMean - effectiveBgLum);

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
    float edgeSig = saturate((cellEdgeMax - 4.0f) / 12.0f); 
    float lumSig = saturate((avgLumDiff - 4.0f) / 24.0f);   
    float signalStrength = max(edgeSig, lumSig);
    float blendStrength = kSignalStrengthFloor +
                          (1.0f - kSignalStrengthFloor) * signalStrength;

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
    curFg = saturate(curY/255.0f + (curFg - curY/255.0f) * (adaptiveSat / 256.0f));

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

    // Temporal Stability (Ghosting Reduction)
    // We blend the current frame's color with the previous frame's color to reduce flickering.
    // However, too much blending causes "ghosting" (trails behind moving objects).
    float3 prevFg = UnpackColor(history.x);
    float3 prevBg = UnpackColor(history.y);
    
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
    if (diffBg < resetThresh) {
        curBg = lerp(prevBg, curBg, 230.0f/255.0f);
    }

    // Write back history for the next frame
    uint fg24 = PackColor(curFg) & 0x00FFFFFF;
    HistoryBuffer[cellIndex] = uint2(fg24 | (bitmask << 24), PackColor(curBg));

    AsciiCell cell;
    cell.ch = kBrailleBase + bitmask;
    cell.fg = PackColor(curFg);
    cell.bg = PackColor(curBg);
    
    // Fix for black backgrounds on full cells:
    // If a cell has no background dots (all FG or empty), but the calculated BG color is bright enough,
    // we force the background flag to true so the renderer draws the background color.
    bool hasBg = (bgCount > 0);
    if (!hasBg && GetLuma(curBg) > 10.0f) {
        hasBg = true;
    }
    cell.hasBg = hasBg;

    OutputBuffer[cellIndex] = cell;
}
)";

    struct GpuAsciiCell {
        uint32_t ch;
        uint32_t fg;
        uint32_t bg;
        uint32_t hasBg;
    };

    // Stats Compute Shader
    const char* kStatsShaderSource = R"(
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
    uint padding[2];
};

Texture2D<float> TextureY : register(t0);
RWStructuredBuffer<uint4> Stats : register(u0); // x=bgLum, y=lumRange

groupshared uint histogram[256];

[numthreads(256, 1, 1)]
void CSStats(uint3 tid : SV_DispatchThreadID) {
    // Initialize histogram
    histogram[tid.x] = 0;
    GroupMemoryBarrierWithGroupSync();

    // Strided sampling (similar to CPU 10x10 but parallel)
    // We want to cover the whole image with 256 threads.
    // Each thread handles a vertical strip or scattered pixels.
    
    uint stride = 10;
    for (uint y = 0; y < height; y += stride) {
        for (uint x = tid.x * stride; x < width; x += 256 * stride) {
             float yVal = TextureY.Load(int3(x, y, 0)); // Load raw value (0.0-1.0)
             uint bin = (uint)(yVal * 255.0f + 0.5f);
             bin = min(bin, 255);
             InterlockedAdd(histogram[bin], 1);
        }
    }
    
    GroupMemoryBarrierWithGroupSync();
    
    if (tid.x == 0) {
        // Find Mode (bgLum)
        uint maxCount = 0;
        uint mode = 0;
        uint totalSamples = 0;
        
        for (uint i = 0; i < 256; ++i) {
            uint count = histogram[i];
            totalSamples += count;
            if (count > maxCount) {
                maxCount = count;
                mode = i;
            }
        }
        
        // Find Range (1% - 99%)
        uint targetLow = totalSamples / 100;
        uint targetHigh = totalSamples * 99 / 100;
        uint accum = 0;
        uint low = 0;
        uint high = 255;
        bool lowFound = false;
        
        for (uint j = 0; j < 256; ++j) {
            accum += histogram[j];
            if (!lowFound && accum >= targetLow) {
                low = j;
                lowFound = true;
            }
            if (accum >= targetHigh) {
                high = j;
                break;
            }
        }
        
        uint range = max(1, high - low);
        if (range < 80) range = 80;
        
        Stats[0] = uint4(mode, range, 0, 0);
    }
}
)";
}

GpuAsciiRenderer::GpuAsciiRenderer() {}

GpuAsciiRenderer::~GpuAsciiRenderer() {}

bool GpuAsciiRenderer::Initialize(int maxWidth, int maxHeight, std::string* error) {
    if (!CreateDevice()) {
        if (error) *error = "Failed to create D3D11 device";
        return false;
    }
    if (!CompileComputeShader(error)) return false;

    // Create Samplers
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    
    if (FAILED(m_device->CreateSamplerState(&sampDesc, &m_linearSampler))) {
        if (error) *error = "Failed to create linear sampler";
        return false;
    }

    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(m_device->CreateSamplerState(&sampDesc, &m_pointSampler))) {
        if (error) *error = "Failed to create point sampler";
        return false;
    }

    // Buffers will be created on first render or here if we knew exact size
    return true;
}

bool GpuAsciiRenderer::CreateDevice() {
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT creationFlags = 0;
#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags,
        featureLevels, 1, D3D11_SDK_VERSION,
        &m_device, nullptr, &m_context
    );
    return SUCCEEDED(hr);
}

bool GpuAsciiRenderer::CompileComputeShader(std::string* error) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    // Compile RGBA Shader
    HRESULT hr = D3DCompile(
        kComputeShaderSource, strlen(kComputeShaderSource),
        "EmbeddedShader", nullptr, nullptr,
        "CSMain", "cs_5_0",
        0, 0, &blob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::string msg = (char*)errorBlob->GetBufferPointer();
            std::cerr << "Shader Compile Error (RGBA): " << msg << std::endl;
            if (error) *error = "Shader Compile Error (RGBA): " + msg;
        } else {
            if (error) *error = "Shader Compile Error (RGBA): Unknown error";
        }
        return false;
    }

    hr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_computeShader);
    if (FAILED(hr)) {
        if (error) *error = "Failed to create compute shader (RGBA)";
        return false;
    }

    // Compile NV12 Shader
    D3D_SHADER_MACRO defines[] = { "NV12_INPUT", "1", nullptr, nullptr };
    blob.Reset();
    errorBlob.Reset();

    hr = D3DCompile(
        kComputeShaderSource, strlen(kComputeShaderSource),
        "EmbeddedShader", defines, nullptr,
        "CSMain", "cs_5_0",
        0, 0, &blob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::string msg = (char*)errorBlob->GetBufferPointer();
            std::cerr << "Shader Compile Error (NV12): " << msg << std::endl;
            if (error) *error = "Shader Compile Error (NV12): " + msg;
        } else {
            if (error) *error = "Shader Compile Error (NV12): Unknown error";
        }
        return false;
    }

    hr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_computeShaderNV12);
    if (FAILED(hr)) {
        if (error) *error = "Failed to create compute shader (NV12)";
        return false;
    }

    // Compile Stats Shader
    blob.Reset();
    errorBlob.Reset();
    hr = D3DCompile(
        kStatsShaderSource, strlen(kStatsShaderSource),
        "StatsShader", nullptr, nullptr,
        "CSStats", "cs_5_0",
        0, 0, &blob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::string msg = (char*)errorBlob->GetBufferPointer();
            std::cerr << "Shader Compile Error (Stats): " << msg << std::endl;
            if (error) *error = "Shader Compile Error (Stats): " + msg;
        } else {
            if (error) *error = "Shader Compile Error (Stats): Unknown error";
        }
        return false;
    }

    hr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_statsShader);
    if (FAILED(hr)) {
        if (error) *error = "Failed to create compute shader (Stats)";
        return false;
    }

    return SUCCEEDED(hr);
}

bool GpuAsciiRenderer::CreateStatsBuffer() {
    if (m_statsBuffer) return true;

    D3D11_BUFFER_DESC bufDesc = {};
    bufDesc.ByteWidth = sizeof(uint32_t) * 4; // uint4
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufDesc.StructureByteStride = sizeof(uint32_t) * 4;

    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_statsBuffer))) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = 1;
    if (FAILED(m_device->CreateUnorderedAccessView(m_statsBuffer.Get(), &uavDesc, &m_statsUAV))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = 1;
    if (FAILED(m_device->CreateShaderResourceView(m_statsBuffer.Get(), &srvDesc, &m_statsSRV))) return false;

    return true;
}

bool GpuAsciiRenderer::CreateRGBATextures(int width, int height) {
    if (m_inputTexture) {
        D3D11_TEXTURE2D_DESC desc;
        m_inputTexture->GetDesc(&desc);
        if (desc.Width == width && desc.Height == height) {
            return true;
        }
        m_inputTexture.Reset();
        m_inputSRV.Reset();
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 0; // Full chain
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, &m_inputTexture))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_inputTexture.Get(), nullptr, &m_inputSRV))) return false;

    return true;
}

bool GpuAsciiRenderer::CreateNV12Textures(int width, int height, bool is10Bit) {
    // Check if we need to recreate textures (size or format change)
    DXGI_FORMAT targetY = is10Bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    DXGI_FORMAT targetUV = is10Bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;

    if (m_textureY) {
        D3D11_TEXTURE2D_DESC desc;
        m_textureY->GetDesc(&desc);
        if (desc.Width == width && desc.Height == height && desc.Format == targetY) {
            return true;
        }
        // Release old textures
        m_textureY.Reset();
        m_srvY.Reset();
        m_textureUV.Reset();
        m_srvUV.Reset();
    }

    // Y Texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = targetY;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    
    if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, &m_textureY))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_textureY.Get(), nullptr, &m_srvY))) return false;

    // UV Texture
    texDesc.Width = width / 2;
    texDesc.Height = height / 2;
    texDesc.Format = targetUV;
    
    if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, &m_textureUV))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_textureUV.Get(), nullptr, &m_srvUV))) return false;

    return true;
}

bool GpuAsciiRenderer::RenderNV12(const uint8_t* yuv, int width, int height, int stride, int planeHeight, 
                                bool fullRange, YuvMatrix yuvMatrix, YuvTransfer yuvTransfer,
                                bool is10Bit, AsciiArt& out, std::string* error) {
    if (!m_device) {
        std::string initErr;
        if (!Initialize(width, height, &initErr)) {
            if (error) *error = "Failed to initialize GPU renderer: " + initErr;
            return false;
        }
    }

    int outW = out.width;
    int outH = out.height;
    if (outW <= 0 || outH <= 0) return false;

    if (!CreateBuffers(width, height, outW, outH)) {
        if (error) *error = "Failed to create GPU buffers";
        return false;
    }
    
    if (!CreateNV12Textures(width, height, is10Bit)) {
        if (error) *error = "Failed to create NV12 textures";
        return false;
    }

    if (!CreateStatsBuffer()) {
        if (error) *error = "Failed to create stats buffer";
        return false;
    }

    // Upload Y
    m_context->UpdateSubresource(m_textureY.Get(), 0, nullptr, yuv, stride, 0);
    
    // Upload UV
    // UV plane starts after Y plane (stride * planeHeight)
    const uint8_t* uvData = yuv + (stride * planeHeight);
    m_context->UpdateSubresource(m_textureUV.Get(), 0, nullptr, uvData, stride, 0);

    // Update Constants
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        Constants* cb = (Constants*)mapped.pData;
        cb->width = width;
        cb->height = height;
        cb->outWidth = outW;
        cb->outHeight = outH;
        cb->time = 0.0f;
        cb->frameCount++;
        cb->isFullRange = fullRange ? 1 : 0;
        cb->bitDepth = is10Bit ? 10u : 8u;
        cb->yuvMatrix = static_cast<uint32_t>(yuvMatrix);
        cb->yuvTransfer = static_cast<uint32_t>(yuvTransfer);
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    // 1. Calculate Stats
    m_context->CSSetShader(m_statsShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* statsSRVs[] = { m_srvY.Get() }; // Only Y needed
    m_context->CSSetShaderResources(0, 1, statsSRVs);
    ID3D11UnorderedAccessView* statsUAVs[] = { m_statsUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 1, statsUAVs, nullptr);
    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    m_context->CSSetConstantBuffers(0, 1, cbs);

    m_context->Dispatch(1, 1, 1); // Single group of 256 threads

    // Unbind Stats UAV
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

    // 2. Main Pass
    m_context->CSSetShader(m_computeShaderNV12.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { m_srvY.Get(), m_srvUV.Get(), m_statsSRV.Get() };
    m_context->CSSetShaderResources(0, 3, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get(), m_historyUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);

    ID3D11SamplerState* samplers[] = { m_linearSampler.Get(), m_pointSampler.Get() };
    m_context->CSSetSamplers(0, 2, samplers);

    UINT dispatchX = (outW + 7) / 8;
    UINT dispatchY = (outH + 7) / 8;
    m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind
    ID3D11UnorderedAccessView* nullUAV2[] = { nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 2, nullUAV2, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr, nullptr, nullptr };
    m_context->CSSetShaderResources(0, 3, nullSRV);

    // Readback (Same as Render)
    m_context->CopyResource(m_outputStagingBuffer.Get(), m_outputBuffer.Get());
    
    if (SUCCEEDED(m_context->Map(m_outputStagingBuffer.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        GpuAsciiCell* gpuCells = (GpuAsciiCell*)mapped.pData;
        out.cells.resize(outW * outH);
        
        for (int i = 0; i < outW * outH; ++i) {
            out.cells[i].ch = (wchar_t)gpuCells[i].ch;
            
            uint32_t fg = gpuCells[i].fg;
            out.cells[i].fg.r = (fg) & 0xFF;
            out.cells[i].fg.g = (fg >> 8) & 0xFF;
            out.cells[i].fg.b = (fg >> 16) & 0xFF;

            uint32_t bg = gpuCells[i].bg;
            out.cells[i].bg.r = (bg) & 0xFF;
            out.cells[i].bg.g = (bg >> 8) & 0xFF;
            out.cells[i].bg.b = (bg >> 16) & 0xFF;

            out.cells[i].hasBg = gpuCells[i].hasBg != 0;
        }
        m_context->Unmap(m_outputStagingBuffer.Get(), 0);
    }

    return true;
}

bool GpuAsciiRenderer::CreateBuffers(int width, int height, int outW, int outH) {
    if (m_currentWidth == width && m_currentHeight == height && 
        m_currentOutW == outW && m_currentOutH == outH) {
        return true;
    }

    // Output Buffer
    D3D11_BUFFER_DESC bufDesc = {};
    bufDesc.ByteWidth = sizeof(GpuAsciiCell) * outW * outH;
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufDesc.StructureByteStride = sizeof(GpuAsciiCell);

    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_outputBuffer))) return false;
    
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = outW * outH;
    
    if (FAILED(m_device->CreateUnorderedAccessView(m_outputBuffer.Get(), &uavDesc, &m_outputUAV))) return false;

    // History Buffer
    bufDesc.ByteWidth = sizeof(uint32_t) * 2 * outW * outH; // uint2
    bufDesc.StructureByteStride = sizeof(uint32_t) * 2;
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_historyBuffer))) return false;
    if (FAILED(m_device->CreateUnorderedAccessView(m_historyBuffer.Get(), &uavDesc, &m_historyUAV))) return false;

    // Clear history initially
    UINT clearVals[4] = {0,0,0,0};
    m_context->ClearUnorderedAccessViewUint(m_historyUAV.Get(), clearVals);

    // Staging Buffer for Readback
    bufDesc.Usage = D3D11_USAGE_STAGING;
    bufDesc.BindFlags = 0;
    bufDesc.MiscFlags = 0;
    bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bufDesc.StructureByteStride = 0; // Not needed for staging
    bufDesc.ByteWidth = sizeof(GpuAsciiCell) * outW * outH;
    
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_outputStagingBuffer))) return false;

    // Constant Buffer
    bufDesc.ByteWidth = sizeof(Constants);
    bufDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bufDesc.MiscFlags = 0;
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_constantBuffer))) return false;

    m_currentWidth = width;
    m_currentHeight = height;
    m_currentOutW = outW;
    m_currentOutH = outH;
    return true;
}

bool GpuAsciiRenderer::Render(const uint8_t* rgba, int width, int height, AsciiArt& out, std::string* error) {
    if (!m_device) {
        std::string initErr;
        if (!Initialize(width, height, &initErr)) {
            if (error) *error = "Failed to initialize GPU renderer: " + initErr;
            return false;
        }
    }

    // Calculate output size (same logic as CPU)
    int outW = out.width;
    int outH = out.height;
    if (outW <= 0 || outH <= 0) return false;

    if (!CreateBuffers(width, height, outW, outH)) {
        if (error) *error = "Failed to create GPU buffers";
        return false;
    }

    if (!CreateRGBATextures(width, height)) {
        if (error) *error = "Failed to create RGBA textures";
        return false;
    }

    // Upload Texture
    m_context->UpdateSubresource(m_inputTexture.Get(), 0, nullptr, rgba, width * 4, 0);
    
    // Generate Mips
    m_context->GenerateMips(m_inputSRV.Get());

    // Calculate stats (bgLum, lumRange) from RGBA
    // Sample a subset of pixels for performance
    int sampleStep = 16; // Sample every 16th pixel
    uint32_t histogram[256] = {0};
    int totalSamples = 0;
    
    for (int y = 0; y < height; y += sampleStep) {
        const uint8_t* row = rgba + y * width * 4;
        for (int x = 0; x < width; x += sampleStep) {
            const uint8_t* p = row + x * 4;
            // Calculate luma: 0.2126 R + 0.7152 G + 0.0722 B
            int luma = (p[0] * 54 + p[1] * 183 + p[2] * 18) >> 8; // Approx integer math
            if (luma > 255) luma = 255;
            histogram[luma]++;
            totalSamples++;
        }
    }
    
    // Find range (5% - 95%)
    int count = 0;
    int low = 0;
    int targetLow = totalSamples * 5 / 100;
    for (int i = 0; i < 256; ++i) {
        count += histogram[i];
        if (count >= targetLow) {
            low = i;
            break;
        }
    }
    
    count = 0;
    int high = 255;
    int targetHigh = totalSamples * 95 / 100;
    for (int i = 0; i < 256; ++i) {
        count += histogram[i];
        if (count >= targetHigh) {
            high = i;
            break;
        }
    }
    
    int lumRange = (std::max)(1, high - low);
    if (lumRange < 80) lumRange = 80; // Clamp like in main.cpp
    
    // Find bgLum (mode)
    int bgLum = 0;
    uint32_t maxCount = 0;
    for (int i = 0; i < 256; ++i) {
        if (histogram[i] > maxCount) {
            maxCount = histogram[i];
            bgLum = i;
        }
    }

    // Upload Stats to StatsBuffer
    if (!CreateStatsBuffer()) {
        if (error) *error = "Failed to create stats buffer";
        return false;
    }
    
    uint32_t statsData[4] = { (uint32_t)bgLum, (uint32_t)lumRange, 0, 0 };
    m_context->UpdateSubresource(m_statsBuffer.Get(), 0, nullptr, statsData, 0, 0);

    // Update Constants
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        Constants* cb = (Constants*)mapped.pData;
        cb->width = width;
        cb->height = height;
        cb->outWidth = outW;
        cb->outHeight = outH;
        cb->time = 0.0f;
        cb->frameCount++;
        cb->isFullRange = 1; 
        cb->bitDepth = 8;
        cb->yuvMatrix = static_cast<uint32_t>(YuvMatrix::Bt709);
        cb->yuvTransfer = static_cast<uint32_t>(YuvTransfer::Sdr);
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    // Dispatch
    m_context->CSSetShader(m_computeShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { m_inputSRV.Get(), m_statsSRV.Get() };
    m_context->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get(), m_historyUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    m_context->CSSetConstantBuffers(0, 1, cbs);

    ID3D11SamplerState* samplers[] = { m_linearSampler.Get(), m_pointSampler.Get() };
    m_context->CSSetSamplers(0, 2, samplers);

    UINT dispatchX = (outW + 7) / 8;
    UINT dispatchY = (outH + 7) / 8;
    m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind UAVs
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 2, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr, nullptr };
    m_context->CSSetShaderResources(0, 2, nullSRV);

    // Readback
    m_context->CopyResource(m_outputStagingBuffer.Get(), m_outputBuffer.Get());
    
    if (SUCCEEDED(m_context->Map(m_outputStagingBuffer.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        GpuAsciiCell* gpuCells = (GpuAsciiCell*)mapped.pData;
        out.cells.resize(outW * outH);
        
        for (int i = 0; i < outW * outH; ++i) {
            out.cells[i].ch = (wchar_t)gpuCells[i].ch;
            
            uint32_t fg = gpuCells[i].fg;
            out.cells[i].fg.r = (fg) & 0xFF;
            out.cells[i].fg.g = (fg >> 8) & 0xFF;
            out.cells[i].fg.b = (fg >> 16) & 0xFF;

            uint32_t bg = gpuCells[i].bg;
            out.cells[i].bg.r = (bg) & 0xFF;
            out.cells[i].bg.g = (bg >> 8) & 0xFF;
            out.cells[i].bg.b = (bg >> 16) & 0xFF;

            out.cells[i].hasBg = gpuCells[i].hasBg != 0;
        }
        m_context->Unmap(m_outputStagingBuffer.Get(), 0);
    }

    return true;
}
