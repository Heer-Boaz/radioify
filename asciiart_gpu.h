#ifndef ASCIIART_GPU_H
#define ASCIIART_GPU_H

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <d3d11.h>
#include <wrl/client.h>
#include "asciiart.h" // For AsciiArt struct

class GpuAsciiRenderer {
public:
    GpuAsciiRenderer();
    ~GpuAsciiRenderer();

    bool Initialize(int maxWidth, int maxHeight);
    
    // Render from RGBA buffer (CPU -> GPU -> CPU)
    bool Render(const uint8_t* rgba, int width, int height, AsciiArt& out, std::string* error);

private:
    bool CreateDevice();
    bool CompileComputeShader();
    bool CreateBuffers(int width, int height, int outW, int outH);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_computeShader;

    // Resources
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_inputTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_inputSRV;
    
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_outputBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_outputUAV;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_outputStagingBuffer;

    // History for temporal stability
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_historyBuffer;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_historyUAV;

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;

    struct Constants {
        uint32_t width;
        uint32_t height;
        uint32_t outWidth;
        uint32_t outHeight;
        float time;
        uint32_t frameCount;
        uint32_t bgLum; // Background luminance estimate (0-255)
        uint32_t padding;
    };

    int m_currentWidth = 0;
    int m_currentHeight = 0;
    int m_currentOutW = 0;
    int m_currentOutH = 0;
};

#endif // ASCIIART_GPU_H
