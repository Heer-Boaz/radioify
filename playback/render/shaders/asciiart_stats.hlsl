
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
RWStructuredBuffer<uint4> Stats : register(u0); // x=bgLum, y=lumRange

groupshared uint histogram[256];

static const uint kLumSmoothAlpha = 40u;
static const uint kLumResetDelta = 28u;
static const float3 kLumaCoeff = float3(0.2126f, 0.7152f, 0.0722f);

float LoadLuma(uint x, uint y) {
#if defined(NV12_INPUT)
    float yVal = TextureY.Load(int3(x, y, 0));
    return saturate(yVal) * 255.0f;
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
            Stats[0] = uint4(0, 80, 0, 0);
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
                    sum += LoadLuma(sx, sy);
                    ++count;
                }
            }
            if (count == 0u) {
                continue;
            }
            float avg = sum / (float)count;
            uint bin = (uint)(avg + 0.5f);
            bin = min(bin, 255u);
            InterlockedAdd(histogram[bin], 1);
        }
    }
    
    GroupMemoryBarrierWithGroupSync();
    
    if (tid.x == 0) {
        uint4 prev = Stats[0];
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

        if (frameCount > 0) {
            int modeDelta = (int)mode - (int)prev.x;
            if (abs(modeDelta) < (int)kLumResetDelta) {
                mode = (uint)((int)prev.x + (modeDelta * (int)kLumSmoothAlpha >> 8));
            }
            int rangeDelta = (int)range - (int)prev.y;
            if (abs(rangeDelta) < (int)kLumResetDelta) {
                range = (uint)((int)prev.y + (rangeDelta * (int)kLumSmoothAlpha >> 8));
                if (range < 80u) range = 80u;
            }
        }

        Stats[0] = uint4(mode, range, 0, 0);
    }
}
