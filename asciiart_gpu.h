#ifndef ASCIIART_GPU_H
#define ASCIIART_GPU_H

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <d3d11.h>
#include "videoprocessor.h"
#include "asciiart.h"

class GpuAsciiRenderer {
public:
    GpuAsciiRenderer();
    ~GpuAsciiRenderer();

    bool Initialize(int maxWidth, int maxHeight, std::string* error = nullptr);
    
    // Initialize with an existing D3D11 device (for device sharing with video decoder)
    bool InitializeWithDevice(ID3D11Device* device, ID3D11DeviceContext* context,
                              int maxWidth, int maxHeight, std::string* error = nullptr);
    
    // Get the D3D11 device (for sharing with video decoder)
    ID3D11Device* device() const { return m_device.Get(); }
    ID3D11DeviceContext* context() const { return m_context.Get(); }
    
    // Render from RGBA buffer (CPU -> GPU -> CPU)
    bool Render(const uint8_t* rgba, int width, int height, AsciiArt& out, std::string* error);

    // Render from NV12 buffer (CPU -> GPU -> CPU)
    bool RenderNV12(const uint8_t* yuv, int width, int height, int stride, int planeHeight, 
                   bool fullRange, YuvMatrix yuvMatrix, YuvTransfer yuvTransfer,
                   bool is10Bit, AsciiArt& out, std::string* error);
    
    // Render directly from D3D11 NV12 texture (zero-copy GPU path)
    // The texture must be on the same device as this renderer
    bool RenderNV12Texture(ID3D11Texture2D* texture, int arrayIndex,
                           int width, int height,
                           bool fullRange, YuvMatrix yuvMatrix, YuvTransfer yuvTransfer,
                           bool is10Bit, AsciiArt& out, std::string* error);

    const char* lastNv12TexturePath() const { return m_lastNv12TexturePath; }
    const std::string& lastNv12TextureDetail() const {
        return m_lastNv12TextureDetail;
    }

    void ClearHistory();

private:
    bool CreateDevice();
    bool CreateDeviceWithFlags(UINT flags);
    bool CreateComputeShaders(std::string* error);
    bool CreateBuffers(int width, int height, int outW, int outH);
    bool CreateStatsBuffer();
    
    // Shared rendering logic (called after textures are set up)
    bool RenderNV12Internal(int width, int height, bool fullRange, 
                            YuvMatrix yuvMatrix, YuvTransfer yuvTransfer,
                            bool is10Bit, AsciiArt& out, std::string* error);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_computeShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_computeShaderNV12;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_statsShaderRgba;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_statsShaderNv12;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_bgClampShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_syncHistoryShader;

    GpuVideoFrameCache m_frameCache;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_linearSampler;

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_outputBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_outputUAV;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_outputStagingBuffers[2];
    int m_outputStagingIndex = 0;
    bool m_outputStagingPrimed = false;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_metaBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_metaUAV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_metaSRV;

    // Stats Buffer
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_statsBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_statsUAV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_statsSRV;

    // History for temporal stability
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_historyBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_historyUAV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_historySRV;

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;

    struct Constants {
        uint32_t width;
        uint32_t height;
        uint32_t outWidth;
        uint32_t outHeight;
        float time;
        uint32_t frameCount;
        uint32_t isFullRange;
        uint32_t bitDepth;
        uint32_t yuvMatrix;
        uint32_t yuvTransfer;
        uint32_t padding[2];
    };

    int m_currentWidth = 0;
    int m_currentHeight = 0;
    int m_currentOutW = 0;
    int m_currentOutH = 0;
    const char* m_lastNv12TexturePath = "unknown";
    std::string m_lastNv12TextureDetail;
};

#endif // ASCIIART_GPU_H
