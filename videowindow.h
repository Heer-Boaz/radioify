#pragma once

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <string>
#include <memory>

#include "videocolor.h"
#include "consoleinput.h"
#include "videoprocessor.h"
#include <vector>
#include <mutex>

struct WindowUiState {
    float progress = 0.0f;
    float overlayAlpha = 0.0f;
    bool isPaused = false;
    bool vsyncEnabled = true;
    // UI text/metadata to display when overlay is visible
    std::string title; // filename or label
    double displaySec = 0.0; // current time shown in overlay
    double totalSec = -1.0; // total duration (or -1 if unknown)
    int volPct = 0; // volume percent for display
};

struct IDXGISwapChain2;

class VideoWindow {
public:
    VideoWindow();
    ~VideoWindow();

    bool Open(int width, int height, const std::string& title);
    void Close();

    void Present(GpuVideoFrameCache& frameCache, const WindowUiState& ui);
    // Render the UI overlay using the last cached video frame as background
    void PresentOverlay(GpuVideoFrameCache& frameCache, const WindowUiState& ui);
    void PresentBackbuffer();
    void WaitForFrameLatency(DWORD timeoutMs);
    void SetVsync(bool enabled);
    
    bool IsOpen() const { return m_hWnd != nullptr; }
    bool IsVisible() const { return m_hWnd && IsWindowVisible(m_hWnd); }
    bool IsVsyncEnabled() const {
        return m_presentInterval.load(std::memory_order_relaxed) != 0;
    }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    // Get viewport geometry for mouse coordinate mapping (used for seeking in window mode)
    void GetViewportGeometry(float& outViewX, float& outViewY, float& outViewW, float& outViewH) const {
        outViewX = m_viewportX;
        outViewY = m_viewportY;
        outViewW = m_viewportW;
        outViewH = m_viewportH;
    }
    void ShowWindow(bool show);
    void PollEvents();
    bool PollInput(InputEvent& ev);
    void Cleanup();

private:
    void DrawOverlay(const WindowUiState& ui);
    void UpdateViewport(int width, int height);
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    bool CreateSwapChain(int width, int height);
    void ResetSwapChain();
    void Resize(int width, int height);

    HWND m_hWnd = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> m_swapChain2;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_uiShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
    std::atomic<UINT> m_presentInterval{1};
    HANDLE m_frameLatencyWaitableObject = nullptr;
    std::mutex m_frameLatencyMutex;

    // frame cache is owned and managed externally by videoplayback

    // Text overlay texture (CPU rasterized via GDI)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_textTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_textSrv;
    int m_textWidth = 0;
    int m_textHeight = 0;
    
    int m_width = 0;
    int m_height = 0;
    int m_videoWidth = 1280;
    int m_videoHeight = 720;
    
    // Viewport geometry (for mouse coordinate mapping)
    float m_viewportX = 0.0f;
    float m_viewportY = 0.0f;
    float m_viewportW = 0.0f;
    float m_viewportH = 0.0f;

    std::vector<InputEvent> m_inputQueue;
    std::mutex m_inputMutex;
    // Cache last window title to avoid repeated SetWindowText calls
    std::string m_lastWindowTitle;
    std::string m_baseWindowTitle;

    // Fullscreen helpers
    LONG m_prevStyle = 0;
    LONG m_prevExStyle = 0; // saved extended style for restoring on exit
    RECT m_prevRect{};
    bool m_isFullscreen = false;
    // Guard to ignore WM_SIZE events while we're transitioning to/from fullscreen
    bool m_ignoreWindowSizeEvents = false;
    // When true, Present should skip until the render-target view and sizes are ready
    bool m_waitingForRenderTarget = false;
    bool MakeFullscreen();
    bool ExitFullscreen();
};
