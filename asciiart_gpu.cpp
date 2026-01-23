#include "asciiart_gpu.h"
#include "gpu_shared.h"
#include <d3d11.h>
#include <d3d10.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#pragma comment(lib, "d3d11.lib")

#define BG_CLAMP_DEBUG 0

namespace {
    // Precompiled shader bytecode (generated from shaders/*.hlsl)
    #include "asciiart_gpu_cs_main.h"
    #include "asciiart_gpu_cs_main_nv12.h"
    #include "asciiart_gpu_cs_bg_clamp.h"
    #include "asciiart_gpu_cs_sync_history.h"
    #include "asciiart_stats_cs_rgba.h"
    #include "asciiart_stats_cs_nv12.h"

    struct GpuAsciiCell {
        uint32_t ch;
        uint32_t fg;
        uint32_t bg;
        uint32_t hasBg;
    };

}

GpuAsciiRenderer::GpuAsciiRenderer() {}

GpuAsciiRenderer::~GpuAsciiRenderer() {}

void GpuAsciiRenderer::ClearHistory() {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (m_context && m_historyUAV) {
        UINT clearValues[4] = { 0, 0, 0, 0 };
        m_context->ClearUnorderedAccessViewUint(m_historyUAV.Get(), clearValues);
    }
}

bool GpuAsciiRenderer::Initialize(int maxWidth, int maxHeight, std::string* error) {
    if (!CreateDevice()) {
        if (error) *error = "Failed to create D3D11 device";
        return false;
    }
    if (!CreateComputeShaders(error)) return false;

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

    // Buffers will be created on first render or here if we knew exact size
    return true;
}

bool GpuAsciiRenderer::InitializeWithDevice(ID3D11Device* device, ID3D11DeviceContext* context,
                                            int maxWidth, int maxHeight, std::string* error) {
    if (!device || !context) {
        if (error) *error = "Invalid device or context";
        return false;
    }
    
    m_device = device;
    m_context = context;
    
    if (!CreateComputeShaders(error)) return false;

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

    return true;
}

bool GpuAsciiRenderer::CreateDevice() {
    // For device sharing with video decoder, we need D3D11_CREATE_DEVICE_VIDEO_SUPPORT
    return CreateDeviceWithFlags(D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
}

bool GpuAsciiRenderer::CreateDeviceWithFlags(UINT extraFlags) {
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT creationFlags = extraFlags;
#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags,
        featureLevels, 1, D3D11_SDK_VERSION,
        &m_device, nullptr, &m_context
    );
    
    if (FAILED(hr) && (extraFlags & D3D11_CREATE_DEVICE_VIDEO_SUPPORT)) {
        // Fallback without video support flag
        creationFlags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags,
            featureLevels, 1, D3D11_SDK_VERSION,
            &m_device, nullptr, &m_context
        );
    }
    
    if (SUCCEEDED(hr)) {
        // Enable multithread protection for device sharing
        Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(m_device.As(&multithread))) {
            multithread->SetMultithreadProtected(TRUE);
        }
    }
    
    return SUCCEEDED(hr);
}

