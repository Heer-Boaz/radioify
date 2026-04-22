#pragma once

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <memory>

#include "videocolor.h"
#include "consoleinput.h"
#include "consolescreen.h"
#include "gpu_text_grid.h"
#include "videoprocessor.h"
#include "video_output_color.h"
#include "subtitle_font_attachments.h"
#include <vector>
#include <mutex>

struct WindowUiState {
    struct ControlButton {
        std::string text;
        bool active = false;
        bool hovered = false;
    };

    struct SubtitleCue {
        struct TextRun {
            std::string text;
            bool hasPrimaryColor = false;
            Color primaryColor{255, 255, 255};
            float primaryAlpha = 1.0f;
            bool hasBackColor = false;
            Color backColor{0, 0, 0};
            float backAlpha = 0.55f;
        };

        std::string text;
        std::string rawText;
        std::vector<TextRun> textRuns;
        bool hasTransform = false;
        float sizeScale = 1.0f;
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        std::string fontName;
        bool bold = false;
        bool italic = false;
        bool underline = false;
        bool assStyled = false;
        bool hasPrimaryColor = false;
        Color primaryColor{255, 255, 255};
        float primaryAlpha = 1.0f;
        bool hasBackColor = false;
        Color backColor{0, 0, 0};
        float backAlpha = 0.55f;
        int64_t startUs = 0;
        int64_t endUs = 0;
        int alignment = 2;  // ASS alignment (1..9), default bottom-center.
        int layer = 0;
        bool hasPosition = false;
        float posX = 0.5f;  // normalized in video viewport
        float posY = 0.9f;  // normalized in video viewport
        bool hasClip = false;
        bool inverseClip = false;
        float clipX1 = 0.0f;
        float clipY1 = 0.0f;
        float clipX2 = 1.0f;
        float clipY2 = 1.0f;
        float marginVNorm = 0.0f;
        float marginLNorm = 0.0f;
        float marginRNorm = 0.0f;
    };

    float progress = 0.0f;
    float overlayAlpha = 0.0f;
    bool isPaused = false;
    // UI text/metadata to display when overlay is visible
    std::string title; // filename or label
    std::string progressSuffix; // playback time/volume line
    std::vector<ControlButton> controlButtons;
    std::vector<SubtitleCue> subtitleCues; // active subtitle cues for current frame
    int64_t subtitleClockUs = 0;
    std::shared_ptr<const std::string> subtitleAssScript;
    std::shared_ptr<const SubtitleFontAttachmentList> subtitleAssFonts;
    std::string subtitleRenderError;
    double displaySec = 0.0; // current time shown in overlay
    double totalSec = -1.0; // total duration (or -1 if unknown)
    int volPct = 0; // volume percent for display
    std::string subtitle; // current subtitle cue text
    float subtitleAlpha = 0.0f; // subtitle opacity
    std::vector<std::string> debugLines;
};

struct IDXGISwapChain2;
struct ShaderConstants;

namespace videowindow_file_drop {
class DropTargetRegistration;
}

class VideoWindow {
public:
    VideoWindow();
    ~VideoWindow();

    bool Open(int width, int height, const std::string& title,
              bool startFullscreen = true);
    void Close();

