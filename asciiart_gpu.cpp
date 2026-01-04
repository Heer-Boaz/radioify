#include "asciiart_gpu.h"
#include <d3d11.h>
#include <vector>

#pragma comment(lib, "d3d11.lib")

namespace {
    // Precompiled shader bytecode (generated from shaders/*.hlsl)
    #include "asciiart_gpu_cs_main.h"
    #include "asciiart_gpu_cs_main_nv12.h"
    #include "asciiart_gpu_cs_detail.h"
    #include "asciiart_gpu_cs_detail_nv12.h"
    #include "asciiart_stats_cs.h"

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
    if (!createShader(kComputeShaderCsDetail, kComputeShaderCsDetail_Size,
                      m_detailShader, "CSDetail RGBA")) {
        return false;
    }
    if (!createShader(kComputeShaderCsDetailNv12, kComputeShaderCsDetailNv12_Size,
                      m_detailShaderNV12, "CSDetail NV12")) {
        return false;
    }
    if (!createShader(kStatsShaderCs, kStatsShaderCs_Size, m_statsShader,
                      "CSStats")) {
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

bool GpuAsciiRenderer::CreateRGBATextures(int width, int height) {
    if (m_inputTexture) {
        D3D11_TEXTURE2D_DESC desc;
        m_inputTexture->GetDesc(&desc);
        if (desc.Width == width && desc.Height == height) {
            return true;
        }
        m_inputTexture.Reset();
        m_inputSRV.Reset();
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 0; // Full chain
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, &m_inputTexture))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_inputTexture.Get(), nullptr, &m_inputSRV))) return false;

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
                                bool fullRange, YuvMatrix yuvMatrix, YuvTransfer yuvTransfer,
                                bool is10Bit, AsciiArt& out, std::string* error) {
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
        cb->bitDepth = is10Bit ? 10u : 8u;
        cb->yuvMatrix = static_cast<uint32_t>(yuvMatrix);
        cb->yuvTransfer = static_cast<uint32_t>(yuvTransfer);
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

    UINT dispatchX = (outW + 7) / 8;
    UINT dispatchY = (outH + 7) / 8;

    // 2. Detail Pass (per-cell signal strength)
    m_context->CSSetShader(m_detailShaderNV12.Get(), nullptr, 0);
    ID3D11ShaderResourceView* detailSrvs[] = { m_srvY.Get(), m_srvUV.Get(), m_statsSRV.Get(), nullptr };
    m_context->CSSetShaderResources(0, 4, detailSrvs);
    ID3D11UnorderedAccessView* detailUavs[] = { nullptr, nullptr, m_signalUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 3, detailUavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);

    ID3D11SamplerState* samplers[] = { m_linearSampler.Get(), m_pointSampler.Get() };
    m_context->CSSetSamplers(0, 2, samplers);

    m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind detail pass resources
    ID3D11UnorderedAccessView* nullUAV2[] = { nullptr, nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV2, nullptr);
    ID3D11ShaderResourceView* nullSRV2[] = { nullptr, nullptr, nullptr, nullptr };
    m_context->CSSetShaderResources(0, 4, nullSRV2);

    // 3. Main Pass
    m_context->CSSetShader(m_computeShaderNV12.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { m_srvY.Get(), m_srvUV.Get(), m_statsSRV.Get(), m_signalSRV.Get() };
    m_context->CSSetShaderResources(0, 4, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get(), m_historyUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->CSSetSamplers(0, 2, samplers);

    m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind
    ID3D11UnorderedAccessView* nullUAV3[] = { nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 2, nullUAV3, nullptr);
    ID3D11ShaderResourceView* nullSRV3[] = { nullptr, nullptr, nullptr, nullptr };
    m_context->CSSetShaderResources(0, 4, nullSRV3);

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

    // Signal Buffer (per-cell strength for spatial smoothing)
    bufDesc.ByteWidth = sizeof(uint32_t) * outW * outH;
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufDesc.CPUAccessFlags = 0;
    bufDesc.StructureByteStride = sizeof(uint32_t);
    if (FAILED(m_device->CreateBuffer(&bufDesc, nullptr, &m_signalBuffer))) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC sigUavDesc = {};
    sigUavDesc.Format = DXGI_FORMAT_UNKNOWN;
    sigUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    sigUavDesc.Buffer.NumElements = outW * outH;
    if (FAILED(m_device->CreateUnorderedAccessView(m_signalBuffer.Get(), &sigUavDesc, &m_signalUAV))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sigSrvDesc = {};
    sigSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    sigSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sigSrvDesc.Buffer.NumElements = outW * outH;
    if (FAILED(m_device->CreateShaderResourceView(m_signalBuffer.Get(), &sigSrvDesc, &m_signalSRV))) return false;

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

    if (!CreateRGBATextures(width, height)) {
        if (error) *error = "Failed to create RGBA textures";
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

    // Upload Stats to StatsBuffer
    if (!CreateStatsBuffer()) {
        if (error) *error = "Failed to create stats buffer";
        return false;
    }
    
    uint32_t statsData[4] = { (uint32_t)bgLum, (uint32_t)lumRange, 0, 0 };
    m_context->UpdateSubresource(m_statsBuffer.Get(), 0, nullptr, statsData, 0, 0);

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
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get(), m_pointSampler.Get() };

    UINT dispatchX = (outW + 7) / 8;
    UINT dispatchY = (outH + 7) / 8;

    // Detail Pass (per-cell signal strength)
    m_context->CSSetShader(m_detailShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* detailSrvs[] = { m_inputSRV.Get(), m_statsSRV.Get(), nullptr, nullptr };
    m_context->CSSetShaderResources(0, 4, detailSrvs);
    ID3D11UnorderedAccessView* detailUavs[] = { nullptr, nullptr, m_signalUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 3, detailUavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->CSSetSamplers(0, 2, samplers);

    m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind detail pass resources
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr, nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 3, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr, nullptr, nullptr, nullptr };
    m_context->CSSetShaderResources(0, 4, nullSRV);

    // Main Pass
    m_context->CSSetShader(m_computeShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { m_inputSRV.Get(), m_statsSRV.Get(), nullptr, m_signalSRV.Get() };
    m_context->CSSetShaderResources(0, 4, srvs);
    ID3D11UnorderedAccessView* uavs[] = { m_outputUAV.Get(), m_historyUAV.Get() };
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    m_context->CSSetConstantBuffers(0, 1, cbs);
    m_context->CSSetSamplers(0, 2, samplers);

    m_context->Dispatch(dispatchX, dispatchY, 1);

    // Unbind UAVs
    ID3D11UnorderedAccessView* nullUAV2[] = { nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 2, nullUAV2, nullptr);
    ID3D11ShaderResourceView* nullSRV2[] = { nullptr, nullptr, nullptr, nullptr };
    m_context->CSSetShaderResources(0, 4, nullSRV2);

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
