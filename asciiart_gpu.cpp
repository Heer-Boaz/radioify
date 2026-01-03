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
    uint isFullRange;
    uint padding[1];
};

#ifdef NV12_INPUT
Texture2D<float> TextureY : register(t0);
Texture2D<float2> TextureUV : register(t1);
StructuredBuffer<uint4> StatsBuffer : register(t2); // x=bgLum, y=lumRange
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
static const int kMinContrastForBraille = 8; // Reduced from 15 to allow finer details
static const int kEdgeShift = 3;
static const int kColorLift = 0;
static const int kColorSaturation = 340;
static const int kTemporalResetDelta = 48;
static const int kColorAlpha = 180; // Reduced from 220 for more stability
static const int kBgAlpha = 140;    // Reduced from 180 for more stability
static const int kInkMinLuma = 40;  // Reduced from 110 to allow dark details
static const int kBgMinLuma = 10;   // Reduced from 20
static const int kInkMaxScale = 1280;
static const int kEdgeMin = 4; // Reverted to 4 for sensitivity, noise handled by logic
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

float SampleLumaArea(float2 centerUV, float2 dotSizePx) {
    // 4x4 adaptive Gaussian filter (16 samples)
    // Reverted to 16 samples to fix TDR/Freeze, but added Gaussian weighting for precision
    float2 texSize = float2(width, height);
    float2 step = dotSizePx / 4.0f / texSize;
    // Start at top-left of the dot area
    float2 start = centerUV - (dotSizePx * 0.5f / texSize) + (step * 0.5f);

    float sum = 0;
    float weightSum = 0;
    
    [unroll]
    for (int dy = 0; dy < 4; ++dy) {
        [unroll]
        for (int dx = 0; dx < 4; ++dx) {
            // Center of 4x4 grid is (1.5, 1.5)
            float distX = abs((float)dx - 1.5f);
            float distY = abs((float)dy - 1.5f);
            // Max dist sq = 1.5^2 + 1.5^2 = 4.5
            float normDist = (distX * distX + distY * distY) / 4.5f;
            
            // Weight: 1.0 at center, ~0.4 at corners
            float weight = 1.0f - (normDist * 0.6f);
            
            sum += SampleLumaRaw(start + float2((float)dx, (float)dy) * step, LinearSampler) * weight;
            weightSum += weight;
        }
    }
    return sum / weightSum;
}