    void Present(GpuVideoFrameCache& frameCache, const WindowUiState& ui,
                 bool nonBlocking);
    // Render the UI overlay using the last cached video frame as background
    void PresentOverlay(GpuVideoFrameCache& frameCache, const WindowUiState& ui,
                        bool nonBlocking);
    // Render a full-screen text grid (TUI) into the window backbuffer.
    void PresentTextGrid(const std::vector<ScreenCell>& cells, int cols, int rows,
                         bool nonBlocking);
    void PresentGpuTextGrid(const GpuTextGridFrame& frame, bool nonBlocking);
    void PresentBackbuffer();
    HANDLE GetFrameLatencyWaitableObject();
    void SetVsync(bool enabled);
    std::string GetSubtitleRenderError() const;
    void SetCaptureAllMouseInput(bool enabled) { m_captureAllMouseInput = enabled; }
    void SetPictureInPictureInteractiveRects(const std::vector<RECT>& rects);
    void SetTextGridMinimumSize(int cols, int rows);
    void SetCursorVisible(bool visible);
    bool TogglePictureInPicture();
    bool SetPictureInPicture(bool enabled);
    bool ExitPictureInPictureToFullscreen();
    void SetTextGridPresentationEnabled(bool enabled);
    bool IsTextGridPresentationEnabled() const {
        return m_textGridPresentationEnabled.load(std::memory_order_relaxed);
    }
    bool IsPictureInPicture() const {
        return m_pictureInPicture.load(std::memory_order_relaxed);
    }
    bool IsFullscreen() const { return m_isFullscreen; }
    bool PictureInPictureRestoresFullscreen() const {
        return m_pipRestoreFullscreen;
    }
    bool SetFullscreen(bool enabled);
    void GetTextGridSize(int& outCols, int& outRows) const {
        outCols = m_textGridCols.load(std::memory_order_relaxed);
        outRows = m_textGridRows.load(std::memory_order_relaxed);
    }
    void GetTextGridCellSize(int& outCellWidth,
                                         int& outCellHeight) const;
    bool GetWindowBounds(RECT* outRect) const;
    bool GetPictureInPictureRestoreBounds(RECT* outRect) const;
    bool SetWindowBounds(const RECT& rect);
    
    bool IsOpen() const { return m_hWnd != nullptr; }
    bool IsVisible() const { return m_hWnd && IsWindowVisible(m_hWnd); }
    bool IsVsyncEnabled() const {
        return m_presentInterval.load(std::memory_order_relaxed) != 0;
    }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    std::string OutputColorDebugLine() const;
    bool OutputUsesHdr() const;
    // Get viewport geometry for mouse coordinate mapping (used for seeking in window mode)
    void GetViewportGeometry(float& outViewX, float& outViewY, float& outViewW, float& outViewH) const {
        outViewX = m_viewportX;
        outViewY = m_viewportY;
        outViewW = m_viewportW;
        outViewH = m_viewportH;
    }
    void ShowWindow(bool show);
    bool PollEvents();
    bool PollInput(InputEvent& ev);
    bool ConsumeCloseRequested();
    HANDLE CloseRequestedWaitHandle() const { return m_closeRequestedEvent; }
    void Cleanup();

private:
    void DrawOverlay(const WindowUiState& ui);
    void UpdateViewport(int width, int height);
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    bool CreateSwapChain(int width, int height);
    void ResetSwapChain();
    void Resize(int width, int height);
    RECT CalculatePictureInPictureRect() const;
    double PictureInPictureAspectRatio() const;
    SIZE PictureInPictureMinimumSize() const;
    int PictureInPictureInteractiveTop() const;
    void AdjustPictureInPictureSizingRect(WPARAM edge, RECT* rect) const;
    bool EnterPictureInPicture();
    enum class PictureInPictureExitTarget {
        Restore,
        Fullscreen,
    };
    bool ExitPictureInPicture(
        PictureInPictureExitTarget target = PictureInPictureExitTarget::Restore);
    bool PictureInPictureHasInteractiveRects() const;
    bool PictureInPicturePointInInteractiveRect(int x, int y) const;
    LRESULT HitTestPictureInPicture(int x, int y) const;
    int PictureInPictureResizeBorderPx() const;
    int PictureInPictureVisualBorderPx() const;
    void DrawPictureInPictureBorder(ID3D11DeviceContext* context);
    bool DrawGpuTextGridFrame(ID3D11Device* device,
                              ID3D11DeviceContext* context,
                              const GpuTextGridFrame& frame,
                              const D3D11_VIEWPORT& viewport);
    UINT TextGridDpi() const;
    SIZE TextGridCellSize() const;
    bool EnsureGpuTextGlyphAtlas(ID3D11Device* device, int cellWidth,
                                 int cellHeight, UINT dpi, int fontWeight);
    bool EnsureGpuTextGridConstants(ID3D11Device* device);
    void FillOutputColorConstants(ShaderConstants& constants) const;
    uint32_t OutputColorSpaceShaderValue() const;
    float OutputSdrWhiteNits() const;
    float OutputPeakNits() const;
    float OutputFullFrameNits() const;
    float AsciiGlyphPeakNits() const;
    void SetOutputColorAttemptStatus(const std::string& status);
    void EnableFileDrop();
    void DisableFileDrop();

