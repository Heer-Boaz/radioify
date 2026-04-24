#include "asciiart_gpu.h"
#include "playback/video/gpu/gpu_shared.h"
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#pragma comment(lib, "d3d11.lib")

namespace {
    // Precompiled shader bytecode (generated from playback/render/shaders/*.hlsl)
    #include "asciiart_gpu_cs_main.h"
    #include "asciiart_gpu_cs_main_nv12.h"
    #include "asciiart_gpu_cs_dot_prefilter.h"
    #include "asciiart_gpu_cs_dot_prefilter_nv12.h"
    #include "asciiart_gpu_cs_dot_edge.h"
    #include "asciiart_gpu_cs_refine_continuity.h"
    #include "asciiart_gpu_cs_sync_history.h"
    #include "asciiart_stats_cs_rgba.h"
    #include "asciiart_stats_cs_nv12.h"

    struct GpuAsciiCell {
        uint32_t ch;
        uint32_t fg;
        uint32_t bg;
        uint32_t hasBg;
    };

    struct GpuAsciiDotSample {
        uint32_t color;
        uint32_t metrics;
    };

#define RADIOIFY_ASCII_BOOL(name, value) constexpr bool name = value;
#define RADIOIFY_ASCII_FLOAT(name, value) constexpr float name = value;
#define RADIOIFY_ASCII_COUNT(name, value) constexpr int name = value;
#define RADIOIFY_ASCII_LUMA_U8(name, value) constexpr int name = value;
#define RADIOIFY_ASCII_SIGNAL_U8(name, value) constexpr int name = value;
#define RADIOIFY_ASCII_SCALE_256(name, value) constexpr int name = value;
#include "asciiart_constants.inc"
#undef RADIOIFY_ASCII_BOOL
#undef RADIOIFY_ASCII_FLOAT
#undef RADIOIFY_ASCII_COUNT
#undef RADIOIFY_ASCII_LUMA_U8
#undef RADIOIFY_ASCII_SIGNAL_U8
#undef RADIOIFY_ASCII_SCALE_256

}

bool GpuAsciiRenderer::ReadOutputBuffer(AsciiArt& out, int outW, int outH) {
    bool readbackReady = false;

    if (m_outputStagingPendingCount > 0) {
        int readIndex = m_outputStagingReadIndex;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = m_context->Map(m_outputStagingBuffers[readIndex].Get(), 0,
                                    D3D11_MAP_READ,
                                    D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (SUCCEEDED(hr)) {
            GpuAsciiCell* gpuCells = static_cast<GpuAsciiCell*>(mapped.pData);
            out.cells.resize(outW * outH);

            for (int i = 0; i < outW * outH; ++i) {
                out.cells[i].ch = static_cast<wchar_t>(gpuCells[i].ch);

                uint32_t fg = gpuCells[i].fg;
                out.cells[i].fg.r = (fg) & 0xFF;
                out.cells[i].fg.g = (fg >> 8) & 0xFF;
                out.cells[i].fg.b = (fg >> 16) & 0xFF;

                uint32_t bg = gpuCells[i].bg;
                out.cells[i].bg.r = (bg) & 0xFF;
                out.cells[i].bg.g = (bg >> 8) & 0xFF;
                out.cells[i].bg.b = (bg >> 16) & 0xFF;

                out.cells[i].hasBg = (gpuCells[i].hasBg & 1u) != 0;
            }
            m_context->Unmap(m_outputStagingBuffers[readIndex].Get(), 0);
            m_outputStagingPending[readIndex] = false;
            m_outputStagingReadIndex =
                (readIndex + 1) % kOutputStagingBufferCount;
            --m_outputStagingPendingCount;
            readbackReady = true;
        } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
            m_outputStagingPending[readIndex] = false;
            m_outputStagingReadIndex =
                (readIndex + 1) % kOutputStagingBufferCount;
            --m_outputStagingPendingCount;
        }
    }

    if (m_outputStagingPendingCount < kOutputStagingBufferCount) {
        int writeIndex = m_outputStagingWriteIndex;
        for (int i = 0; i < kOutputStagingBufferCount; ++i) {
            if (!m_outputStagingPending[writeIndex]) {
                break;
            }
            writeIndex = (writeIndex + 1) % kOutputStagingBufferCount;
        }
        m_context->CopyResource(m_outputStagingBuffers[writeIndex].Get(),
                                m_outputBuffer.Get());
        m_context->Flush();
        m_outputStagingPending[writeIndex] = true;
        m_outputStagingWriteIndex =
            (writeIndex + 1) % kOutputStagingBufferCount;
        ++m_outputStagingPendingCount;
    }
    return readbackReady;
}

