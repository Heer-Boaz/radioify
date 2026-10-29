#ifndef RADIOIFY_VIDEO_HDR_COLOR_HLSLI
#define RADIOIFY_VIDEO_HDR_COLOR_HLSLI

#include "video_color_math.hlsli"

// HDR cell colours retain their source transfer and BT.2020 primaries. PQ
// values are relative to 10000 nits; HLG remains relative to the output peak.
float3 RadioifyHdrCodeToLinear2020(float3 code, uint transfer, uint colorMatrix)
{
    float3 linearRgb = (transfer == 1u) ? RadioifyPqEotf(saturate(code))
                                       : RadioifyHlgEotf(saturate(code));
    return (colorMatrix == 2u) ? linearRgb
                          : RadioifyLinearBt709ToBt2020(linearRgb);
}

float3 RadioifyHdrLinearToCode(float3 linearRgb, uint transfer)
{
    float3 code = RadioifyHlgOetf(linearRgb);
    if (transfer == 1u) {
        code = float3(RadioifyPqOetf(linearRgb.r),
                       RadioifyPqOetf(linearRgb.g),
                       RadioifyPqOetf(linearRgb.b));
    }
    return code;
}

uint RadioifyPackHdrColor(float3 linear2020, uint transfer)
{
    uint3 code = (uint3)(saturate(RadioifyHdrLinearToCode(
        max(linear2020, 0.0), transfer)) * 1023.0 + 0.5);
    return code.r | (code.g << 10u) | (code.b << 20u) | (transfer << 30u);
}

float3 RadioifyUnpackHdrColor(uint packed)
{
    float3 code = float3(packed & 1023u, (packed >> 10u) & 1023u,
                        (packed >> 20u) & 1023u) / 1023.0;
    return RadioifyHdrCodeToLinear2020(code, packed >> 30u, 2u);
}

#endif
