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
    float pad3;
    float pad4;
    float pad5;
};

PS_INPUT VS(uint vid : SV_VertexID) {
    PS_INPUT output;
    output.tex = float2(vid & 1, vid >> 1);
    output.pos = float4(output.tex * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

Texture2D texText : register(t3);
Texture2D texSubtitle : register(t4);
SamplerState sam : register(s0);

float4 PS_UI(PS_INPUT input) : SV_Target {
    float2 uv = input.tex;
    float4 uiColor = float4(0, 0, 0, 0);
    bool uiHit = false;

    if (uiAlpha > 0.01) {
        // Progress bar at bottom (only UI element from console) - thinner
        if (uv.y > 0.96 && uv.y < 0.985 && uv.x > 0.02 && uv.x < 0.98) {
            float barX = (uv.x - 0.02) / 0.96;
            if (barX < uiProgress) uiColor = float4(1, 0.8, 0.2, 0.9);
            else uiColor = float4(0.3, 0.3, 0.3, 0.7);
            uiHit = true;
        }

        // Central pause icon (draw two small vertical bars)
        if (uiPaused != 0) {
            float2 c = float2(0.5, 0.5);
            float2 d = abs(uv - c);
            // Two vertical bars centered horizontally
            if (d.y < 0.06) {
                // left bar
                if (uv.x > 0.48 && uv.x < 0.495) { uiColor = float4(1,1,1,0.95); uiHit = true; }
                // right bar
                if (uv.x > 0.505 && uv.x < 0.52) { uiColor = float4(1,1,1,0.95); uiHit = true; }
            }
        }

        // Text overlay sampled from a CPU-generated texture (t3)
        if (uiTextHeight > 0.0 && uv.y >= uiTextTop && uv.y <= (uiTextTop + uiTextHeight) && uv.x >= uiTextLeft && uv.x <= (uiTextLeft + uiTextWidth)) {
            float localY = (uv.y - uiTextTop) / uiTextHeight;
            float localX = (uv.x - uiTextLeft) / uiTextWidth;
            float2 textUV = float2(localX, localY);
            float4 t = texText.Sample(sam, textUV);
            if (t.a > 0.01) {
                uiColor = t;
                uiHit = true;
            }
        }
        if (uiHit) uiColor.a *= uiAlpha;
    }

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
                subtitleColor.a *= subtitleAlpha;
                subtitleHit = true;
            }
        }
    }

    if (!uiHit && !subtitleHit) discard;

    float4 color = uiHit ? uiColor : subtitleColor;
    if (uiHit && subtitleHit) {
        // Source-over composition inside the overlay pass:
        // subtitle layer over UI layer, then GPU blend-state composites over video.
        color.rgb = lerp(uiColor.rgb, subtitleColor.rgb, subtitleColor.a);
        color.a = subtitleColor.a + uiColor.a * (1.0 - subtitleColor.a);
    }
    return color;
}