GpuAsciiRenderer::GpuAsciiRenderer() {}

GpuAsciiRenderer::~GpuAsciiRenderer() {}

void GpuAsciiRenderer::SetGpuTimingEnabled(bool enabled,
                                           bool blockUntilReady) {
    m_gpuTimingEnabled = enabled;
    m_gpuTimingBlockUntilReady = blockUntilReady;
    if (!enabled) {
        m_lastGpuTiming = GpuTimingResult{};
    }
}

bool GpuAsciiRenderer::BeginAsciiGpuTiming(
    Microsoft::WRL::ComPtr<ID3D11Query>& disjoint,
    Microsoft::WRL::ComPtr<ID3D11Query>& start) {
    m_lastGpuTiming = GpuTimingResult{};
    if (!m_gpuTimingEnabled || !m_device || !m_context) {
        return false;
    }

    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    if (FAILED(m_device->CreateQuery(&qd, disjoint.GetAddressOf()))) {
        return false;
    }
    qd.Query = D3D11_QUERY_TIMESTAMP;
    if (FAILED(m_device->CreateQuery(&qd, start.GetAddressOf()))) {
        disjoint.Reset();
        return false;
    }

    m_context->Begin(disjoint.Get());
    m_context->End(start.Get());
    return true;
}

void GpuAsciiRenderer::EndAsciiGpuTiming(
    Microsoft::WRL::ComPtr<ID3D11Query>& disjoint,
    Microsoft::WRL::ComPtr<ID3D11Query>& start) {
    if (!disjoint || !start || !m_context || !m_device) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Query> end;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_TIMESTAMP;
    if (FAILED(m_device->CreateQuery(&qd, end.GetAddressOf()))) {
        m_context->End(disjoint.Get());
        return;
    }

    m_context->End(end.Get());
    m_context->End(disjoint.Get());

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
    UINT64 startTime = 0;
    UINT64 endTime = 0;
    auto readTiming = [&]() -> HRESULT {
        HRESULT hr = m_context->GetData(disjoint.Get(), &disjointData,
                                        sizeof(disjointData), 0);
        if (hr != S_OK) return hr;
        hr = m_context->GetData(start.Get(), &startTime, sizeof(startTime), 0);
        if (hr != S_OK) return hr;
        return m_context->GetData(end.Get(), &endTime, sizeof(endTime), 0);
    };

    HRESULT hr = readTiming();
    for (int attempts = 0; m_gpuTimingBlockUntilReady && hr == S_FALSE &&
                           attempts < 10000; ++attempts) {
        Sleep(0);
        hr = readTiming();
    }

    if (hr == S_OK && !disjointData.Disjoint && endTime >= startTime &&
        disjointData.Frequency != 0) {
        m_lastGpuTiming.valid = true;
        m_lastGpuTiming.asciiComputeMs =
            (static_cast<double>(endTime - startTime) /
             static_cast<double>(disjointData.Frequency)) *
            1000.0;
    }
}

void GpuAsciiRenderer::ClearHistory() {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (m_context && m_historyUAV) {
        UINT clearValues[4] = { 0, 0, 0, 0 };
        m_context->ClearUnorderedAccessViewUint(m_historyUAV.Get(), clearValues);
    }
    if (m_context && m_statsUAV) {
        UINT clearValues[4] = { static_cast<UINT>(kLumMinimumRange), 0, 0, 0 };
        m_context->ClearUnorderedAccessViewUint(m_statsUAV.Get(), clearValues);
    }
    m_frameCount = 0;
}

