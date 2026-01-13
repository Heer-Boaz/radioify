#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include "videocolor.h"

// Shared viewport calculation logic
struct VideoViewport {
    float x, y, w, h;
};

VideoViewport calculateViewport(int windowW, int windowH, int videoW, int videoH);

// Shared GPU frame management and caching
class GpuVideoFrameCache {
public:
    GpuVideoFrameCache() = default;
    ~GpuVideoFrameCache() = default;

    // Update from an existing GPU texture
    bool Update(ID3D11Device* device, ID3D11DeviceContext* context,
                ID3D11Texture2D* texture, int arrayIndex, int width, int height,
                bool fullRange, YuvMatrix matrix, YuvTransfer transfer, int bitDepth);

    // Update from an RGBA CPU buffer
    bool Update(ID3D11Device* device, ID3D11DeviceContext* context,
                const uint8_t* rgba, int stride, int width, int height);

    // Update from an NV12/P010 CPU YUV buffer
    bool UpdateNV12(ID3D11Device* device, ID3D11DeviceContext* context,
                    const uint8_t* yuv, int stride, int planeHeight, int width, int height,
                    bool fullRange, YuvMatrix matrix, YuvTransfer transfer, int bitDepth);

    void Reset();

    bool HasFrame() const { return m_srvY != nullptr || m_srvRGBA != nullptr; }

    ID3D11ShaderResourceView* GetSrvY() const { return m_srvY.Get(); }
    ID3D11ShaderResourceView* GetSrvUV() const { return m_srvUV.Get(); }
    ID3D11ShaderResourceView* GetSrvRGBA() const { return m_srvRGBA.Get(); }

    bool IsYuv() const { return m_srvY != nullptr; }
    bool IsRgba() const { return m_srvRGBA != nullptr; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    bool IsFullRange() const { return m_fullRange; }
    YuvMatrix GetMatrix() const { return m_matrix; }
    YuvTransfer GetTransfer() const { return m_transfer; }
    int GetBitDepth() const { return m_bitDepth; }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texYuv; // Planar (NV12/P010) or first plane
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texRGBA;
    
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srvY;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srvUV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srvRGBA;

    int m_width = 0;
    int m_height = 0;
    bool m_fullRange = false;
    YuvMatrix m_matrix = YuvMatrix::Bt709;
    YuvTransfer m_transfer = YuvTransfer::Sdr;
    int m_bitDepth = 8;

    bool EnsureNV12(ID3D11Device* device, int width, int height, int bitDepth);
    bool EnsureRGBA(ID3D11Device* device, int width, int height);
};
