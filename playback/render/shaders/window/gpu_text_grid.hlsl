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
    float outputSdrWhiteNits;
    float outputPeakNits;
    float outputFullFrameNits;
    float asciiGlyphPeakNits;
};

static const uint CELL_FLAG_TRANSPARENT_BG = 2u;

#include "hdr_generate.hlsli"

float3 packedRgb(uint rgb) {
    return float3(
        float(rgb & 0xFFu),
        float((rgb >> 8) & 0xFFu),
        float((rgb >> 16) & 0xFFu)) / 255.0;
}

float4 PS_GPU_TEXT_GRID(PS_INPUT input) : SV_Target {
    uint width = 0u;
    uint height = 0u;
    gridTex.GetDimensions(width, height);

    float2 gridPos = input.tex * float2(width, height);
    uint2 cellPos = min(uint2(gridPos), uint2(width - 1u, height - 1u));
    uint4 cell = gridTex.Load(int3(cellPos, 0));

    float2 local = frac(gridPos);
    uint glyphIndex = cell.r;

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
    float3 fg = packedRgb(cell.g);
    if (outputColorSpace == RADIOIFY_OUTPUT_COLOR_SDR) {
        if ((cell.a & CELL_FLAG_TRANSPARENT_BG) != 0u) {
            return float4(fg, coverage);
        }
        float3 bg = packedRgb(cell.b);
        return float4(lerp(bg, fg, coverage), 1.0);
    }

    float3 fgNits = RadioifySdr709ToAsciiNits709(
        fg, outputSdrWhiteNits, asciiGlyphPeakNits, 1.85);
    if ((cell.a & CELL_FLAG_TRANSPARENT_BG) != 0u) {
        return float4(
            RadioifyEncodeNits709ToOutput(fgNits, outputColorSpace,
                                          outputSdrWhiteNits),
            coverage);
    }
    float3 bg = packedRgb(cell.b);
    float3 bgNits = RadioifySdr709ToAsciiNits709(
        bg, outputSdrWhiteNits, asciiGlyphPeakNits, 1.0);
    float3 composedNits = lerp(bgNits, fgNits, coverage);
    return float4(
        RadioifyEncodeNits709ToOutput(composedNits, outputColorSpace,
                                      outputSdrWhiteNits),
        1.0);
}
