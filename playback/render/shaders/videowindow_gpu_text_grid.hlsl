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

#include "videowindow_hdr_generate.hlsli"

float3 packedRgb(uint rgb) {
    return float3(
        float(rgb & 0xFFu),
        float((rgb >> 8) & 0xFFu),
        float((rgb >> 16) & 0xFFu)) / 255.0;
}

float3 ConvertSdrToOutput(float3 rgb, float emissiveBoost) {
    return RadioifySdr709ToOutput(rgb, outputColorSpace, sdrWhiteScale,
                                  outputMaxNits, emissiveBoost);
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
    float3 fg = ConvertSdrToOutput(packedRgb(cell.g), 1.85);
    if ((cell.a & CELL_FLAG_TRANSPARENT_BG) != 0u) {
        return float4(fg, coverage);
    }
    float3 bg = ConvertSdrToOutput(packedRgb(cell.b), 1.0);
    return float4(lerp(bg, fg, coverage), 1.0);
}
