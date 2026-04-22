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

static const float SDR_REFERENCE_WHITE_NITS = 80.0;
static const float PQ_REFERENCE_MAX_NITS = 10000.0;
static const uint OUTPUT_COLOR_SDR = 0u;
static const uint OUTPUT_COLOR_HDR10 = 2u;

float SrgbToLinear(float v) {
    v = saturate(v);
    return (v <= 0.04045) ? (v / 12.92) : pow((v + 0.055) / 1.055, 2.4);
}

float PQOetf(float v) {
    float m1 = 2610.0 / 16384.0;
    float m2 = 2523.0 / 32.0;
    float c1 = 3424.0 / 4096.0;
    float c2 = 2413.0 / 128.0;
    float c3 = 2392.0 / 128.0;
    float vp = pow(max(v, 0.0), m1);
    return pow((c1 + c2 * vp) / (1.0 + c3 * vp), m2);
}

float3 LinearBt709ToBt2020(float3 v) {
    return float3(
        0.6274 * v.r + 0.3293 * v.g + 0.0433 * v.b,
        0.0691 * v.r + 0.9195 * v.g + 0.0114 * v.b,
        0.0164 * v.r + 0.0880 * v.g + 0.8956 * v.b);
}

float3 ConvertSdrToOutput(float3 rgb) {
    if (outputColorSpace == OUTPUT_COLOR_SDR) return saturate(rgb);
    float3 linearRgb =
        float3(SrgbToLinear(rgb.r), SrgbToLinear(rgb.g), SrgbToLinear(rgb.b));
    linearRgb *= max(sdrWhiteScale, 0.0);
    if (outputColorSpace == OUTPUT_COLOR_HDR10) {
        float3 bt2020 = LinearBt709ToBt2020(linearRgb);
        float3 pqInput =
            bt2020 * (SDR_REFERENCE_WHITE_NITS / PQ_REFERENCE_MAX_NITS);
        return float3(PQOetf(pqInput.r), PQOetf(pqInput.g), PQOetf(pqInput.b));
    }
    return linearRgb;
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
