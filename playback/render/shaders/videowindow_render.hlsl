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
    float sdrWhiteScale;
    float outputMaxNits;
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

static const float SDR_REFERENCE_WHITE_NITS = 80.0;
static const float PQ_REFERENCE_MAX_NITS = 10000.0;
static const float PQ_EOTF_DENOM_EPSILON = 1.0e-6;
static const uint OUTPUT_COLOR_SDR = 0u;
static const uint OUTPUT_COLOR_SCRGB = 1u;
static const uint OUTPUT_COLOR_HDR10 = 2u;

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

float PQEotf(float v) {
    float m1 = 2610.0 / 16384.0;
    float m2 = 2523.0 / 32.0;
    float c1 = 3424.0 / 4096.0;
    float c2 = 2413.0 / 128.0;
    float c3 = 2392.0 / 128.0;
    float vp = pow(max(v, 0.0), 1.0 / m2);
    float denom = max(c2 - c3 * vp, PQ_EOTF_DENOM_EPSILON);
    return pow(max((vp - c1) / denom, 0.0), 1.0 / m1);
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

float3 PQOetf(float3 v) {
    return float3(PQOetf(v.r), PQOetf(v.g), PQOetf(v.b));
}

float HlgEotf(float v) {
    const float a = 0.17883277;
    const float b = 1.0 - 4.0 * a;
    const float c = 0.5 - a * log(4.0 * a);
    if (v <= 0.5) return (v * v) / 3.0;
    return (exp((v - c) / a) + b) / 12.0;
}

float ToneMapFilmic(float x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float LinearToSrgb(float v) {
    v = max(v, 0.0);
    return (v <= 0.0031308) ? (v * 12.92) : (1.055 * pow(v, 1.0 / 2.4) - 0.055);
}

float SrgbToLinear(float v) {
    v = saturate(v);
    return (v <= 0.04045) ? (v / 12.92) : pow((v + 0.055) / 1.055, 2.4);
}

float3 SrgbToLinear(float3 v) {
    return float3(SrgbToLinear(v.r), SrgbToLinear(v.g), SrgbToLinear(v.b));
}

float3 LinearBt2020ToBt709(float3 v) {
    return float3(
        1.6605 * v.r - 0.5876 * v.g - 0.0728 * v.b,
       -0.1246 * v.r + 1.1329 * v.g - 0.0083 * v.b,
       -0.0182 * v.r - 0.1006 * v.g + 1.1187 * v.b);
}

float3 LinearBt709ToBt2020(float3 v) {
    return float3(
        0.6274 * v.r + 0.3293 * v.g + 0.0433 * v.b,
        0.0691 * v.r + 0.9195 * v.g + 0.0114 * v.b,
        0.0164 * v.r + 0.0880 * v.g + 0.8956 * v.b);
}

float3 ApplyHdrToSdr(float3 v) {
    v = saturate(v);
    if (yuvTransfer == 1) {
        float3 linearRgb = float3(PQEotf(v.r), PQEotf(v.g), PQEotf(v.b));
        float3 mapped = float3(ToneMapFilmic(linearRgb.r * 100.0), ToneMapFilmic(linearRgb.g * 100.0), ToneMapFilmic(linearRgb.b * 100.0));
        v = float3(LinearToSrgb(mapped.r), LinearToSrgb(mapped.g), LinearToSrgb(mapped.b));
    } else if (yuvTransfer == 2) {
        float3 linearRgb = float3(HlgEotf(v.r), HlgEotf(v.g), HlgEotf(v.b));
        float3 mapped = float3(ToneMapFilmic(linearRgb.r * 100.0), ToneMapFilmic(linearRgb.g * 100.0), ToneMapFilmic(linearRgb.b * 100.0));
        v = float3(LinearToSrgb(mapped.r), LinearToSrgb(mapped.g), LinearToSrgb(mapped.b));
    }
    return v;
}

float3 ConvertVideoToOutput(float3 rgb) {
    float3 outputRgb = saturate(rgb);

    if (outputColorSpace == OUTPUT_COLOR_SDR) {
        if (yuvTransfer != 0u) {
            outputRgb = ApplyHdrToSdr(rgb);
        }
    } else if (outputColorSpace == OUTPUT_COLOR_HDR10) {
        if (yuvTransfer == 1u && yuvMatrix == 2u) {
            outputRgb = saturate(rgb);
        } else {
            float3 linearRgb;
            if (yuvTransfer == 1u) {
                linearRgb = float3(PQEotf(rgb.r), PQEotf(rgb.g), PQEotf(rgb.b));
                if (yuvMatrix != 2u) {
                    linearRgb = LinearBt709ToBt2020(linearRgb);
                }
                linearRgb *= PQ_REFERENCE_MAX_NITS;
            } else if (yuvTransfer == 2u) {
                linearRgb = float3(HlgEotf(rgb.r), HlgEotf(rgb.g), HlgEotf(rgb.b));
                if (yuvMatrix != 2u) {
                    linearRgb = LinearBt709ToBt2020(linearRgb);
                }
                linearRgb *= max(outputMaxNits, SDR_REFERENCE_WHITE_NITS);
            } else {
                linearRgb = LinearBt709ToBt2020(SrgbToLinear(rgb));
                linearRgb *= SDR_REFERENCE_WHITE_NITS * max(sdrWhiteScale, 0.0);
            }
            outputRgb = PQOetf(linearRgb / PQ_REFERENCE_MAX_NITS);
        }
    } else if (yuvTransfer == 1u) {
        float3 linearRgb = float3(PQEotf(rgb.r), PQEotf(rgb.g), PQEotf(rgb.b));
        linearRgb *= PQ_REFERENCE_MAX_NITS / SDR_REFERENCE_WHITE_NITS;
        if (yuvMatrix == 2u) linearRgb = LinearBt2020ToBt709(linearRgb);
        outputRgb = max(linearRgb, 0.0);
    } else if (yuvTransfer == 2u) {
        float3 linearRgb = float3(HlgEotf(rgb.r), HlgEotf(rgb.g), HlgEotf(rgb.b));
        float hlgScale = max(outputMaxNits / SDR_REFERENCE_WHITE_NITS,
                             max(sdrWhiteScale, 1.0));
        linearRgb *= hlgScale;
        if (yuvMatrix == 2u) linearRgb = LinearBt2020ToBt709(linearRgb);
        outputRgb = max(linearRgb, 0.0);
    } else {
        outputRgb = SrgbToLinear(rgb) * max(sdrWhiteScale, 0.0);
    }

    return outputRgb;
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
