#include "asciiart_gpu.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <iostream>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {
    // Embedded HLSL shader source
    const char* kComputeShaderSource = R"(
cbuffer Constants : register(b0) {
    uint width;
    uint height;
    uint outWidth;
    uint outHeight;
    float time;
    uint frameCount;
    uint bgLum;
    uint lumRange;
    uint isFullRange;
    uint padding[3];
};

#ifdef NV12_INPUT
Texture2D<float> TextureY : register(t0);
Texture2D<float2> TextureUV : register(t1);
#else
Texture2D<float4> InputTexture : register(t0);
#endif

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

struct AsciiCell {
    uint ch;
    uint fg;
    uint bg;
    uint hasBg;
};

RWStructuredBuffer<AsciiCell> OutputBuffer : register(u0);
RWStructuredBuffer<uint2> HistoryBuffer : register(u1); // x=Fg, y=Bg (packed)

static const uint kBrailleBase = 0x2800;
static const float3 kLumaCoeff = float3(0.2126f, 0.7152f, 0.0722f);

// Constants from CPU version
static const int kMinContrastForBraille = 15;
static const int kEdgeShift = 3;
static const int kColorLift = 0;
static const int kColorSaturation = 340;
static const int kTemporalResetDelta = 48;
static const int kColorAlpha = 220; 
static const int kBgAlpha = 180;
static const int kInkMinLuma = 110;
static const int kBgMinLuma = 20;
static const int kInkMaxScale = 1280;
static const int kEdgeMin = 4;
static const int kEdgeBoost = 245;
static const int kDitherBias = 48;
static const int kAAScoreBandMin = 12;
static const int kAAScoreBandMax = 144;

// Dithering thresholds (optimized for 2x4 braille)
// Ranks: 0, 4, 2, 6, 1, 5, 3, 7
static const int kDitherThresholdByBit[8] = { 
    (0 * 255 + 4) / 8, // 0
    (4 * 255 + 4) / 8, // 1
    (2 * 255 + 4) / 8, // 2
    (6 * 255 + 4) / 8, // 3
    (1 * 255 + 4) / 8, // 4
    (5 * 255 + 4) / 8, // 5
    (3 * 255 + 4) / 8, // 6
    (7 * 255 + 4) / 8  // 7
};

uint PackColor(float3 c) {
    uint r = (uint)(saturate(c.r) * 255.0f);
    uint g = (uint)(saturate(c.g) * 255.0f);
    uint b = (uint)(saturate(c.b) * 255.0f);
    return (r) | (g << 8) | (b << 16) | 0xFF000000;
}

float3 UnpackColor(uint c) {
    float r = (float)(c & 0xFF) / 255.0f;
    float g = (float)((c >> 8) & 0xFF) / 255.0f;
    float b = (float)((c >> 16) & 0xFF) / 255.0f;
    return float3(r, g, b);
}

float GetLuma(float3 c) {
    return dot(c, kLumaCoeff) * 255.0f;
}

float SampleLumaRaw(float2 uv, SamplerState s) {
#ifdef NV12_INPUT
    float y = TextureY.SampleLevel(s, uv, 0);
    if (!isFullRange) {
        y = (y - 16.0/255.0) * (255.0/219.0);
    }
    return y * 255.0f;
#else
    return GetLuma(InputTexture.SampleLevel(s, uv, 0).rgb);
#endif
}

float SampleLuma(float2 uv) { return SampleLumaRaw(uv, LinearSampler); }
float SampleLumaPoint(float2 uv) { return SampleLumaRaw(uv, PointSampler); }

float GetInkLevelFromLum(float lum) {
    float norm = lum / 255.0f;
    float x = norm; // kInkUseBright = true
    float coverage = pow(x, 0.50f); // kInkGamma
    if (coverage > 0.001f) {
        // Reduced bias (0.12 -> 0.06) to allow for 1-dot characters (smooth 0->1->2 transition)
        coverage = coverage * 1.85f + 0.06f; 
    }
    // Cutoff tuned to suppress noise but allow 1-dot characters (which start around 0.17)
    if (coverage < 0.15f) coverage = 0.0f; 
    return saturate(coverage);
}

