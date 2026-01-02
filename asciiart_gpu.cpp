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
    uint padding;
};

Texture2D<float4> InputTexture : register(t0);
SamplerState LinearSampler : register(s0);

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
static const int kEdgeShift = 10;
static const int kColorLift = 40;
static const int kColorSaturation = 256;
static const int kTemporalResetDelta = 40;
static const int kColorAlpha = 128; // 0.5 in fixed point
static const int kBgAlpha = 128;

// Dithering thresholds (simplified)
static const int kDitherThresholdByBit[8] = { 128, 64, 192, 32, 160, 96, 224, 16 };

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

        // Sample Center
        float4 color = InputTexture.SampleLevel(LinearSampler, float2(u, v), 0);
        float luma = GetLuma(color.rgb);
        
        // Sobel (Approximate by sampling neighbors)
        float u_l = u - dotW/width; float u_r = u + dotW/width;
        float v_t = v - dotH/height; float v_b = v + dotH/height;
        
        float l_l = GetLuma(InputTexture.SampleLevel(LinearSampler, float2(u_l, v), 0).rgb);
        float l_r = GetLuma(InputTexture.SampleLevel(LinearSampler, float2(u_r, v), 0).rgb);
        float l_t = GetLuma(InputTexture.SampleLevel(LinearSampler, float2(u, v_t), 0).rgb);
        float l_b = GetLuma(InputTexture.SampleLevel(LinearSampler, float2(u, v_b), 0).rgb);
        
        float gx = l_r - l_l;
        float gy = l_b - l_t;
        float edge = sqrt(gx*gx + gy*gy);
        
        // Local Contrast
        float avgNeighbor = (l_l + l_r + l_t + l_b) * 0.25f;
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

    // 2. Adaptive Thresholding
    float cellLumRange = cellLumMax - cellLumMin;
    float cellLumMean = cellLumSum / 8.0f;
    bool useLocalThreshold = cellLumRange > 20.0f;
    float localMidpoint = useLocalThreshold ? ((cellLumMin + cellLumMax) * 0.5f) : (float)bgLum;

    // 3. Scoring
    for (int i = 0; i < 8; ++i) {
        float lumDiff = useLocalThreshold ? abs(dots[i].luma - localMidpoint) : abs(dots[i].luma - (float)bgLum);
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
    float avgLumDiff = abs(cellLumMean - (float)bgLum);
    // Simplified InkLevelFromLum mapping (linear approx)
    float targetCoverage = saturate(avgLumDiff / 128.0f); 
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
        
        // Edge Boost
        if (!shouldActivate && dots[rank].edge > 30.0f) { // kEdgeMin * 3 approx
             // Simplified edge boost
             shouldActivate = true;
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

    // Color Lift & Saturation (Simplified)
    curFg = max(curFg, (float)kColorLift / 255.0f);
    
    // Temporal Stability
    uint2 history = HistoryBuffer[cellIndex];
    float3 prevFg = UnpackColor(history.x);
    float3 prevBg = UnpackColor(history.y);
    
    // Simple blend if difference is small
    if (length(curFg - prevFg) < (float)kTemporalResetDelta/255.0f) {
        curFg = lerp(prevFg, curFg, (float)kColorAlpha/255.0f);
    }
    if (length(curBg - prevBg) < (float)kTemporalResetDelta/255.0f) {
        curBg = lerp(prevBg, curBg, (float)kBgAlpha/255.0f);
    }

    // Write back history
    HistoryBuffer[cellIndex] = uint2(PackColor(curFg), PackColor(curBg));

    AsciiCell cell;
    cell.ch = kBrailleBase + bitmask;
    cell.fg = PackColor(curFg);
    cell.bg = PackColor(curBg);
    cell.hasBg = (bgCount > 0);

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

bool GpuAsciiRenderer::Initialize(int maxWidth, int maxHeight) {
    if (!CreateDevice()) return false;
    if (!CompileComputeShader()) return false;
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

bool GpuAsciiRenderer::CompileComputeShader() {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompile(
        kComputeShaderSource, strlen(kComputeShaderSource),
        "EmbeddedShader", nullptr, nullptr,
        "CSMain", "cs_5_0",
        0, 0, &blob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "Shader Compile Error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        }
        return false;
    }

    hr = m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_computeShader);
    return SUCCEEDED(hr);
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
        if (!Initialize(width, height)) {
            if (error) *error = "Failed to initialize GPU renderer";
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
        cb->bgLum = 0; // Default to dark background assumption
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
