// Video frame rendering shader - handles YUV/RGBA to RGB conversion with color correction.
// Playback controls/progress are rendered by the shared GPU text-grid overlay pass.

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

cbuffer Constants : register(b0) {
    uint isFullRange;
    uint yuvMatrix;
    uint yuvTransfer;
    uint bitDepth;
    float uiProgress;
    float uiAlpha;
    uint uiPaused;
    uint uiHasRGBA;
    uint uiVolPct;
    uint uiRotationQuarterTurns;
    float uiTextTop;
    float uiTextHeight;
    float uiTextLeft;
    float uiTextWidth;
    float subtitleTop;
    float subtitleHeight;
    float subtitleLeft;
    float subtitleWidth;
    float subtitleAlpha;
    uint outputColorSpace;
    float outputSdrWhiteNits;
    float outputPeakNits;
    float outputFullFrameNits;
    float asciiGlyphPeakNits;
};

PS_INPUT VS(uint vid : SV_VertexID) {
    PS_INPUT output;
    output.tex = float2(vid & 1, vid >> 1);
    output.pos = float4(output.tex * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

float2 RotateInputUV(float2 uv) {
    uint turns = uiRotationQuarterTurns & 3u;
    if (turns == 1u) return float2(uv.y, 1.0f - uv.x);
    if (turns == 2u) return float2(1.0f - uv.x, 1.0f - uv.y);
    if (turns == 3u) return float2(1.0f - uv.y, uv.x);
    return uv;
}

Texture2D texY : register(t0);
Texture2D texUV : register(t1);
Texture2D texRGBA : register(t2);
SamplerState sam : register(s0);

#include "hdr_generate.hlsli"

float ExpandYNorm(float yNorm) {
    float maxCode = (float)((1u << bitDepth) - 1u);
    float yCode = yNorm * maxCode;
    uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
    float yMin = (isFullRange != 0) ? 0.0f : (float)(16u << shift);
    float yMax = (isFullRange != 0) ? maxCode : (float)(235u << shift);
    return saturate((yCode - yMin) / max(yMax - yMin, 1.0f));
}

float2 ExpandUV(float2 uvNorm) {
    float maxCode = (float)((1u << bitDepth) - 1u);
    float2 uvCode = uvNorm * maxCode;
    uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
    float cMid = (float)(128u << shift);
    float cMin = (isFullRange != 0) ? 0.0f : (float)(16u << shift);
    float cMax = (isFullRange != 0) ? maxCode : (float)(240u << shift);
    return (uvCode - cMid) / max(cMax - cMin, 1.0f);
}

float3 ConvertVideoToOutput(float3 rgb) {
    return RadioifyVideoToOutput(rgb, yuvTransfer, yuvMatrix,
                                 outputColorSpace, outputSdrWhiteNits,
                                 outputPeakNits, outputFullFrameNits);
}

float4 PS(PS_INPUT input) : SV_Target {
    float2 srcUv = RotateInputUV(input.tex);
    if (uiHasRGBA != 0) {
        float4 c = texRGBA.Sample(sam, srcUv);
        return float4(ConvertVideoToOutput(c.rgb), 1.0);
    }

    float y = ExpandYNorm(texY.Sample(sam, srcUv).r);
    float2 uv = ExpandUV(texUV.Sample(sam, srcUv).rg);
    
    float3 rgb = float3(
        y + 1.5748 * uv.y,
        y - 0.1873 * uv.x - 0.4681 * uv.y,
        y + 1.8556 * uv.x);
    if (yuvMatrix == 2) {
        rgb = float3(
            y + 1.4746 * uv.y,
            y - 0.16455 * uv.x - 0.57135 * uv.y,
            y + 1.8814 * uv.x);
    } else if (yuvMatrix == 1) {
        rgb = float3(
            y + 1.4020 * uv.y,
            y - 0.3441 * uv.x - 0.7141 * uv.y,
            y + 1.7720 * uv.x);
    }

    return float4(ConvertVideoToOutput(rgb), 1.0);
}
