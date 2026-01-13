#include "videoprocessor.h"
#include <algorithm>

VideoViewport calculateViewport(int windowW, int windowH, int videoW, int videoH) {
    if (windowW <= 0 || windowH <= 0 || videoW <= 0 || videoH <= 0) {
        return { 0.0f, 0.0f, static_cast<float>(windowW), static_cast<float>(windowH) };
    }

    float windowAspect = (float)windowW / (float)windowH;
    float videoAspect = (float)videoW / (float)videoH;

    VideoViewport vp;
    if (windowAspect > videoAspect) {
        vp.h = (float)windowH;
        vp.w = vp.h * videoAspect;
        vp.x = (windowW - vp.w) / 2.0f;
        vp.y = 0;
    } else {
        vp.w = (float)windowW;
        vp.h = vp.w / videoAspect;
        vp.x = 0;
        vp.y = (windowH - vp.h) / 2.0f;
    }
    return vp;
}

bool GpuVideoFrameCache::Update(ID3D11Device* device, ID3D11DeviceContext* context,
                                ID3D11Texture2D* texture, int arrayIndex, int width, int height,
                                bool fullRange, YuvMatrix matrix, YuvTransfer transfer, int bitDepth) {
    if (!texture) return false;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    if (desc.Format == DXGI_FORMAT_NV12 || desc.Format == DXGI_FORMAT_P010) {
        if (!EnsureNV12(device, width, height, bitDepth)) return false;

        m_width = width;
        m_height = height;
        m_bitDepth = bitDepth;
        m_fullRange = fullRange;
        m_matrix = matrix;
        m_transfer = transfer;

        // Copy the planar texture planes. NV12/P010 have 2 planes (Y and UV).
        // In DX11, these are represented as separate subresources.
        context->CopySubresourceRegion(m_texYuv.Get(), 0, 0, 0, 0, texture, D3D11CalcSubresource(0, arrayIndex, desc.MipLevels), nullptr);
        context->CopySubresourceRegion(m_texYuv.Get(), 1, 0, 0, 0, texture, D3D11CalcSubresource(1, arrayIndex, desc.MipLevels), nullptr);
        
        m_srvRGBA.Reset();
        m_texRGBA.Reset();
    } else {
        // RGBA path
        if (!EnsureRGBA(device, width, height)) return false;
        
        m_width = width;
        m_height = height;

        D3D11_BOX srcBox;
        srcBox.left = 0; srcBox.top = 0; srcBox.front = 0;
        srcBox.right = width; srcBox.bottom = height; srcBox.back = 1;
        context->CopySubresourceRegion(m_texRGBA.Get(), 0, 0, 0, 0, texture, D3D11CalcSubresource(0, arrayIndex, desc.MipLevels), &srcBox);
        
        m_srvY.Reset(); m_srvUV.Reset();
        m_texYuv.Reset();
    }

    return true;
}

bool GpuVideoFrameCache::Update(ID3D11Device* device, ID3D11DeviceContext* context,
                                const uint8_t* rgba, int stride, int width, int height) {
    if (!EnsureRGBA(device, width, height)) return false;
    context->UpdateSubresource(m_texRGBA.Get(), 0, nullptr, rgba, stride, 0);
    
    m_width = width;
    m_height = height;
    m_srvY.Reset(); m_srvUV.Reset();
    m_texYuv.Reset();
    return true;
}

bool GpuVideoFrameCache::UpdateNV12(ID3D11Device* device, ID3D11DeviceContext* context,
                                    const uint8_t* yuv, int stride, int planeHeight, int width, int height,
                                    bool fullRange, YuvMatrix matrix, YuvTransfer transfer, int bitDepth) {
    if (!EnsureNV12(device, width, height, bitDepth)) return false;

    // Update the planes separately
    context->UpdateSubresource(m_texYuv.Get(), 0, nullptr, yuv, stride, 0);
    context->UpdateSubresource(m_texYuv.Get(), 1, nullptr, yuv + (stride * planeHeight), stride, 0);

    m_width = width;
    m_height = height;
    m_fullRange = fullRange;
    m_matrix = matrix;
    m_transfer = transfer;
    m_bitDepth = bitDepth;
    
    m_srvRGBA.Reset();
    m_texRGBA.Reset();
    return true;
}

bool GpuVideoFrameCache::EnsureNV12(ID3D11Device* device, int width, int height, int bitDepth) {
    if (m_texYuv && m_width == width && m_height == height && m_bitDepth == bitDepth && m_srvY) return true;

    m_texYuv.Reset(); m_srvY.Reset(); m_srvUV.Reset();
    
    DXGI_FORMAT yuvFormat = (bitDepth > 8) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = yuvFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, m_texYuv.GetAddressOf()))) return false;

    // Create SRV for Y plane
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = (bitDepth > 8) ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(m_texYuv.Get(), &srvDesc, m_srvY.GetAddressOf()))) return false;

    // Create SRV for UV plane
    srvDesc.Format = (bitDepth > 8) ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(device->CreateShaderResourceView(m_texYuv.Get(), &srvDesc, m_srvUV.GetAddressOf()))) return false;

    return true;
}

bool GpuVideoFrameCache::EnsureRGBA(ID3D11Device* device, int width, int height) {
    if (m_texRGBA && m_width == width && m_height == height && m_srvRGBA) return true;

    m_texRGBA.Reset(); m_srvRGBA.Reset();
    
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, m_texRGBA.GetAddressOf()))) return false;
    if (FAILED(device->CreateShaderResourceView(m_texRGBA.Get(), nullptr, m_srvRGBA.GetAddressOf()))) return false;

    return true;
}

void GpuVideoFrameCache::Reset() {
    m_texYuv.Reset(); m_texRGBA.Reset();
    m_srvY.Reset(); m_srvUV.Reset(); m_srvRGBA.Reset();
}