float GetEdgeBoostFromMag(float edge) {
    float range = 255.0f - (float)kEdgeMin;
    if (edge <= (float)kEdgeMin || range <= 0.0f) return 0.0f;
    float boost = (edge - (float)kEdgeMin) * (float)kEdgeBoost / range;
    return min(boost, (float)kEdgeBoost);
}

float4 SampleInput(float2 uv) {
    float4 color;
#ifdef NV12_INPUT
    float y = TextureY.SampleLevel(LinearSampler, uv, 0);
    float2 uv_val = TextureUV.SampleLevel(LinearSampler, uv, 0);
    
    float u, v;
    
    if (isFullRange) {
        u = uv_val.x - 0.5;
        v = uv_val.y - 0.5;
    } else {
        // Limited Range Expansion (16-235 -> 0-255 for Y, 16-240 -> 0-255 for UV)
        y = (y - 16.0/255.0) * (255.0/219.0);
        u = (uv_val.x - 128.0/255.0) * (255.0/224.0);
        v = (uv_val.y - 128.0/255.0) * (255.0/224.0);
    }

    // Rec. 709 (HD)
    float r = y + 1.5748 * v;
    float g = y - 0.1873 * u - 0.4681 * v;
    float b = y + 1.8556 * u;
    color = float4(r, g, b, 1.0);
#else
    color = InputTexture.SampleLevel(LinearSampler, uv, 0);
#endif

    // Fix washed out colors (HDR->SDR tone mapping approx)
    float3 rgb = color.rgb;
    float luma = dot(rgb, kLumaCoeff);
    rgb = lerp(luma.xxx, rgb, 1.3f); // Boost saturation
    rgb = (rgb - 0.5f) * 1.1f + 0.5f; // Boost contrast

    return float4(rgb, color.a);
}