    HWND m_hWnd = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> m_swapChain2;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_uiShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_gpuTextGridShader;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_uiBlendState;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
    VideoOutputColorState m_outputColorState;
    std::string m_outputColorAttemptStatus;
    std::atomic<UINT> m_presentInterval{1};
    HANDLE m_frameLatencyWaitableObject = nullptr;
    std::mutex m_frameLatencyMutex;

    // frame cache is owned and managed externally by videoplayback

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_subtitleTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_subtitleSrv;
    int m_subtitleWidth = 0;
    int m_subtitleHeight = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_tuiTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_tuiSrv;
    int m_tuiTexWidth = 0;
    int m_tuiTexHeight = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_gpuTextGridTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_gpuTextGridSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_gpuTextGlyphAtlasTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_gpuTextGlyphAtlasSrv;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_gpuTextGridConstants;
    int m_gpuTextGlyphAtlasCellWidth = 0;
    int m_gpuTextGlyphAtlasCellHeight = 0;
    UINT m_gpuTextGlyphAtlasDpi = 0;
    int m_gpuTextGlyphAtlasWeight = 0;
    int m_gpuTextGridCols = 0;
    int m_gpuTextGridRows = 0;
    GpuTextGridFrame m_windowOverlayTextGrid;
    
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
    std::unique_ptr<videowindow_file_drop::DropTargetRegistration>
        m_fileDropTarget;
    // Cache last window title to avoid repeated SetWindowText calls
    std::string m_lastWindowTitle;
    std::string m_baseWindowTitle;

    // Fullscreen helpers
    LONG m_prevStyle = 0;
    LONG m_prevExStyle = 0; // saved extended style for restoring on exit
    RECT m_prevRect{};
    bool m_isFullscreen = false;
    std::atomic<bool> m_pictureInPicture{false};
    std::atomic<bool> m_textGridPresentationEnabled{false};
    std::atomic<int> m_textGridCols{0};
    std::atomic<int> m_textGridRows{0};
    std::atomic<int> m_textGridMinCols{0};
    std::atomic<int> m_textGridMinRows{0};
    mutable std::mutex m_pictureInPictureInteractiveRectsMutex;
    std::vector<RECT> m_pictureInPictureInteractiveRects;
    bool m_pipRestoreFullscreen = false;
    LONG m_pipRestoreStyle = 0;
    LONG m_pipRestoreExStyle = 0;
    RECT m_pipRestoreRect{};
    // Guard to ignore WM_SIZE events while we're transitioning to/from fullscreen
    bool m_ignoreWindowSizeEvents = false;
    // When true, Present should skip until the render-target view and sizes are ready
    bool m_waitingForRenderTarget = false;
    bool m_captureAllMouseInput = false;
    std::atomic<bool> m_cursorVisible{true};
    std::atomic<bool> m_closeRequested{false};
    HANDLE m_closeRequestedEvent = nullptr;
    DWORD m_windowThreadId = 0;
    mutable std::mutex m_subtitleStateMutex;
    std::string m_subtitleRenderError;
    void setSubtitleRenderError(std::string error);
    bool MakeFullscreen();
    bool ExitFullscreen();
};