float GetInkLevelFromLum(float lum) {
    float norm = lum / 255.0f;
    float x = norm; // kInkUseBright = true
    // Gamma 0.65: Slightly steeper to delay onset of dots
    float coverage = pow(x, 0.65f); 
    if (coverage > 0.001f) {
        // Reduced gain (1.30) to prevent early saturation to 6-dots
        coverage = coverage * 1.30f; 
    }
    // Cutoff 0.03: Filter very faint noise
    if (coverage < 0.03f) coverage = 0.0f; 
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
    // Removed aggressive tone mapping that amplified noise
    // Just a slight contrast boost
    rgb = (rgb - 0.5f) * 1.05f + 0.5f; 
    color.rgb = saturate(rgb);

    return float4(rgb, color.a);
}
)"
R"(
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

        // Area sampling for stable luma/color
        float2 dotSize = float2(dotW, dotH);
        float luma = SampleLumaArea(float2(u, v), dotSize);
        
        // For color, we still use the center sample (or could implement SampleInputArea)
        // Using center sample for color is usually fine as luma drives the structure
        float4 color = SampleInput(float2(u, v)); 
        
        // 3x3 Sobel Edge Detection (using area samples for stability)
        // Use dot stride to match CPU behavior (detect edges between dots, not pixels)
        float texDx = dotW / (float)width;
        float texDy = dotH / (float)height;
        
        float l00 = SampleLumaArea(float2(u - texDx, v - texDy), dotSize);
        float l01 = SampleLumaArea(float2(u, v - texDy), dotSize);
        float l02 = SampleLumaArea(float2(u + texDx, v - texDy), dotSize);
        
        float l10 = SampleLumaArea(float2(u - texDx, v), dotSize);
        // l11 is center (luma)
        float l12 = SampleLumaArea(float2(u + texDx, v), dotSize);
        
        float l20 = SampleLumaArea(float2(u - texDx, v + texDy), dotSize);
        float l21 = SampleLumaArea(float2(u, v + texDy), dotSize);
        float l22 = SampleLumaArea(float2(u + texDx, v + texDy), dotSize);

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
    uint bgLum = StatsBuffer[0].x;
    uint lumRange = StatsBuffer[0].y;

    float effectiveBgLum = (float)bgLum;
    float effectiveLumRange = max((float)lumRange, 80.0f); // Clamp to prevent noise amplification
    if (!isFullRange) {
        effectiveBgLum = (effectiveBgLum - 16.0f) * (255.0f / 219.0f);
        effectiveLumRange = effectiveLumRange * (255.0f / 219.0f);
    }

    // 2. Adaptive Thresholding (Per-Sub-Pixel Logic like asciiart.ts)
    // We abandon the "Target Dots" approach in favor of direct thresholding
    // to preserve small details.

    // Braille bit mapping
    // 0=(0,0), 1=(0,1), 2=(0,2), 3=(1,0), 4=(1,1), 5=(1,2), 6=(0,3), 7=(1,3)
    int bitMap[8] = {0, 1, 2, 6, 3, 4, 5, 7};

    uint bitmask = 0;
    float3 sumInk = float3(0,0,0);
    float3 sumBg = float3(0,0,0);
    int inkCount = 0;
    int bgCount = 0;

    // Base threshold (DELTA in ts)
    float baseThreshold = 30.0f; 

    for (int i = 0; i < 8; ++i) {
        // Edge-aware threshold modulation
        // TS: deltaThr = Math.max(10, DELTA - 0.2 * edges[p]);
        // Our edge is 0-255 approx.
        float threshold = max(10.0f, baseThreshold - dots[i].edge * 0.2f);

        // Check against global background luminance
        float lumDiff = abs(dots[i].luma - effectiveBgLum);
        
        // Check if "near background" (TS: colorDistSq < BG_DIST)
        // We use luma diff as a proxy for simplicity and speed here, 
        // but could check color distance if needed.
        // TS uses BG_DIST = 32^2 = 1024. Sqrt(1024) = 32.
        // So if lumDiff < 32, it's "near bg".
        // But wait, TS logic is: dotSet = !nearBg && lumDiff >= deltaThr
        // If nearBg is true, dotSet is false.
        // Effectively: if color distance is small, it's BG.
        // If color distance is large AND luma diff is large, it's FG.
        
        bool isDot = (lumDiff >= threshold);

        // Additional check: if it's just noise?
        // We rely on the "Intelligent Contrast" later to hide noise.
        
        if (isDot) {
            bitmask |= (1 << bitMap[dots[i].idx]);
            sumInk += dots[i].color;
            inkCount++;
        } else {
            sumBg += dots[i].color;
            bgCount++;
        }
    }

    // Calculate stats for contrast logic
    float avgLumDiff = abs(cellLumMean - effectiveBgLum);

    // 7. Color Processing
    float3 curFg = (inkCount > 0) ? (sumInk / inkCount) : (sumAll / 8.0f);
    float3 curBg = (bgCount > 0) ? (sumBg / bgCount) : (sumAll / 8.0f);

    // Color Lift
    curFg = max(curFg, (float)kColorLift / 255.0f);
    curBg = max(curBg, (float)kColorLift / 255.0f);

    // Intelligent Contrast Management
    // 1. Noise Suppression: Blend FG into BG for weak signals
    // 2. Detail Enhancement: Boost contrast for strong signals
    
    float edgeSig = saturate((cellEdgeMax - 4.0f) / 12.0f); // Ramp 4->16 (Fast transition for edges)
    float lumSig = saturate((avgLumDiff - 4.0f) / 24.0f);   // Ramp 4->28 (Slower for luma)
    float signalStrength = max(edgeSig, lumSig);

    // Dampening: If signal is weak, hide it (blend to BG)
    curFg = lerp(curBg, curFg, signalStrength);

    // Boosting: If signal is strong (especially edges), increase contrast
    if (signalStrength > 0.8f) {
        float boost = (signalStrength - 0.8f) * 5.0f; // 0.0 -> 1.0
        float3 center = (curFg + curBg) * 0.5f;
        float3 delta = curFg - center;
        
        // Boost FG significantly (up to 50%)
        curFg = center + delta * (1.0f + boost * 0.5f);
        
        // Boost BG gently (up to 10%) to avoid crushing it to black
        // We want to preserve the context while making the foreground pop
        curBg = center - delta * (1.0f + boost * 0.1f); 
        
        curFg = saturate(curFg);
        curBg = saturate(curBg);
    }

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
    // Reduced saturation boost for dark colors to prevent blue/purple artifacts
    curY = GetLuma(curFg);
    float adaptiveSat = (float)kColorSaturation;
    // If dark, reduce saturation instead of boosting it
    if (curY < 60.0f) {
        adaptiveSat = adaptiveSat * (curY / 60.0f); 
    }
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

    // Temporal Stability (Reduced ghosting)
    uint2 history = HistoryBuffer[cellIndex];
    float3 prevFg = UnpackColor(history.x);
    float3 prevBg = UnpackColor(history.y);
    
    // Perceptual Weighted Distance
    float diffFg = dot(abs(curFg - prevFg), kLumaCoeff);
    float diffBg = dot(abs(curBg - prevBg), kLumaCoeff);
    
    float resetThresh = (float)kTemporalResetDelta / 255.0f;

    // Increased alpha for faster updates (less ghosting)
    // Old: 180/255 (~0.7), New: 230/255 (~0.9)
    if (diffFg < resetThresh) {
        curFg = lerp(prevFg, curFg, 230.0f/255.0f);
    }
    if (diffBg < resetThresh) {
        curBg = lerp(prevBg, curBg, 230.0f/255.0f);
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

    // Stats Compute Shader
    const char* kStatsShaderSource = R"(
cbuffer Constants : register(b0) {
    uint width;
    uint height;
    uint padding[2];
};

Texture2D<float> TextureY : register(t0);
RWStructuredBuffer<uint4> Stats : register(u0); // x=bgLum, y=lumRange

groupshared uint histogram[256];

[numthreads(256, 1, 1)]
void CSStats(uint3 tid : SV_DispatchThreadID) {
    // Initialize histogram
    histogram[tid.x] = 0;
    GroupMemoryBarrierWithGroupSync();

    // Strided sampling (similar to CPU 10x10 but parallel)
    // We want to cover the whole image with 256 threads.
    // Each thread handles a vertical strip or scattered pixels.
    
    uint stride = 10;
    for (uint y = 0; y < height; y += stride) {
        for (uint x = tid.x * stride; x < width; x += 256 * stride) {
             float yVal = TextureY.Load(int3(x, y, 0)); // Load raw value (0.0-1.0)
             uint bin = (uint)(yVal * 255.0f + 0.5f);
             bin = min(bin, 255);
             InterlockedAdd(histogram[bin], 1);
        }
    }
    
    GroupMemoryBarrierWithGroupSync();
    
    if (tid.x == 0) {
        // Find Mode (bgLum)
        uint maxCount = 0;
        uint mode = 0;
        uint totalSamples = 0;
        
        for (uint i = 0; i < 256; ++i) {
            uint count = histogram[i];
            totalSamples += count;
            if (count > maxCount) {
                maxCount = count;
                mode = i;
            }
        }
        
        // Find Range (1% - 99%)
        uint targetLow = totalSamples / 100;
        uint targetHigh = totalSamples * 99 / 100;
        uint accum = 0;
        uint low = 0;
        uint high = 255;
        bool lowFound = false;
        
        for (uint j = 0; j < 256; ++j) {
            accum += histogram[j];
            if (!lowFound && accum >= targetLow) {
                low = j;
                lowFound = true;
            }
            if (accum >= targetHigh) {
                high = j;
                break;
            }
        }
        
        uint range = max(1, high - low);
        if (range < 80) range = 80;
        
        Stats[0] = uint4(mode, range, 0, 0);
    }
}
)";
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

    // Compile Stats Shader
    blob.Reset();
    errorBlob.Reset();
    hr = D3DCompile(
        kStatsShaderSource, strlen(kStatsShaderSource),
        "StatsShader", nullptr, nullptr,
        "CSStats", "cs_5_0",
        0, 0, &blob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::string msg = (char*)errorBlob->GetBufferPointer();
            std::cerr << "Shader Compile Error (Stats): " << msg << std::endl;
            if (error) *error = "Shader Compile Error (Stats): " + msg;
        } else {
            if (error) *error = "Shader Compile Error (Stats): Unknown error";
        }
        return false;
    }

    hr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_statsShader);
    if (FAILED(hr)) {
        if (error) *error = "Failed to create compute shader (Stats)";
        return false;
    }

    return SUCCEEDED(hr);
}