struct DotInfo {
    int idx;
    int score;
    float luma;
    float edge;
    float contrast;
    float3 color;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= outWidth || DTid.y >= outHeight)
        return;

    uint cellIndex = DTid.y * outWidth + DTid.x;
    
    float cellW = (float)width / (float)outWidth;
    float cellH = (float)height / (float)outHeight;
    float dotW = cellW / 2.0f;
    float dotH = cellH / 4.0f;

    DotInfo dots[8];
    float cellLumMin = 255.0f;
    float cellLumMax = 0.0f;
    float cellLumSum = 0.0f;
    float cellEdgeMax = 0.0f;
    float cellContrastMax = 0.0f;

    float3 sumAll = float3(0,0,0);

    // 1. Gather Data
    for (int i = 0; i < 8; ++i) {
        int col = i / 4;
        int row = i % 4;
        
        float u = (DTid.x * cellW + col * dotW + dotW * 0.5f) / width;
        float v = (DTid.y * cellH + row * dotH + dotH * 0.5f) / height;

        // Supersampling (4x) for smoother dots and less shimmering
        float offU = dotW / width * 0.25f;
        float offV = dotH / height * 0.25f;

        float4 c1 = SampleInput(float2(u - offU, v - offV));
        float4 c2 = SampleInput(float2(u + offU, v - offV));
        float4 c3 = SampleInput(float2(u - offU, v + offV));
        float4 c4 = SampleInput(float2(u + offU, v + offV));
        
        float4 color = (c1 + c2 + c3 + c4) * 0.25f;
        float luma = GetLuma(color.rgb);
        
        // 3x3 Sobel Edge Detection (using center sample for performance/sharpness)
        // Use dot stride to match CPU behavior (detect edges between dots, not pixels)
        float texDx = dotW / (float)width;
        float texDy = dotH / (float)height;
        
        float l00 = SampleLuma(float2(u - texDx, v - texDy));
        float l01 = SampleLuma(float2(u, v - texDy));
        float l02 = SampleLuma(float2(u + texDx, v - texDy));
        
        float l10 = SampleLuma(float2(u - texDx, v));
        // l11 is center (luma), but we sample neighbors for gradient
        float l12 = SampleLuma(float2(u + texDx, v));
        
        float l20 = SampleLuma(float2(u - texDx, v + texDy));
        float l21 = SampleLuma(float2(u, v + texDy));
        float l22 = SampleLuma(float2(u + texDx, v + texDy));

        float gx = (l02 + 2.0f*l12 + l22) - (l00 + 2.0f*l10 + l20);
        float gy = (l20 + 2.0f*l21 + l22) - (l00 + 2.0f*l01 + l02);
        float edge = sqrt(gx*gx + gy*gy) / 4.0f; // Normalize to approx 0-255 range
        
        // Local Contrast
        float avgNeighbor = (l00 + l01 + l02 + l10 + l12 + l20 + l21 + l22) * 0.125f;
        float contrast = abs(luma - avgNeighbor);

        dots[i].idx = i;
        dots[i].luma = luma;
        dots[i].edge = edge;
        dots[i].contrast = contrast;
        dots[i].color = color.rgb;

        cellLumMin = min(cellLumMin, luma);
        cellLumMax = max(cellLumMax, luma);
        cellLumSum += luma;
        cellEdgeMax = max(cellEdgeMax, edge);
        cellContrastMax = max(cellContrastMax, contrast);
        sumAll += color.rgb;
    }

    // Adjust bgLum and lumRange for Limited Range if needed
    float effectiveBgLum = (float)bgLum;
    float effectiveLumRange = (float)lumRange;
    if (!isFullRange) {
        effectiveBgLum = (effectiveBgLum - 16.0f) * (255.0f / 219.0f);
        effectiveLumRange = effectiveLumRange * (255.0f / 219.0f);
    }

    // 2. Adaptive Thresholding
    float cellLumRange = cellLumMax - cellLumMin;
    float cellLumMean = cellLumSum / 8.0f;
    bool useLocalThreshold = cellLumRange > 20.0f;
    float localMidpoint = useLocalThreshold ? ((cellLumMin + cellLumMax) * 0.5f) : effectiveBgLum;

    // 3. Scoring
    for (int i = 0; i < 8; ++i) {
        float lumDiff = useLocalThreshold ? abs(dots[i].luma - localMidpoint) : abs(dots[i].luma - effectiveBgLum);
        dots[i].score = (int)(lumDiff * 2.0f + dots[i].edge + dots[i].contrast * 0.5f);
    }

    // 4. Sorting (Bubble Sort for 8 elements)
    for (int i = 0; i < 7; ++i) {
        for (int j = 0; j < 7 - i; ++j) {
            if (dots[j].score < dots[j+1].score) {
                DotInfo temp = dots[j];
                dots[j] = dots[j+1];
                dots[j+1] = temp;
            }
        }
    }

    // 5. Target Dots Calculation
    float avgLumDiff = abs(cellLumMean - effectiveBgLum);
    
    // Subtract noise floor (3 levels) to ensure true blacks in dark scenes
    avgLumDiff = max(0.0f, avgLumDiff - 3.0f);

    if (effectiveLumRange > 0) {
        avgLumDiff = avgLumDiff * 255.0f / effectiveLumRange;
    }
    float targetCoverage = GetInkLevelFromLum(avgLumDiff);
    int targetDots = (int)(8.0f * targetCoverage + 0.5f);

    float detailScore = max(cellLumRange, max(cellEdgeMax, cellContrastMax));
    int minDots = 0;
    if (detailScore >= kMinContrastForBraille) minDots = 1;
    if (detailScore >= kMinContrastForBraille * 2) minDots = 2;
    if (targetDots < minDots) targetDots = minDots;

    // 6. Activation
    uint bitmask = 0;
    float3 sumInk = float3(0,0,0);
    float3 sumBg = float3(0,0,0);
    int inkCount = 0;
    int bgCount = 0;

    // Braille bit mapping
    // 0=(0,0), 1=(0,1), 2=(0,2), 3=(1,0), 4=(1,1), 5=(1,2), 6=(0,3), 7=(1,3)
    int bitMap[8] = {0, 1, 2, 6, 3, 4, 5, 7};

    // AA Band Logic
    int aaBand = 0;
    int aaCutoffScore = 0;
    bool useAABand = false;
    if (targetDots > 0) {
        int scoreRange = dots[0].score - dots[7].score;
        aaBand = min(scoreRange, kAAScoreBandMax);
        useAABand = (detailScore >= kMinContrastForBraille && aaBand >= kAAScoreBandMin);
        aaCutoffScore = dots[targetDots - 1].score;
    } else {
        aaCutoffScore = dots[0].score;
    }

    for (int rank = 0; rank < 8; ++rank) {
        int idx = dots[rank].idx;
        bool shouldActivate = false;
        
        if (rank < targetDots) {
            shouldActivate = true;
        } else if (rank == targetDots && targetCoverage > 0) {
            // Dithering
            int ditherThresh = kDitherThresholdByBit[bitMap[idx]];
            int fractional = (int)((8.0f * targetCoverage * 255.0f)) % 255;
            shouldActivate = fractional > ditherThresh;
        }
        
        if (!shouldActivate && useAABand) {
            int scoreDelta = aaCutoffScore - dots[rank].score;
            if (scoreDelta >= 0 && scoreDelta < aaBand) {
                int numer = (aaBand - scoreDelta) * 255 + (aaBand / 2);
                int ditherThresh = kDitherThresholdByBit[bitMap[idx]];
                shouldActivate = numer > ditherThresh * aaBand;
            }
        }

        // Edge Boost
        if (!shouldActivate && dots[rank].edge > (float)kEdgeMin * 3.0f) {
             float edgeBonus = GetEdgeBoostFromMag(dots[rank].edge);
             int ditherThresh = kDitherThresholdByBit[bitMap[idx]] - kDitherBias;
             if (ditherThresh < 0) ditherThresh = 0;
             shouldActivate = edgeBonus > (float)ditherThresh;
        }

        if (shouldActivate) {
            bitmask |= (1 << bitMap[idx]);
            sumInk += dots[rank].color;
            inkCount++;
        } else {
            sumBg += dots[rank].color;
            bgCount++;
        }
    }

    // 7. Color Processing
    float3 curFg = (inkCount > 0) ? (sumInk / inkCount) : (sumAll / 8.0f);
    float3 curBg = (bgCount > 0) ? (sumBg / bgCount) : (sumAll / 8.0f);

    // Color Lift
    curFg = max(curFg, (float)kColorLift / 255.0f);
    curBg = max(curBg, (float)kColorLift / 255.0f);

    // Luma Correction (Ink)
    float curY = GetLuma(curFg);
    if (curY < (float)kInkMinLuma) {
        if (curY <= 0.0f) {
            curFg = (float)kInkMinLuma / 255.0f;
        } else {
            float scale = ((float)kInkMinLuma * 256.0f) / curY;
            if (scale > (float)kInkMaxScale) scale = (float)kInkMaxScale;
            curFg = min(1.0f, (curFg * scale) / 256.0f);
        }
    }

    // Adaptive Saturation (Ink)
    curY = GetLuma(curFg);
    float adaptiveSat = (float)kColorSaturation + ((255.0f - curY) / 4.0f);
    if (adaptiveSat > 400.0f) adaptiveSat = 400.0f;
    curFg = saturate(curY/255.0f + (curFg - curY/255.0f) * (adaptiveSat / 256.0f));

    // Luma Correction (Bg)
    float bgY = GetLuma(curBg);
    if (bgY < (float)kBgMinLuma) {
        if (bgY <= 0.0f) {
            curBg = (float)kBgMinLuma / 255.0f;
        } else {
            float scale = ((float)kBgMinLuma * 256.0f) / bgY;
            curBg = min(1.0f, (curBg * scale) / 256.0f);
        }
    }

    // Temporal Stability
    uint2 history = HistoryBuffer[cellIndex];
    float3 prevFg = UnpackColor(history.x);
    float3 prevBg = UnpackColor(history.y);
    
    // Perceptual Weighted Distance (Better than Manhattan/Euclidean)
    // Green changes are much more visible than Blue changes.
    // Using Rec. 709 luma coefficients as weights for the difference.
    float diffFg = dot(abs(curFg - prevFg), kLumaCoeff);
    float diffBg = dot(abs(curBg - prevBg), kLumaCoeff);
    
    float resetThresh = (float)kTemporalResetDelta / 255.0f;

    if (diffFg < resetThresh) {
        curFg = lerp(prevFg, curFg, (float)kColorAlpha/255.0f);
    }
    if (diffBg < resetThresh) {
        curBg = lerp(prevBg, curBg, (float)kBgAlpha/255.0f);
    }

    // Write back history
    HistoryBuffer[cellIndex] = uint2(PackColor(curFg), PackColor(curBg));

    AsciiCell cell;
    cell.ch = kBrailleBase + bitmask;
    cell.fg = PackColor(curFg);
    cell.bg = PackColor(curBg);
    
    // Fix for black backgrounds on full cells:
    bool hasBg = (bgCount > 0);
    if (!hasBg && GetLuma(curBg) > 10.0f) {
        hasBg = true;
    }
    cell.hasBg = hasBg;

    OutputBuffer[cellIndex] = cell;
}
)";

    struct GpuAsciiCell {
        uint32_t ch;
        uint32_t fg;
        uint32_t bg;
        uint32_t hasBg;
    };
}

