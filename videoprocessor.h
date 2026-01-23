#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
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
    void MarkFrameInFlight(ID3D11DeviceContext* context);

    bool HasFrame() const { return m_format != CacheFormat::None; }

    ID3D11ShaderResourceView* GetSrvY() const {
        return (m_format == CacheFormat::Yuv) ? m_srvY[m_activeIndex].Get() : nullptr;
    }
    ID3D11ShaderResourceView* GetSrvUV() const {
        return (m_format == CacheFormat::Yuv) ? m_srvUV[m_activeIndex].Get() : nullptr;
    }
    ID3D11ShaderResourceView* GetSrvRGBA() const {
        return (m_format == CacheFormat::Rgba) ? m_srvRGBA[m_activeIndex].Get() : nullptr;
    }

    bool IsYuv() const { return m_format == CacheFormat::Yuv; }
    bool IsRgba() const { return m_format == CacheFormat::Rgba; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    bool IsFullRange() const { return m_fullRange; }
    YuvMatrix GetMatrix() const { return m_matrix; }
    YuvTransfer GetTransfer() const { return m_transfer; }
    int GetBitDepth() const { return m_bitDepth; }

private:
    static constexpr int kFrameBufferCount = 4;
    enum class CacheFormat {
        None,
        Yuv,
        Rgba,
    };
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, kFrameBufferCount> m_texYuv; // Planar (NV12/P010) or first plane
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, kFrameBufferCount> m_texRGBA;
    
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, kFrameBufferCount> m_srvY;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, kFrameBufferCount> m_srvUV;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, kFrameBufferCount> m_srvRGBA;
    std::array<Microsoft::WRL::ComPtr<ID3D11Query>, kFrameBufferCount> m_gpuDone;
    std::array<bool, kFrameBufferCount> m_gpuInFlight{};

#if defined(RADIOIFY_ENABLE_STAGING_UPLOAD)
    // Staging resources used by the upload path to avoid driver implicit syncs
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingYuv; // same format as m_texYuv (NV12/P010), usage=STAGING
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingRGBA;
    bool EnsureStagingNV12(ID3D11Device* device, int width, int height, int bitDepth);
    bool EnsureStagingRGBA(ID3D11Device* device, int width, int height);
    bool UploadNV12ToDefaultViaStaging(ID3D11DeviceContext* context, int dstIndex, const uint8_t* yuv, int stride, int planeHeight, int width, int height);
    bool UploadRGBAToDefaultViaStaging(ID3D11DeviceContext* context, int dstIndex, const uint8_t* rgba, int stride, int width, int height);
#endif

    int m_width = 0;
    int m_height = 0;
    bool m_fullRange = false;
    YuvMatrix m_matrix = YuvMatrix::Bt709;
    YuvTransfer m_transfer = YuvTransfer::Sdr;
    int m_bitDepth = 8;
    int m_activeIndex = 0;
    int m_writeIndex = 0;
    CacheFormat m_format = CacheFormat::None;

    bool EnsureGpuQueries(ID3D11Device* device);
    bool IsBufferReady(ID3D11DeviceContext* context, int index);
    int AcquireWriteIndex(ID3D11DeviceContext* context);
    bool EnsureNV12(ID3D11Device* device, int width, int height, int bitDepth);
    bool EnsureRGBA(ID3D11Device* device, int width, int height);
};
