#pragma once

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "core/native_wait_handle.h"
#include "core/waitable_signal.h"
#include "core/windows_handle.h"
#include "playback/video/color.h"
#include "input_event.h"
#include "consolescreen.h"
#include "playback/video/framebuffer/gpu_text_grid.h"
#include "playback/video/framebuffer/window/gpu_text_grid_composition.h"
#include "playback/video/framebuffer/frame_snapshot.h"
#include "playback/overlay/context_menu.h"
#include "playback/overlay/interaction.h"
#include "playback/video/gpu/videoprocessor.h"
#include "playback/video/framebuffer/video_output_color.h"
#include "playback/video/subtitle/font_attachments.h"
#include "playback/video/timeline_preview_types.h"
#include "playback/video/edit/view.h"
#include "display_lifecycle.h"
#include "input_controller.h"
#include "present.h"
#include <vector>
#include <mutex>

class GpuRuntime;

struct WindowUiState {
    struct ControlButton {
        playback_overlay::OverlayControlId id =
            playback_overlay::OverlayControlId::Radio;
        std::string text;
        bool active = false;
        bool hovered = false;
        bool enabled = true;
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
    bool chromeVisible = false;
    bool isPaused = false;
    // UI text/metadata to display when overlay is visible
    std::string title; // filename or label
    std::string progressSuffix; // playback time/volume line
    std::shared_ptr<const std::string> transientMessage;
    std::vector<ControlButton> controlButtons;
    std::vector<SubtitleCue> subtitleCues; // active subtitle cues for current frame
    int64_t subtitleClockUs = 0;
    std::shared_ptr<const std::string> subtitleAssScript;
    std::shared_ptr<const SubtitleFontAttachmentList> subtitleAssFonts;
    std::string subtitleRenderError;
    double displaySec = 0.0; // current time shown in overlay
    int volPct = 0; // volume percent for display
    std::string subtitle; // current subtitle cue text
    float subtitleAlpha = 0.0f; // subtitle opacity
    std::vector<std::string> debugLines;
    playback_overlay::ContextMenuSnapshot contextMenu;
    playback_video_timeline_preview::Snapshot timelinePreview;
    playback_video_edit::EditSnapshot videoEdit;
    playback_video_edit::ExportProgress videoEditExport;
    playback_video_edit::Prompt videoEditPrompt =
        playback_video_edit::Prompt::None;
};

struct IDXGISwapChain2;
struct ShaderConstants;

enum class VideoWindowFocus {
    KeepCurrentFocus,
    TakeForegroundFocus,
};

// The durable windowed state behind fullscreen and picture-in-picture.
// normalBounds uses the same workspace coordinates as WINDOWPLACEMENT.
struct VideoWindowedPlacement {
    RECT normalBounds{};
    bool maximized = false;
};

class VideoWindow {
public:
    static constexpr int kDefaultVideoClientWidth = 1280;
    static constexpr int kDefaultVideoClientHeight = 720;

    explicit VideoWindow(GpuRuntime& gpu);
    ~VideoWindow();

    // Creates the native resources without exposing an intermediate window.
    // The owner thread must apply the requested presentation or call Show().
    bool Open(int width, int height, const std::string& title);
    void Close();