GpuAsciiRenderer::GpuAsciiRenderer() {}

GpuAsciiRenderer::~GpuAsciiRenderer() {}

bool GpuAsciiRenderer::Initialize(int maxWidth, int maxHeight, std::string* error) {
    if (!CreateDevice()) {
        if (error) *error = "Failed to create D3D11 device";
        return false;
    }
    if (!CompileComputeShader(error)) return false;

    // Create Samplers
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    
    if (FAILED(m_device->CreateSamplerState(&sampDesc, &m_linearSampler))) {
        if (error) *error = "Failed to create linear sampler";
        return false;
    }

    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(m_device->CreateSamplerState(&sampDesc, &m_pointSampler))) {
        if (error) *error = "Failed to create point sampler";
        return false;
    }

    // Buffers will be created on first render or here if we knew exact size
    return true;
}

bool GpuAsciiRenderer::CreateDevice() {
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT creationFlags = 0;
#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags,
        featureLevels, 1, D3D11_SDK_VERSION,
        &m_device, nullptr, &m_context
    );
    return SUCCEEDED(hr);
}

bool GpuAsciiRenderer::CompileComputeShader(std::string* error) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    // Compile RGBA Shader
    HRESULT hr = D3DCompile(
        kComputeShaderSource, strlen(kComputeShaderSource),
        "EmbeddedShader", nullptr, nullptr,
        "CSMain", "cs_5_0",
        0, 0, &blob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::string msg = (char*)errorBlob->GetBufferPointer();
            std::cerr << "Shader Compile Error (RGBA): " << msg << std::endl;
            if (error) *error = "Shader Compile Error (RGBA): " + msg;
        } else {
            if (error) *error = "Shader Compile Error (RGBA): Unknown error";
        }
        return false;
    }

    hr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_computeShader);
    if (FAILED(hr)) {
        if (error) *error = "Failed to create compute shader (RGBA)";
        return false;
    }

    // Compile NV12 Shader
    D3D_SHADER_MACRO defines[] = { "NV12_INPUT", "1", nullptr, nullptr };
    blob.Reset();
    errorBlob.Reset();

    hr = D3DCompile(
        kComputeShaderSource, strlen(kComputeShaderSource),
        "EmbeddedShader", defines, nullptr,
        "CSMain", "cs_5_0",
        0, 0, &blob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::string msg = (char*)errorBlob->GetBufferPointer();
            std::cerr << "Shader Compile Error (NV12): " << msg << std::endl;
            if (error) *error = "Shader Compile Error (NV12): " + msg;
        } else {
            if (error) *error = "Shader Compile Error (NV12): Unknown error";
        }
        return false;
    }

    hr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_computeShaderNV12);
    if (FAILED(hr)) {
        if (error) *error = "Failed to create compute shader (NV12)";
        return false;
    }
    return SUCCEEDED(hr);
}