void GpuAsciiRenderer::ResetSessionState() {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    m_frameCache.Reset();
    m_outputBuffer.Reset();
    m_outputUAV.Reset();
    m_rawOutputBuffer.Reset();
    m_rawOutputUAV.Reset();
    m_rawOutputSRV.Reset();
    m_dotSampleBuffer.Reset();
    m_dotSampleUAV.Reset();
    m_dotSampleSRV.Reset();
    m_dotEdgeBuffer.Reset();
    m_dotEdgeUAV.Reset();
    m_dotEdgeSRV.Reset();
    for (int i = 0; i < kOutputStagingBufferCount; ++i) {
        m_outputStagingBuffers[i].Reset();
        m_outputStagingPending[i] = false;
    }
    m_statsBuffer.Reset();
    m_statsUAV.Reset();
    m_statsSRV.Reset();
    m_historyBuffer.Reset();
    m_historyUAV.Reset();
    m_constantBuffer.Reset();
    m_outputStagingReadIndex = 0;
    m_outputStagingWriteIndex = 0;
    m_outputStagingPendingCount = 0;
    m_currentWidth = 0;
    m_currentHeight = 0;
    m_currentOutW = 0;
    m_currentOutH = 0;
    m_frameCount = 0;
    m_lastNv12TexturePath = "unknown";
    m_lastNv12TextureDetail.clear();
}

