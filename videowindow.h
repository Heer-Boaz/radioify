#pragma once

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <memory>

#include "videocolor.h"
#include "consoleinput.h"
#include <vector>
#include <mutex>

struct WindowUiState {
    float progress = 0.0f;
    float overlayAlpha = 0.0f;
    bool isPaused = false;
};

class VideoWindow {
public:
    VideoWindow();
    ~VideoWindow();

    bool Open(int width, int height, const std::string& title);
    void Close();

    void Present(ID3D11Texture2D* texture, int arrayIndex, int width, int height,
                 bool fullRange, YuvMatrix matrix, YuvTransfer transfer, int bitDepth,
                 const WindowUiState& ui);
    bool IsOpen() const { return m_hWnd != nullptr; }
    bool IsVisible() const { return m_hWnd && IsWindowVisible(m_hWnd); }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    void ShowWindow(bool show);
    void PollEvents();
    bool PollInput(InputEvent& ev);

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    bool CreateSwapChain(int width, int height);
    void Resize(int width, int height);

    HWND m_hWnd = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_uiShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_copyTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_copySrvY;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_copySrvUV;
    
    int m_width = 0;
    int m_height = 0;

    std::vector<InputEvent> m_inputQueue;
    std::mutex m_inputMutex;
};
