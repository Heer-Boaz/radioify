#include "videowindow.h"
#include "timing_log.h"
#include "gpu_shared.h"
#include <dxgi1_3.h>
#include <iostream>
#include <vector>
#include <mutex>
#include <string>
#include <cmath>
#include <cstdio>

#include <chrono>
#include <thread>
#include <sstream>

#include "videowindow_vs.h"
#include "videowindow_ps.h"
#include "videowindow_ps_ui.h"

static inline std::string now_ms() {
    using namespace std::chrono;
    auto t = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    std::ostringstream ss; ss << t; return ss.str();
}
static inline std::string thread_id_str() {
    std::ostringstream ss; ss << std::this_thread::get_id(); return ss.str();
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {
    struct ShaderConstants {
        uint32_t isFullRange;
        uint32_t yuvMatrix;
        uint32_t yuvTransfer;
        uint32_t bitDepth;

        float progress;
        float overlayAlpha;
        uint32_t isPaused;
        uint32_t hasRGBA;

        uint32_t volPct;
        uint32_t pad0;
        float textTop;
        float textHeight;

        float textLeft;
        float textWidth;
        uint32_t pad1;
        uint32_t pad2;
    };

    static_assert((sizeof(ShaderConstants) % 16) == 0, "ShaderConstants size must be 16-byte aligned");

    static std::string formatTimeDouble(double s) {
        if (!(s >= 0.0) || !std::isfinite(s)) return std::string("--:--");
        int total = static_cast<int>(std::llround(s));
        int h = total / 3600;
        int m = (total % 3600) / 60;
        int sec = total % 60;
        char buf[64];
        if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
        else std::snprintf(buf, sizeof(buf), "%02d:%02d", m, sec);
        return std::string(buf);
    }

    // Glyph font for ASCII-style overlay (5x7) -----------------------------------------------------------------
    typedef struct MenuGlyph { char c; uint8_t rows[7]; } MenuGlyph;

    static const MenuGlyph kMenuGlyphs[] = {
        {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
        {'+', {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
        {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
        {'.', {0x00,0x00,0x00,0x00,0x00,0x06,0x06}},
        {'/', {0x01,0x02,0x04,0x08,0x10,0x00,0x00}},
        {':', {0x00,0x04,0x04,0x00,0x04,0x04,0x00}},
        {'?', {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}},
        {'0', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
        {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
        {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
        {'3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
        {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
        {'5', {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}},
        {'6', {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}},
        {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
        {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
        {'9', {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}},
        {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
        {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
        {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
        {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
        {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
        {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
        {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
        {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
        {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
        {'J', {0x07,0x02,0x02,0x02,0x12,0x12,0x0C}},
        {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
        {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
        {'M', {0x11,0x1B,0x15,0x11,0x11,0x11,0x11}},
        {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
        {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
        {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
        {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
        {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
        {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
        {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
        {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
        {'V', {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04}},
        {'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
        {'X', {0x11,0x0A,0x04,0x04,0x04,0x0A,0x11}},
        {'Y', {0x11,0x0A,0x04,0x04,0x04,0x04,0x04}},
        {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
        {'(', {0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
        {')', {0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
        {'_', {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}},
    };

    static const uint8_t* menu_glyph_rows(char c) {
        static const uint8_t k_unknown[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
        for (size_t i = 0; i < sizeof(kMenuGlyphs) / sizeof(kMenuGlyphs[0]); ++i) {
            if (kMenuGlyphs[i].c == c) {
                return kMenuGlyphs[i].rows;
            }
        }
        return k_unknown;
    }

    // Bitmap-font text rasterizer (5x7 glyphs) into BGRA pixels
    static bool renderTextToBitmap(const std::string& utf8, int width, int height, std::vector<uint8_t>& outPixels) {
        outPixels.clear();
        if (width <= 0 || height <= 0) return false;
        // Uppercase the input (font table is uppercase + digits/punct)
        std::string text;
        for (unsigned char ch : utf8) {
            if (ch >= 'a' && ch <= 'z') text.push_back(static_cast<char>(ch - 'a' + 'A'));
            else text.push_back(ch);
        }
        const int glyphW = 5;
        const int glyphH = 7;
        const int spacing = 1;
        int charCount = static_cast<int>(text.size());
        // Compute scale constrained by both available height and width so glyphs keep correct proportions
        int maxScaleVert = std::max(1, height / glyphH);
        int maxScaleHoriz = std::max(1, width / std::max(1, charCount * (glyphW + spacing)));
        // cap scale based on window size so it can grow on fullscreen but not unbounded
        int maxCap = std::max(3, height / 80);
        int scale = std::min({maxScaleVert, maxScaleHoriz, maxCap});
        if (scale < 1) scale = 1;
        int textPxH = glyphH * scale;
        // Create BGRA buffer, clear to transparent
        outPixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
        int totalTextWidth = charCount * (glyphW + spacing) * scale;
        int startX = std::max(0, (width - totalTextWidth) / 2);
        int yOff = std::max(0, (height - textPxH) / 2);
        for (int ci = 0; ci < charCount; ++ci) {
            char c = text[ci];
            const uint8_t* rows = menu_glyph_rows(c);
            int baseX = startX + ci * (glyphW + spacing) * scale;
            for (int gy = 0; gy < glyphH; ++gy) {
                uint8_t row = rows[gy];
                for (int gx = 0; gx < glyphW; ++gx) {
                    bool set = ((row >> (glyphW - 1 - gx)) & 0x1) != 0;
                    if (!set) continue;
                    // scale pixel block
                    for (int sy = 0; sy < scale; ++sy) {
                        int py = yOff + gy * scale + sy;
                        if (py < 0 || py >= height) continue;
                        for (int sx = 0; sx < scale; ++sx) {
                            int px = baseX + gx * scale + sx;
                            if (px < 0 || px >= width) continue;
                            size_t idx = (static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)) * 4;
                            // white color, fully opaque (BGRA)
                            outPixels[idx + 0] = 255;
                            outPixels[idx + 1] = 255;
                            outPixels[idx + 2] = 255;
                            outPixels[idx + 3] = 255;
                        }
                    }
                }
            }
        }
        return true;
    }

    #if 0
    // Runtime shader sources retained for reference; build uses precompiled blobs.
    // Video frame rendering shader (combined from videowindow_render.hlsl)
    const char* g_shaderSource = R"(
// Video frame rendering shader - handles YUV/RGBA to RGB conversion with color correction
// Used for main video frame rendering with integrated overlay elements

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

Texture2D texY : register(t0);
Texture2D texUV : register(t1);
Texture2D texRGBA : register(t2);
Texture2D texText : register(t3);
SamplerState sam : register(s0);

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
    return pow(max(vp - c1, 0.0) / (c2 - c3 * vp), 1.0 / m1);
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

float4 PS(PS_INPUT input) : SV_Target {
    if (uiHasRGBA != 0) {
        float4 c = texRGBA.Sample(sam, input.tex);
        return float4(saturate(c.rgb), 1.0);
    }

    float y = ExpandYNorm(texY.Sample(sam, input.tex).r);
    float2 uv = ExpandUV(texUV.Sample(sam, input.tex).rg);
    
    float r, g, b;
    if (yuvMatrix == 2) { r = y + 1.4746 * uv.y; g = y - 0.16455 * uv.x - 0.57135 * uv.y; b = y + 1.8814 * uv.x; }
    else if (yuvMatrix == 1) { r = y + 1.4020 * uv.y; g = y - 0.3441 * uv.x - 0.7141 * uv.y; b = y + 1.7720 * uv.x; }
    else { r = y + 1.5748 * uv.y; g = y - 0.1873 * uv.x - 0.4681 * uv.y; b = y + 1.8556 * uv.x; }
    
    float3 rgb = float3(r, g, b);
    if (yuvTransfer != 0) rgb = ApplyHdrToSdr(rgb);
    return float4(saturate(rgb), 1.0);
}
    )";

    // Overlay-only rendering shader (from videowindow_overlay.hlsl)
    const char* g_overlayShaderSource = R"(
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
    )";
    #endif
}

VideoWindow::VideoWindow() {}

VideoWindow::~VideoWindow() {
    Close();
}

bool VideoWindow::MakeFullscreen() {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd || !m_swapChain) return false;
    // Save current style and rect
    m_prevStyle = GetWindowLong(m_hWnd, GWL_STYLE);
    GetWindowRect(m_hWnd, &m_prevRect);

    // Feature toggle: when false we DO NOT attempt any fallback (exclusive fullscreen)
    static constexpr bool kAllowFullscreenFallback = false; // TEMP: user requested no fallback

    // Prefer borderless fullscreen (WS_POPUP) as the only allowed method when fallback is disabled
    HMONITOR hm = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);

    // Save extended style so we can restore it on exit
    m_prevExStyle = GetWindowLong(m_hWnd, GWL_EXSTYLE);
    // diagnostic: entering fullscreen (log removed)

    if (GetMonitorInfo(hm, &mi)) {
        UINT monW = static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left);
        UINT monH = static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);

        // Diagnostic: print current swapchain desc before attempting ResizeBuffers
        {
            DXGI_SWAP_CHAIN_DESC desc = {};
            HRESULT g = m_swapChain->GetDesc(&desc);
            if (SUCCEEDED(g)) {
                // swapchain desc diagnostic removed
            } else {
                std::fprintf(stderr, "VideoWindow: GetDesc failed (0x%08X)\n", static_cast<unsigned int>(g));
            }
        }

        // Prepare window for borderless fullscreen first: set style/exstyle/position so the window is ready
        // Prevent WM_SIZE messages from racing and resetting our freshly-applied fullscreen size
        m_ignoreWindowSizeEvents = true;
        SetWindowLong(m_hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        LONG newEx = m_prevExStyle & ~(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
        LONG prevExSet = SetWindowLong(m_hWnd, GWL_EXSTYLE, newEx);
    // SetWindowLong diagnostic removed

        SetWindowPos(m_hWnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, monW, monH, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        ::ShowWindow(m_hWnd, SW_SHOW);
        // Force a layout/update before resizing buffers
        UpdateWindow(m_hWnd);

        // Prepare to recreate the swapchain at monitor resolution.
        // Unbind and clear context to release any references to the swapchain buffers.
        {
            ID3D11Device* dev = getSharedGpuDevice();
            if (dev) {
                ID3D11DeviceContext* ctx = nullptr;
                dev->GetImmediateContext(&ctx);
                if (ctx) {
                    ID3D11ShaderResourceView* nullSRVs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
                    ctx->PSSetShaderResources(0, 5, nullSRVs);
                    ID3D11RenderTargetView* nullRTV = nullptr;
                    ctx->OMSetRenderTargets(0, &nullRTV, nullptr);
                    ctx->VSSetShader(nullptr, nullptr, 0);
                    ctx->PSSetShader(nullptr, nullptr, 0);
                    ctx->ClearState();
                    ctx->Flush();
                    ctx->Release();
                    // Unbound RTVs/SRVs diagnostic removed
                }
            }
            if (m_renderTargetView) { m_renderTargetView.Reset(); }
            ResetSwapChain();
        }

        // Recreate the swapchain sized to the monitor (borderless windowed fullscreen)
        // Indicate that Present must wait for the new RTV/backbuffer to be ready
        m_waitingForRenderTarget = true;
        if (!CreateSwapChain(static_cast<int>(monW), static_cast<int>(monH))) {
            std::fprintf(stderr, "VideoWindow: CreateSwapChain(borderless) failed\n");
            if (!kAllowFullscreenFallback) {
                std::fprintf(stderr, "VideoWindow: fullscreen fallback disabled; aborting (create swapchain failed)\n");
                m_ignoreWindowSizeEvents = false;
                return false;
            }
        } else {
            // Success: ensure focus and topmost ordering
            SetForegroundWindow(m_hWnd);
            SetActiveWindow(m_hWnd);
            SetFocus(m_hWnd);
            BringWindowToTop(m_hWnd);
            // Defensive: explicitly call Resize to ensure internal size/state is updated
            Resize(static_cast<int>(monW), static_cast<int>(monH));
            // after CreateSwapChain diagnostic removed
            DWORD err = GetLastError();
            // entered borderless fullscreen (diagnostic removed)
            m_isFullscreen = true;
            // Allow WM_SIZE processing again now that we've updated internal state
            m_ignoreWindowSizeEvents = false;
            return true;
        }
    }

    // If we reach here and fallback is disabled, do not try other methods
    if (!kAllowFullscreenFallback) {
        std::fprintf(stderr, "VideoWindow: borderless fullscreen could not be started and fallback is disabled\n");
        m_ignoreWindowSizeEvents = false;
        return false;
    }

    // Fallback (disabled by default): try exclusive fullscreen as last resort
    HRESULT hr = m_swapChain->SetFullscreenState(TRUE, NULL);
    if (SUCCEEDED(hr)) {
        // exclusive fullscreen success message removed
        // Attempt to resize buffers to monitor resolution
        if (GetMonitorInfo(hm, &mi)) {
            UINT monW = static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left);
            UINT monH = static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);
            HRESULT r3 = m_swapChain->ResizeBuffers(0, monW, monH, DXGI_FORMAT_UNKNOWN, 0);
            if (SUCCEEDED(r3)) {
                Resize(static_cast<int>(monW), static_cast<int>(monH));
                m_isFullscreen = true;
                // exclusive fullscreen buffers resized message removed
                return true;
            }
            std::fprintf(stderr, "VideoWindow: ResizeBuffers(exclusive) failed (0x%08X)\n", static_cast<unsigned int>(r3));
        }
    } else {
        std::fprintf(stderr, "VideoWindow: SetFullscreenState(TRUE) failed (0x%08X)\n", static_cast<unsigned int>(hr));
    }

    std::fprintf(stderr, "VideoWindow: all fullscreen methods failed\n");
    return false;
}

bool VideoWindow::ExitFullscreen() {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd || !m_swapChain) return false;
    HRESULT hr = m_swapChain->SetFullscreenState(FALSE, NULL);
    if (FAILED(hr)) {
        std::fprintf(stderr, "VideoWindow: SetFullscreenState(FALSE) failed (0x%08X)\n", static_cast<unsigned int>(hr));
        // even if DXGI failed, try to restore window style
    }
    // Restore style and position
    SetWindowLong(m_hWnd, GWL_STYLE, m_prevStyle);
    // Restore extended style as well
    SetWindowLong(m_hWnd, GWL_EXSTYLE, m_prevExStyle);

    // Remove topmost if we set it earlier and restore z-order
    SetWindowPos(m_hWnd, HWND_NOTOPMOST, m_prevRect.left, m_prevRect.top, m_prevRect.right - m_prevRect.left, m_prevRect.bottom - m_prevRect.top, SWP_NOZORDER | SWP_FRAMECHANGED);

    // Resize back to previous logical size
    Resize(m_prevRect.right - m_prevRect.left, m_prevRect.bottom - m_prevRect.top);

    // Restore keyboard focus so shortcuts keep working
    ::ShowWindow(m_hWnd, SW_RESTORE);
    SetForegroundWindow(m_hWnd);
    SetActiveWindow(m_hWnd);
    SetFocus(m_hWnd);

    m_isFullscreen = false;
    // exited fullscreen message removed
    return true;
}

void VideoWindow::Cleanup() {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) return;

    // Unbind shader resources / UAVs / RTVs and clear state to avoid driver pinning
    ID3D11ShaderResourceView* nullSRVs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    context->PSSetShaderResources(0, 5, nullSRVs);
    context->CSSetShaderResources(0, 5, nullSRVs);

    ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
    context->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);

    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(0, &nullRTV, nullptr);

    context->VSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);

    // Force clear/flush to ensure driver releases any references
    context->ClearState();
    context->Flush();
    context->Release();

    // Reset COM objects we hold
    m_renderTargetView.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    m_uiShader.Reset();
    m_sampler.Reset();
    m_constantBuffer.Reset();
    // frame cache is now owned/managed externally
    m_textTexture.Reset();
    m_textSrv.Reset();
}

LRESULT CALLBACK VideoWindow::WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    VideoWindow* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
        pThis = (VideoWindow*)pCreate->lpCreateParams;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)pThis);
    } else {
        pThis = (VideoWindow*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    }
    // If we're in a fullscreen transition we may get WM_SIZE messages from the system
    // that reflect the old client size; ignore WM_SIZE while m_ignoreWindowSizeEvents
    if (pThis && uMsg == WM_SIZE && pThis->m_ignoreWindowSizeEvents) {
        return 0;
    }

    if (pThis) {
        if (uMsg == WM_SIZE) {
            pThis->Resize(LOWORD(lParam), HIWORD(lParam));
            return 0;
        }
        if (uMsg == WM_CLOSE) {
            ::ShowWindow(pThis->m_hWnd, SW_HIDE);
            return 0;
        }
        if (uMsg == WM_DESTROY) {
            pThis->m_hWnd = nullptr;
            return 0;
        }

        if (uMsg == WM_KEYDOWN) {
            InputEvent ev;
            ev.type = InputEvent::Type::Key;
            ev.key.vk = (WORD)wParam;
            
            // Map VK to ASCII for common keys if possible
            if (wParam >= 'A' && wParam <= 'Z') ev.key.ch = (char)wParam;
            else if (wParam == VK_SPACE) ev.key.ch = ' ';
            else if (wParam == VK_ESCAPE) ev.key.ch = 27;
            else if (wParam == VK_OEM_4) ev.key.ch = '[';
            else if (wParam == VK_OEM_6) ev.key.ch = ']';

            if (GetKeyState(VK_CONTROL) & 0x8000) ev.key.control |= LEFT_CTRL_PRESSED;
            if (GetKeyState(VK_SHIFT) & 0x8000) ev.key.control |= SHIFT_PRESSED;
            if (GetKeyState(VK_MENU) & 0x8000) ev.key.control |= LEFT_ALT_PRESSED;

            std::lock_guard<std::mutex> lock(pThis->m_inputMutex);
            pThis->m_inputQueue.push_back(ev);
            return 0;
        }

        if (uMsg == WM_LBUTTONDOWN) {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            // Mouse seeking in bottom area (like terminal)
            if (pThis->m_height > 0 && y > static_cast<int>(pThis->m_height * 0.90)) {
                InputEvent ev;
                ev.type = InputEvent::Type::Mouse;
                ev.mouse.pos.X = (SHORT)x;
                ev.mouse.pos.Y = (SHORT)y;
                ev.mouse.buttonState = FROM_LEFT_1ST_BUTTON_PRESSED;
                ev.mouse.eventFlags = 0;
                ev.mouse.control = 0x80000000; // Custom flag for window-originated event
                std::lock_guard<std::mutex> lock(pThis->m_inputMutex);
                pThis->m_inputQueue.push_back(ev);
            }
            return 0;
        }

        if (uMsg == WM_MOUSEMOVE) {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            bool leftPressed = (wParam & MK_LBUTTON) != 0;
            // Enqueue hover/move events when mouse is over the bottom area so the UI overlay can be triggered
            if (pThis->m_height > 0 && y > static_cast<int>(pThis->m_height * 0.90)) {
                InputEvent ev;
                ev.type = InputEvent::Type::Mouse;
                ev.mouse.pos.X = (SHORT)x;
                ev.mouse.pos.Y = (SHORT)y;
                ev.mouse.buttonState = leftPressed ? FROM_LEFT_1ST_BUTTON_PRESSED : 0;
                ev.mouse.eventFlags = MOUSE_MOVED;
                ev.mouse.control = 0x80000000; // Custom flag for window-originated event
                std::lock_guard<std::mutex> lock(pThis->m_inputMutex);
                pThis->m_inputQueue.push_back(ev);
            }
            return 0;
        }
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

bool VideoWindow::Open(int width, int height, const std::string& title) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    HINSTANCE hInstance = GetModuleHandle(NULL);
    const wchar_t* className = L"RadioifyVideoWindow";

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    if (!GetClassInfoExW(hInstance, className, &wc)) {
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassExW(&wc);
    }

    RECT wr = { 0, 0, width, height };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    m_hWnd = CreateWindowExW(
        0, className, std::wstring(title.begin(), title.end()).c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, hInstance, this);

    if (!m_hWnd) return false;

    // Remember base window title so we can temporarily update it while overlay is visible
    m_lastWindowTitle = title;
    m_baseWindowTitle = title;

    if (!CreateSwapChain(width, height)) {
        ::DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
        return false;
    }

    // Attempt to enter fullscreen immediately after creating swapchain
    // This is best-effort (may fail on some systems) but improves UX for video playback
    // Call MakeFullscreen only when Open is invoked for a video window; allow callers to override
    MakeFullscreen();

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) {
        std::fprintf(stderr, "VideoWindow: no device in Open()\n");
        Close();
        if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; }
        return false;
    }

    // Ensure multithread protection if available (ascii renderer enables this)
    {
        Microsoft::WRL::ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
            mt->SetMultithreadProtected(TRUE);
        }
    }

    HRESULT hr = device->CreateVertexShader(kVideoWindowVs, kVideoWindowVs_Size, NULL, &m_vertexShader);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreateVertexShader failed (0x%08X)\n", static_cast<unsigned int>(hr)); Close(); if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; } return false; }
    hr = device->CreatePixelShader(kVideoWindowPs, kVideoWindowPs_Size, NULL, &m_pixelShader);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreatePixelShader(PS) failed (0x%08X)\n", static_cast<unsigned int>(hr)); Close(); if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; } return false; }
    hr = device->CreatePixelShader(kVideoWindowPsUi, kVideoWindowPsUi_Size, NULL, &m_uiShader);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreatePixelShader(UI) failed (0x%08X)\n", static_cast<unsigned int>(hr)); Close(); if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; } return false; }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(ShaderConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbDesc, NULL, &m_constantBuffer);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreateBuffer(constant) failed (0x%08X)\n", static_cast<unsigned int>(hr)); Close(); if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; } return false; }

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampDesc, &m_sampler);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreateSamplerState failed (0x%08X)\n", static_cast<unsigned int>(hr)); Close(); if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; } return false; }

    ::ShowWindow(m_hWnd, SW_SHOW);
    m_width = width;
    m_height = height;

    return true;
}

void VideoWindow::ResetSwapChain() {
    std::lock_guard<std::mutex> lock(m_frameLatencyMutex);
    if (m_frameLatencyWaitableObject) {
        CloseHandle(m_frameLatencyWaitableObject);
        m_frameLatencyWaitableObject = nullptr;
    }
    m_swapChain2.Reset();
    m_swapChain.Reset();
}

bool VideoWindow::CreateSwapChain(int width, int height) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    ID3D11Device* device = getSharedGpuDevice();
    if (!device) {
        std::fprintf(stderr, "VideoWindow: no shared GPU device available\n");
        return false;
    }
    if (width <= 0 || height <= 0) {
        std::fprintf(stderr, "VideoWindow: invalid swapchain dimensions %d x %d\n", width, height);
        return false;
    }

    ResetSwapChain();
    
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    
    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    dxgiDevice->GetAdapter(&dxgiAdapter);

    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory2;
    HRESULT factoryHr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory2));
    if (SUCCEEDED(factoryHr) && dxgiFactory2) {
        DXGI_SWAP_CHAIN_DESC1 scd1 = {};
        scd1.Width = static_cast<UINT>(width);
        scd1.Height = static_cast<UINT>(height);
        scd1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd1.SampleDesc.Count = 1;
        scd1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd1.BufferCount = 2;
        scd1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        scd1.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT hr = dxgiFactory2->CreateSwapChainForHwnd(
            device, m_hWnd, &scd1, nullptr, nullptr, &swapChain1);
        if (SUCCEEDED(hr)) {
            m_swapChain = swapChain1;
            swapChain1.As(&m_swapChain2);
            if (m_swapChain2) {
                m_swapChain2->SetMaximumFrameLatency(1);
                std::lock_guard<std::mutex> latencyLock(m_frameLatencyMutex);
                m_frameLatencyWaitableObject =
                    m_swapChain2->GetFrameLatencyWaitableObject();
            }
            Resize(width, height);
            return true;
        }
    }

    Microsoft::WRL::ComPtr<IDXGIFactory> dxgiFactory;
    if (FAILED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory))) || !dxgiFactory) {
        std::fprintf(stderr, "VideoWindow: failed to get DXGI factory\n");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = m_hWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    if (FAILED(dxgiFactory->CreateSwapChain(device, &scd, &m_swapChain))) {
        std::fprintf(stderr, "VideoWindow: CreateSwapChain failed\n");
        return false;
    }

    Resize(width, height);
    return true;
}

void VideoWindow::Resize(int width, int height) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_swapChain) return;
    m_renderTargetView.Reset();
    UINT flags = 0;
    if (m_swapChain2) {
        flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }
    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags);
    if (FAILED(hr)) return;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) return;

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;
    hr = device->CreateRenderTargetView(backBuffer.Get(), NULL, &m_renderTargetView);
    if (FAILED(hr)) {
        m_renderTargetView.Reset();
        return;
    }

    m_width = width;
    m_height = height;
    // Mark that the render target is ready so Present can proceed
    m_waitingForRenderTarget = false;
}

void VideoWindow::Close() {
    // Hide the window first to release focus/ownership of the monitor
    if (m_hWnd) {
        ::ShowWindow(m_hWnd, SW_HIDE);
    }
    // Perform centralized cleanup (unbind, ClearState, flush, reset local resources)
    Cleanup();

    // If swapchain is in full-screen exclusive, revert to windowed first and restore style
    if (m_swapChain && m_isFullscreen) {
        (void)m_swapChain->SetFullscreenState(FALSE, NULL);
    }
    // Restore windowed style if we changed it
    if (m_hWnd && m_isFullscreen) {
        SetWindowLong(m_hWnd, GWL_STYLE, m_prevStyle);
        SetWindowPos(m_hWnd, NULL, m_prevRect.left, m_prevRect.top, m_prevRect.right - m_prevRect.left, m_prevRect.bottom - m_prevRect.top, SWP_NOZORDER | SWP_FRAMECHANGED);
        m_isFullscreen = false;
    }

    // Release swapchain last
    ResetSwapChain();

    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
}

void VideoWindow::ShowWindow(bool show) {
    if (m_hWnd) {
        ::ShowWindow(m_hWnd, show ? SW_SHOW : SW_HIDE);
    }
}

void VideoWindow::PollEvents() {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

bool VideoWindow::PollInput(InputEvent& ev) {
    std::lock_guard<std::mutex> lock(m_inputMutex);
    if (m_inputQueue.empty()) return false;
    ev = m_inputQueue.front();
    m_inputQueue.erase(m_inputQueue.begin());
    return true;
}

void VideoWindow::SetVsync(bool enabled) {
    m_presentInterval.store(enabled ? 1u : 0u, std::memory_order_relaxed);
}

void VideoWindow::WaitForFrameLatency(DWORD timeoutMs) {
    HANDLE waitHandle = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_frameLatencyMutex);
        waitHandle = m_frameLatencyWaitableObject;
    }
    if (!waitHandle) return;
    WaitForSingleObject(waitHandle, timeoutMs);
}

void VideoWindow::UpdateViewport(int width, int height) {
    VideoViewport vp = calculateViewport(width, height, m_videoWidth, m_videoHeight);
    m_viewportX = vp.x;
    m_viewportY = vp.y;
    m_viewportW = vp.w;
    m_viewportH = vp.h;
}

void VideoWindow::Present(GpuVideoFrameCache& frameCache, const WindowUiState& ui) {
    std::unique_lock<std::recursive_mutex> lock(getSharedGpuMutex());
#if RADIOIFY_ENABLE_TIMING_LOG
    fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present enter (wnd=%p swap=%p visible=%d)\n", now_ms().c_str(), thread_id_str().c_str(), (void*)m_hWnd, (void*)m_swapChain.Get(), m_hWnd ? IsWindowVisible(m_hWnd) : 0);
#endif
    if (!m_hWnd || !m_swapChain || !IsWindowVisible(m_hWnd)) {
#if RADIOIFY_ENABLE_TIMING_LOG
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present early exit: window/swap not ready\n", now_ms().c_str(), thread_id_str().c_str());
#endif
        return;
    }
    if (!frameCache.HasFrame()) {
#if RADIOIFY_ENABLE_TIMING_LOG
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present early exit: no frame in cache\n", now_ms().c_str(), thread_id_str().c_str());
#endif
        return;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain = m_swapChain;
    UINT presentInterval = m_presentInterval.load(std::memory_order_relaxed);

    // Refresh window dimensions to avoid stale size during fullscreen transitions
    RECT rect;
    if (GetClientRect(m_hWnd, &rect)) {
        m_width = rect.right - rect.left;
        m_height = rect.bottom - rect.top;
    }

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context || !m_constantBuffer || !m_renderTargetView) return;

    m_videoWidth = frameCache.GetWidth();
    m_videoHeight = frameCache.GetHeight();

#if RADIOIFY_ENABLE_TIMING_LOG
    fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present frame w=%d h=%d ui.displaySec=%.3f\n", now_ms().c_str(), thread_id_str().c_str(), m_videoWidth, m_videoHeight, ui.displaySec);
#endif

    // Render
    float clearColor[4] = { 0, 0, 0, 1 };
    context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
    context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), NULL);

    UpdateViewport(m_width, m_height);
    D3D11_VIEWPORT viewport = { m_viewportX, m_viewportY, m_viewportW, m_viewportH, 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);

    context->IASetInputLayout(NULL);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(m_vertexShader.Get(), NULL, 0);
    context->PSSetShader(m_pixelShader.Get(), NULL, 0);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            ShaderConstants sc{};
            sc.isFullRange = frameCache.IsFullRange() ? 1 : 0;
            sc.yuvMatrix = (uint32_t)frameCache.GetMatrix();
            sc.yuvTransfer = (uint32_t)frameCache.GetTransfer();
            sc.bitDepth = (uint32_t)frameCache.GetBitDepth();
            sc.hasRGBA = frameCache.IsRgba() ? 1u : 0u;
            std::memcpy(mapped.pData, &sc, sizeof(ShaderConstants));
            context->Unmap(m_constantBuffer.Get(), 0);
        }
    }

    if (frameCache.IsRgba()) {
        ID3D11ShaderResourceView* srvs[3] = { nullptr, nullptr, frameCache.GetSrvRGBA() };
        context->PSSetShaderResources(0, 3, srvs);
    } else {
        ID3D11ShaderResourceView* srvs[2] = { frameCache.GetSrvY(), frameCache.GetSrvUV() };
        context->PSSetShaderResources(0, 2, srvs);
    }

#if defined(RADIOIFY_ENABLE_GPU_TIMING)
    {
        Microsoft::WRL::ComPtr<ID3D11Query> qDisjoint, qStart, qEnd;
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ID3D11Device* device = getSharedGpuDevice();
        if (device) {
            device->CreateQuery(&qd, qDisjoint.GetAddressOf());
            qd.Query = D3D11_QUERY_TIMESTAMP;
            device->CreateQuery(&qd, qStart.GetAddressOf());
            device->CreateQuery(&qd, qEnd.GetAddressOf());

            context->Begin(qDisjoint.Get());
            context->End(qStart.Get());
            context->Draw(4, 0);
            DrawOverlay(ui);
            context->End(qEnd.Get());
            context->End(qDisjoint.Get());

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
            while (context->GetData(qDisjoint.Get(), &disjoint, sizeof(disjoint), 0) == S_FALSE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            UINT64 t1 = 0, t2 = 0;
            context->GetData(qStart.Get(), &t1, sizeof(t1), 0);
            context->GetData(qEnd.Get(), &t2, sizeof(t2), 0);
            double gpu_ms = 0.0;
            if (!disjoint.Disjoint) gpu_ms = (double)(t2 - t1) / (double)disjoint.Frequency * 1000.0;
            fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present GPU draw+overlay time %.3f ms\n", now_ms().c_str(), thread_id_str().c_str(), gpu_ms);
        } else {
            context->Draw(4, 0);
            DrawOverlay(ui);
        }
    }
#else
    context->Draw(4, 0);
    DrawOverlay(ui);
#endif

#if RADIOIFY_ENABLE_TIMING_LOG
    fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present about to Present()\n", now_ms().c_str(), thread_id_str().c_str());
#endif
    lock.unlock();
    if (!swapChain) return;
    HRESULT presHr = swapChain->Present(presentInterval, 0);
#if RADIOIFY_ENABLE_TIMING_LOG
    if (FAILED(presHr)) {
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present Present() FAILED 0x%08X\n", now_ms().c_str(), thread_id_str().c_str(), static_cast<unsigned int>(presHr));
    } else {
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present Present() OK\n", now_ms().c_str(), thread_id_str().c_str());
    }
#endif
}

void VideoWindow::DrawOverlay(const WindowUiState& ui) {
    if (ui.overlayAlpha <= 0.01f) return;

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context || !m_constantBuffer) return;
    
    context->PSSetShader(m_uiShader.Get(), NULL, 0);
    context->VSSetShader(m_vertexShader.Get(), NULL, 0);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
    
    float textHeightNorm = 0.0f;
    float textTopNorm = 0.0f;
    float textLeftNorm = 0.0f;
    float textWidthNorm = 0.0f;

    // Optional: Render text to texture
    if (!ui.title.empty()) {
        int textPxH = std::clamp((int)std::round(m_height * 0.06f), 14, 48);
        std::string timeLabel = ui.totalSec > 0.0
            ? (formatTimeDouble(ui.displaySec) + " / " + formatTimeDouble(ui.totalSec))
            : formatTimeDouble(ui.displaySec);
        std::string textLine = ui.title + "  " + timeLabel +
            (ui.vsyncEnabled ? "  VSync: On" : "  VSync: Off");
        
        // Very rough estimate of width
        int estScale = std::max(1, textPxH / 7);
        int totalTextWidth = (int)textLine.size() * 6 * estScale; 
        int textPxW = std::min(m_width, totalTextWidth);

        if (textPxW > 0 && textPxH > 0) {
            if (!m_textTexture || m_textWidth != textPxW || m_textHeight != textPxH) {
                m_textWidth = textPxW;
                m_textHeight = textPxH;
                m_textTexture.Reset();
                m_textSrv.Reset();
                
                D3D11_TEXTURE2D_DESC texDesc = {};
                texDesc.Width = textPxW;
                texDesc.Height = textPxH;
                texDesc.MipLevels = 1;
                texDesc.ArraySize = 1;
                texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                texDesc.SampleDesc.Count = 1;
                texDesc.Usage = D3D11_USAGE_DEFAULT;
                texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                
                if (SUCCEEDED(device->CreateTexture2D(&texDesc, NULL, &m_textTexture))) {
                    device->CreateShaderResourceView(m_textTexture.Get(), NULL, &m_textSrv);
                }
            }
            
            std::vector<uint8_t> bmp;
            if (m_textTexture && renderTextToBitmap(textLine, textPxW, textPxH, bmp)) {
                D3D11_BOX box{0, 0, 0, (UINT)textPxW, (UINT)textPxH, 1};
                context->UpdateSubresource(m_textTexture.Get(), 0, &box, bmp.data(), textPxW * 4, 0);
            }

            textHeightNorm = (float)textPxH / m_height;
            textTopNorm = 0.95f - textHeightNorm;
            textWidthNorm = (float)textPxW / m_width;
            textLeftNorm = (1.0f - textWidthNorm) * 0.5f;
        }
    }

    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            ShaderConstants sc{};
            sc.progress = ui.progress;
            sc.overlayAlpha = ui.overlayAlpha;
            sc.isPaused = ui.isPaused ? 1 : 0;
            sc.volPct = (uint32_t)std::clamp(ui.volPct, 0, 100);
            sc.textTop = textTopNorm;
            sc.textHeight = textHeightNorm;
            sc.textLeft = textLeftNorm;
            sc.textWidth = textWidthNorm;
            std::memcpy(mapped.pData, &sc, sizeof(ShaderConstants));
            context->Unmap(m_constantBuffer.Get(), 0);
        }
    }
    
    context->Draw(4, 0);
    
    if (m_textSrv) {
        context->PSSetShaderResources(3, 1, m_textSrv.GetAddressOf());
        context->Draw(4, 0);
    }

    ID3D11ShaderResourceView* nullSRVs[4] = { nullptr };
    context->PSSetShaderResources(0, 4, nullSRVs);
}

void VideoWindow::PresentOverlay(GpuVideoFrameCache& frameCache, const WindowUiState& ui) {
    std::unique_lock<std::recursive_mutex> lock(getSharedGpuMutex());
#if RADIOIFY_ENABLE_TIMING_LOG
    fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay enter (wnd=%p swap=%p visible=%d)\n", now_ms().c_str(), thread_id_str().c_str(), (void*)m_hWnd, (void*)m_swapChain.Get(), m_hWnd ? IsWindowVisible(m_hWnd) : 0);
#endif
    if (!m_hWnd || !m_swapChain || !IsWindowVisible(m_hWnd)) {
#if RADIOIFY_ENABLE_TIMING_LOG
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay early exit: window/swap not ready\n", now_ms().c_str(), thread_id_str().c_str());
#endif
        return;
    }
    if (!frameCache.HasFrame()) {
#if RADIOIFY_ENABLE_TIMING_LOG
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay early exit: no frame in cache\n", now_ms().c_str(), thread_id_str().c_str());
#endif
        return;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain = m_swapChain;
    UINT presentInterval = m_presentInterval.load(std::memory_order_relaxed);

    // Refresh window dimensions to avoid stale size during fullscreen transitions
    RECT rect;
    if (GetClientRect(m_hWnd, &rect)) {
        m_width = rect.right - rect.left;
        m_height = rect.bottom - rect.top;
    }

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context || !m_renderTargetView || !m_constantBuffer) return;

    float clearColor[4] = { 0, 0, 0, 1 };
    context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
    context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), NULL);

    UpdateViewport(m_width, m_height);
    D3D11_VIEWPORT viewport = { m_viewportX, m_viewportY, m_viewportW, m_viewportH, 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);

    context->IASetInputLayout(NULL);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(m_vertexShader.Get(), NULL, 0);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    // Video path
    context->PSSetShader(m_pixelShader.Get(), NULL, 0);
    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            ShaderConstants sc{};
            sc.isFullRange = frameCache.IsFullRange() ? 1 : 0;
            sc.yuvMatrix = (uint32_t)frameCache.GetMatrix();
            sc.yuvTransfer = (uint32_t)frameCache.GetTransfer();
            sc.bitDepth = (uint32_t)frameCache.GetBitDepth();
            sc.hasRGBA = frameCache.IsRgba() ? 1u : 0u;
            std::memcpy(mapped.pData, &sc, sizeof(ShaderConstants));
            context->Unmap(m_constantBuffer.Get(), 0);
        }
    }

    if (frameCache.IsRgba()) {
        ID3D11ShaderResourceView* srvs[3] = { nullptr, nullptr, frameCache.GetSrvRGBA() };
        context->PSSetShaderResources(0, 3, srvs);
    } else {
        ID3D11ShaderResourceView* srvs[2] = { frameCache.GetSrvY(), frameCache.GetSrvUV() };
        context->PSSetShaderResources(0, 2, srvs);
    }

    context->Draw(4, 0);
    DrawOverlay(ui);

#if RADIOIFY_ENABLE_TIMING_LOG
    fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay about to Present()\n", now_ms().c_str(), thread_id_str().c_str());
#endif
    lock.unlock();
    if (!swapChain) return;
    HRESULT presHr = swapChain->Present(presentInterval, 0);
#if RADIOIFY_ENABLE_TIMING_LOG
    if (FAILED(presHr)) {
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay Present() FAILED 0x%08X\n", now_ms().c_str(), thread_id_str().c_str(), static_cast<unsigned int>(presHr));
    } else {
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay Present() OK\n", now_ms().c_str(), thread_id_str().c_str());
    }
#endif
}

void VideoWindow::PresentBackbuffer() {
    std::unique_lock<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd || !m_swapChain || !IsWindowVisible(m_hWnd)) return;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain = m_swapChain;
    UINT presentInterval = m_presentInterval.load(std::memory_order_relaxed);
    lock.unlock();
    if (!swapChain) return;
    swapChain->Present(presentInterval, 0);
}