bool GpuAsciiRenderer::CreateComputeShaders(std::string* error) {
    if (!m_device) {
        if (error) *error = "D3D11 device is not initialized";
        return false;
    }

    auto createShader = [&](const unsigned char* data, unsigned int size,
                            Microsoft::WRL::ComPtr<ID3D11ComputeShader>& out,
                            const char* label) {
        if (!data || size == 0) {
            if (error) *error = std::string("Missing shader bytecode: ") + label;
            return false;
        }
        HRESULT hr = m_device->CreateComputeShader(
            data, static_cast<SIZE_T>(size), nullptr, out.GetAddressOf());
        if (FAILED(hr)) {
            if (error) {
                *error = std::string("Failed to create compute shader: ") + label;
            }
            return false;
        }
        return true;
    };

    if (!createShader(kComputeShaderCsMain, kComputeShaderCsMain_Size,
                      m_computeShader, "CSMain RGBA")) {
        return false;
    }
    if (!createShader(kComputeShaderCsMainNv12, kComputeShaderCsMainNv12_Size,
                      m_computeShaderNV12, "CSMain NV12")) {
        return false;
    }
    if (!createShader(kComputeShaderCsBgClamp, kComputeShaderCsBgClamp_Size,
                      m_bgClampShader, "CSBgClamp")) {
        return false;
    }
    if (!createShader(kComputeShaderCsSyncHistory, kComputeShaderCsSyncHistory_Size,
                      m_syncHistoryShader, "CSSyncHistory")) {
        return false;
    }
    if (!createShader(kStatsShaderCsRgba, kStatsShaderCsRgba_Size,
                      m_statsShaderRgba, "CSStats RGBA")) {
        return false;
    }
    if (!createShader(kStatsShaderCsNv12, kStatsShaderCsNv12_Size,
                      m_statsShaderNv12, "CSStats NV12")) {
        return false;
    }

    return true;
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

// Resources handled by m_frameCache

bool GpuAsciiRenderer::RenderNV12(const uint8_t* yuv, int width, int height, int stride, int planeHeight, 
                                bool fullRange, YuvMatrix yuvMatrix, YuvTransfer yuvTransfer,
                                bool is10Bit, AsciiArt& out, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
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
    
    if (!m_frameCache.UpdateNV12(m_device.Get(), m_context.Get(), yuv, stride, planeHeight, width, height, 
                                 fullRange, yuvMatrix, yuvTransfer, is10Bit ? 10 : 8)) {
        if (error) *error = "Failed to update NV12 cache";
        return false;
    }

    if (!CreateStatsBuffer()) {
        if (error) *error = "Failed to create stats buffer";
        return false;
    }

    ID3D11ShaderResourceView* srvY = m_frameCache.GetSrvY();
    ID3D11ShaderResourceView* srvUV = m_frameCache.GetSrvUV();

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
        cb->bitDepth = is10Bit ? 10u : 8u;
        cb->yuvMatrix = static_cast<uint32_t>(yuvMatrix);
        cb->yuvTransfer = static_cast<uint32_t>(yuvTransfer);
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    // 1. Calculate Stats
    m_context->CSSetShader(m_statsShaderNv12.Get(), nullptr, 0);
    ID3D11ShaderResourceView* statsSRVs[] = { srvY }; // Only Y needed
    m_context->CSSetShaderResources(0, 1, statsSRVs);
    ID3D11UnorderedAccessView* statsUAVs[] = { m_statsUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 1, statsUAVs, nullptr);
    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    m_context->CSSetConstantBuffers(0, 1, cbs);

    m_context->Dispatch(1, 1, 1); // Single group of 256 threads

    // Unbind Stats UAV
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

    UINT dispatchX = (outW + 7) / 8;
    UINT dispatchY = (outH + 7) / 8;

    // 2. Main Pass
    m_context->CSSetShader(m_computeShaderNV12.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { srvY, srvUV, m_statsSRV.Get() };
    m_context->CSSetShaderResources(0, 3, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get(), m_historyUAV.Get(), m_metaUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
    m_context->CSSetSamplers(0, 1, samplers);

    m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind
    ID3D11UnorderedAccessView* nullUAV3[] = { nullptr, nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV3, nullptr);
    ID3D11ShaderResourceView* nullSRV5[] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    m_context->CSSetShaderResources(0, 5, nullSRV5);

    // 3. BG Clamp Pass
    m_context->CSSetShader(m_bgClampShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* clampSrvs[] = { nullptr, nullptr, nullptr, m_historySRV.Get(), m_metaSRV.Get() };
    m_context->CSSetShaderResources(0, 5, clampSrvs);
    ID3D11UnorderedAccessView* clampUavs[] = { m_outputUAV.Get(), nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 3, clampUavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->Dispatch(dispatchX, dispatchY, 1);

    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV3, nullptr);
    m_context->CSSetShaderResources(0, 5, nullSRV5);

    // 4. Sync History
    m_context->CSSetShader(m_syncHistoryShader.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* syncUavs[] = { m_outputUAV.Get(), m_historyUAV.Get(), nullptr };
    m_context->CSSetUnorderedAccessViews(0, 3, syncUavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->Dispatch(dispatchX, dispatchY, 1);

    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV3, nullptr);
    m_context->CSSetShaderResources(0, 5, nullSRV5);

    m_frameCache.MarkFrameInFlight(m_context.Get());

    // Readback (double-buffered staging to reduce GPU stalls)
    int writeIndex = m_outputStagingIndex;
    int readIndex = m_outputStagingPrimed ? (1 - writeIndex) : writeIndex;
    m_context->CopyResource(m_outputStagingBuffers[writeIndex].Get(), m_outputBuffer.Get());
    
    if (SUCCEEDED(m_context->Map(m_outputStagingBuffers[readIndex].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        GpuAsciiCell* gpuCells = (GpuAsciiCell*)mapped.pData;
        out.cells.resize(outW * outH);
#if BG_CLAMP_DEBUG
        uint64_t clampCount = 0;
#endif
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

            uint32_t hasBgRaw = gpuCells[i].hasBg;
#if BG_CLAMP_DEBUG
            if ((hasBgRaw & 2u) != 0) {
                ++clampCount;
            }
#endif
            out.cells[i].hasBg = (hasBgRaw & 1u) != 0;
        }
#if BG_CLAMP_DEBUG
        uint64_t total = static_cast<uint64_t>(outW) * outH;
        if (total > 0) {
            double pct = (static_cast<double>(clampCount) * 100.0) /
                         static_cast<double>(total);
            std::fprintf(stderr, "bg clamp (gpu): %.2f%% (%llu/%llu)\n", pct,
                         static_cast<unsigned long long>(clampCount),
                         static_cast<unsigned long long>(total));
        }
#endif
        m_context->Unmap(m_outputStagingBuffers[readIndex].Get(), 0);
        m_outputStagingPrimed = true;
    }
    m_outputStagingIndex = 1 - writeIndex;

    return true;
}

bool GpuAsciiRenderer::CreateBuffers(int width, int height, int outW, int outH) {
    if (m_currentWidth == width && m_currentHeight == height && 
        m_currentOutW == outW && m_currentOutH == outH) {
        return true;
    }

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
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bufDesc.StructureByteStride = sizeof(uint32_t) * 2;
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_historyBuffer))) return false;
    if (FAILED(m_device->CreateUnorderedAccessView(m_historyBuffer.Get(), &uavDesc, &m_historyUAV))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = outW * outH;
    if (FAILED(m_device->CreateShaderResourceView(m_historyBuffer.Get(), &srvDesc, &m_historySRV))) return false;

    // Meta Buffer
    bufDesc.ByteWidth = sizeof(uint32_t) * outW * outH;
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bufDesc.StructureByteStride = sizeof(uint32_t);
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_metaBuffer))) return false;
    if (FAILED(m_device->CreateUnorderedAccessView(m_metaBuffer.Get(), &uavDesc, &m_metaUAV))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_metaBuffer.Get(), &srvDesc, &m_metaSRV))) return false;

    // Clear history initially
    UINT clearVals[4] = {0,0,0,0};
    m_context->ClearUnorderedAccessViewUint(m_historyUAV.Get(), clearVals);

    // Staging Buffers for Readback (double-buffered)
    bufDesc.Usage = D3D11_USAGE_STAGING;
    bufDesc.BindFlags = 0;
    bufDesc.MiscFlags = 0;
    bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bufDesc.StructureByteStride = 0; // Not needed for staging
    bufDesc.ByteWidth = sizeof(GpuAsciiCell) * outW * outH;

    for (int i = 0; i < 2; ++i) {
        if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_outputStagingBuffers[i]))) return false;
    }
    m_outputStagingIndex = 0;
    m_outputStagingPrimed = false;

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
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
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

    if (!m_frameCache.Update(m_device.Get(), m_context.Get(), rgba, width * 4, width, height)) {
        if (error) *error = "Failed to update RGBA cache";
        return false;
    }

    if (!CreateStatsBuffer()) {
        if (error) *error = "Failed to create stats buffer";
        return false;
    }

    ID3D11ShaderResourceView* inputSRV = m_frameCache.GetSrvRGBA();

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
        cb->isFullRange = 1; 
        cb->bitDepth = 8;
        cb->yuvMatrix = static_cast<uint32_t>(YuvMatrix::Bt709);
        cb->yuvTransfer = static_cast<uint32_t>(YuvTransfer::Sdr);
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };

    UINT dispatchX = (outW + 7) / 8;
    UINT dispatchY = (outH + 7) / 8;

    // Calculate Stats
    m_context->CSSetShader(m_statsShaderRgba.Get(), nullptr, 0);
    ID3D11ShaderResourceView* statsSRVs[] = { inputSRV };
    m_context->CSSetShaderResources(0, 1, statsSRVs);
    ID3D11UnorderedAccessView* statsUAVs[] = { m_statsUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 1, statsUAVs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);

    m_context->Dispatch(1, 1, 1);

    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

    // Main Pass
    m_context->CSSetShader(m_computeShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { inputSRV, m_statsSRV.Get(), nullptr };
    m_context->CSSetShaderResources(0, 3, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get(), m_historyUAV.Get(), m_metaUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->CSSetSamplers(0, 1, samplers);

    m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind UAVs
    ID3D11UnorderedAccessView* nullUAV2[] = { nullptr, nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV2, nullptr);
    ID3D11ShaderResourceView* nullSRV2[] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    m_context->CSSetShaderResources(0, 5, nullSRV2);

    // BG Clamp Pass
    m_context->CSSetShader(m_bgClampShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* clampSrvs[] = { nullptr, nullptr, nullptr, m_historySRV.Get(), m_metaSRV.Get() };
    m_context->CSSetShaderResources(0, 5, clampSrvs);
    ID3D11UnorderedAccessView* clampUavs[] = { m_outputUAV.Get(), nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 3, clampUavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->Dispatch(dispatchX, dispatchY, 1);

    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV2, nullptr);
    m_context->CSSetShaderResources(0, 5, nullSRV2);

    // Sync History
    m_context->CSSetShader(m_syncHistoryShader.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* syncUavs[] = { m_outputUAV.Get(), m_historyUAV.Get(), nullptr };
    m_context->CSSetUnorderedAccessViews(0, 3, syncUavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->Dispatch(dispatchX, dispatchY, 1);

    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV2, nullptr);
    m_context->CSSetShaderResources(0, 5, nullSRV2);

    m_frameCache.MarkFrameInFlight(m_context.Get());

    // Readback (double-buffered staging to reduce GPU stalls)
    int writeIndex = m_outputStagingIndex;
    int readIndex = m_outputStagingPrimed ? (1 - writeIndex) : writeIndex;
    m_context->CopyResource(m_outputStagingBuffers[writeIndex].Get(), m_outputBuffer.Get());
    
    if (SUCCEEDED(m_context->Map(m_outputStagingBuffers[readIndex].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
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
        m_context->Unmap(m_outputStagingBuffers[readIndex].Get(), 0);
        m_outputStagingPrimed = true;
    }
    m_outputStagingIndex = 1 - writeIndex;

    return true;
}

bool GpuAsciiRenderer::RenderNV12Texture(ID3D11Texture2D* texture, int arrayIndex,
                                         int width, int height,
                                         bool fullRange, YuvMatrix yuvMatrix, YuvTransfer yuvTransfer,
                                         bool is10Bit, AsciiArt& out, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!texture) {
        if (error) *error = "Null texture provided";
        return false;
    }
    
    if (!m_device) {
        if (error) *error = "GPU renderer not initialized";
        return false;
    }

    int outW = out.width;
    int outH = out.height;
    if (outW <= 0 || outH <= 0) {
        if (error) *error = "Invalid output dimensions";
        return false;
    }

    // Check texture format
    D3D11_TEXTURE2D_DESC srcDesc;
    texture->GetDesc(&srcDesc);
    const char* fmtLabel = "unknown";
    if (srcDesc.Format == DXGI_FORMAT_NV12) {
        fmtLabel = "NV12";
    } else if (srcDesc.Format == DXGI_FORMAT_P010) {
        fmtLabel = "P010";
    }

    // Check device compatibility
    Microsoft::WRL::ComPtr<ID3D11Device> texDevice;
    texture->GetDevice(texDevice.GetAddressOf());
    if (texDevice.Get() != m_device.Get()) {
        if (error) *error = "Texture is on a different D3D11 device";
        return false;
    }

    // Check array index
    // Use shared frame cache
    if (!m_frameCache.Update(m_device.Get(), m_context.Get(), texture, arrayIndex, width, height,
                              fullRange, yuvMatrix, yuvTransfer, is10Bit ? 10 : 8)) {
        if (error) *error = "Failed to update frame cache";
        return false;
    }

    // Create buffers if needed
    if (!CreateBuffers(width, height, outW, outH)) {
        if (error) *error = "Failed to create GPU buffers";
        return false;
    }

    if (!CreateStatsBuffer()) {
        if (error) *error = "Failed to create stats buffer";
        return false;
    }

    ID3D11ShaderResourceView* srvY = m_frameCache.GetSrvY();
    ID3D11ShaderResourceView* srvUV = m_frameCache.GetSrvUV();

    m_lastNv12TexturePath = "gpu_cache";
    {
        char buf[128];
        const char* fmtLabel = (srcDesc.Format == DXGI_FORMAT_NV12) ? "NV12" : 
                               (srcDesc.Format == DXGI_FORMAT_P010) ? "P010" : "OTHER";
        std::snprintf(buf, sizeof(buf), "path=gpu_cache fmt=%s bind=0x%X",
                      fmtLabel, static_cast<unsigned int>(srcDesc.BindFlags));
        m_lastNv12TextureDetail = buf;
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
        cb->isFullRange = fullRange ? 1 : 0;
        cb->bitDepth = is10Bit ? 10u : 8u;
        cb->yuvMatrix = static_cast<uint32_t>(yuvMatrix);
        cb->yuvTransfer = static_cast<uint32_t>(yuvTransfer);
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    // 1. Calculate Stats
    m_context->CSSetShader(m_statsShaderNv12.Get(), nullptr, 0);
    ID3D11ShaderResourceView* statsSRVs[] = { srvY };
    m_context->CSSetShaderResources(0, 1, statsSRVs);
    ID3D11UnorderedAccessView* statsUAVs[] = { m_statsUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 1, statsUAVs, nullptr);
    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    m_context->CSSetConstantBuffers(0, 1, cbs);

    m_context->Dispatch(1, 1, 1);

    // Unbind Stats UAV
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

    UINT dispatchX = (outW + 7) / 8;
    UINT dispatchY = (outH + 7) / 8;

    // 2. Main Pass
    m_context->CSSetShader(m_computeShaderNV12.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { srvY, srvUV, m_statsSRV.Get() };
    m_context->CSSetShaderResources(0, 3, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get(), m_historyUAV.Get(), m_metaUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
    m_context->CSSetSamplers(0, 1, samplers);

    m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind
    ID3D11UnorderedAccessView* nullUAV3[] = { nullptr, nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV3, nullptr);
    ID3D11ShaderResourceView* nullSRV5[] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    m_context->CSSetShaderResources(0, 5, nullSRV5);

    // 3. BG Clamp Pass
    m_context->CSSetShader(m_bgClampShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* clampSrvs[] = { nullptr, nullptr, nullptr, m_historySRV.Get(), m_metaSRV.Get() };
    m_context->CSSetShaderResources(0, 5, clampSrvs);
    ID3D11UnorderedAccessView* clampUavs[] = { m_outputUAV.Get(), nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 3, clampUavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->Dispatch(dispatchX, dispatchY, 1);

    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV3, nullptr);
    m_context->CSSetShaderResources(0, 5, nullSRV5);

    // 4. Sync History
    m_context->CSSetShader(m_syncHistoryShader.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* syncUavs[] = { m_outputUAV.Get(), m_historyUAV.Get(), nullptr };
    m_context->CSSetUnorderedAccessViews(0, 3, syncUavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->Dispatch(dispatchX, dispatchY, 1);

    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV3, nullptr);
    m_context->CSSetShaderResources(0, 5, nullSRV5);

    m_frameCache.MarkFrameInFlight(m_context.Get());

    // Readback (double-buffered staging to reduce GPU stalls)
    int writeIndex = m_outputStagingIndex;
    int readIndex = m_outputStagingPrimed ? (1 - writeIndex) : writeIndex;
    m_context->CopyResource(m_outputStagingBuffers[writeIndex].Get(), m_outputBuffer.Get());
    
    if (SUCCEEDED(m_context->Map(m_outputStagingBuffers[readIndex].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
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

            uint32_t hasBgRaw = gpuCells[i].hasBg;
            out.cells[i].hasBg = (hasBgRaw & 1u) != 0;
        }
        m_context->Unmap(m_outputStagingBuffers[readIndex].Get(), 0);
        m_outputStagingPrimed = true;
    }
    m_outputStagingIndex = 1 - writeIndex;

    return true;
}

bool GpuAsciiRenderer::RenderFromCache(GpuVideoFrameCache& cache, AsciiArt& out,
                                       std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_device || !m_context) {
        if (error) *error = "GPU renderer not initialized";
        return false;
    }
    if (!cache.HasFrame()) {
        if (error) *error = "Shared cache has no frame";
        return false;
    }

    int width = cache.GetWidth();
    int height = cache.GetHeight();
    int outW = out.width;
    int outH = out.height;
    if (outW <= 0 || outH <= 0) {
        if (error) *error = "Invalid output dimensions";
        return false;
    }

    if (!CreateBuffers(width, height, outW, outH)) {
        if (error) *error = "Failed to create GPU buffers";
        return false;
    }

    if (!CreateStatsBuffer()) {
        if (error) *error = "Failed to create stats buffer";
        return false;
    }

    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
    UINT dispatchX = (outW + 7) / 8;
    UINT dispatchY = (outH + 7) / 8;

    if (cache.IsYuv()) {
        ID3D11ShaderResourceView* srvY = cache.GetSrvY();
        ID3D11ShaderResourceView* srvUV = cache.GetSrvUV();
        if (!srvY || !srvUV) {
            if (error) *error = "Shared cache missing YUV SRVs";
            return false;
        }

        m_lastNv12TexturePath = "shared_cache";
        {
            char buf[128];
            const char* fmtLabel = (cache.GetBitDepth() > 8) ? "P010" : "NV12";
            std::snprintf(buf, sizeof(buf), "path=shared_cache fmt=%s", fmtLabel);
            m_lastNv12TextureDetail = buf;
        }

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            Constants* cb = (Constants*)mapped.pData;
            cb->width = width;
            cb->height = height;
            cb->outWidth = outW;
            cb->outHeight = outH;
            cb->time = 0.0f;
            cb->frameCount++;
            cb->isFullRange = cache.IsFullRange() ? 1 : 0;
            cb->bitDepth = static_cast<uint32_t>(cache.GetBitDepth());
            cb->yuvMatrix = static_cast<uint32_t>(cache.GetMatrix());
            cb->yuvTransfer = static_cast<uint32_t>(cache.GetTransfer());
            m_context->Unmap(m_constantBuffer.Get(), 0);
        }

        m_context->CSSetShader(m_statsShaderNv12.Get(), nullptr, 0);
        ID3D11ShaderResourceView* statsSRVs[] = { srvY };
        m_context->CSSetShaderResources(0, 1, statsSRVs);
        ID3D11UnorderedAccessView* statsUAVs[] = { m_statsUAV.Get() };
        m_context->CSSetUnorderedAccessViews(0, 1, statsUAVs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        m_context->Dispatch(1, 1, 1);

        ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
        m_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

        m_context->CSSetShader(m_computeShaderNV12.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { srvY, srvUV, m_statsSRV.Get() };
        m_context->CSSetShaderResources(0, 3, srvs);
        ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get(), m_historyUAV.Get(), m_metaUAV.Get() };
        m_context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        m_context->CSSetSamplers(0, 1, samplers);
        m_context->Dispatch(dispatchX, dispatchY, 1);

        ID3D11UnorderedAccessView* nullUAV3[] = { nullptr, nullptr, nullptr };
        m_context->CSSetUnorderedAccessViews(0, 3, nullUAV3, nullptr);
        ID3D11ShaderResourceView* nullSRV5[] = { nullptr, nullptr, nullptr, nullptr, nullptr };
        m_context->CSSetShaderResources(0, 5, nullSRV5);

        m_context->CSSetShader(m_bgClampShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* clampSrvs[] = { nullptr, nullptr, nullptr, m_historySRV.Get(), m_metaSRV.Get() };
        m_context->CSSetShaderResources(0, 5, clampSrvs);
        ID3D11UnorderedAccessView* clampUavs[] = { m_outputUAV.Get(), nullptr, nullptr };
        m_context->CSSetUnorderedAccessViews(0, 3, clampUavs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        m_context->Dispatch(dispatchX, dispatchY, 1);

        m_context->CSSetUnorderedAccessViews(0, 3, nullUAV3, nullptr);
        m_context->CSSetShaderResources(0, 5, nullSRV5);

        m_context->CSSetShader(m_syncHistoryShader.Get(), nullptr, 0);
        ID3D11UnorderedAccessView* syncUavs[] = { m_outputUAV.Get(), m_historyUAV.Get(), nullptr };
        m_context->CSSetUnorderedAccessViews(0, 3, syncUavs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        m_context->Dispatch(dispatchX, dispatchY, 1);

        m_context->CSSetUnorderedAccessViews(0, 3, nullUAV3, nullptr);
        m_context->CSSetShaderResources(0, 5, nullSRV5);
    } else if (cache.IsRgba()) {
        ID3D11ShaderResourceView* inputSRV = cache.GetSrvRGBA();
        if (!inputSRV) {
            if (error) *error = "Shared cache missing RGBA SRV";
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            Constants* cb = (Constants*)mapped.pData;
            cb->width = width;
            cb->height = height;
            cb->outWidth = outW;
            cb->outHeight = outH;
            cb->time = 0.0f;
            cb->frameCount++;
            cb->isFullRange = 1;
            cb->bitDepth = 8;
            cb->yuvMatrix = static_cast<uint32_t>(YuvMatrix::Bt709);
            cb->yuvTransfer = static_cast<uint32_t>(YuvTransfer::Sdr);
            m_context->Unmap(m_constantBuffer.Get(), 0);
        }

        m_context->CSSetShader(m_statsShaderRgba.Get(), nullptr, 0);
        ID3D11ShaderResourceView* statsSRVs[] = { inputSRV };
        m_context->CSSetShaderResources(0, 1, statsSRVs);
        ID3D11UnorderedAccessView* statsUAVs[] = { m_statsUAV.Get() };
        m_context->CSSetUnorderedAccessViews(0, 1, statsUAVs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        m_context->Dispatch(1, 1, 1);

        ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
        m_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

        m_context->CSSetShader(m_computeShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { inputSRV, m_statsSRV.Get(), nullptr };
        m_context->CSSetShaderResources(0, 3, srvs);
        ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get(), m_historyUAV.Get(), m_metaUAV.Get() };
        m_context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        m_context->CSSetSamplers(0, 1, samplers);
        m_context->Dispatch(dispatchX, dispatchY, 1);

        ID3D11UnorderedAccessView* nullUAV2[] = { nullptr, nullptr, nullptr };
        m_context->CSSetUnorderedAccessViews(0, 3, nullUAV2, nullptr);
        ID3D11ShaderResourceView* nullSRV2[] = { nullptr, nullptr, nullptr, nullptr, nullptr };
        m_context->CSSetShaderResources(0, 5, nullSRV2);

        m_context->CSSetShader(m_bgClampShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* clampSrvs[] = { nullptr, nullptr, nullptr, m_historySRV.Get(), m_metaSRV.Get() };
        m_context->CSSetShaderResources(0, 5, clampSrvs);
        ID3D11UnorderedAccessView* clampUavs[] = { m_outputUAV.Get(), nullptr, nullptr };
        m_context->CSSetUnorderedAccessViews(0, 3, clampUavs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        m_context->Dispatch(dispatchX, dispatchY, 1);

        m_context->CSSetUnorderedAccessViews(0, 3, nullUAV2, nullptr);
        m_context->CSSetShaderResources(0, 5, nullSRV2);

        m_context->CSSetShader(m_syncHistoryShader.Get(), nullptr, 0);
        ID3D11UnorderedAccessView* syncUavs[] = { m_outputUAV.Get(), m_historyUAV.Get(), nullptr };
        m_context->CSSetUnorderedAccessViews(0, 3, syncUavs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        m_context->Dispatch(dispatchX, dispatchY, 1);

        m_context->CSSetUnorderedAccessViews(0, 3, nullUAV2, nullptr);
        m_context->CSSetShaderResources(0, 5, nullSRV2);
    } else {
        if (error) *error = "Shared cache format unsupported";
        return false;
    }

    cache.MarkFrameInFlight(m_context.Get());

    int writeIndex = m_outputStagingIndex;
    int readIndex = m_outputStagingPrimed ? (1 - writeIndex) : writeIndex;
    m_context->CopyResource(m_outputStagingBuffers[writeIndex].Get(), m_outputBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_outputStagingBuffers[readIndex].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
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
        m_context->Unmap(m_outputStagingBuffers[readIndex].Get(), 0);
        m_outputStagingPrimed = true;
    }
    m_outputStagingIndex = 1 - writeIndex;

    return true;
}
