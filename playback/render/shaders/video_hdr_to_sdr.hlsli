#ifndef RADIOIFY_VIDEO_HDR_TO_SDR_HLSLI
#define RADIOIFY_VIDEO_HDR_TO_SDR_HLSLI

#include "video_color_math.hlsli"

float RadioifyHdrTransferToSdr(float v, uint yuvTransfer, float hdrScale,
                               uint transferPq, uint transferHlg)
{
    v = saturate(v);
    if (yuvTransfer == transferPq) {
        v = RadioifyPqEotf(v);
    } else if (yuvTransfer == transferHlg) {
        v = RadioifyHlgEotf(v);
    }
    v = saturate(RadioifyToneMapFilmic(v * hdrScale));
    return RadioifyLinearToSrgb(v);
}

float3 RadioifyHdrTransferToSdr(float3 v, uint yuvTransfer, float hdrScale,
                                uint transferPq, uint transferHlg)
{
    v = saturate(v);
    if (yuvTransfer == transferPq) {
        v = RadioifyPqEotf(v);
    } else if (yuvTransfer == transferHlg) {
        v = RadioifyHlgEotf(v);
    }
    v = saturate(RadioifyToneMapFilmic(v * hdrScale));
    return RadioifyLinearToSrgb(v);
}

#endif
