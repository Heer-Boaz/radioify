// Overlay-only rendering shader - updates UI without rendering video frame
// Used for progress bar and UI updates during seeking/pausing

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
    uint uiPad0;
    float uiTextTop;
    float uiTextHeight;
    float uiTextLeft;
    float uiTextWidth;
};

PS_INPUT VS(uint vid : SV_VertexID) {
    PS_INPUT output;
    output.tex = float2(vid & 1, vid >> 1);
    output.pos = float4(output.tex * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

Texture2D texText : register(t3);
SamplerState sam : register(s0);

float4 PS_UI(PS_INPUT input) : SV_Target {
    if (uiAlpha <= 0.01) discard;
    float2 uv = input.tex;
    float4 color = float4(0, 0, 0, 0);
    bool hit = false;

    // Progress bar at bottom (only UI element from console) - thinner
    if (uv.y > 0.96 && uv.y < 0.985 && uv.x > 0.02 && uv.x < 0.98) {
        float barX = (uv.x - 0.02) / 0.96;
        if (barX < uiProgress) color = float4(1, 0.8, 0.2, 0.9);
        else color = float4(0.3, 0.3, 0.3, 0.7);
        hit = true;
    }

    // Central pause icon (draw two small vertical bars)
    if (uiPaused != 0) {
        float2 c = float2(0.5, 0.5);
        float2 d = abs(uv - c);
        // Two vertical bars centered horizontally
        if (d.y < 0.06) {
            // left bar
            if (uv.x > 0.48 && uv.x < 0.495) { color = float4(1,1,1,0.95); hit = true; }
            // right bar
            if (uv.x > 0.505 && uv.x < 0.52) { color = float4(1,1,1,0.95); hit = true; }
        }
    }

    // Text overlay sampled from a CPU-generated texture (t3)
    if (uiTextHeight > 0.0 && uv.y >= uiTextTop && uv.y <= (uiTextTop + uiTextHeight) && uv.x >= uiTextLeft && uv.x <= (uiTextLeft + uiTextWidth)) {
        float localY = (uv.y - uiTextTop) / uiTextHeight;
        float localX = (uv.x - uiTextLeft) / uiTextWidth;
        float2 textUV = float2(localX, localY);
        float4 t = texText.Sample(sam, textUV);
        if (t.a > 0.01) {
            color = t;
            hit = true;
        }
    }

    if (!hit) discard;
    return color * uiAlpha;
}
