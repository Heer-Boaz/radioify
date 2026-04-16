struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

Texture2D<uint4> gridTex : register(t0);

static const uint CELL_FLAG_BRAILLE = 1u;

float3 packedRgb(uint rgb) {
    return float3(
        float(rgb & 0xFFu),
        float((rgb >> 8) & 0xFFu),
        float((rgb >> 16) & 0xFFu)) / 255.0;
}

uint glyphRow(uint ch, uint row) {
    if (row >= 7u) return 0u;
    switch (ch) {
        case 32u: return 0u;
        case 35u: { uint r[7] = {10u, 31u, 10u, 10u, 31u, 10u, 0u}; return r[row]; }
        case 37u: { uint r[7] = {19u, 19u, 2u, 4u, 8u, 25u, 25u}; return r[row]; }
        case 40u: { uint r[7] = {2u, 4u, 8u, 8u, 8u, 4u, 2u}; return r[row]; }
        case 41u: { uint r[7] = {8u, 4u, 2u, 2u, 2u, 4u, 8u}; return r[row]; }
        case 43u: { uint r[7] = {0u, 4u, 4u, 31u, 4u, 4u, 0u}; return r[row]; }
        case 45u: { uint r[7] = {0u, 0u, 0u, 31u, 0u, 0u, 0u}; return r[row]; }
        case 46u: { uint r[7] = {0u, 0u, 0u, 0u, 0u, 6u, 6u}; return r[row]; }
        case 47u: { uint r[7] = {1u, 2u, 4u, 8u, 16u, 0u, 0u}; return r[row]; }
        case 48u: { uint r[7] = {14u, 17u, 19u, 21u, 25u, 17u, 14u}; return r[row]; }
        case 49u: { uint r[7] = {4u, 12u, 4u, 4u, 4u, 4u, 14u}; return r[row]; }
        case 50u: { uint r[7] = {14u, 17u, 1u, 2u, 4u, 8u, 31u}; return r[row]; }
        case 51u: { uint r[7] = {30u, 1u, 1u, 14u, 1u, 1u, 30u}; return r[row]; }
        case 52u: { uint r[7] = {2u, 6u, 10u, 18u, 31u, 2u, 2u}; return r[row]; }
        case 53u: { uint r[7] = {31u, 16u, 16u, 30u, 1u, 1u, 30u}; return r[row]; }
        case 54u: { uint r[7] = {14u, 16u, 16u, 30u, 17u, 17u, 14u}; return r[row]; }
        case 55u: { uint r[7] = {31u, 1u, 2u, 4u, 8u, 8u, 8u}; return r[row]; }
        case 56u: { uint r[7] = {14u, 17u, 17u, 14u, 17u, 17u, 14u}; return r[row]; }
        case 57u: { uint r[7] = {14u, 17u, 17u, 15u, 1u, 1u, 14u}; return r[row]; }
        case 58u: { uint r[7] = {0u, 4u, 4u, 0u, 4u, 4u, 0u}; return r[row]; }
        case 63u: { uint r[7] = {14u, 17u, 1u, 2u, 4u, 0u, 4u}; return r[row]; }
        case 65u: { uint r[7] = {14u, 17u, 17u, 31u, 17u, 17u, 17u}; return r[row]; }
        case 66u: { uint r[7] = {30u, 17u, 17u, 30u, 17u, 17u, 30u}; return r[row]; }
        case 67u: { uint r[7] = {14u, 17u, 16u, 16u, 16u, 17u, 14u}; return r[row]; }
        case 68u: { uint r[7] = {30u, 17u, 17u, 17u, 17u, 17u, 30u}; return r[row]; }
        case 69u: { uint r[7] = {31u, 16u, 16u, 30u, 16u, 16u, 31u}; return r[row]; }
        case 70u: { uint r[7] = {31u, 16u, 16u, 30u, 16u, 16u, 16u}; return r[row]; }
        case 71u: { uint r[7] = {14u, 17u, 16u, 23u, 17u, 17u, 15u}; return r[row]; }
        case 72u: { uint r[7] = {17u, 17u, 17u, 31u, 17u, 17u, 17u}; return r[row]; }
        case 73u: { uint r[7] = {14u, 4u, 4u, 4u, 4u, 4u, 14u}; return r[row]; }
        case 74u: { uint r[7] = {7u, 2u, 2u, 2u, 18u, 18u, 12u}; return r[row]; }
        case 75u: { uint r[7] = {17u, 18u, 20u, 24u, 20u, 18u, 17u}; return r[row]; }
        case 76u: { uint r[7] = {16u, 16u, 16u, 16u, 16u, 16u, 31u}; return r[row]; }
        case 77u: { uint r[7] = {17u, 27u, 21u, 17u, 17u, 17u, 17u}; return r[row]; }
        case 78u: { uint r[7] = {17u, 25u, 21u, 19u, 17u, 17u, 17u}; return r[row]; }
        case 79u: { uint r[7] = {14u, 17u, 17u, 17u, 17u, 17u, 14u}; return r[row]; }
        case 80u: { uint r[7] = {30u, 17u, 17u, 30u, 16u, 16u, 16u}; return r[row]; }
        case 81u: { uint r[7] = {14u, 17u, 17u, 17u, 21u, 18u, 13u}; return r[row]; }
        case 82u: { uint r[7] = {30u, 17u, 17u, 30u, 20u, 18u, 17u}; return r[row]; }
        case 83u: { uint r[7] = {15u, 16u, 16u, 14u, 1u, 1u, 30u}; return r[row]; }
        case 84u: { uint r[7] = {31u, 4u, 4u, 4u, 4u, 4u, 4u}; return r[row]; }
        case 85u: { uint r[7] = {17u, 17u, 17u, 17u, 17u, 17u, 14u}; return r[row]; }
        case 86u: { uint r[7] = {17u, 17u, 17u, 17u, 10u, 10u, 4u}; return r[row]; }
        case 87u: { uint r[7] = {17u, 17u, 17u, 21u, 21u, 27u, 17u}; return r[row]; }
        case 88u: { uint r[7] = {17u, 10u, 4u, 4u, 4u, 10u, 17u}; return r[row]; }
        case 89u: { uint r[7] = {17u, 10u, 4u, 4u, 4u, 4u, 4u}; return r[row]; }
        case 90u: { uint r[7] = {31u, 1u, 2u, 4u, 8u, 16u, 31u}; return r[row]; }
        case 91u: { uint r[7] = {30u, 16u, 16u, 16u, 16u, 16u, 30u}; return r[row]; }
        case 92u: { uint r[7] = {16u, 8u, 4u, 2u, 1u, 0u, 0u}; return r[row]; }
        case 93u: { uint r[7] = {15u, 1u, 1u, 1u, 1u, 1u, 15u}; return r[row]; }
        case 95u: { uint r[7] = {0u, 0u, 0u, 0u, 0u, 0u, 31u}; return r[row]; }
        case 124u: { uint r[7] = {4u, 4u, 4u, 4u, 4u, 4u, 4u}; return r[row]; }
        case 126u: { uint r[7] = {0u, 0u, 8u, 21u, 2u, 0u, 0u}; return r[row]; }
        default: { uint r[7] = {14u, 17u, 1u, 2u, 4u, 0u, 4u}; return r[row]; }
    }
}