bool GpuAsciiRenderer::CreateStatsBuffer() {
    if (m_statsBuffer) return true;

    D3D11_BUFFER_DESC bufDesc = {};
    bufDesc.ByteWidth = sizeof(uint32_t) * 4; // uint4
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufDesc.StructureByteStride = sizeof(uint32_t) * 4;

    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_statsBuffer))) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = 1;
    if (FAILED(m_device->CreateUnorderedAccessView(m_statsBuffer.Get(), &uavDesc, &m_statsUAV))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = 1;
    if (FAILED(m_device->CreateShaderResourceView(m_statsBuffer.Get(), &srvDesc, &m_statsSRV))) return false;

    return true;
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
                                bool fullRange, bool is10Bit, AsciiArt& out, std::string* error) {
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

    if (!CreateStatsBuffer()) {
        if (error) *error = "Failed to create stats buffer";
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
        cb->isFullRange = fullRange ? 1 : 0;
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    // 1. Calculate Stats
    m_context->CSSetShader(m_statsShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* statsSRVs[] = { m_srvY.Get() }; // Only Y needed
    m_context->CSSetShaderResources(0, 1, statsSRVs);
    ID3D11UnorderedAccessView* statsUAVs[] = { m_statsUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 1, statsUAVs, nullptr);
    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    m_context->CSSetConstantBuffers(0, 1, cbs);

    m_context->Dispatch(1, 1, 1); // Single group of 256 threads

    // Unbind Stats UAV
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

    // 2. Main Pass
    m_context->CSSetShader(m_computeShaderNV12.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { m_srvY.Get(), m_srvUV.Get(), m_statsSRV.Get() };
    m_context->CSSetShaderResources(0, 3, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get(), m_historyUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);

    ID3D11SamplerState* samplers[] = { m_linearSampler.Get(), m_pointSampler.Get() };
    m_context->CSSetSamplers(0, 2, samplers);

    UINT dispatchX = (outW + 7) / 8;
    UINT dispatchY = (outH + 7) / 8;
    m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind
    ID3D11UnorderedAccessView* nullUAV2[] = { nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 2, nullUAV2, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr, nullptr, nullptr };
    m_context->CSSetShaderResources(0, 3, nullSRV);

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
        // RGBA path doesn't use StatsBuffer yet, so we might need to zero these or handle differently
        // For now, just set isFullRange. The shader will read garbage from StatsBuffer if we don't bind it.
        // Ideally RGBA path should also use StatsCS or pass these via CB.
        // But since we removed them from CB, we must use StatsBuffer.
        // Let's just bind a dummy or run StatsCS for RGBA too.
        cb->isFullRange = 1; 
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
