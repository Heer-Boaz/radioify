
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

float PQEotf(float v) {
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 128.0f;
    const float c3 = 2392.0f / 128.0f;
    float vp = pow(max(v, 0.0f), 1.0f / m2);
    return pow(max(vp - c1, 0.0f) / max(c2 - c3 * vp, 1e-6f), 1.0f / m1);
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

float ToneMapFilmic(float x) {
    const float A = 0.15f;
    const float B = 0.50f;
    const float C = 0.10f;
    const float D = 0.20f;
    const float E = 0.02f;
    const float F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) -
           E / F;
}

float LinearToSrgb(float v) {
    v = max(v, 0.0f);
    if (v <= 0.0031308f) {
        return v * 12.92f;
    }
    return 1.055f * pow(v, 1.0f / 2.4f) - 0.055f;
}

float ApplyHdrToSdr(float v) {
    if (yuvTransfer == kTransferPq) {
        v = PQEotf(v);
    } else if (yuvTransfer == kTransferHlg) {
        v = HlgEotf(v);
    }
    v = saturate(ToneMapFilmic(v * kHdrScale));
    return LinearToSrgb(v);
}

float LoadLinearLuma(uint x, uint y) {
#if defined(NV12_INPUT)
    float yVal = TextureY.Load(int3(x, y, 0));
    return ExpandYNorm(yVal) * 255.0f;
#else
    float3 rgb = InputTexture.Load(int3(x, y, 0)).rgb;
    return saturate(dot(rgb, kLumaCoeff)) * 255.0f;
#endif
}

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
            LumaRange[0] = 80u;
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

            float sum = 0.0f;
            uint count = 0u;
            for (uint sy = syStart; sy < syEnd; ++sy) {
                for (uint sx = sxStart; sx < sxEnd; ++sx) {
                    sum += LoadLinearLuma(sx, sy);
                    ++count;
                }
            }
            if (count == 0u) {
                continue;
            }
            float avg = sum / (float)count;
#if defined(NV12_INPUT)
            if (yuvTransfer != kTransferSdr) {
                avg = saturate(ApplyHdrToSdr(avg / 255.0f)) * 255.0f;
            }
#endif
            uint bin = (uint)(avg + 0.5f);
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
        
        uint range = max(1, high - low);
        if (range < 80) range = 80;

        if (frameCount > 0) {
            int rangeDelta = (int)range - (int)prevRange;
            if (abs(rangeDelta) < (int)kLumResetDelta) {
                range = (uint)((int)prevRange + (rangeDelta * (int)kLumSmoothAlpha >> 8));
                if (range < 80u) range = 80u;
            }
        }

        LumaRange[0] = range;
    }
}
