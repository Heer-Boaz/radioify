#ifndef RADIOIFY_VIDEO_LUMINANCE_STATS_HLSLI
#define RADIOIFY_VIDEO_LUMINANCE_STATS_HLSLI

uint RadioifyResolveLumaRange(uint low, uint high, uint minimumRange)
{
    uint range = (high > low) ? (high - low) : 1u;
    return max(minimumRange, range);
}

uint RadioifySmoothLumaRange(uint currentRange, uint previousRange,
                             uint frameCount, uint resetDelta,
                             uint smoothAlphaQ8, uint minimumRange)
{
    uint range = max(currentRange, minimumRange);
    if (frameCount > 0u) {
        int rangeDelta = (int)range - (int)previousRange;
        if (abs(rangeDelta) < (int)resetDelta) {
            range = (uint)((int)previousRange +
                           (rangeDelta * (int)smoothAlphaQ8 >> 8));
            range = max(range, minimumRange);
        }
    }
    return range;
}

#endif
