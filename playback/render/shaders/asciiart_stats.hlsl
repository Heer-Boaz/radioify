
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

#if defined(NV12_INPUT)
Texture2D<float> TextureY : register(t0);
#else
Texture2D<float4> InputTexture : register(t0);
#endif
RWStructuredBuffer<uint> LumaRange : register(u0);

groupshared uint histogram[256];

static const float3 kLumaCoeff = float3(0.2126f, 0.7152f, 0.0722f);
static const uint kTransferSdr = 0u;
static const uint kTransferPq = 1u;
static const uint kTransferHlg = 2u;

#define RADIOIFY_ASCII_BOOL(name, value) static const bool name = value;
#define RADIOIFY_ASCII_FLOAT(name, value) static const float name = value;
#define RADIOIFY_ASCII_COUNT(name, value) static const int name = value;
#define RADIOIFY_ASCII_LUMA_U8(name, value) static const uint name = (uint)(value);
#define RADIOIFY_ASCII_SIGNAL_U8(name, value) static const uint name = (uint)(value);
#define RADIOIFY_ASCII_SCALE_256(name, value) static const uint name = (uint)(value);
#include "../../video/ascii/asciiart_constants.inc"
#undef RADIOIFY_ASCII_BOOL
#undef RADIOIFY_ASCII_FLOAT
#undef RADIOIFY_ASCII_COUNT
#undef RADIOIFY_ASCII_LUMA_U8
#undef RADIOIFY_ASCII_SIGNAL_U8
#undef RADIOIFY_ASCII_SCALE_256

#include "video_luminance_stats.hlsli"
#include "video_hdr_to_sdr.hlsli"

float CodeFromNorm(float norm) {
    norm = saturate(norm);
    if (bitDepth > 8u) {
        uint shift = 16u - bitDepth;
        return norm * 65535.0f / (float)(1u << shift);
    }
    return norm * 255.0f;
}

float ExpandYNorm(float yNorm) {
    float maxCode = (float)((1u << bitDepth) - 1u);
    float yCode = min(CodeFromNorm(yNorm), maxCode);
    uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
    float yMin = (isFullRange != 0u) ? 0.0f : (float)(16u << shift);
    float yMax = (isFullRange != 0u) ? maxCode : (float)(235u << shift);
    return saturate((yCode - yMin) / max(yMax - yMin, 1.0f));
}

float ExpandYCode(float yCode) {
    float maxCode = (float)((1u << bitDepth) - 1u);
    yCode = min(max(yCode, 0.0f), maxCode);
    uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
    float yMin = (isFullRange != 0u) ? 0.0f : (float)(16u << shift);
    float yMax = (isFullRange != 0u) ? maxCode : (float)(235u << shift);
    return saturate((yCode - yMin) / max(yMax - yMin, 1.0f));
}

float ApplyHdrToSdr(float v) {
    return RadioifyHdrTransferToSdr(v, yuvTransfer, kHdrScale, kTransferPq,
                                    kTransferHlg);
}

static const uint kYCoeffR = 13933u;
static const uint kYCoeffG = 46871u;
static const uint kYCoeffB = 4732u;

uint RgbToYByte(uint r, uint g, uint b) {
    uint y = (r * kYCoeffR + g * kYCoeffG + b * kYCoeffB) >> 16;
    return min(y, 255u);
}

uint CodeFromNormRounded(float norm) {
    uint maxCode = (1u << bitDepth) - 1u;
    return min((uint)(CodeFromNorm(norm) + 0.5f), maxCode);
}

#if defined(NV12_INPUT)
uint NormalizeYCodeToByte(uint yCode) {
    float y = ExpandYCode((float)yCode);
    if (yuvTransfer != kTransferSdr) {
        y = ApplyHdrToSdr(y);
    }
    return (uint)(saturate(y) * 255.0f + 0.5f);
}
#else
uint LoadRgbaLumaByte(uint x, uint y) {
    float3 rgb = InputTexture.Load(int3(x, y, 0)).rgb;
    rgb = saturate(rgb);
    uint r = (uint)(rgb.r * 255.0f + 0.5f);
    uint g = (uint)(rgb.g * 255.0f + 0.5f);
    uint b = (uint)(rgb.b * 255.0f + 0.5f);
    return RgbToYByte(r, g, b);
}
#endif

[numthreads(256, 1, 1)]
void CSStats(uint3 tid : SV_DispatchThreadID) {
    // Initialize histogram
    histogram[tid.x] = 0;
    GroupMemoryBarrierWithGroupSync();

    uint scaledW = outWidth * 2u;
    uint scaledH = outHeight * 4u;
    if (scaledW == 0u || scaledH == 0u || width == 0u || height == 0u) {
        GroupMemoryBarrierWithGroupSync();
        if (tid.x == 0) {
            LumaRange[0] = kLumMinimumRange;
        }
        return;
    }

    for (uint y = 0; y < scaledH; ++y) {
        uint syStart = (y * height) / scaledH;
        uint syEnd = ((y + 1u) * height) / scaledH;
        if (syEnd <= syStart) syEnd = syStart + 1u;
        if (syEnd > height) syEnd = height;

        for (uint x = tid.x; x < scaledW; x += 256u) {
            uint sxStart = (x * width) / scaledW;
            uint sxEnd = ((x + 1u) * width) / scaledW;
            if (sxEnd <= sxStart) sxEnd = sxStart + 1u;
            if (sxEnd > width) sxEnd = width;

            uint sum = 0u;
            uint count = 0u;
            for (uint sy = syStart; sy < syEnd; ++sy) {
                for (uint sx = sxStart; sx < sxEnd; ++sx) {
#if defined(NV12_INPUT)
                    sum += CodeFromNormRounded(TextureY.Load(int3((int)sx, (int)sy, 0)));
#else
                    sum += LoadRgbaLumaByte(sx, sy);
#endif
                    ++count;
                }
            }
            if (count == 0u) {
                continue;
            }
#if defined(NV12_INPUT)
            uint bin = NormalizeYCodeToByte((sum + (count >> 1)) / count);
#else
            uint bin = sum / count;
#endif
            bin = min(bin, 255u);
            InterlockedAdd(histogram[bin], 1);
        }
    }
    
    GroupMemoryBarrierWithGroupSync();
    
    if (tid.x == 0) {
        uint prevRange = LumaRange[0];
        uint totalSamples = 0;

        for (uint i = 0; i < 256; ++i) {
            totalSamples += histogram[i];
        }
        
        // Find Range (1% - 99%)
        uint targetLow = max(1u, (totalSamples + 50u) / 100u);
        uint targetHigh = max(1u, (totalSamples * 99u + 50u) / 100u);
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
        
        uint range = RadioifyResolveLumaRange(low, high, kLumMinimumRange);
        range = RadioifySmoothLumaRange(range, prevRange, frameCount,
                                        kLumResetDelta, kLumSmoothAlpha,
                                        kLumMinimumRange);

        LumaRange[0] = range;
    }
}
