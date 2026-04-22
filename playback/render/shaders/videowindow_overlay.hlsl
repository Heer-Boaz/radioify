// Subtitle-only overlay shader. Playback controls/progress are rendered by the
// shared GPU text-grid overlay pass used by ASCII PiP.

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
    float sdrWhiteScale;
    float outputMaxNits;
};

PS_INPUT VS(uint vid : SV_VertexID) {
    PS_INPUT output;
    output.tex = float2(vid & 1, vid >> 1);
    output.pos = float4(output.tex * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

Texture2D texSubtitle : register(t4);
SamplerState sam : register(s0);

#include "videowindow_hdr_generate.hlsli"

float3 ConvertSdrToOutput(float3 rgb) {
    return RadioifySdr709ToOutput(rgb, outputColorSpace, sdrWhiteScale,
                                  outputMaxNits, 1.0);
}

float4 PS_UI(PS_INPUT input) : SV_Target {
    float2 uv = input.tex;
    float4 subtitleColor = float4(0, 0, 0, 0);
    bool subtitleHit = false;
    if (subtitleAlpha > 0.01 && subtitleHeight > 0.0) {
        if (uv.y >= subtitleTop && uv.y <= (subtitleTop + subtitleHeight) &&
            uv.x >= subtitleLeft && uv.x <= (subtitleLeft + subtitleWidth)) {
            float localY = (uv.y - subtitleTop) / subtitleHeight;
            float localX = (uv.x - subtitleLeft) / subtitleWidth;
            float2 textUV = float2(localX, localY);
            float4 t = texSubtitle.Sample(sam, textUV);
            if (t.a > 0.01) {
                subtitleColor = t;
                subtitleColor.rgb = ConvertSdrToOutput(subtitleColor.rgb);
                subtitleColor.a *= subtitleAlpha;
                subtitleHit = true;
            }
        }
    }

    if (!subtitleHit) discard;
    return subtitleColor;
}