bool GpuAsciiRenderer::CreateNV12Textures(int width, int height, bool is10Bit) {
    // Check if we need to recreate textures (size or format change)
    DXGI_FORMAT targetY = is10Bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    DXGI_FORMAT targetUV = is10Bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;

    if (m_textureY) {
        D3D11_TEXTURE2D_DESC desc;
        m_textureY->GetDesc(&desc);
        if (desc.Width == width && desc.Height == height && desc.Format == targetY) {
            return true;
        }
        // Release old textures
        m_textureY.Reset();
        m_srvY.Reset();
        m_textureUV.Reset();
        m_srvUV.Reset();
    }

    // Y Texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = targetY;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    
    if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, &m_textureY))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_textureY.Get(), nullptr, &m_srvY))) return false;

    // UV Texture
    texDesc.Width = width / 2;
    texDesc.Height = height / 2;
    texDesc.Format = targetUV;
    
    if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, &m_textureUV))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_textureUV.Get(), nullptr, &m_srvUV))) return false;

    return true;
}

bool GpuAsciiRenderer::RenderNV12(const uint8_t* yuv, int width, int height, int stride, int planeHeight, 
                                int bgLum, int lumRange, bool fullRange, bool is10Bit, AsciiArt& out, std::string* error) {
    if (!m_device) {
        std::string initErr;
        if (!Initialize(width, height, &initErr)) {
            if (error) *error = "Failed to initialize GPU renderer: " + initErr;
            return false;
        }
    }

    int outW = out.width;
    int outH = out.height;
    if (outW <= 0 || outH <= 0) return false;

    if (!CreateBuffers(width, height, outW, outH)) {
        if (error) *error = "Failed to create GPU buffers";
        return false;
    }
    
    if (!CreateNV12Textures(width, height, is10Bit)) {
        if (error) *error = "Failed to create NV12 textures";
        return false;
    }

    // Upload Y
    m_context->UpdateSubresource(m_textureY.Get(), 0, nullptr, yuv, stride, 0);
    
    // Upload UV
    // UV plane starts after Y plane (stride * planeHeight)
    const uint8_t* uvData = yuv + (stride * planeHeight);
    m_context->UpdateSubresource(m_textureUV.Get(), 0, nullptr, uvData, stride, 0);

    // Update Constants
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        Constants* cb = (Constants*)mapped.pData;
        cb->width = width;
        cb->height = height;
        cb->outWidth = outW;
        cb->outHeight = outH;
        cb->time = 0.0f;
        cb->frameCount++;
        cb->bgLum = bgLum;
        cb->lumRange = lumRange;
        cb->isFullRange = fullRange ? 1 : 0;
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    // Dispatch
    m_context->CSSetShader(m_computeShaderNV12.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { m_srvY.Get(), m_srvUV.Get() };
    m_context->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get(), m_historyUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    m_context->CSSetConstantBuffers(0, 1, cbs);

    ID3D11SamplerState* samplers[] = { m_linearSampler.Get(), m_pointSampler.Get() };
    m_context->CSSetSamplers(0, 2, samplers);

    UINT dispatchX = (outW + 7) / 8;
    UINT dispatchY = (outH + 7) / 8;
    m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 2, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr, nullptr };
    m_context->CSSetShaderResources(0, 2, nullSRV);

    // Readback (Same as Render)
    m_context->CopyResource(m_outputStagingBuffer.Get(), m_outputBuffer.Get());
    
    if (SUCCEEDED(m_context->Map(m_outputStagingBuffer.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        GpuAsciiCell* gpuCells = (GpuAsciiCell*)mapped.pData;
        out.cells.resize(outW * outH);
        
        for (int i = 0; i < outW * outH; ++i) {
            out.cells[i].ch = (wchar_t)gpuCells[i].ch;
            
            uint32_t fg = gpuCells[i].fg;
            out.cells[i].fg.r = (fg) & 0xFF;
            out.cells[i].fg.g = (fg >> 8) & 0xFF;
            out.cells[i].fg.b = (fg >> 16) & 0xFF;

            uint32_t bg = gpuCells[i].bg;
            out.cells[i].bg.r = (bg) & 0xFF;
            out.cells[i].bg.g = (bg >> 8) & 0xFF;
            out.cells[i].bg.b = (bg >> 16) & 0xFF;

            out.cells[i].hasBg = gpuCells[i].hasBg != 0;
        }
        m_context->Unmap(m_outputStagingBuffer.Get(), 0);
    }

    return true;
}

bool GpuAsciiRenderer::CreateBuffers(int width, int height, int outW, int outH) {
    if (m_currentWidth == width && m_currentHeight == height && 
        m_currentOutW == outW && m_currentOutH == outH) {
        return true;
    }

    // Input Texture (with MipMaps)
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 0; // Full chain
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT; // Default for UpdateSubresource/GenerateMips
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, &m_inputTexture))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_inputTexture.Get(), nullptr, &m_inputSRV))) return false;

    // Output Buffer
    D3D11_BUFFER_DESC bufDesc = {};
    bufDesc.ByteWidth = sizeof(GpuAsciiCell) * outW * outH;
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufDesc.StructureByteStride = sizeof(GpuAsciiCell);

    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_outputBuffer))) return false;
    
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = outW * outH;
    
    if (FAILED(m_device->CreateUnorderedAccessView(m_outputBuffer.Get(), &uavDesc, &m_outputUAV))) return false;

    // History Buffer
    bufDesc.ByteWidth = sizeof(uint32_t) * 2 * outW * outH; // uint2
    bufDesc.StructureByteStride = sizeof(uint32_t) * 2;
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_historyBuffer))) return false;
    if (FAILED(m_device->CreateUnorderedAccessView(m_historyBuffer.Get(), &uavDesc, &m_historyUAV))) return false;

    // Clear history initially
    UINT clearVals[4] = {0,0,0,0};
    m_context->ClearUnorderedAccessViewUint(m_historyUAV.Get(), clearVals);

    // Staging Buffer for Readback
    bufDesc.Usage = D3D11_USAGE_STAGING;
    bufDesc.BindFlags = 0;
    bufDesc.MiscFlags = 0;
    bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bufDesc.StructureByteStride = 0; // Not needed for staging
    bufDesc.ByteWidth = sizeof(GpuAsciiCell) * outW * outH;
    
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_outputStagingBuffer))) return false;

    // Constant Buffer
    bufDesc.ByteWidth = sizeof(Constants);
    bufDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bufDesc.MiscFlags = 0;
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_constantBuffer))) return false;

    m_currentWidth = width;
    m_currentHeight = height;
    m_currentOutW = outW;
    m_currentOutH = outH;
    return true;
}