    void Present(GpuVideoFrameCache& frameCache, const WindowUiState& ui);
    // Render the UI overlay using the last cached video frame as background
    void PresentOverlay(GpuVideoFrameCache& frameCache, const WindowUiState& ui);
    // Must be called on the window/presenter thread.
    VideoFrameSnapshotResult CaptureCurrentFrame(
        GpuVideoFrameCache& frameCache, const WindowUiState& ui);
    // Render a full-screen text grid (TUI) into the window backbuffer.
    void PresentTextGrid(const std::vector<ScreenCell>& cells, int cols, int rows);
    void PresentGpuTextGrid(
        const GpuTextGridFrame& frame,
        const playback_overlay::InteractionMap& interactions = {});
    void PresentBackbuffer();
    void WaitForFramePacing(std::chrono::milliseconds timeout) const;
    void SetVsync(bool enabled);
    std::string GetSubtitleRenderError() const;
    void SetCaptureAllMouseInput(bool enabled) { m_captureAllMouseInput = enabled; }
    void SetSystemMediaInputEnabled(bool enabled) {
        m_systemMediaInputEnabled.store(enabled, std::memory_order_relaxed);
    }
    playback_overlay::InteractionHit OverlayHitAt(
        double x, double y, bool capturedProgress = false) const;
    bool OverlayEditBoundaryHandleAt(double x, double y) const;
    void SetTextGridMinimumSize(int cols, int rows);
    void SetCursorVisible(bool visible);
    // Presentation mutations are owner-thread operations. Cross-thread
    // callers must dispatch through WindowPresenter.
    bool SetPictureInPicture(bool enabled, VideoWindowFocus focus);
    bool ExitPictureInPictureToFullscreen(VideoWindowFocus focus);
    void SetTextGridPresentationEnabled(bool enabled);
    bool IsTextGridPresentationEnabled() const {
        return m_textGridPresentationEnabled.load(std::memory_order_relaxed);
    }
    bool IsPictureInPicture() const {
        return m_pictureInPicture.load(std::memory_order_relaxed);
    }
    bool IsFullscreen() const { return m_isFullscreen; }
    bool SetFullscreen(bool enabled, VideoWindowFocus focus);
    // Restores a persisted presentation without first exposing an
    // intermediate windowed surface.
    bool RestoreWindowed(const VideoWindowedPlacement& placement,
                         VideoWindowFocus focus);
    bool RestoreFullscreen(const VideoWindowedPlacement& placement,
                           VideoWindowFocus focus);
    bool RestorePictureInPicture(const VideoWindowedPlacement& placement,
                                 VideoWindowFocus focus);
    void GetTextGridSize(int& outCols, int& outRows) const {
        outCols = m_textGridCols.load(std::memory_order_relaxed);
        outRows = m_textGridRows.load(std::memory_order_relaxed);
    }
    void GetTextGridCellSize(int& outCellWidth,
                                         int& outCellHeight) const;
    bool GetWindowBounds(RECT* outRect) const;
    // Must be called on the window owner thread. Returns the durable windowed
    // placement even while the window is fullscreen or in PiP.
    bool GetWindowedPlacement(VideoWindowedPlacement* outPlacement) const;
    // Changes geometry without changing visibility or foreground ownership.
    bool SetWindowBounds(const RECT& rect);
    
    bool IsOpen() const { return m_hWnd != nullptr; }
    bool IsVisible() const { return m_hWnd && IsWindowVisible(m_hWnd); }
    HWND NativeWindowHandle() const { return m_hWnd; }
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
    void Activate();
    bool Show(VideoWindowFocus focus);
    bool PollEvents();
    bool PollInput(InputEvent& ev);
    NativeWaitHandle InputWaitHandle() const;
    bool ConsumeCloseRequested();
    NativeWaitHandle CloseRequestedWaitHandle() const;
    bool EnableFileDrop();
    void DisableFileDrop();
    void Cleanup();

private:
    GpuRuntime& m_gpu;
    struct WindowRestoreState {
        LONG style = 0;
        LONG exStyle = 0;
        WINDOWPLACEMENT placement{};

        WindowRestoreState() {
            placement.length = sizeof(WINDOWPLACEMENT);
        }
    };

    struct FrameRenderGeometry {
        int width = 0;
        int height = 0;
        VideoViewport viewport{};
    };