bool brailleInk(uint mask, float2 local) {
    float2 dotGrid = local * float2(2.0, 4.0);
    uint col = min((uint)floor(dotGrid.x), 1u);
    uint row = min((uint)floor(dotGrid.y), 3u);
    uint bit = col == 0u ? (row == 3u ? 6u : row)
                         : (row == 3u ? 7u : row + 3u);
    if (((mask >> bit) & 1u) == 0u) return false;

    float2 p = frac(dotGrid);
    return p.x >= 0.18 && p.x <= 0.82 && p.y >= 0.14 && p.y <= 0.86;
}

float4 PS_GPU_TEXT_GRID(PS_INPUT input) : SV_Target {
    uint width = 0u;
    uint height = 0u;
    gridTex.GetDimensions(width, height);

    float2 gridPos = input.tex * float2(width, height);
    uint2 cellPos = min(uint2(gridPos), uint2(width - 1u, height - 1u));
    uint4 cell = gridTex.Load(int3(cellPos, 0));

    float2 local = frac(gridPos);
    bool ink = false;
    if ((cell.a & CELL_FLAG_BRAILLE) != 0u) {
        ink = brailleInk(cell.r, local);
    } else {
        uint glyphX = (uint)floor(local.x * 6.0);
        uint glyphY = (uint)floor(local.y * 8.0);
        uint rowBits = glyphRow(cell.r, glyphY);
        ink = glyphX < 5u && ((rowBits >> (4u - glyphX)) & 1u) != 0u;
    }

    return float4(packedRgb(ink ? cell.g : cell.b), 1.0);
}
