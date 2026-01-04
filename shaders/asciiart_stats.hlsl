
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