    bool DrawVideoFrame(GpuVideoFrameCache& frameCache,
                        ID3D11Device* device,
                        ID3D11DeviceContext* context,
                        ID3D11RenderTargetView* renderTarget,
                        const FrameRenderGeometry& geometry,
                        const VideoOutputColorState& outputColor,
                        const WindowUiState& ui,
                        bool includePlaybackOverlay,
                        const char* timingStage,
                        playback_overlay::InteractionMap* outInteractions);
    bool BindVideoFrame(GpuVideoFrameCache& frameCache,
                        ID3D11DeviceContext* context,
                        const D3D11_VIEWPORT& viewport,
                        const VideoOutputColorState& outputColor);
    void UnbindVideoFrame(ID3D11DeviceContext* context);
    void DrawOverlay(ID3D11Device* device,
                     ID3D11DeviceContext* context,
                     const WindowUiState& ui,
                     const FrameRenderGeometry& geometry,
                     const VideoOutputColorState& outputColor,
                     bool includePlaybackOverlay,
                     playback_overlay::InteractionMap* outInteractions);
    void UpdateViewport(int width, int height);
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void ReleaseSwapChainBackBufferReferences();
    bool CreateSwapChain(int width, int height);
    bool RecreateSwapChainForCurrentDisplay(const char* reason);
    void HandlePresentResult(HRESULT hr, const char* stage);
    HRESULT PresentSwapChain(IDXGISwapChain* swapChain,
                             const VideoWindowPresentArgs& presentArgs,
                             const char* stage);
    void ResetSwapChain();
    void Resize(int width, int height);
    RECT CalculatePictureInPictureRect() const;
    double PictureInPictureAspectRatio() const;
    SIZE PictureInPictureMinimumSize() const;
    void AdjustPictureInPictureSizingRect(WPARAM edge, RECT* rect) const;
    bool EnterPictureInPicture(VideoWindowFocus focus,
                               const WindowRestoreState* restoreState = nullptr);
    enum class PictureInPictureExitTarget {
        Windowed,
        Fullscreen,
    };
    bool ExitPictureInPicture(PictureInPictureExitTarget target,
                              VideoWindowFocus focus);
    LRESULT HitTestPictureInPicture(int x, int y) const;
    int PictureInPictureResizeBorderPx() const;
    int PictureInPictureVisualBorderPx() const;
    void DrawPictureInPictureBorder(ID3D11DeviceContext* context);
    bool DrawGpuTextGridFrame(ID3D11Device* device,
                              ID3D11DeviceContext* context,
                              const GpuTextGridFrame& frame,
                              const D3D11_VIEWPORT& viewport,
                              GpuTextGridComposition composition);
    UINT TextGridDpi() const;
    SIZE TextGridCellSize() const;
    bool EnsureGpuTextGlyphAtlas(ID3D11Device* device, int cellWidth,
                                 int cellHeight, UINT dpi, int fontWeight);
    bool EnsureGpuTextGridConstants(ID3D11Device* device);
    void FillOutputColorConstants(
        ShaderConstants& constants,
        const VideoOutputColorState& outputColor) const;
    uint32_t OutputColorSpaceShaderValue() const;
    float OutputSdrWhiteNits() const;
    float OutputPeakNits() const;
    float OutputFullFrameNits() const;
    float AsciiGlyphPeakNits() const;
    void SetOutputColorAttemptStatus(const std::string& status);
    void SetOverlayInteractionMap(
        playback_overlay::InteractionMap interactions);
    bool OverlayInteractionAt(double x, double y) const;
    bool ShouldQueueWindowMouseEvent(int x, int y) const;
    void OnClientResizedByWindow(int width, int height);
    void OnDisplayChangedByWindow(int width, int height);
    void RequestCloseFromWindow();
    void ApplyPendingDisplayChange();
    bool ApplyWindowBounds(const RECT& rect);
    bool ActivateForegroundSurface();
    bool CaptureWindowRestoreState(WindowRestoreState& state) const;
    WindowRestoreState WindowRestoreStateFor(
        const VideoWindowedPlacement& placement) const;
    bool ApplyWindowRestoreState(const WindowRestoreState& state,
                                 VideoWindowFocus focus);

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
    UniqueWindowsHandle m_frameLatencyWaitableObject;
    mutable std::mutex m_frameLatencyMutex;

    // frame cache is owned and managed externally by video playback

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
    GpuVideoFrameCache m_timelinePreviewFrameCache;
    uint64_t m_timelinePreviewImageId = 0;
    
    int m_width = 0;
    int m_height = 0;
    int m_videoWidth = kDefaultVideoClientWidth;
    int m_videoHeight = kDefaultVideoClientHeight;
    
    // Viewport geometry (for mouse coordinate mapping)
    float m_viewportX = 0.0f;
    float m_viewportY = 0.0f;
    float m_viewportW = 0.0f;
    float m_viewportH = 0.0f;

    WindowInputController m_input;
    bool m_windowMouseInputActive = false;
    bool m_trackingMouseLeave = false;
    bool m_trackingNonClientMouseLeave = false;

    // Exact native state to restore after temporary presentation modes.
    WindowRestoreState m_fullscreenRestoreState;
    bool m_isFullscreen = false;
    std::atomic<bool> m_pictureInPicture{false};
    std::atomic<bool> m_textGridPresentationEnabled{false};
    std::atomic<int> m_textGridCols{0};
    std::atomic<int> m_textGridRows{0};
    std::atomic<int> m_textGridMinCols{0};
    std::atomic<int> m_textGridMinRows{0};
    mutable std::mutex m_overlayInteractionMutex;
    playback_overlay::InteractionMap m_overlayInteractions;
    WindowRestoreState m_pictureInPictureRestoreState;
    WindowDisplayLifecycle m_displayLifecycle;
    bool m_captureAllMouseInput = false;
    std::atomic<bool> m_systemMediaInputEnabled{true};
    bool m_leftMouseCaptureActive = false;
    bool m_editBoundaryCaptureActive = false;
    std::atomic<bool> m_cursorVisible{true};
    WaitableSignal m_closeRequest;
    DWORD m_windowThreadId = 0;
    mutable std::mutex m_subtitleStateMutex;
    std::string m_subtitleRenderError;
    void setSubtitleRenderError(std::string error);
    bool MakeFullscreen(VideoWindowFocus focus,
                        const WindowRestoreState* restoreState = nullptr);
    bool ExitFullscreen(VideoWindowFocus focus);
};