bool GpuAsciiRenderer::Initialize(std::string* error) {
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

bool GpuAsciiRenderer::InitializeWithDevice(ID3D11Device* device,
                                            ID3D11DeviceContext* context,
                                            std::string* error) {
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

    auto tryCreate = [&](UINT flags) -> HRESULT {
        m_device.Reset();
        m_context.Reset();
        return D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            featureLevels, 1, D3D11_SDK_VERSION,
            &m_device, nullptr, &m_context
        );
    };

    HRESULT hr = tryCreate(creationFlags);

    const bool hasVideoSupport =
        (creationFlags & D3D11_CREATE_DEVICE_VIDEO_SUPPORT) != 0;
    const bool hasDebug = (creationFlags & D3D11_CREATE_DEVICE_DEBUG) != 0;

    if (FAILED(hr) && hasVideoSupport) {
        hr = tryCreate(creationFlags & ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
    }
    if (FAILED(hr) && hasDebug) {
        hr = tryCreate(creationFlags & ~D3D11_CREATE_DEVICE_DEBUG);
    }
    if (FAILED(hr) && hasVideoSupport && hasDebug) {
        hr = tryCreate(creationFlags & ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT &
                       ~D3D11_CREATE_DEVICE_DEBUG);
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
    if (!createShader(kComputeShaderCsDotPrefilter,
                      kComputeShaderCsDotPrefilter_Size,
                      m_dotPrefilterShader, "CSDotPrefilter RGBA")) {
        return false;
    }
    if (!createShader(kComputeShaderCsDotPrefilterNv12,
                      kComputeShaderCsDotPrefilterNv12_Size,
                      m_dotPrefilterShaderNV12, "CSDotPrefilter NV12")) {
        return false;
    }
    if (!createShader(kComputeShaderCsDotEdge, kComputeShaderCsDotEdge_Size,
                      m_dotEdgeShader, "CSDotEdge")) {
        return false;
    }
    if (!createShader(kComputeShaderCsRefineContinuity,
                      kComputeShaderCsRefineContinuity_Size,
                      m_refineContinuityShader, "CSRefineContinuity")) {
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
    bufDesc.ByteWidth = sizeof(uint32_t);
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufDesc.StructureByteStride = sizeof(uint32_t);

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
    if (m_context) {
        UINT clearValues[4] = { static_cast<UINT>(kLumMinimumRange), 0, 0, 0 };
        m_context->ClearUnorderedAccessViewUint(m_statsUAV.Get(), clearValues);
    }

    return true;
}

void GpuAsciiRenderer::BuildDotGrid(ID3D11ComputeShader* prefilterShader,
                                    ID3D11ShaderResourceView* source0,
                                    ID3D11ShaderResourceView* source1,
                                    UINT dotDispatchX,
                                    UINT dotDispatchY,
                                    ID3D11Buffer* constantBuffer) {
    ID3D11UnorderedAccessView* nullUAV3[] = { nullptr, nullptr, nullptr };
    ID3D11ShaderResourceView* nullSRV5[] = {
        nullptr, nullptr, nullptr, nullptr, nullptr
    };
    ID3D11Buffer* cbs[] = { constantBuffer };
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };

    m_context->CSSetShader(prefilterShader, nullptr, 0);
    ID3D11ShaderResourceView* prefilterSRVs[] = {
        source0, source1, nullptr, nullptr, nullptr
    };
    m_context->CSSetShaderResources(0, 5, prefilterSRVs);
    ID3D11UnorderedAccessView* prefilterUAVs[] = {
        nullptr, nullptr, m_dotSampleUAV.Get()
    };
    m_context->CSSetUnorderedAccessViews(0, 3, prefilterUAVs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->CSSetSamplers(0, 1, samplers);
    m_context->Dispatch(dotDispatchX, dotDispatchY, 1);

    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV3, nullptr);
    m_context->CSSetShaderResources(0, 5, nullSRV5);

    m_context->CSSetShader(m_dotEdgeShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* edgeSRVs[] = {
        nullptr, nullptr, nullptr, nullptr, m_dotSampleSRV.Get()
    };
    m_context->CSSetShaderResources(0, 5, edgeSRVs);
    ID3D11UnorderedAccessView* edgeUAVs[] = {
        nullptr, nullptr, m_dotEdgeUAV.Get()
    };
    m_context->CSSetUnorderedAccessViews(0, 3, edgeUAVs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->Dispatch(dotDispatchX, dotDispatchY, 1);

    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV3, nullptr);
    m_context->CSSetShaderResources(0, 5, nullSRV5);
}

void GpuAsciiRenderer::RefineOutputAndSyncHistory(UINT dispatchX,
                                                  UINT dispatchY,
                                                  ID3D11Buffer* constantBuffer) {
    ID3D11UnorderedAccessView* nullUAV2[] = { nullptr, nullptr };
    ID3D11ShaderResourceView* nullSRV5[] = {
        nullptr, nullptr, nullptr, nullptr, nullptr
    };

    m_context->CSSetUnorderedAccessViews(0, 2, nullUAV2, nullptr);
    m_context->CSSetShaderResources(0, 5, nullSRV5);

    m_context->CSSetShader(m_refineContinuityShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* refineSRVs[] = {
        nullptr, nullptr, nullptr, m_rawOutputSRV.Get()
    };
    m_context->CSSetShaderResources(0, 4, refineSRVs);
    ID3D11UnorderedAccessView* refineUAVs[] = { m_outputUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 1, refineUAVs, nullptr);
    ID3D11Buffer* cbs[] = { constantBuffer };
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->Dispatch(dispatchX, dispatchY, 1);

    m_context->CSSetUnorderedAccessViews(0, 2, nullUAV2, nullptr);
    m_context->CSSetShaderResources(0, 5, nullSRV5);

    m_context->CSSetShader(m_syncHistoryShader.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* syncUavs[] = {
        m_outputUAV.Get(), m_historyUAV.Get()
    };
    m_context->CSSetUnorderedAccessViews(0, 2, syncUavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->Dispatch(dispatchX, dispatchY, 1);

    m_context->CSSetUnorderedAccessViews(0, 2, nullUAV2, nullptr);
    m_context->CSSetShaderResources(0, 5, nullSRV5);
}

// Resources handled by m_frameCache

bool GpuAsciiRenderer::RenderNV12(const uint8_t* yuv, int width, int height, int stride, int planeHeight, 
                                bool fullRange, YuvMatrix yuvMatrix, YuvTransfer yuvTransfer,
                                bool is10Bit, AsciiArt& out, std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_device) {
        std::string initErr;
        if (!Initialize(&initErr)) {
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
        cb->frameCount = m_frameCount++;
        cb->isFullRange = fullRange ? 1 : 0;
        cb->bitDepth = is10Bit ? 10u : 8u;
        cb->yuvMatrix = static_cast<uint32_t>(yuvMatrix);
        cb->yuvTransfer = static_cast<uint32_t>(yuvTransfer);
        cb->rotationQuarterTurns = 0;
        cb->padding = 0;
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
    UINT dotDispatchX = (outW * 2 + 7) / 8;
    UINT dotDispatchY = (outH * 4 + 7) / 8;

    Microsoft::WRL::ComPtr<ID3D11Query> timingDisjoint;
    Microsoft::WRL::ComPtr<ID3D11Query> timingStart;
    BeginAsciiGpuTiming(timingDisjoint, timingStart);

    BuildDotGrid(m_dotPrefilterShaderNV12.Get(), srvY, srvUV, dotDispatchX,
                 dotDispatchY, m_constantBuffer.Get());

    // 2. Main Pass
    m_context->CSSetShader(m_computeShaderNV12.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {
        nullptr, nullptr, m_statsSRV.Get(), nullptr, m_dotEdgeSRV.Get()
    };
    m_context->CSSetShaderResources(0, 5, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_rawOutputUAV.Get(), m_historyUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
    m_context->CSSetSamplers(0, 1, samplers);

    m_context->Dispatch(dispatchX, dispatchY, 1);

    RefineOutputAndSyncHistory(dispatchX, dispatchY, m_constantBuffer.Get());
    EndAsciiGpuTiming(timingDisjoint, timingStart);

    m_frameCache.MarkFrameInFlight(m_context.Get());

    return ReadOutputBuffer(out, outW, outH);
}

bool GpuAsciiRenderer::CreateBuffers(int width, int height, int outW, int outH) {
    if (m_currentWidth == width && m_currentHeight == height && 
        m_currentOutW == outW && m_currentOutH == outH) {
        return true;
    }

    m_outputBuffer.Reset();
    m_outputUAV.Reset();
    m_rawOutputBuffer.Reset();
    m_rawOutputUAV.Reset();
    m_rawOutputSRV.Reset();
    m_dotSampleBuffer.Reset();
    m_dotSampleUAV.Reset();
    m_dotSampleSRV.Reset();
    m_dotEdgeBuffer.Reset();
    m_dotEdgeUAV.Reset();
    m_dotEdgeSRV.Reset();
    m_statsBuffer.Reset();
    m_statsUAV.Reset();
    m_statsSRV.Reset();
    for (int i = 0; i < kOutputStagingBufferCount; ++i) {
        m_outputStagingBuffers[i].Reset();
        m_outputStagingPending[i] = false;
    }
    m_historyBuffer.Reset();
    m_historyUAV.Reset();
    m_constantBuffer.Reset();
    m_outputStagingReadIndex = 0;
    m_outputStagingWriteIndex = 0;
    m_outputStagingPendingCount = 0;
    m_frameCount = 0;
    m_currentWidth = 0;
    m_currentHeight = 0;
    m_currentOutW = 0;
    m_currentOutH = 0;

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

    // Raw output for the first pass. The refinement pass reads this as SRV and
    // writes the final cells to m_outputBuffer.
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_rawOutputBuffer))) return false;
    if (FAILED(m_device->CreateUnorderedAccessView(m_rawOutputBuffer.Get(), &uavDesc, &m_rawOutputUAV))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC rawSrvDesc = {};
    rawSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    rawSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    rawSrvDesc.Buffer.NumElements = outW * outH;
    if (FAILED(m_device->CreateShaderResourceView(m_rawOutputBuffer.Get(), &rawSrvDesc, &m_rawOutputSRV))) return false;

    const UINT dotCount = static_cast<UINT>(outW * outH * 8);
    bufDesc.ByteWidth = sizeof(GpuAsciiDotSample) * dotCount;
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufDesc.CPUAccessFlags = 0;
    bufDesc.StructureByteStride = sizeof(GpuAsciiDotSample);
    uavDesc.Buffer.NumElements = dotCount;
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_dotSampleBuffer))) return false;
    if (FAILED(m_device->CreateUnorderedAccessView(m_dotSampleBuffer.Get(), &uavDesc, &m_dotSampleUAV))) return false;
    rawSrvDesc.Buffer.NumElements = dotCount;
    if (FAILED(m_device->CreateShaderResourceView(m_dotSampleBuffer.Get(), &rawSrvDesc, &m_dotSampleSRV))) return false;
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_dotEdgeBuffer))) return false;
    if (FAILED(m_device->CreateUnorderedAccessView(m_dotEdgeBuffer.Get(), &uavDesc, &m_dotEdgeUAV))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_dotEdgeBuffer.Get(), &rawSrvDesc, &m_dotEdgeSRV))) return false;

    // History Buffer
    bufDesc.ByteWidth = sizeof(uint32_t) * 2 * outW * outH; // uint2
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufDesc.StructureByteStride = sizeof(uint32_t) * 2;
    uavDesc.Buffer.NumElements = outW * outH;
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_historyBuffer))) return false;
    if (FAILED(m_device->CreateUnorderedAccessView(m_historyBuffer.Get(), &uavDesc, &m_historyUAV))) return false;

    // Clear history initially
    UINT clearVals[4] = {0,0,0,0};
    m_context->ClearUnorderedAccessViewUint(m_historyUAV.Get(), clearVals);

    // Staging buffers for readback.
    bufDesc.Usage = D3D11_USAGE_STAGING;
    bufDesc.BindFlags = 0;
    bufDesc.MiscFlags = 0;
    bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bufDesc.StructureByteStride = 0; // Not needed for staging
    bufDesc.ByteWidth = sizeof(GpuAsciiCell) * outW * outH;

    for (int i = 0; i < kOutputStagingBufferCount; ++i) {
        if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_outputStagingBuffers[i]))) return false;
    }
    m_outputStagingReadIndex = 0;
    m_outputStagingWriteIndex = 0;
    m_outputStagingPendingCount = 0;

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
        if (!Initialize(&initErr)) {
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
        cb->frameCount = m_frameCount++;
        cb->isFullRange = 1; 
        cb->bitDepth = 8;
        cb->yuvMatrix = static_cast<uint32_t>(YuvMatrix::Bt709);
        cb->yuvTransfer = static_cast<uint32_t>(YuvTransfer::Sdr);
        cb->rotationQuarterTurns = 0;
        cb->padding = 0;
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    ID3D11Buffer* cbs[] = { m_constantBuffer.Get() };
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };

    UINT dispatchX = (outW + 7) / 8;
    UINT dispatchY = (outH + 7) / 8;
    UINT dotDispatchX = (outW * 2 + 7) / 8;
    UINT dotDispatchY = (outH * 4 + 7) / 8;

    Microsoft::WRL::ComPtr<ID3D11Query> timingDisjoint;
    Microsoft::WRL::ComPtr<ID3D11Query> timingStart;
    BeginAsciiGpuTiming(timingDisjoint, timingStart);

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

    BuildDotGrid(m_dotPrefilterShader.Get(), inputSRV, nullptr, dotDispatchX,
                 dotDispatchY, m_constantBuffer.Get());

    // Main Pass
    m_context->CSSetShader(m_computeShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {
        nullptr, m_statsSRV.Get(), nullptr, nullptr, m_dotEdgeSRV.Get()
    };
    m_context->CSSetShaderResources(0, 5, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_rawOutputUAV.Get(), m_historyUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->CSSetSamplers(0, 1, samplers);

    m_context->Dispatch(dispatchX, dispatchY, 1);

    RefineOutputAndSyncHistory(dispatchX, dispatchY, m_constantBuffer.Get());
    EndAsciiGpuTiming(timingDisjoint, timingStart);

    m_frameCache.MarkFrameInFlight(m_context.Get());

    return ReadOutputBuffer(out, outW, outH);
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
        const char* textureFmtLabel =
            (srcDesc.Format == DXGI_FORMAT_NV12) ? "NV12" :
            (srcDesc.Format == DXGI_FORMAT_P010) ? "P010" : "OTHER";
        std::snprintf(buf, sizeof(buf), "path=gpu_cache fmt=%s bind=0x%X",
                      textureFmtLabel, static_cast<unsigned int>(srcDesc.BindFlags));
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
        cb->frameCount = m_frameCount++;
        cb->isFullRange = fullRange ? 1 : 0;
        cb->bitDepth = is10Bit ? 10u : 8u;
        cb->yuvMatrix = static_cast<uint32_t>(yuvMatrix);
        cb->yuvTransfer = static_cast<uint32_t>(yuvTransfer);
        cb->rotationQuarterTurns = 0;
        cb->padding = 0;
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
    UINT dotDispatchX = (outW * 2 + 7) / 8;
    UINT dotDispatchY = (outH * 4 + 7) / 8;

    Microsoft::WRL::ComPtr<ID3D11Query> timingDisjoint;
    Microsoft::WRL::ComPtr<ID3D11Query> timingStart;
    BeginAsciiGpuTiming(timingDisjoint, timingStart);

    BuildDotGrid(m_dotPrefilterShaderNV12.Get(), srvY, srvUV, dotDispatchX,
                 dotDispatchY, m_constantBuffer.Get());

    // 2. Main Pass
    m_context->CSSetShader(m_computeShaderNV12.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {
        nullptr, nullptr, m_statsSRV.Get(), nullptr, m_dotEdgeSRV.Get()
    };
    m_context->CSSetShaderResources(0, 5, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_rawOutputUAV.Get(), m_historyUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
    m_context->CSSetSamplers(0, 1, samplers);

    m_context->Dispatch(dispatchX, dispatchY, 1);

    RefineOutputAndSyncHistory(dispatchX, dispatchY, m_constantBuffer.Get());
    EndAsciiGpuTiming(timingDisjoint, timingStart);

    m_frameCache.MarkFrameInFlight(m_context.Get());

    return ReadOutputBuffer(out, outW, outH);
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
    UINT dotDispatchX = (outW * 2 + 7) / 8;
    UINT dotDispatchY = (outH * 4 + 7) / 8;

    Microsoft::WRL::ComPtr<ID3D11Query> timingDisjoint;
    Microsoft::WRL::ComPtr<ID3D11Query> timingStart;

    if (cache.IsYuv()) {
        ID3D11ShaderResourceView* srvY = cache.GetSrvY();
        ID3D11ShaderResourceView* srvUV = cache.GetSrvUV();
        if (!srvY || !srvUV) {
            if (error) *error = "Shared cache missing YUV SRVs";
            return false;
        }
        BeginAsciiGpuTiming(timingDisjoint, timingStart);

        m_lastNv12TexturePath = "shared_cache";
        {
            char buf[128];
            const char* cacheFmtLabel =
                (cache.GetBitDepth() > 8) ? "P010" : "NV12";
            std::snprintf(buf, sizeof(buf), "path=shared_cache fmt=%s",
                          cacheFmtLabel);
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
            cb->frameCount = m_frameCount++;
            cb->isFullRange = cache.IsFullRange() ? 1 : 0;
            cb->bitDepth = static_cast<uint32_t>(cache.GetBitDepth());
            cb->yuvMatrix = static_cast<uint32_t>(cache.GetMatrix());
            cb->yuvTransfer = static_cast<uint32_t>(cache.GetTransfer());
            cb->rotationQuarterTurns =
                static_cast<uint32_t>(cache.GetRotationQuarterTurns() & 3);
            cb->padding = 0;
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

        BuildDotGrid(m_dotPrefilterShaderNV12.Get(), srvY, srvUV,
                     dotDispatchX, dotDispatchY, m_constantBuffer.Get());

        m_context->CSSetShader(m_computeShaderNV12.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = {
            nullptr, nullptr, m_statsSRV.Get(), nullptr, m_dotEdgeSRV.Get()
        };
        m_context->CSSetShaderResources(0, 5, srvs);
        ID3D11UnorderedAccessView* uavs[] = { m_rawOutputUAV.Get(), m_historyUAV.Get() };
        m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        m_context->CSSetSamplers(0, 1, samplers);
        m_context->Dispatch(dispatchX, dispatchY, 1);

        RefineOutputAndSyncHistory(dispatchX, dispatchY, m_constantBuffer.Get());
    } else if (cache.IsRgba()) {
        ID3D11ShaderResourceView* inputSRV = cache.GetSrvRGBA();
        if (!inputSRV) {
            if (error) *error = "Shared cache missing RGBA SRV";
            return false;
        }
        BeginAsciiGpuTiming(timingDisjoint, timingStart);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            Constants* cb = (Constants*)mapped.pData;
            cb->width = width;
            cb->height = height;
            cb->outWidth = outW;
            cb->outHeight = outH;
            cb->time = 0.0f;
            cb->frameCount = m_frameCount++;
            cb->isFullRange = 1;
            cb->bitDepth = 8;
            cb->yuvMatrix = static_cast<uint32_t>(YuvMatrix::Bt709);
            cb->yuvTransfer = static_cast<uint32_t>(YuvTransfer::Sdr);
            cb->rotationQuarterTurns =
                static_cast<uint32_t>(cache.GetRotationQuarterTurns() & 3);
            cb->padding = 0;
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

        BuildDotGrid(m_dotPrefilterShader.Get(), inputSRV, nullptr,
                     dotDispatchX, dotDispatchY, m_constantBuffer.Get());

        m_context->CSSetShader(m_computeShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = {
            nullptr, m_statsSRV.Get(), nullptr, nullptr, m_dotEdgeSRV.Get()
        };
        m_context->CSSetShaderResources(0, 5, srvs);
        ID3D11UnorderedAccessView* uavs[] = { m_rawOutputUAV.Get(), m_historyUAV.Get() };
        m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, cbs);
        m_context->CSSetSamplers(0, 1, samplers);
        m_context->Dispatch(dispatchX, dispatchY, 1);

        RefineOutputAndSyncHistory(dispatchX, dispatchY, m_constantBuffer.Get());
    } else {
        if (error) *error = "Shared cache format unsupported";
        return false;
    }

    EndAsciiGpuTiming(timingDisjoint, timingStart);

    cache.MarkFrameInFlight(m_context.Get());

    return ReadOutputBuffer(out, outW, outH);
}
