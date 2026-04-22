struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

Texture2D<uint4> gridTex : register(t0);
Texture2D<float> glyphAtlasTex : register(t1);

cbuffer GpuTextGridConstants : register(b0) {
    uint glyphCellWidth;
    uint glyphCellHeight;
    uint glyphAtlasCols;
    uint outputColorSpace;
    float sdrWhiteScale;
    float outputMaxNits;
    uint constantsPad0;
    uint constantsPad1;
};

static const uint CELL_FLAG_BRAILLE = 1u;
static const uint CELL_FLAG_TRANSPARENT_BG = 2u;
static const uint BLOCK_GLYPH_ATLAS_START = 111u;
static const uint BRAILLE_GLYPH_ATLAS_START = 119u;
static const float SDR_REFERENCE_WHITE_NITS = 80.0;
static const float PQ_REFERENCE_MAX_NITS = 10000.0;
static const uint OUTPUT_COLOR_SDR = 0u;
static const uint OUTPUT_COLOR_HDR10 = 2u;

float3 packedRgb(uint rgb) {
    return float3(
        float(rgb & 0xFFu),
        float((rgb >> 8) & 0xFFu),
        float((rgb >> 16) & 0xFFu)) / 255.0;
}

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

uint atlasGlyphIndex(uint ch) {
    if (ch >= 32u && ch <= 126u) return ch - 32u;
    if (ch == 0x25B6u) return 95u;
    if (ch == 0x23F8u) return 96u;
    if (ch == 0x25A0u) return 97u;
    if (ch == 0x2022u) return 98u;
    if (ch == 0x2500u || ch == 0x2501u) return 99u;
    if (ch == 0x2502u || ch == 0x2503u) return 100u;
    if (ch == 0x250Cu) return 101u;
    if (ch == 0x2510u) return 102u;
    if (ch == 0x2514u) return 103u;
    if (ch == 0x2518u) return 104u;
    if (ch == 0x251Cu) return 105u;
    if (ch == 0x2524u) return 106u;
    if (ch == 0x252Cu) return 107u;
    if (ch == 0x2534u) return 108u;
    if (ch == 0x253Cu) return 109u;
    if (ch == 0x25CBu) return 110u;
    if (ch >= 0x2588u && ch <= 0x258Fu) {
        return BLOCK_GLYPH_ATLAS_START + (ch - 0x2588u);
    }
    return 31u;
}

float4 PS_GPU_TEXT_GRID(PS_INPUT input) : SV_Target {
    uint width = 0u;
    uint height = 0u;
    gridTex.GetDimensions(width, height);

    float2 gridPos = input.tex * float2(width, height);
    uint2 cellPos = min(uint2(gridPos), uint2(width - 1u, height - 1u));
    uint4 cell = gridTex.Load(int3(cellPos, 0));

    float2 local = frac(gridPos);
    uint glyphIndex = atlasGlyphIndex(cell.r);
    if ((cell.a & CELL_FLAG_BRAILLE) != 0u) {
        glyphIndex = BRAILLE_GLYPH_ATLAS_START + (cell.r & 0xFFu);
    }

    uint atlasCols = max(1u, glyphAtlasCols);
    uint2 cellSize = uint2(max(1u, glyphCellWidth),
                           max(1u, glyphCellHeight));
    uint2 glyphCell = uint2(glyphIndex % atlasCols,
                            glyphIndex / atlasCols);
    uint2 glyphPixel =
        min(uint2(floor(local * float2(cellSize))),
            cellSize - uint2(1u, 1u));
    uint2 atlasPixel =
        glyphCell * cellSize + glyphPixel;
    float coverage = glyphAtlasTex.Load(int3(atlasPixel, 0));
    if ((cell.a & CELL_FLAG_TRANSPARENT_BG) != 0u) {
        return float4(ConvertSdrToOutput(packedRgb(cell.g)), coverage);
    }
    return float4(ConvertSdrToOutput(lerp(packedRgb(cell.b), packedRgb(cell.g), coverage)), 1.0);
}