bool GpuAsciiRenderer::Render(const uint8_t* rgba, int width, int height, AsciiArt& out, std::string* error) {
    if (!m_device) {
        std::string initErr;
        if (!Initialize(width, height, &initErr)) {
            if (error) *error = "Failed to initialize GPU renderer: " + initErr;
            return false;
        }
    }

    // Calculate output size (same logic as CPU)
    int outW = out.width;
    int outH = out.height;
    if (outW <= 0 || outH <= 0) return false;

    if (!CreateBuffers(width, height, outW, outH)) {
        if (error) *error = "Failed to create GPU buffers";
        return false;
    }

    // Upload Texture
    m_context->UpdateSubresource(m_inputTexture.Get(), 0, nullptr, rgba, width * 4, 0);
    
    // Generate Mips
    m_context->GenerateMips(m_inputSRV.Get());

    // Calculate stats (bgLum, lumRange) from RGBA
    // Sample a subset of pixels for performance
    int sampleStep = 16; // Sample every 16th pixel
    uint32_t histogram[256] = {0};
    int totalSamples = 0;
    
    for (int y = 0; y < height; y += sampleStep) {
        const uint8_t* row = rgba + y * width * 4;
        for (int x = 0; x < width; x += sampleStep) {
            const uint8_t* p = row + x * 4;
            // Calculate luma: 0.2126 R + 0.7152 G + 0.0722 B
            int luma = (p[0] * 54 + p[1] * 183 + p[2] * 18) >> 8; // Approx integer math
            if (luma > 255) luma = 255;
            histogram[luma]++;
            totalSamples++;
        }
    }
    
    // Find range (5% - 95%)
    int count = 0;
    int low = 0;
    int targetLow = totalSamples * 5 / 100;
    for (int i = 0; i < 256; ++i) {
        count += histogram[i];
        if (count >= targetLow) {
            low = i;
            break;
        }
    }
    
    count = 0;
    int high = 255;
    int targetHigh = totalSamples * 95 / 100;
    for (int i = 0; i < 256; ++i) {
        count += histogram[i];
        if (count >= targetHigh) {
            high = i;
            break;
        }
    }
    
    int lumRange = (std::max)(1, high - low);
    if (lumRange < 80) lumRange = 80; // Clamp like in main.cpp
    
    // Find bgLum (mode)
    int bgLum = 0;
    uint32_t maxCount = 0;
    for (int i = 0; i < 256; ++i) {
        if (histogram[i] > maxCount) {
            maxCount = histogram[i];
            bgLum = i;
        }
    }

    // Update Constants
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        Constants* cb = (Constants*)mapped.pData;
        cb->width = width;
        cb->height = height;
        cb->outWidth = outW;
        cb->outHeight = outH;
        cb->time = 0.0f;
        cb->frameCount++;
        cb->bgLum = bgLum;
        cb->lumRange = lumRange;
        cb->isFullRange = 1; // RGBA is always full range
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    // Dispatch
    m_context->CSSetShader(m_computeShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { m_inputSRV.Get() };
    m_context->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get(), m_historyUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    m_context->CSSetConstantBuffers(0, 1, cbs);

    ID3D11SamplerState* samplers[] = { m_linearSampler.Get(), m_pointSampler.Get() };
    m_context->CSSetSamplers(0, 2, samplers);

    UINT dispatchX = (outW + 7) / 8;
    UINT dispatchY = (outH + 7) / 8;
    m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind UAVs
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 2, nullUAV, nullptr);

    // Readback
    m_context->CopyResource(m_outputStagingBuffer.Get(), m_outputBuffer.Get());
    
    if (SUCCEEDED(m_context->Map(m_outputStagingBuffer.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        GpuAsciiCell* gpuCells = (GpuAsciiCell*)mapped.pData;
        out.cells.resize(outW * outH);
        
        for (int i = 0; i < outW * outH; ++i) {
            out.cells[i].ch = (wchar_t)gpuCells[i].ch;
            
            uint32_t fg = gpuCells[i].fg;
            out.cells[i].fg.r = (fg) & 0xFF;
            out.cells[i].fg.g = (fg >> 8) & 0xFF;
            out.cells[i].fg.b = (fg >> 16) & 0xFF;

            uint32_t bg = gpuCells[i].bg;
            out.cells[i].bg.r = (bg) & 0xFF;
            out.cells[i].bg.g = (bg >> 8) & 0xFF;
            out.cells[i].bg.b = (bg >> 16) & 0xFF;

            out.cells[i].hasBg = gpuCells[i].hasBg != 0;
        }
        m_context->Unmap(m_outputStagingBuffer.Get(), 0);
    }

    return true;
}
