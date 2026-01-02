// asciiart.hlsl
// Compute Shader voor Radioify ASCII rendering

cbuffer Constants : register(b0) {
    uint width;        // Input image width
    uint height;       // Input image height
    uint outWidth;     // Output grid width
    uint outHeight;    // Output grid height
    float time;        // Voor eventuele animatie/noise (optioneel)
    uint frameCount;   // Voor temporal stability (optioneel)
    uint2 padding;
};

Texture2D<float4> InputTexture : register(t0);
SamplerState LinearSampler : register(s0);

struct AsciiCell {
    uint ch;        // wchar_t is 16-bit, maar we gebruiken 32-bit voor alignment
    uint fg;        // Packed RGBA (A is ignored/padding)
    uint bg;        // Packed RGBA
    uint hasBg;     // bool als uint
};

RWStructuredBuffer<AsciiCell> OutputBuffer : register(u0);

// Braille base offset
static const uint kBrailleBase = 0x2800;

// Luminance coefficients (Rec. 709)
static const float3 kLumaCoeff = float3(0.2126f, 0.7152f, 0.0722f);

// Braille dot offsets (2x4 grid)
// 0 3
// 1 4
// 2 5
// 6 7  <-- Braille dot numbering is weird
static const uint kDotMap[8] = { 0, 1, 2, 6, 3, 4, 5, 7 };

// Helper om kleur te packen
uint PackColor(float3 c) {
    uint r = (uint)(saturate(c.r) * 255.0f);
    uint g = (uint)(saturate(c.g) * 255.0f);
    uint b = (uint)(saturate(c.b) * 255.0f);
    return (r) | (g << 8) | (b << 16) | 0xFF000000;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= outWidth || DTid.y >= outHeight)
        return;

    // Bereken de cel-index
    uint cellIndex = DTid.y * outWidth + DTid.x;

    // Input coördinaten mapping
    // We mappen 1 ASCII cel naar een blokje pixels in de input
    // Braille is 2 dots breed, 4 dots hoog.
    
    float cellW = (float)width / (float)outWidth;
    float cellH = (float)height / (float)outHeight;
    
    // Dot grootte binnen de cel
    float dotW = cellW / 2.0f;
    float dotH = cellH / 4.0f;

    float3 sumInk = float3(0, 0, 0);
    float3 sumBg = float3(0, 0, 0);
    int inkCount = 0;
    int bgCount = 0;
    uint bitmask = 0;

    // Loop door de 8 dots van het braille karakter
    for (int i = 0; i < 8; ++i) {
        // Braille grid positie (0..1, 0..3)
        // Mapping: 0->(0,0), 1->(0,1), 2->(0,2), 3->(1,0), 4->(1,1), 5->(1,2), 6->(0,3), 7->(1,3)
        // Wacht, de standaard braille nummering is anders dan XY.
        // Laten we XY (col, row) gebruiken:
        // (0,0) (1,0)
        // (0,1) (1,1)
        // (0,2) (1,2)
        // (0,3) (1,3)
        
        int col = i / 4; // 0 of 1
        int row = i % 4; // 0..3
        
        // Sample positie: midden van de dot
        float u = (DTid.x * cellW + col * dotW + dotW * 0.5f) / width;
        float v = (DTid.y * cellH + row * dotH + dotH * 0.5f) / height;

        float4 color = InputTexture.SampleLevel(LinearSampler, float2(u, v), 0);
        float luma = dot(color.rgb, kLumaCoeff);

        // Simpele threshold logic (kan uitgebreid worden met dithering)
        // Hier gebruiken we een vaste threshold voor eenvoud, 
        // maar je kunt hier noise aan toevoegen voor dithering.
        float threshold = 0.3f; // Instelbaar
        
        // Braille bit index mapping
        // 0=(0,0), 1=(0,1), 2=(0,2), 3=(1,0), 4=(1,1), 5=(1,2), 6=(0,3), 7=(1,3)
        // De braille unicode bits zijn:
        // Bit 0: (0,0)
        // Bit 1: (0,1)
        // Bit 2: (0,2)
        // Bit 3: (1,0)
        // Bit 4: (1,1)
        // Bit 5: (1,2)
        // Bit 6: (0,3)
        // Bit 7: (1,3)
        
        int bitIndex = -1;
        if (col == 0) {
            if (row == 0) bitIndex = 0;
            else if (row == 1) bitIndex = 1;
            else if (row == 2) bitIndex = 2;
            else if (row == 3) bitIndex = 6;
        } else {
            if (row == 0) bitIndex = 3;
            else if (row == 1) bitIndex = 4;
            else if (row == 2) bitIndex = 5;
            else if (row == 3) bitIndex = 7;
        }

        if (luma > threshold) {
            bitmask |= (1 << bitIndex);
            sumInk += color.rgb;
            inkCount++;
        } else {
            sumBg += color.rgb;
            bgCount++;
        }
    }

    // Kleur berekening
    float3 finalFg = (inkCount > 0) ? (sumInk / inkCount) : float3(1, 1, 1); // Fallback wit
    float3 finalBg = (bgCount > 0) ? (sumBg / bgCount) : float3(0, 0, 0);

    // Color lift (beetje boosten van donkere kleuren)
    finalFg = max(finalFg, 0.1f);

    // Output schrijven
    AsciiCell cell;
    cell.ch = kBrailleBase + bitmask;
    cell.fg = PackColor(finalFg);
    cell.bg = PackColor(finalBg);
    cell.hasBg = (bgCount > 0); // Of logic based on user pref

    OutputBuffer[cellIndex] = cell;
}
