#include "videowindow.h"
#include "timing_log.h"
#include "gpu_shared.h"
#include "videowindow_internal.h"
#include "videowindow_present.h"
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdarg>
#include <iostream>
#include <vector>
#include <mutex>
#include <string>
#include <utility>
#include <unordered_set>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <chrono>
#include <thread>
#include <sstream>

#include "videowindow_vs.h"
#include "videowindow_ps.h"
#include "videowindow_ps_ui.h"
#include "videowindow_ps_gpu_text_grid.h"
#include "playback/overlay/overlay.h"
#include "playback/input/media_keys.h"
#include "subtitle_caption_style.h"
#include "ui_helpers.h"
#if RADIOIFY_HAS_LIBASS
extern "C" {
#include <ass/ass.h>
}
#endif

static inline std::string now_ms() {
    using namespace std::chrono;
    auto t = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    std::ostringstream ss; ss << t; return ss.str();
}
static inline std::string thread_id_str() {
    std::ostringstream ss; ss << std::this_thread::get_id(); return ss.str();
}

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {
    static std::wstring utf8ToWide(const std::string& text) {
        if (text.empty()) return {};
        int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
        if (needed <= 0) {
            needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
        }
        if (needed <= 0) {
            std::wstring fallback;
            fallback.reserve(text.size());
            for (unsigned char ch : text) {
                fallback.push_back(static_cast<wchar_t>(ch));
            }
            return fallback;
        }
        std::wstring out(static_cast<size_t>(needed), L'\0');
        int written = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()),
                                          out.data(), needed);
        if (written <= 0) return {};
        return out;
    }

    static bool buildWindowOverlayTextGrid(
        const WindowUiState& ui, int cols, int rows,
        GpuTextGridFrame& outFrame) {
        cols = std::max(1, cols);
        rows = std::max(1, rows);

        const playback_overlay::OverlayCellLayout layout =
            playback_overlay::layoutWindowOverlayCells(ui, cols, rows);
        return playback_overlay::renderOverlayToGpuTextGrid(
            layout, playback_overlay::OverlayRenderStyles{}, ui.progress,
            outFrame);
    }

    enum class AssRenderStatus {
        ok_no_glyph,
        ok_with_glyph,
        error_init_or_parse
    };

    struct AssRenderResult {
        AssRenderStatus status = AssRenderStatus::ok_no_glyph;
        std::string errorMessage;
    };

    static uint64_t assScriptFingerprint(const std::string& script) {
        constexpr uint64_t kFNVOffset = 1469598103934665603ull;
        constexpr uint64_t kFNVPrime = 1099511628211ull;
        uint64_t hash = kFNVOffset;
        for (unsigned char ch : script) {
            hash ^= static_cast<uint64_t>(ch);
            hash *= kFNVPrime;
        }
        return hash;
    }

#if RADIOIFY_HAS_LIBASS
    class LibassOverlayRenderer {
    public:
        LibassOverlayRenderer() = default;

        ~LibassOverlayRenderer() {
            resetAssState();
        }

        AssRenderResult render(const std::shared_ptr<const std::string>& assScript,
                               const std::shared_ptr<const SubtitleFontAttachmentList>& assFonts,
                               int64_t clockUs, int canvasW, int canvasH,
                               std::vector<uint8_t>* outCanvas) {
            if (!outCanvas || canvasW <= 0 || canvasH <= 0) {
                return {AssRenderStatus::error_init_or_parse,
                        "Invalid ASS canvas target."};
            }

            const size_t pixelBytes = static_cast<size_t>(canvasW) *
                                      static_cast<size_t>(canvasH) * 4u;
            if (outCanvas->size() != pixelBytes) {
                outCanvas->assign(pixelBytes, static_cast<uint8_t>(0));
            } else {
                std::fill(outCanvas->begin(), outCanvas->end(),
                          static_cast<uint8_t>(0));
            }

            if (!assScript || assScript->empty()) {
                return {AssRenderStatus::ok_no_glyph, {}};
            }

            const uint64_t scriptHash = assScriptFingerprint(*assScript);
            std::string parseError;
            if (!ensureTrack(assScript, assFonts, &parseError)) {
                if (parseError.empty()) {
                    parseError = "Failed to parse ASS script.";
                }
                logErrorOnce(scriptHash, parseError);
                return {AssRenderStatus::error_init_or_parse, parseError};
            }

            ass_set_frame_size(renderer_, canvasW, canvasH);
            ass_set_storage_size(renderer_, canvasW, canvasH);

            lastErrorMessage_.clear();
            int detectChange = 0;
            ASS_Image* img = ass_render_frame(
                renderer_, track_,
                static_cast<long long>(std::max<int64_t>(0, clockUs / 1000)),
                &detectChange);
            (void)detectChange;

            if (!lastErrorMessage_.empty()) {
                std::string renderError = "libass render error: " + lastErrorMessage_;
                logErrorOnce(scriptHash, renderError);
                return {AssRenderStatus::error_init_or_parse, renderError};
            }

            bool drewAny = false;
            for (ASS_Image* cur = img; cur != nullptr; cur = cur->next) {
                if (!cur->bitmap || cur->w <= 0 || cur->h <= 0) continue;
                const uint8_t r = static_cast<uint8_t>((cur->color >> 24) & 0xFFu);
                const uint8_t g = static_cast<uint8_t>((cur->color >> 16) & 0xFFu);
                const uint8_t b = static_cast<uint8_t>((cur->color >> 8) & 0xFFu);
                const float colorAlpha =
                    static_cast<float>(255u - (cur->color & 0xFFu)) / 255.0f;
                if (colorAlpha <= 0.0f) continue;
                drewAny = true;

                for (int y = 0; y < cur->h; ++y) {
                    const int dstY = cur->dst_y + y;
                    if (dstY < 0 || dstY >= canvasH) continue;
                    const uint8_t* srcRow =
                        cur->bitmap + static_cast<std::ptrdiff_t>(y) * cur->stride;
                    for (int x = 0; x < cur->w; ++x) {
                        const int dstX = cur->dst_x + x;
                        if (dstX < 0 || dstX >= canvasW) continue;
                        const float cov = static_cast<float>(srcRow[x]) / 255.0f;
                        if (cov <= 0.0f) continue;

                        const float srcA = std::clamp(cov * colorAlpha, 0.0f, 1.0f);
                        const size_t dstIdx =
                            (static_cast<size_t>(dstY) * static_cast<size_t>(canvasW) +
                             static_cast<size_t>(dstX)) *
                            4u;
                        const float dstA =
                            static_cast<float>((*outCanvas)[dstIdx + 3]) / 255.0f;
                        const float outA = srcA + dstA * (1.0f - srcA);
                        if (outA <= 0.0001f) continue;

                        const float srcRgb[3] = {
                            static_cast<float>(b) / 255.0f,
                            static_cast<float>(g) / 255.0f,
                            static_cast<float>(r) / 255.0f};
                        for (int c = 0; c < 3; ++c) {
                            const float dstC =
                                static_cast<float>((*outCanvas)[dstIdx + static_cast<size_t>(c)]) / 255.0f;
                            const float outC =
                                (srcRgb[c] * srcA + dstC * dstA * (1.0f - srcA)) / outA;
                            (*outCanvas)[dstIdx + static_cast<size_t>(c)] = static_cast<uint8_t>(
                                std::lround(255.0f * std::clamp(outC, 0.0f, 1.0f)));
                        }
                        (*outCanvas)[dstIdx + 3] = static_cast<uint8_t>(
                            std::lround(255.0f * std::clamp(outA, 0.0f, 1.0f)));
                    }
                }
            }

            if (drewAny) {
                return {AssRenderStatus::ok_with_glyph, {}};
            }
            return {AssRenderStatus::ok_no_glyph, {}};
        }

    private:
        bool ensureRenderer(
            const std::shared_ptr<const SubtitleFontAttachmentList>& assFonts,
            std::string* outError) {
            std::shared_ptr<const SubtitleFontAttachmentList> loadedFonts =
                loadedFonts_.lock();
            if (library_ && renderer_ && loadedFonts.get() == assFonts.get()) {
                return true;
            }

            resetAssState();

            library_ = ass_library_init();
            if (!library_) {
                initError_ = "libass library initialization failed";
                if (outError) *outError = initError_;
                return false;
            }
            ass_set_message_cb(library_, &LibassOverlayRenderer::logCallback, this);

            if (assFonts) {
                for (const SubtitleFontAttachment& attachment : *assFonts) {
                    if (attachment.filename.empty() || attachment.data.empty() ||
                        attachment.data.size() >
                            static_cast<size_t>((std::numeric_limits<int>::max)())) {
                        continue;
                    }
                    ass_add_font(
                        library_, attachment.filename.c_str(),
                        reinterpret_cast<const char*>(attachment.data.data()),
                        static_cast<int>(attachment.data.size()));
                }
            }

            renderer_ = ass_renderer_init(library_);
            if (!renderer_) {
                initError_ = "libass renderer initialization failed";
                if (outError) *outError = initError_;
                return false;
            }
            ass_set_fonts(renderer_, nullptr, nullptr, 1, nullptr, 1);
            loadedFonts_ = assFonts;
            return true;
        }

        bool ensureTrack(const std::shared_ptr<const std::string>& assScript,
                         const std::shared_ptr<const SubtitleFontAttachmentList>& assFonts,
                         std::string* outError) {
            if (!assScript) {
                if (outError) *outError = "libass track input is invalid.";
                return false;
            }
            if (!ensureRenderer(assFonts, outError)) {
                return false;
            }
            if (track_) {
                std::shared_ptr<const std::string> loaded = loadedScript_.lock();
                if (loaded && loaded.get() == assScript.get()) return true;
            }

            if (track_) {
                ass_free_track(track_);
                track_ = nullptr;
            }
            loadedScript_.reset();
            scriptCache_.clear();

            scriptCache_ = *assScript;
            if (scriptCache_.empty()) {
                if (outError) *outError = "ASS script is empty.";
                return false;
            }

            lastErrorMessage_.clear();
            track_ = ass_read_memory(library_, scriptCache_.data(),
                                     static_cast<int>(scriptCache_.size()), nullptr);
            if (!track_) {
                if (outError) {
                    if (!lastErrorMessage_.empty()) {
                        *outError = "libass parse error: " + lastErrorMessage_;
                    } else {
                        *outError = "libass failed to parse ASS script.";
                    }
                }
                scriptCache_.clear();
                return false;
            }
            if (track_->n_events <= 0) {
                if (outError) {
                    *outError = "libass parsed ASS script without dialogue events.";
                }
                ass_free_track(track_);
                track_ = nullptr;
                scriptCache_.clear();
                return false;
            }
            loadedScript_ = assScript;
            return true;
        }

        void resetAssState() {
            if (track_) {
                ass_free_track(track_);
                track_ = nullptr;
            }
            if (renderer_) {
                ass_renderer_done(renderer_);
                renderer_ = nullptr;
            }
            if (library_) {
                ass_library_done(library_);
                library_ = nullptr;
            }
            loadedScript_.reset();
            loadedFonts_.reset();
            scriptCache_.clear();
            initError_.clear();
            lastErrorMessage_.clear();
        }

        void logErrorOnce(uint64_t scriptHash, const std::string& message) {
            if (message.empty()) return;
            if (loggedErrorHashes_.insert(scriptHash).second) {
                std::fprintf(stderr,
                             "[%s] [tid=%s] ASS renderer error [script=%016llx]: %s\n",
                             now_ms().c_str(), thread_id_str().c_str(),
                             static_cast<unsigned long long>(scriptHash),
                             message.c_str());
            }
        }

        static void logCallback(int level, const char* fmt, va_list va,
                                void* data) {
            if (level > 1 || !fmt || !data) return;
            auto* self = static_cast<LibassOverlayRenderer*>(data);
            if (!self) return;

            char buffer[1024];
            const int written =
                std::vsnprintf(buffer, sizeof(buffer), fmt, va);
            if (written <= 0) return;
            self->lastErrorMessage_.assign(buffer);
            while (!self->lastErrorMessage_.empty()) {
                char tail = self->lastErrorMessage_.back();
                if (tail == '\n' || tail == '\r' || tail == '\t' || tail == ' ') {
                    self->lastErrorMessage_.pop_back();
                } else {
                    break;
                }
            }
        }

        ASS_Library* library_ = nullptr;
        ASS_Renderer* renderer_ = nullptr;
        ASS_Track* track_ = nullptr;
        std::weak_ptr<const std::string> loadedScript_;
        std::weak_ptr<const SubtitleFontAttachmentList> loadedFonts_;
        std::string scriptCache_;
        std::string initError_;
        std::string lastErrorMessage_;
        std::unordered_set<uint64_t> loggedErrorHashes_;
    };

    static AssRenderResult renderAssSubtitlesToCanvas(
        const std::shared_ptr<const std::string>& assScript,
        const std::shared_ptr<const SubtitleFontAttachmentList>& assFonts,
        int64_t clockUs, int canvasW, int canvasH,
        std::vector<uint8_t>* outCanvas) {
        static LibassOverlayRenderer renderer;
        return renderer.render(assScript, assFonts, clockUs, canvasW, canvasH,
                               outCanvas);
    }
#else
    static AssRenderResult renderAssSubtitlesToCanvas(
        const std::shared_ptr<const std::string>& assScript,
        const std::shared_ptr<const SubtitleFontAttachmentList>& assFonts,
        int64_t clockUs, int canvasW, int canvasH,
        std::vector<uint8_t>* outCanvas) {
        (void)assScript;
        (void)assFonts;
        (void)clockUs;
        (void)canvasW;
        (void)canvasH;
        (void)outCanvas;
        return {AssRenderStatus::error_init_or_parse,
                "ASS renderer unavailable: build without libass."};
    }
#endif

    struct SubtitleBitmapLayout {
        int width = 0;
        int height = 0;
        int fontPx = 16;
        int marginPx = 0;
        RECT textRect{0, 0, 0, 0};
    };

    static HFONT createCaptionFont(int fontPx, const CaptionStyleProfile& captionStyle,
                                   const wchar_t* cueFontName, float cueScaleX,
                                   bool cueBold, bool cueItalic,
                                   bool cueUnderline) {
        const float safeScaleX = std::clamp(cueScaleX, 0.40f, 3.5f);
        int weight = cueBold ? FW_BOLD
                             : ((captionStyle.fontStyle == 7) ? FW_SEMIBOLD
                                                              : FW_NORMAL);
        int widthPx = 0;
        if (std::abs(safeScaleX - 1.0f) > 0.02f) {
            widthPx = std::max(
                1, static_cast<int>(std::lround(
                       static_cast<double>(std::max(8, fontPx)) * 0.48 *
                       static_cast<double>(safeScaleX))));
        }
        const wchar_t* face = (cueFontName && cueFontName[0] != L'\0')
                                  ? cueFontName
                                  : captionFontFaceForStyle(captionStyle.fontStyle);
        return CreateFontW(-std::max(8, fontPx), widthPx, 0, 0, weight,
                           cueItalic ? TRUE : FALSE,
                           cueUnderline ? TRUE : FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    }

    static bool computeSubtitleLayout(const std::wstring& text,
                                      int viewportWidth,
                                      int viewportHeight,
                                      const CaptionStyleProfile& captionStyle,
                                      const std::wstring& cueFontName,
                                      float cueScaleX, bool cueBold,
                                      bool cueItalic, bool cueUnderline,
                                      SubtitleBitmapLayout* outLayout) {
        if (!outLayout || text.empty() || viewportWidth <= 0 || viewportHeight <= 0) {
            return false;
        }

        const int areaHeight = std::min(viewportHeight, viewportWidth);
        // VLC uses 6.25% relsize by default: i_font_size = area_height * 6.25 / 100.
        int targetLinePx = static_cast<int>(
            static_cast<double>(areaHeight) * 0.0625 *
            std::max(0.60f, captionStyle.sizeScale));
        targetLinePx = std::clamp(targetLinePx, 10, std::max(10, viewportHeight / 4));
        int fontPx = targetLinePx;

        HDC hdc = CreateCompatibleDC(nullptr);
        if (!hdc) return false;
        HFONT font = createCaptionFont(fontPx, captionStyle, cueFontName.c_str(),
                                       cueScaleX, cueBold, cueItalic,
                                       cueUnderline);
        HGDIOBJ oldFont = nullptr;
        if (font) oldFont = SelectObject(hdc, font);

        // GDI and VLC/freetype do not map "font size" identically. Normalize to
        // the target line-height so visual size tracks VLC defaults more closely.
        TEXTMETRICW tm{};
        if (font && GetTextMetricsW(hdc, &tm) && tm.tmHeight > 0) {
            const int correctedPx = std::max(
                8, static_cast<int>(std::lround(
                       static_cast<double>(fontPx) * targetLinePx / tm.tmHeight)));
            if (correctedPx != fontPx) {
                if (oldFont) SelectObject(hdc, oldFont);
                DeleteObject(font);
                fontPx = correctedPx;
                font = createCaptionFont(fontPx, captionStyle, cueFontName.c_str(),
                                         cueScaleX, cueBold, cueItalic,
                                         cueUnderline);
                oldFont = font ? SelectObject(hdc, font) : nullptr;
            }
        }

        const int maxTextWidth = std::max(24, static_cast<int>(std::lround(
            static_cast<double>(viewportWidth) * 0.92)));
        const int maxTextHeight = std::max(
            16, static_cast<int>(std::lround(static_cast<double>(viewportHeight) * 0.80)));

        RECT measure{0, 0, maxTextWidth, maxTextHeight};
        UINT measureFlags = DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX |
                            DT_CALCRECT;
        DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &measure,
                  measureFlags);

        int textW = (std::max)(1, static_cast<int>(measure.right - measure.left));
        int textH = (std::max)(1, static_cast<int>(measure.bottom - measure.top));
        int marginPx = (captionStyle.backgroundAlpha > 0.01f)
                           ? std::max(2, fontPx / 4)  // VLC-like text bg margin
                           : std::max(1, fontPx / 8);

        outLayout->fontPx = fontPx;
        outLayout->marginPx = marginPx;
        outLayout->width =
            std::clamp(textW + marginPx * 2, 8, std::max(8, viewportWidth));
        outLayout->height =
            std::clamp(textH + marginPx * 2, 8, std::max(8, viewportHeight));
        outLayout->textRect = RECT{marginPx, marginPx,
                                   std::max(marginPx + 1, outLayout->width - marginPx),
                                   std::max(marginPx + 1, outLayout->height - marginPx)};

        if (oldFont) SelectObject(hdc, oldFont);
        if (font) DeleteObject(font);
        DeleteDC(hdc);
        return true;
    }

    static bool renderSubtitleTextToBitmap(const std::string& text,
                                           const SubtitleBitmapLayout& layout,
                                           const CaptionStyleProfile& captionStyle,
                                           const std::wstring& cueFontName,
                                           float cueScaleX, bool cueBold,
                                           bool cueItalic, bool cueUnderline,
                                           std::vector<uint8_t>& outPixels) {
        outPixels.clear();
        if (layout.width <= 0 || layout.height <= 0 || text.empty()) return false;

        std::wstring wide = utf8ToWide(text);
        if (wide.empty()) return false;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = layout.width;
        bmi.bmiHeader.biHeight = -layout.height;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC hdc = CreateCompatibleDC(nullptr);
        if (!hdc) return false;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
            if (dib) DeleteObject(dib);
            DeleteDC(hdc);
            return false;
        }

        HGDIOBJ oldBmp = SelectObject(hdc, dib);
        HFONT font = createCaptionFont(layout.fontPx, captionStyle,
                                       cueFontName.c_str(), cueScaleX, cueBold,
                                       cueItalic, cueUnderline);
        HGDIOBJ oldFont = nullptr;
        if (font) oldFont = SelectObject(hdc, font);

        const uint8_t bgR = captionStyle.backgroundR;
        const uint8_t bgG = captionStyle.backgroundG;
        const uint8_t bgB = captionStyle.backgroundB;
        const uint8_t bgA = static_cast<uint8_t>(std::lround(
            255.0f * std::clamp(captionStyle.backgroundAlpha, 0.0f, 1.0f)));
        const uint8_t textA = static_cast<uint8_t>(std::lround(
            255.0f * std::clamp(captionStyle.textAlpha, 0.0f, 1.0f)));

        uint8_t* px = reinterpret_cast<uint8_t*>(bits);
        const size_t pixelBytes =
            static_cast<size_t>(layout.width) * static_cast<size_t>(layout.height) * 4u;
        constexpr uint8_t kSentinelB = 1;
        constexpr uint8_t kSentinelG = 0;
        constexpr uint8_t kSentinelR = 1;
        for (size_t i = 0; i < pixelBytes; i += 4) {
            px[i + 0] = kSentinelB;
            px[i + 1] = kSentinelG;
            px[i + 2] = kSentinelR;
            px[i + 3] = 0;
        }

        if (bgA > 0) {
            for (int y = 0; y < layout.height; ++y) {
                for (int x = 0; x < layout.width; ++x) {
                    size_t i = static_cast<size_t>(y * layout.width + x) * 4u;
                    px[i + 0] = bgB;
                    px[i + 1] = bgG;
                    px[i + 2] = bgR;
                    px[i + 3] = bgA;
                }
            }
        }

        SetBkMode(hdc, TRANSPARENT);
        UINT drawFlags = DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX;

        auto drawTextOffset = [&](int dx, int dy, COLORREF color) {
            RECT r = layout.textRect;
            OffsetRect(&r, dx, dy);
            SetTextColor(hdc, color);
            DrawTextW(hdc, wide.c_str(), static_cast<int>(wide.size()), &r,
                      drawFlags);
        };

        const int outlinePx =
            std::max(1, static_cast<int>(std::lround(layout.fontPx * 0.04f)));
        const int shadowPx =
            std::max(1, static_cast<int>(std::lround(layout.fontPx * 0.06f)));
        auto drawOutline = [&](int radius, COLORREF color) {
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    if (dx * dx + dy * dy > radius * radius + radius) continue;
                    drawTextOffset(dx, dy, color);
                }
            }
        };

        switch (captionStyle.fontEffect) {
            case 2:  // Raised
                drawTextOffset(-1, -1, RGB(245, 245, 245));
                drawTextOffset(1, 1, RGB(25, 25, 25));
                break;
            case 3:  // Depressed
                drawTextOffset(-1, -1, RGB(25, 25, 25));
                drawTextOffset(1, 1, RGB(245, 245, 245));
                break;
            case 4:  // Uniform outline
                drawOutline(outlinePx, RGB(0, 0, 0));
                break;
            case 5:  // Drop shadow
                drawTextOffset(shadowPx, shadowPx, RGB(0, 0, 0));
                break;
            default:
                break;  // None/default
        }

        SetTextColor(hdc, RGB(captionStyle.textR, captionStyle.textG, captionStyle.textB));
        RECT mainTextRect = layout.textRect;
        DrawTextW(hdc, wide.c_str(), static_cast<int>(wide.size()), &mainTextRect,
                  drawFlags);

        outPixels.assign(px, px + pixelBytes);
        bool hasText = false;
        const uint8_t baseB = (bgA > 0) ? bgB : kSentinelB;
        const uint8_t baseG = (bgA > 0) ? bgG : kSentinelG;
        const uint8_t baseR = (bgA > 0) ? bgR : kSentinelR;
        for (size_t i = 0; i < outPixels.size(); i += 4) {
            const uint8_t b = outPixels[i + 0];
            const uint8_t g = outPixels[i + 1];
            const uint8_t r = outPixels[i + 2];
            const int diff = std::abs(static_cast<int>(b) - static_cast<int>(baseB)) +
                             std::abs(static_cast<int>(g) - static_cast<int>(baseG)) +
                             std::abs(static_cast<int>(r) - static_cast<int>(baseR));
            if (diff > 8) {
                outPixels[i + 3] = std::max(textA, bgA);
                hasText = true;
            } else if (bgA == 0) {
                outPixels[i + 3] = 0;
            } else {
                outPixels[i + 3] = bgA;
            }
        }

        if (oldFont) SelectObject(hdc, oldFont);
        if (font) DeleteObject(font);
        if (oldBmp) SelectObject(hdc, oldBmp);
        DeleteObject(dib);
        DeleteDC(hdc);

        return hasText;
    }

    #if 0
    // Runtime shader sources retained for reference; build uses precompiled blobs.
    // Video frame rendering shader (combined from videowindow_render.hlsl)
    const char* g_shaderSource = R"(
// Video frame rendering shader - handles YUV/RGBA to RGB conversion with color correction.
// Playback controls/progress are rendered by the shared GPU text-grid overlay pass.

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

cbuffer Constants : register(b0) {
    uint isFullRange;
    uint yuvMatrix;
    uint yuvTransfer;
    uint bitDepth;
    float uiProgress;
    float uiAlpha;
    uint uiPaused;
    uint uiHasRGBA;
    uint uiVolPct;
    uint uiPad0;
    float uiTextTop;
    float uiTextHeight;
    float uiTextLeft;
    float uiTextWidth;
};

PS_INPUT VS(uint vid : SV_VertexID) {
    PS_INPUT output;
    output.tex = float2(vid & 1, vid >> 1);
    output.pos = float4(output.tex * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

Texture2D texY : register(t0);
Texture2D texUV : register(t1);
Texture2D texRGBA : register(t2);
SamplerState sam : register(s0);

float ExpandYNorm(float yNorm) {
    float maxCode = (float)((1u << bitDepth) - 1u);
    float yCode = yNorm * maxCode;
    uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
    float yMin = (isFullRange != 0) ? 0.0f : (float)(16u << shift);
    float yMax = (isFullRange != 0) ? maxCode : (float)(235u << shift);
    return saturate((yCode - yMin) / max(yMax - yMin, 1.0f));
}

float2 ExpandUV(float2 uvNorm) {
    float maxCode = (float)((1u << bitDepth) - 1u);
    float2 uvCode = uvNorm * maxCode;
    uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
    float cMid = (float)(128u << shift);
    float cMin = (isFullRange != 0) ? 0.0f : (float)(16u << shift);
    float cMax = (isFullRange != 0) ? maxCode : (float)(240u << shift);
    return (uvCode - cMid) / max(cMax - cMin, 1.0f);
}

float PQEotf(float v) {
    float m1 = 2610.0 / 16384.0;
    float m2 = 2523.0 / 32.0;
    float c1 = 3424.0 / 4096.0;
    float c2 = 2413.0 / 128.0;
    float c3 = 2392.0 / 128.0;
    float vp = pow(max(v, 0.0), 1.0 / m2);
    return pow(max(vp - c1, 0.0) / (c2 - c3 * vp), 1.0 / m1);
}

float HlgEotf(float v) {
    const float a = 0.17883277;
    const float b = 1.0 - 4.0 * a;
    const float c = 0.5 - a * log(4.0 * a);
    if (v <= 0.5) return (v * v) / 3.0;
    return (exp((v - c) / a) + b) / 12.0;
}

float ToneMapFilmic(float x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float LinearToSrgb(float v) {
    v = max(v, 0.0);
    return (v <= 0.0031308) ? (v * 12.92) : (1.055 * pow(v, 1.0 / 2.4) - 0.055);
}

float3 ApplyHdrToSdr(float3 v) {
    v = saturate(v);
    if (yuvTransfer == 1) {
        float3 linearRgb = float3(PQEotf(v.r), PQEotf(v.g), PQEotf(v.b));
        float3 mapped = float3(ToneMapFilmic(linearRgb.r * 100.0), ToneMapFilmic(linearRgb.g * 100.0), ToneMapFilmic(linearRgb.b * 100.0));
        v = float3(LinearToSrgb(mapped.r), LinearToSrgb(mapped.g), LinearToSrgb(mapped.b));
    } else if (yuvTransfer == 2) {
        float3 linearRgb = float3(HlgEotf(v.r), HlgEotf(v.g), HlgEotf(v.b));
        float3 mapped = float3(ToneMapFilmic(linearRgb.r * 100.0), ToneMapFilmic(linearRgb.g * 100.0), ToneMapFilmic(linearRgb.b * 100.0));
        v = float3(LinearToSrgb(mapped.r), LinearToSrgb(mapped.g), LinearToSrgb(mapped.b));
    }
    return v;
}

float4 PS(PS_INPUT input) : SV_Target {
    if (uiHasRGBA != 0) {
        float4 c = texRGBA.Sample(sam, input.tex);
        return float4(saturate(c.rgb), 1.0);
    }

    float y = ExpandYNorm(texY.Sample(sam, input.tex).r);
    float2 uv = ExpandUV(texUV.Sample(sam, input.tex).rg);
    
    float r, g, b;
    if (yuvMatrix == 2) { r = y + 1.4746 * uv.y; g = y - 0.16455 * uv.x - 0.57135 * uv.y; b = y + 1.8814 * uv.x; }
    else if (yuvMatrix == 1) { r = y + 1.4020 * uv.y; g = y - 0.3441 * uv.x - 0.7141 * uv.y; b = y + 1.7720 * uv.x; }
    else { r = y + 1.5748 * uv.y; g = y - 0.1873 * uv.x - 0.4681 * uv.y; b = y + 1.8556 * uv.x; }
    
    float3 rgb = float3(r, g, b);
    if (yuvTransfer != 0) rgb = ApplyHdrToSdr(rgb);
    return float4(saturate(rgb), 1.0);
}
    )";

    // Subtitle-only overlay shader (from videowindow_overlay.hlsl)
    const char* g_overlayShaderSource = R"(
// Subtitle-only overlay shader. Playback controls/progress are rendered by the
// shared GPU text-grid overlay pass used by ASCII PiP.

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

cbuffer Constants : register(b0) {
    uint isFullRange;
    uint yuvMatrix;
    uint yuvTransfer;
    uint bitDepth;
    float uiProgress;
    float uiAlpha;
    uint uiPaused;
    uint uiHasRGBA;
    uint uiVolPct;
    uint uiRotationQuarterTurns;
    float uiTextTop;
    float uiTextHeight;
    float uiTextLeft;
    float uiTextWidth;
    float subtitleTop;
    float subtitleHeight;
    float subtitleLeft;
    float subtitleWidth;
    float subtitleAlpha;
    float pad3;
    float pad4;
    float pad5;
};

PS_INPUT VS(uint vid : SV_VertexID) {
    PS_INPUT output;
    output.tex = float2(vid & 1, vid >> 1);
    output.pos = float4(output.tex * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

Texture2D texSubtitle : register(t4);
SamplerState sam : register(s0);

float4 PS_UI(PS_INPUT input) : SV_Target {
    float2 uv = input.tex;
    if (subtitleAlpha > 0.01 && subtitleHeight > 0.0) {
        if (uv.y >= subtitleTop && uv.y <= (subtitleTop + subtitleHeight) &&
            uv.x >= subtitleLeft && uv.x <= (subtitleLeft + subtitleWidth)) {
        float localY = (uv.y - subtitleTop) / subtitleHeight;
        float localX = (uv.x - subtitleLeft) / subtitleWidth;
        float2 textUV = float2(localX, localY);
        float4 t = texSubtitle.Sample(sam, textUV);
        if (t.a > 0.01) {
                t.a *= subtitleAlpha;
                return t;
            }
        }
    }
    discard;
    return float4(0, 0, 0, 0);
}
    )";
    #endif
}

VideoWindow::VideoWindow()
    : m_closeRequestedEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

VideoWindow::~VideoWindow() {
    Close();
    if (m_closeRequestedEvent) {
        CloseHandle(m_closeRequestedEvent);
        m_closeRequestedEvent = nullptr;
    }
}

uint32_t VideoWindow::OutputColorSpaceShaderValue() const {
    return static_cast<uint32_t>(m_outputColorState.encoding);
}

float VideoWindow::OutputSdrWhiteNits() const {
    if (VideoOutputUsesHdr(m_outputColorState)) {
        assert(m_outputColorState.outputSdrWhiteNitsAvailable);
        assert(std::isfinite(m_outputColorState.outputSdrWhiteNits));
        assert(m_outputColorState.outputSdrWhiteNits > 0.0f);
    }
    return m_outputColorState.outputSdrWhiteNits;
}

float VideoWindow::OutputPeakNits() const {
    if (VideoOutputUsesHdr(m_outputColorState)) {
        assert(std::isfinite(m_outputColorState.outputPeakNits));
        assert(m_outputColorState.outputPeakNits > 0.0f);
    }
    return m_outputColorState.outputPeakNits;
}

float VideoWindow::OutputFullFrameNits() const {
    if (VideoOutputUsesHdr(m_outputColorState)) {
        assert(std::isfinite(m_outputColorState.outputFullFrameNits));
        assert(m_outputColorState.outputFullFrameNits > 0.0f);
    }
    return m_outputColorState.outputFullFrameNits;
}

float VideoWindow::AsciiGlyphPeakNits() const {
    if (VideoOutputUsesHdr(m_outputColorState)) {
        assert(std::isfinite(m_outputColorState.asciiGlyphPeakNits));
        assert(m_outputColorState.asciiGlyphPeakNits > 0.0f);
    }
    return m_outputColorState.asciiGlyphPeakNits;
}

void VideoWindow::FillOutputColorConstants(ShaderConstants& constants) const {
    constants.outputColorSpace = OutputColorSpaceShaderValue();
    constants.outputSdrWhiteNits = OutputSdrWhiteNits();
    constants.outputPeakNits = OutputPeakNits();
    constants.outputFullFrameNits = OutputFullFrameNits();
    constants.asciiGlyphPeakNits = AsciiGlyphPeakNits();
}

void VideoWindow::SetOutputColorAttemptStatus(const std::string& status) {
    m_outputColorAttemptStatus = status;
}

std::string VideoWindow::OutputColorDebugLine() const {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    return VideoOutputColorStateDebugLine(m_outputColorState,
                                          m_outputColorAttemptStatus);
}

bool VideoWindow::OutputUsesHdr() const {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    return VideoOutputUsesHdr(m_outputColorState);
}

void VideoWindow::SetCursorVisible(bool visible) {
    bool old = m_cursorVisible.exchange(visible, std::memory_order_relaxed);
    if (old == visible) {
        return;
    }
    if (!m_hWnd) {
        return;
    }
    if (visible) {
        ::SetCursor(::LoadCursor(NULL, IDC_ARROW));
    } else {
        ::SetCursor(nullptr);
    }
    ::PostMessage(m_hWnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(m_hWnd),
                  MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void VideoWindow::SetPictureInPictureInteractiveRects(
    const std::vector<RECT>& rects) {
    std::lock_guard<std::mutex> lock(m_pictureInPictureInteractiveRectsMutex);
    m_pictureInPictureInteractiveRects = rects;
}

void VideoWindow::SetTextGridMinimumSize(int cols, int rows) {
    m_textGridMinCols.store(std::max(0, cols),
                                        std::memory_order_relaxed);
    m_textGridMinRows.store(std::max(0, rows),
                                        std::memory_order_relaxed);
}

double VideoWindow::PictureInPictureAspectRatio() const {
    double aspect = 16.0 / 9.0;
    if (m_videoWidth > 0 && m_videoHeight > 0) {
        aspect = static_cast<double>(m_videoWidth) /
                 static_cast<double>(m_videoHeight);
    } else if (m_width > 0 && m_height > 0) {
        aspect = static_cast<double>(m_width) / static_cast<double>(m_height);
    }
    if (m_textGridPresentationEnabled.load(std::memory_order_relaxed)) {
        const SIZE cellSize = TextGridCellSize();
        const double cellAspect =
            static_cast<double>(std::max<LONG>(1, cellSize.cx)) /
            static_cast<double>(std::max<LONG>(1, cellSize.cy));
        aspect *= 2.0 * cellAspect;
    }
    return std::clamp(aspect, 0.50, 3.00);
}

SIZE VideoWindow::PictureInPictureMinimumSize() const {
    const double aspect = PictureInPictureAspectRatio();
    const bool textGridPresentation =
        m_textGridPresentationEnabled.load(std::memory_order_relaxed);
    const int kMinLongEdge = textGridPresentation ? 320 : 260;
    const int kMinShortEdge = textGridPresentation ? 180 : 96;
    int minWidth = aspect >= 1.0 ? kMinLongEdge : kMinShortEdge;
    int minHeight = aspect >= 1.0 ? kMinShortEdge : kMinLongEdge;

    if (textGridPresentation) {
        const SIZE cellSize = TextGridCellSize();
        const int cellWidth = std::max(1, static_cast<int>(cellSize.cx));
        const int cellHeight = std::max(1, static_cast<int>(cellSize.cy));
        minWidth = std::max(
            minWidth, m_textGridMinCols.load(
                          std::memory_order_relaxed) *
                          cellWidth);
        minHeight = std::max(
            minHeight, m_textGridMinRows.load(
                           std::memory_order_relaxed) *
                           cellHeight);
    }

    SIZE size{};
    size.cy = static_cast<LONG>(std::max(
        minHeight,
        static_cast<int>(std::ceil(static_cast<double>(minWidth) / aspect))));
    size.cx = static_cast<LONG>(std::ceil(size.cy * aspect));
    if (size.cx < minWidth) {
        size.cx = minWidth;
        size.cy = static_cast<LONG>(std::ceil(size.cx / aspect));
    }
    return size;
}

int VideoWindow::PictureInPictureInteractiveTop() const {
    if (m_width <= 0 || m_height <= 0) return 0;
    const int edge = PictureInPictureResizeBorderPx();
    int top = m_height -
              std::max(48, static_cast<int>(std::lround(m_height * 0.22)));
    if (m_textGridPresentationEnabled.load(std::memory_order_relaxed)) {
        const int rows =
            m_textGridRows.load(std::memory_order_relaxed);
        const int interactiveRows = rows > 0 ? std::min(rows, 8) : 8;
        top = rows > 0
                  ? std::min(m_height,
                             std::max(0, rows - interactiveRows) *
                                 static_cast<int>(TextGridCellSize().cy))
                  : m_height -
                        std::max(edge,
                                 static_cast<int>(
                                     std::lround(m_height * 0.42)));
    }
    return std::clamp(top, 0, m_height);
}

int VideoWindow::PictureInPictureResizeBorderPx() const {
    UINT dpi = 96;
    if (m_hWnd) {
        dpi = GetDpiForWindow(m_hWnd);
    }
    return std::clamp(static_cast<int>(MulDiv(6, static_cast<int>(dpi), 96)),
                      4, 10);
}

int VideoWindow::PictureInPictureVisualBorderPx() const {
    UINT dpi = 96;
    if (m_hWnd) {
        dpi = GetDpiForWindow(m_hWnd);
    }
    return std::clamp(static_cast<int>(MulDiv(1, static_cast<int>(dpi), 96)),
                      1, 2);
}

void VideoWindow::DrawPictureInPictureBorder(ID3D11DeviceContext* context) {
    if (!m_pictureInPicture.load(std::memory_order_relaxed) || !context ||
        !m_renderTargetView || m_width <= 1 || m_height <= 1) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))) ||
        !context1) {
        return;
    }

    const LONG border = static_cast<LONG>(PictureInPictureVisualBorderPx());
    const D3D11_RECT rects[] = {
        {0, 0, static_cast<LONG>(m_width), border},
        {0, static_cast<LONG>(m_height) - border,
         static_cast<LONG>(m_width), static_cast<LONG>(m_height)},
        {0, border, border, static_cast<LONG>(m_height) - border},
        {static_cast<LONG>(m_width) - border, border,
         static_cast<LONG>(m_width), static_cast<LONG>(m_height) - border},
    };
    const float color[4] = {0.20f, 0.22f, 0.25f, 1.0f};
    context1->ClearView(
        m_renderTargetView.Get(), color, rects,
        static_cast<UINT>(sizeof(rects) / sizeof(rects[0])));
}

void VideoWindow::AdjustPictureInPictureSizingRect(WPARAM edge,
                                                   RECT* rect) const {
    if (!rect) return;
    const double aspect = PictureInPictureAspectRatio();
    const SIZE minSize = PictureInPictureMinimumSize();

    const bool leftEdge = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT ||
                          edge == WMSZ_BOTTOMLEFT;
    const bool rightEdge = edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT ||
                           edge == WMSZ_BOTTOMRIGHT;
    const bool topEdge = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT ||
                         edge == WMSZ_TOPRIGHT;
    const bool bottomEdge = edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT ||
                            edge == WMSZ_BOTTOMRIGHT;
    const bool horizontalOnly = (leftEdge || rightEdge) && !topEdge && !bottomEdge;
    const bool verticalOnly = (topEdge || bottomEdge) && !leftEdge && !rightEdge;
    const bool corner = (leftEdge || rightEdge) && (topEdge || bottomEdge);

    int width = std::max(1, static_cast<int>(rect->right - rect->left));
    int height = std::max(1, static_cast<int>(rect->bottom - rect->top));

    bool deriveWidthFromHeight = verticalOnly;
    if (corner) {
        const int currentW = std::max(1, m_width);
        const int currentH = std::max(1, m_height);
        const double horizontalDelta = std::abs(width - currentW);
        const double verticalDelta = std::abs(height - currentH) * aspect;
        deriveWidthFromHeight = verticalDelta > horizontalDelta;
    }

    if (deriveWidthFromHeight) {
        height = std::max<int>(height, minSize.cy);
        width = static_cast<int>(std::lround(height * aspect));
        if (width < minSize.cx) {
            width = minSize.cx;
            height = static_cast<int>(std::lround(width / aspect));
        }
    } else {
        width = std::max<int>(width, minSize.cx);
        height = static_cast<int>(std::lround(width / aspect));
        if (height < minSize.cy) {
            height = minSize.cy;
            width = static_cast<int>(std::lround(height * aspect));
        }
    }

    if (corner) {
        if (leftEdge) {
            rect->left = rect->right - width;
        } else {
            rect->right = rect->left + width;
        }
        if (topEdge) {
            rect->top = rect->bottom - height;
        } else {
            rect->bottom = rect->top + height;
        }
        return;
    }

    if (horizontalOnly) {
        const LONG centerY = rect->top + (rect->bottom - rect->top) / 2;
        if (leftEdge) {
            rect->left = rect->right - width;
        } else {
            rect->right = rect->left + width;
        }
        rect->top = centerY - height / 2;
        rect->bottom = rect->top + height;
        return;
    }

    if (verticalOnly) {
        const LONG centerX = rect->left + (rect->right - rect->left) / 2;
        if (topEdge) {
            rect->top = rect->bottom - height;
        } else {
            rect->bottom = rect->top + height;
        }
        rect->left = centerX - width / 2;
        rect->right = rect->left + width;
    }
}

RECT VideoWindow::CalculatePictureInPictureRect() const {
    RECT work{0, 0, 1280, 720};
    HMONITOR monitor = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfo(monitor, &monitorInfo)) {
        work = monitorInfo.rcWork;
    }

    const int workLeft = static_cast<int>(work.left);
    const int workTop = static_cast<int>(work.top);
    const int workRight = static_cast<int>(work.right);
    const int workBottom = static_cast<int>(work.bottom);
    const int workW = std::max(1, workRight - workLeft);
    const int workH = std::max(1, workBottom - workTop);
    const int margin = std::clamp(std::min(workW, workH) / 40, 12, 32);
    const int usableW = std::max(160, workW - margin * 2);
    const int usableH = std::max(120, workH - margin * 2);
    const SIZE minSize = PictureInPictureMinimumSize();
    const int minW =
        std::min(usableW, std::max(160, static_cast<int>(minSize.cx)));
    const int maxW = std::max(minW, std::min(usableW, 720));

    int targetW = std::clamp(workW * 28 / 100, minW, maxW);
    const double aspect = PictureInPictureAspectRatio();

    int targetH = std::max(1, static_cast<int>(std::lround(targetW / aspect)));
    if (targetH > usableH) {
        targetH = usableH;
        targetW = std::max(1, static_cast<int>(std::lround(targetH * aspect)));
        if (targetW < minW && minSize.cy <= usableH) {
            targetW = minW;
            targetH =
                std::max(1, static_cast<int>(std::lround(targetW / aspect)));
        }
    }

    const int left = std::max(workLeft + margin,
                              workRight - margin - targetW);
    const int top = std::max(workTop + margin,
                             workBottom - margin - targetH);
    return RECT{left, top, left + targetW, top + targetH};
}

bool VideoWindow::PictureInPictureHasInteractiveRects() const {
    std::lock_guard<std::mutex> lock(m_pictureInPictureInteractiveRectsMutex);
    return !m_pictureInPictureInteractiveRects.empty();
}

bool VideoWindow::PictureInPicturePointInInteractiveRect(int x, int y) const {
    std::lock_guard<std::mutex> lock(m_pictureInPictureInteractiveRectsMutex);
    for (const RECT& rect : m_pictureInPictureInteractiveRects) {
        if (x >= rect.left && x < rect.right && y >= rect.top &&
            y < rect.bottom) {
            return true;
        }
    }
    return false;
}

LRESULT VideoWindow::HitTestPictureInPicture(int x, int y) const {
    if (!m_pictureInPicture.load(std::memory_order_relaxed)) return HTCLIENT;
    if (m_width <= 0 || m_height <= 0) return HTCAPTION;

    const int edge = PictureInPictureResizeBorderPx();
    const bool left = x >= 0 && x < edge;
    const bool right = x >= m_width - edge && x < m_width;
    const bool top = y >= 0 && y < edge;
    const bool bottom = y >= m_height - edge && y < m_height;

    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;

    if (PictureInPictureHasInteractiveRects()) {
        if (x >= 0 && x < m_width && y >= 0 && y < m_height) {
            return PictureInPicturePointInInteractiveRect(x, y) ? HTCLIENT
                                                               : HTCAPTION;
        }
        return HTCLIENT;
    }

    if (x >= 0 && x < m_width && y >= 0 && y < m_height &&
        y >= PictureInPictureInteractiveTop()) {
        return HTCLIENT;
    }

    const int reservedBottom =
        std::max(48, static_cast<int>(std::lround(m_height * 0.22)));
    if (x >= 0 && x < m_width && y >= 0 && y < m_height - reservedBottom) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

bool VideoWindow::EnterPictureInPicture() {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd || !m_swapChain) return false;
    if (m_pictureInPicture.load(std::memory_order_relaxed)) return true;

    const bool restoreFullscreen = m_isFullscreen;
    if (restoreFullscreen && !ExitFullscreen()) {
        return false;
    }

    m_pipRestoreFullscreen = restoreFullscreen;
    m_pipRestoreStyle = GetWindowLong(m_hWnd, GWL_STYLE);
    m_pipRestoreExStyle = GetWindowLong(m_hWnd, GWL_EXSTYLE);
    GetWindowRect(m_hWnd, &m_pipRestoreRect);

    RECT pipRect = CalculatePictureInPictureRect();
    m_ignoreWindowSizeEvents = true;
    m_pictureInPicture.store(true, std::memory_order_relaxed);
    SetWindowLong(m_hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    LONG pipExStyle =
        (m_pipRestoreExStyle | WS_EX_TOPMOST | WS_EX_TOOLWINDOW) &
        ~WS_EX_APPWINDOW;
    SetWindowLong(m_hWnd, GWL_EXSTYLE, pipExStyle);
    SetWindowPos(m_hWnd, HWND_TOPMOST, pipRect.left, pipRect.top,
                 pipRect.right - pipRect.left, pipRect.bottom - pipRect.top,
                 SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    ::ShowWindow(m_hWnd, SW_SHOW);
    BringWindowToTop(m_hWnd);
    UpdateWindow(m_hWnd);

    RECT client{};
    if (GetClientRect(m_hWnd, &client)) {
        Resize(client.right - client.left, client.bottom - client.top);
    }
    m_ignoreWindowSizeEvents = false;
    return true;
}

bool VideoWindow::ExitPictureInPicture(PictureInPictureExitTarget target) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd || !m_swapChain) return false;
    if (!m_pictureInPicture.load(std::memory_order_relaxed)) {
        return target == PictureInPictureExitTarget::Fullscreen && !m_isFullscreen
                   ? MakeFullscreen()
                   : true;
    }

    const bool targetFullscreen =
        target == PictureInPictureExitTarget::Fullscreen ||
        m_pipRestoreFullscreen;
    m_ignoreWindowSizeEvents = true;
    SetWindowLong(m_hWnd, GWL_STYLE, m_pipRestoreStyle);
    SetWindowLong(m_hWnd, GWL_EXSTYLE, m_pipRestoreExStyle);
    HWND zOrder =
        (m_pipRestoreExStyle & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(m_hWnd, zOrder, m_pipRestoreRect.left, m_pipRestoreRect.top,
                 m_pipRestoreRect.right - m_pipRestoreRect.left,
                 m_pipRestoreRect.bottom - m_pipRestoreRect.top,
                 SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    ::ShowWindow(m_hWnd, SW_RESTORE);
    UpdateWindow(m_hWnd);

    RECT client{};
    if (GetClientRect(m_hWnd, &client)) {
        Resize(client.right - client.left, client.bottom - client.top);
    }
    m_pictureInPicture.store(false, std::memory_order_relaxed);
    SetPictureInPictureInteractiveRects({});
    m_pipRestoreFullscreen = false;
    m_ignoreWindowSizeEvents = false;

    if (targetFullscreen) {
        return MakeFullscreen();
    }
    return true;
}

bool VideoWindow::SetPictureInPicture(bool enabled) {
    if (m_hWnd && m_windowThreadId != 0 &&
        GetCurrentThreadId() != m_windowThreadId) {
        return PostMessage(m_hWnd, kSetPictureInPictureMessage,
                           enabled ? TRUE : FALSE, 0) != 0;
    }
    return enabled ? EnterPictureInPicture() : ExitPictureInPicture();
}

bool VideoWindow::ExitPictureInPictureToFullscreen() {
    if (m_hWnd && m_windowThreadId != 0 &&
        GetCurrentThreadId() != m_windowThreadId) {
        return PostMessage(m_hWnd, kExitPictureInPictureToFullscreenMessage,
                           0, 0) != 0;
    }
    return ExitPictureInPicture(PictureInPictureExitTarget::Fullscreen);
}

void VideoWindow::SetTextGridPresentationEnabled(bool enabled) {
    m_textGridPresentationEnabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        m_textGridCols.store(0, std::memory_order_relaxed);
        m_textGridRows.store(0, std::memory_order_relaxed);
    }
}

bool VideoWindow::GetWindowBounds(RECT* outRect) const {
    if (!outRect || !m_hWnd) {
        return false;
    }
    return GetWindowRect(m_hWnd, outRect) != FALSE;
}

bool VideoWindow::GetPictureInPictureRestoreBounds(RECT* outRect) const {
    if (!outRect || !m_hWnd ||
        !m_pictureInPicture.load(std::memory_order_relaxed)) {
        return false;
    }
    *outRect = m_pipRestoreRect;
    return true;
}

bool VideoWindow::SetWindowBounds(const RECT& rect) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd) {
        return false;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }
    m_ignoreWindowSizeEvents = true;
    HWND zOrder = m_pictureInPicture.load(std::memory_order_relaxed)
                      ? HWND_TOPMOST
                      : HWND_NOTOPMOST;
    SetWindowPos(m_hWnd, zOrder, rect.left, rect.top, width, height,
                 SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    ::ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);

    RECT client{};
    if (GetClientRect(m_hWnd, &client)) {
        Resize(client.right - client.left, client.bottom - client.top);
    }
    m_ignoreWindowSizeEvents = false;
    return true;
}

bool VideoWindow::TogglePictureInPicture() {
    if (m_hWnd && m_windowThreadId != 0 &&
        GetCurrentThreadId() != m_windowThreadId) {
        return PostMessage(m_hWnd, kTogglePictureInPictureMessage, 0, 0) != 0;
    }
    return SetPictureInPicture(
        !m_pictureInPicture.load(std::memory_order_relaxed));
}

bool VideoWindow::SetFullscreen(bool enabled) {
    if (m_hWnd && m_windowThreadId != 0 &&
        GetCurrentThreadId() != m_windowThreadId) {
        return PostMessage(m_hWnd, kSetFullscreenMessage, enabled ? TRUE : FALSE,
                           0) != 0;
    }
    return enabled ? MakeFullscreen() : ExitFullscreen();
}

bool VideoWindow::MakeFullscreen() {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd || !m_swapChain) return false;
    // Save current style and rect
    m_prevStyle = GetWindowLong(m_hWnd, GWL_STYLE);
    GetWindowRect(m_hWnd, &m_prevRect);

    // Feature toggle: when false we DO NOT attempt any fallback (exclusive fullscreen)
    static constexpr bool kAllowFullscreenFallback = false; // TEMP: user requested no fallback

    // Prefer borderless fullscreen (WS_POPUP) as the only allowed method when fallback is disabled
    HMONITOR hm = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);

    // Save extended style so we can restore it on exit
    m_prevExStyle = GetWindowLong(m_hWnd, GWL_EXSTYLE);
    // diagnostic: entering fullscreen (log removed)

    if (GetMonitorInfo(hm, &mi)) {
        UINT monW = static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left);
        UINT monH = static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);

        // Diagnostic: print current swapchain desc before attempting ResizeBuffers
        {
            DXGI_SWAP_CHAIN_DESC desc = {};
            HRESULT g = m_swapChain->GetDesc(&desc);
            if (SUCCEEDED(g)) {
                // swapchain desc diagnostic removed
            } else {
                std::fprintf(stderr, "VideoWindow: GetDesc failed (0x%08X)\n", static_cast<unsigned int>(g));
            }
        }

        // Prepare window for borderless fullscreen first: set style/exstyle/position so the window is ready
        // Prevent WM_SIZE messages from racing and resetting our freshly-applied fullscreen size
        m_ignoreWindowSizeEvents = true;
        SetWindowLong(m_hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        LONG newEx = m_prevExStyle & ~(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
        SetWindowLong(m_hWnd, GWL_EXSTYLE, newEx);
    // SetWindowLong diagnostic removed

        SetWindowPos(m_hWnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, monW, monH, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        ::ShowWindow(m_hWnd, SW_SHOW);
        // Force a layout/update before resizing buffers
        UpdateWindow(m_hWnd);

        // Prepare to recreate the swapchain at monitor resolution.
        // Unbind and clear context to release any references to the swapchain buffers.
        {
            ID3D11Device* dev = getSharedGpuDevice();
            if (dev) {
                ID3D11DeviceContext* ctx = nullptr;
                dev->GetImmediateContext(&ctx);
                if (ctx) {
                    ID3D11ShaderResourceView* nullSRVs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
                    ctx->PSSetShaderResources(0, 5, nullSRVs);
                    ID3D11RenderTargetView* nullRTV = nullptr;
                    ctx->OMSetRenderTargets(0, &nullRTV, nullptr);
                    ctx->VSSetShader(nullptr, nullptr, 0);
                    ctx->PSSetShader(nullptr, nullptr, 0);
                    ctx->ClearState();
                    ctx->Flush();
                    ctx->Release();
                    // Unbound RTVs/SRVs diagnostic removed
                }
            }
            if (m_renderTargetView) { m_renderTargetView.Reset(); }
            ResetSwapChain();
        }

        // Recreate the swapchain sized to the monitor (borderless windowed fullscreen)
        // Indicate that Present must wait for the new RTV/backbuffer to be ready
        m_waitingForRenderTarget = true;
        if (!CreateSwapChain(static_cast<int>(monW), static_cast<int>(monH))) {
            std::fprintf(stderr, "VideoWindow: CreateSwapChain(borderless) failed\n");
            if (!kAllowFullscreenFallback) {
                std::fprintf(stderr, "VideoWindow: fullscreen fallback disabled; aborting (create swapchain failed)\n");
                m_ignoreWindowSizeEvents = false;
                return false;
            }
        } else {
            // Success: ensure focus and topmost ordering
            SetForegroundWindow(m_hWnd);
            SetActiveWindow(m_hWnd);
            SetFocus(m_hWnd);
            BringWindowToTop(m_hWnd);
            // Defensive: explicitly call Resize to ensure internal size/state is updated
            Resize(static_cast<int>(monW), static_cast<int>(monH));
            // after CreateSwapChain diagnostic removed
            // entered borderless fullscreen (diagnostic removed)
            m_isFullscreen = true;
            // Allow WM_SIZE processing again now that we've updated internal state
            m_ignoreWindowSizeEvents = false;
            return true;
        }
    }

    // If we reach here and fallback is disabled, do not try other methods
    if (!kAllowFullscreenFallback) {
        std::fprintf(stderr, "VideoWindow: borderless fullscreen could not be started and fallback is disabled\n");
        m_ignoreWindowSizeEvents = false;
        return false;
    }

    // Fallback (disabled by default): try exclusive fullscreen as last resort
    HRESULT hr = m_swapChain->SetFullscreenState(TRUE, NULL);
    if (SUCCEEDED(hr)) {
        // exclusive fullscreen success message removed
        // Attempt to resize buffers to monitor resolution
        if (GetMonitorInfo(hm, &mi)) {
            UINT monW = static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left);
            UINT monH = static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);
            HRESULT r3 = m_swapChain->ResizeBuffers(0, monW, monH, DXGI_FORMAT_UNKNOWN, 0);
            if (SUCCEEDED(r3)) {
                Resize(static_cast<int>(monW), static_cast<int>(monH));
                m_isFullscreen = true;
                // exclusive fullscreen buffers resized message removed
                return true;
            }
            std::fprintf(stderr, "VideoWindow: ResizeBuffers(exclusive) failed (0x%08X)\n", static_cast<unsigned int>(r3));
        }
    } else {
        std::fprintf(stderr, "VideoWindow: SetFullscreenState(TRUE) failed (0x%08X)\n", static_cast<unsigned int>(hr));
    }

    std::fprintf(stderr, "VideoWindow: all fullscreen methods failed\n");
    return false;
}

bool VideoWindow::ExitFullscreen() {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd || !m_swapChain) return false;
    HRESULT hr = m_swapChain->SetFullscreenState(FALSE, NULL);
    if (FAILED(hr)) {
        std::fprintf(stderr, "VideoWindow: SetFullscreenState(FALSE) failed (0x%08X)\n", static_cast<unsigned int>(hr));
        // even if DXGI failed, try to restore window style
    }
    // Restore style and position
    SetWindowLong(m_hWnd, GWL_STYLE, m_prevStyle);
    // Restore extended style as well
    SetWindowLong(m_hWnd, GWL_EXSTYLE, m_prevExStyle);

    // Remove topmost if we set it earlier and restore z-order
    SetWindowPos(m_hWnd, HWND_NOTOPMOST, m_prevRect.left, m_prevRect.top, m_prevRect.right - m_prevRect.left, m_prevRect.bottom - m_prevRect.top, SWP_NOZORDER | SWP_FRAMECHANGED);

    // Resize back to previous logical size
    Resize(m_prevRect.right - m_prevRect.left, m_prevRect.bottom - m_prevRect.top);

    // Restore keyboard focus so shortcuts keep working
    ::ShowWindow(m_hWnd, SW_RESTORE);
    SetForegroundWindow(m_hWnd);
    SetActiveWindow(m_hWnd);
    SetFocus(m_hWnd);

    m_isFullscreen = false;
    // exited fullscreen message removed
    return true;
}

void VideoWindow::Cleanup() {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) return;

    // Unbind shader resources / UAVs / RTVs and clear state to avoid driver pinning
    ID3D11ShaderResourceView* nullSRVs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    context->PSSetShaderResources(0, 5, nullSRVs);
    context->CSSetShaderResources(0, 5, nullSRVs);

    ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
    context->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);

    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(0, &nullRTV, nullptr);

    context->VSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);

    // Force clear/flush to ensure driver releases any references
    context->ClearState();
    context->Flush();
    context->Release();

    // Reset COM objects we hold
    m_renderTargetView.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    m_uiShader.Reset();
    m_gpuTextGridShader.Reset();
    m_uiBlendState.Reset();
    m_sampler.Reset();
    m_constantBuffer.Reset();
    // frame cache is now owned/managed externally
    m_subtitleTexture.Reset();
    m_subtitleSrv.Reset();
    m_subtitleWidth = 0;
    m_subtitleHeight = 0;
    m_tuiTexture.Reset();
    m_tuiSrv.Reset();
    m_gpuTextGridTexture.Reset();
    m_gpuTextGridSrv.Reset();
    m_gpuTextGlyphAtlasTexture.Reset();
    m_gpuTextGlyphAtlasSrv.Reset();
    m_gpuTextGridConstants.Reset();
    m_gpuTextGlyphAtlasCellWidth = 0;
    m_gpuTextGlyphAtlasCellHeight = 0;
    m_gpuTextGlyphAtlasDpi = 0;
    m_gpuTextGlyphAtlasWeight = 0;
    m_gpuTextGridCols = 0;
    m_gpuTextGridRows = 0;
}

bool VideoWindow::Open(int width, int height, const std::string& title,
                       bool startFullscreen) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    m_input.beginWindowThread();
    m_closeRequested.store(false, std::memory_order_relaxed);
    if (m_closeRequestedEvent) {
        ResetEvent(m_closeRequestedEvent);
    }
    HINSTANCE hInstance = GetModuleHandle(NULL);
    const wchar_t* className = L"RadioifyVideoWindow";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    if (!GetClassInfoExW(hInstance, className, &wc)) {
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassExW(&wc);
    }

    RECT wr = { 0, 0, width, height };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    m_hWnd = CreateWindowExW(
        0, className, std::wstring(title.begin(), title.end()).c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, hInstance, this);

    if (!m_hWnd) {
        m_input.endWindowThread();
        return false;
    }
    m_windowThreadId = GetCurrentThreadId();

    // Remember base window title so we can temporarily update it while overlay is visible
    m_lastWindowTitle = title;
    m_baseWindowTitle = title;

    if (!CreateSwapChain(width, height)) {
        ::DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
        m_windowThreadId = 0;
        m_input.endWindowThread();
        return false;
    }

    // Video playback uses fullscreen by default, but callers can opt out
    // (used by windowed TUI mode).
    if (startFullscreen) {
        MakeFullscreen();
    }

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) {
        std::fprintf(stderr, "VideoWindow: no device in Open()\n");
        Close();
        if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; }
        return false;
    }

    // Ensure multithread protection if available (ascii renderer enables this)
    {
        Microsoft::WRL::ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
            mt->SetMultithreadProtected(TRUE);
        }
    }

    HRESULT hr = device->CreateVertexShader(kVideoWindowVs, kVideoWindowVs_Size, NULL, &m_vertexShader);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreateVertexShader failed (0x%08X)\n", static_cast<unsigned int>(hr)); Close(); if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; } return false; }
    hr = device->CreatePixelShader(kVideoWindowPs, kVideoWindowPs_Size, NULL, &m_pixelShader);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreatePixelShader(PS) failed (0x%08X)\n", static_cast<unsigned int>(hr)); Close(); if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; } return false; }
    hr = device->CreatePixelShader(kVideoWindowPsUi, kVideoWindowPsUi_Size, NULL, &m_uiShader);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreatePixelShader(UI) failed (0x%08X)\n", static_cast<unsigned int>(hr)); Close(); if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; } return false; }
    hr = device->CreatePixelShader(kVideoWindowPsGpuTextGrid,
                                   kVideoWindowPsGpuTextGrid_Size, NULL,
                                   &m_gpuTextGridShader);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreatePixelShader(GPU text grid) failed (0x%08X)\n", static_cast<unsigned int>(hr)); Close(); if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; } return false; }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(ShaderConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbDesc, NULL, &m_constantBuffer);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreateBuffer(constant) failed (0x%08X)\n", static_cast<unsigned int>(hr)); Close(); if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; } return false; }

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampDesc, &m_sampler);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreateSamplerState failed (0x%08X)\n", static_cast<unsigned int>(hr)); Close(); if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; } return false; }

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&blendDesc, &m_uiBlendState);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreateBlendState(UI) failed (0x%08X)\n", static_cast<unsigned int>(hr)); Close(); if (m_hWnd) { ::DestroyWindow(m_hWnd); m_hWnd = nullptr; } return false; }

    ::ShowWindow(m_hWnd, SW_SHOW);
    m_width = width;
    m_height = height;

    return true;
}

void VideoWindow::ResetSwapChain() {
    {
        std::lock_guard<std::mutex> lock(m_frameLatencyMutex);
        if (m_frameLatencyWaitableObject) {
            CloseHandle(m_frameLatencyWaitableObject);
            m_frameLatencyWaitableObject = nullptr;
        }
    }
    m_renderTargetView.Reset();
    m_swapChain2.Reset();
    m_swapChain.Reset();
}

bool VideoWindow::CreateSwapChain(int width, int height) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    ID3D11Device* device = getSharedGpuDevice();
    if (!device) {
        std::fprintf(stderr, "VideoWindow: no shared GPU device available\n");
        return false;
    }
    if (width <= 0 || height <= 0) {
        std::fprintf(stderr, "VideoWindow: invalid swapchain dimensions %d x %d\n", width, height);
        return false;
    }

    ResetSwapChain();
    
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    
    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    dxgiDevice->GetAdapter(&dxgiAdapter);

    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory2;
    HRESULT factoryHr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory2));
    HdrGenerateDecision hdrDecision =
        ResolveHdrGenerateOutputState(m_hWnd, dxgiAdapter.Get());
    VideoOutputColorState desiredColorState = hdrDecision.state;
    SetOutputColorAttemptStatus({});
    std::vector<VideoOutputColorState> colorAttempts;
    colorAttempts.push_back(desiredColorState);
    if (desiredColorState.encoding == VideoOutputColorEncoding::Hdr10) {
        colorAttempts.push_back(VideoOutputScRgbGenerateState(desiredColorState));
    }
    if (VideoOutputUsesHdr(desiredColorState)) {
        colorAttempts.push_back(VideoOutputSdrFallbackState(desiredColorState));
    }
    std::string lastColorAttemptFailure;
    for (const VideoOutputColorState& colorState : colorAttempts) {

        if (SUCCEEDED(factoryHr) && dxgiFactory2) {
            DXGI_SWAP_CHAIN_DESC1 scd1 = {};
            scd1.Width = static_cast<UINT>(width);
            scd1.Height = static_cast<UINT>(height);
            scd1.Format = colorState.swapChainFormat;
            scd1.SampleDesc.Count = 1;
            scd1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd1.BufferCount = 2;
            scd1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            scd1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            scd1.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

            Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
            HRESULT hr = dxgiFactory2->CreateSwapChainForHwnd(
                device, m_hWnd, &scd1, nullptr, nullptr, &swapChain1);
            if (SUCCEEDED(hr)) {
                m_swapChain = swapChain1;
                if (!ApplyVideoOutputColorSpace(*m_swapChain.Get(),
                                                colorState)) {
                    lastColorAttemptFailure =
                        std::string("failed=set_") +
                        VideoOutputColorEncodingName(colorState.encoding);
                    SetOutputColorAttemptStatus(lastColorAttemptFailure);
                    ResetSwapChain();
                    continue;
                }
                m_outputColorState = colorState;
                const char* stage =
                    colorState.encoding == desiredColorState.encoding
                        ? "active"
                        : "downgrade";
                std::string status = VideoOutputColorAttemptStatus(
                    hdrDecision, m_outputColorState, stage);
                if (!lastColorAttemptFailure.empty()) {
                    status += " last_failure=" + lastColorAttemptFailure;
                }
                SetOutputColorAttemptStatus(status);
                swapChain1.As(&m_swapChain2);
                if (m_swapChain2) {
                    m_swapChain2->SetMaximumFrameLatency(1);
                    std::lock_guard<std::mutex> latencyLock(m_frameLatencyMutex);
                    m_frameLatencyWaitableObject =
                        m_swapChain2->GetFrameLatencyWaitableObject();
                }
                Resize(width, height);
                return true;
            } else {
                char status[96];
                std::snprintf(status, sizeof(status), "failed=create_%s:0x%08X",
                              VideoOutputColorEncodingName(colorState.encoding),
                              static_cast<unsigned int>(hr));
                lastColorAttemptFailure = status;
                SetOutputColorAttemptStatus(lastColorAttemptFailure);
            }
        }
    }

    Microsoft::WRL::ComPtr<IDXGIFactory> dxgiFactory;
    if (FAILED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory))) || !dxgiFactory) {
        std::fprintf(stderr, "VideoWindow: failed to get DXGI factory\n");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    m_outputColorState = desiredColorState;
    m_outputColorState.swapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_outputColorState.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    m_outputColorState.encoding = VideoOutputColorEncoding::Sdr;
    std::string legacyStatus = VideoOutputColorAttemptStatus(
        hdrDecision, m_outputColorState, "legacy_sdr");
    if (!lastColorAttemptFailure.empty()) {
        legacyStatus += " last_failure=" + lastColorAttemptFailure;
    }
    SetOutputColorAttemptStatus(legacyStatus);
    scd.BufferDesc.Format = m_outputColorState.swapChainFormat;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = m_hWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    HRESULT scHr = dxgiFactory->CreateSwapChain(device, &scd, &m_swapChain);
    if (FAILED(scHr)) {
        scd.Flags = 0;
        scHr = dxgiFactory->CreateSwapChain(device, &scd, &m_swapChain);
    }
    if (FAILED(scHr)) {
        std::fprintf(stderr, "VideoWindow: CreateSwapChain failed\n");
        return false;
    }

    m_swapChain.As(&m_swapChain2);
    (void)ApplyVideoOutputColorSpace(*m_swapChain.Get(), m_outputColorState);
    if (m_swapChain2) {
        m_swapChain2->SetMaximumFrameLatency(1);
        std::lock_guard<std::mutex> latencyLock(m_frameLatencyMutex);
        m_frameLatencyWaitableObject =
            m_swapChain2->GetFrameLatencyWaitableObject();
    }

    Resize(width, height);
    return true;
}

void VideoWindow::Resize(int width, int height) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_swapChain) return;
    m_renderTargetView.Reset();
    UINT flags = 0;
    if (m_swapChain2) {
        flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }
    HRESULT hr = m_swapChain->ResizeBuffers(
        0, width, height, m_outputColorState.swapChainFormat, flags);
    if (FAILED(hr)) return;
    if (!ApplyVideoOutputColorSpace(*m_swapChain.Get(), m_outputColorState)) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) return;

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;
    hr = device->CreateRenderTargetView(backBuffer.Get(), NULL, &m_renderTargetView);
    if (FAILED(hr)) {
        m_renderTargetView.Reset();
        return;
    }

    m_width = width;
    m_height = height;
    // Mark that the render target is ready so Present can proceed
    m_waitingForRenderTarget = false;
}

void VideoWindow::Close() {
    m_closeRequested.store(false, std::memory_order_relaxed);
    if (m_closeRequestedEvent) {
        ResetEvent(m_closeRequestedEvent);
    }
    SetCursorVisible(true);
    m_input.endWindowThread();
    // Hide the window first to release focus/ownership of the monitor
    if (m_hWnd) {
        ::ShowWindow(m_hWnd, SW_HIDE);
    }
    // Perform centralized cleanup (unbind, ClearState, flush, reset local resources)
    Cleanup();

    // If swapchain is in full-screen exclusive, revert to windowed first and restore style
    if (m_swapChain && m_isFullscreen) {
        (void)m_swapChain->SetFullscreenState(FALSE, NULL);
    }
    // Restore windowed style if we changed it
    if (m_hWnd && m_isFullscreen) {
        SetWindowLong(m_hWnd, GWL_STYLE, m_prevStyle);
        SetWindowPos(m_hWnd, NULL, m_prevRect.left, m_prevRect.top, m_prevRect.right - m_prevRect.left, m_prevRect.bottom - m_prevRect.top, SWP_NOZORDER | SWP_FRAMECHANGED);
        m_isFullscreen = false;
    }

    // Release swapchain last
    ResetSwapChain();
    m_pictureInPicture.store(false, std::memory_order_relaxed);
    m_textGridPresentationEnabled.store(false, std::memory_order_relaxed);
    m_textGridCols.store(0, std::memory_order_relaxed);
    m_textGridRows.store(0, std::memory_order_relaxed);
    m_pipRestoreFullscreen = false;

    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
    m_windowThreadId = 0;
}

void VideoWindow::ShowWindow(bool show) {
    if (m_hWnd) {
        ::ShowWindow(m_hWnd, show ? SW_SHOW : SW_HIDE);
    }
}

bool VideoWindow::PollEvents() {
    MSG msg;
    bool handled = false;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        handled = true;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return handled;
}

bool VideoWindow::PollInput(InputEvent& ev) {
    return m_input.poll(ev);
}

bool VideoWindow::ConsumeCloseRequested() {
    const bool requested = m_closeRequested.exchange(false, std::memory_order_relaxed);
    if (requested && m_closeRequestedEvent) {
        ResetEvent(m_closeRequestedEvent);
    }
    return requested;
}

void VideoWindow::SetVsync(bool enabled) {
    m_presentInterval.store(enabled ? 1u : 0u, std::memory_order_relaxed);
}

HANDLE VideoWindow::GetFrameLatencyWaitableObject() {
    std::lock_guard<std::mutex> lock(m_frameLatencyMutex);
    return m_frameLatencyWaitableObject;
}

std::string VideoWindow::GetSubtitleRenderError() const {
    std::lock_guard<std::mutex> lock(m_subtitleStateMutex);
    return m_subtitleRenderError;
}

void VideoWindow::setSubtitleRenderError(std::string error) {
    std::lock_guard<std::mutex> lock(m_subtitleStateMutex);
    if (m_subtitleRenderError == error) return;
    m_subtitleRenderError = std::move(error);
}

void VideoWindow::UpdateViewport(int width, int height) {
    VideoViewport vp = calculateViewport(width, height, m_videoWidth, m_videoHeight);
    m_viewportX = vp.x;
    m_viewportY = vp.y;
    m_viewportW = vp.w;
    m_viewportH = vp.h;
}

void VideoWindow::Present(GpuVideoFrameCache& frameCache, const WindowUiState& ui,
                          bool nonBlocking) {
    std::unique_lock<std::recursive_mutex> lock(getSharedGpuMutex());
#if RADIOIFY_ENABLE_TIMING_LOG
    fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present enter (wnd=%p swap=%p visible=%d)\n", now_ms().c_str(), thread_id_str().c_str(), (void*)m_hWnd, (void*)m_swapChain.Get(), m_hWnd ? IsWindowVisible(m_hWnd) : 0);
#endif
    if (!m_hWnd || !m_swapChain || !IsWindowVisible(m_hWnd)) {
#if RADIOIFY_ENABLE_TIMING_LOG
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present early exit: window/swap not ready\n", now_ms().c_str(), thread_id_str().c_str());
#endif
        return;
    }
    if (!frameCache.HasFrame()) {
#if RADIOIFY_ENABLE_TIMING_LOG
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present early exit: no frame in cache\n", now_ms().c_str(), thread_id_str().c_str());
#endif
        return;
    }

    // Refresh window dimensions to avoid stale size during fullscreen transitions
    RECT rect;
    if (GetClientRect(m_hWnd, &rect)) {
        m_width = rect.right - rect.left;
        m_height = rect.bottom - rect.top;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain = m_swapChain;
    UINT presentInterval = m_presentInterval.load(std::memory_order_relaxed);

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context || !m_constantBuffer || !m_renderTargetView) return;

    m_videoWidth = frameCache.GetDisplayWidth();
    m_videoHeight = frameCache.GetDisplayHeight();

#if RADIOIFY_ENABLE_TIMING_LOG
    fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present frame w=%d h=%d ui.displaySec=%.3f\n", now_ms().c_str(), thread_id_str().c_str(), m_videoWidth, m_videoHeight, ui.displaySec);
#endif

    // Render
    float clearColor[4] = { 0, 0, 0, 1 };
    context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
    context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), NULL);

    UpdateViewport(m_width, m_height);
    D3D11_VIEWPORT viewport = { m_viewportX, m_viewportY, m_viewportW, m_viewportH, 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);

    context->IASetInputLayout(NULL);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(m_vertexShader.Get(), NULL, 0);
    context->PSSetShader(m_pixelShader.Get(), NULL, 0);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            ShaderConstants sc{};
            sc.isFullRange = frameCache.IsFullRange() ? 1 : 0;
            sc.yuvMatrix = (uint32_t)frameCache.GetMatrix();
            sc.yuvTransfer = (uint32_t)frameCache.GetTransfer();
            sc.bitDepth = (uint32_t)frameCache.GetBitDepth();
            sc.hasRGBA = frameCache.IsRgba() ? 1u : 0u;
            sc.rotationQuarterTurns =
                (uint32_t)(frameCache.GetRotationQuarterTurns() & 3);
            FillOutputColorConstants(sc);
            std::memcpy(mapped.pData, &sc, sizeof(ShaderConstants));
            context->Unmap(m_constantBuffer.Get(), 0);
        }
    }

    if (frameCache.IsRgba()) {
        ID3D11ShaderResourceView* srvs[3] = { nullptr, nullptr, frameCache.GetSrvRGBA() };
        context->PSSetShaderResources(0, 3, srvs);
    } else {
        ID3D11ShaderResourceView* srvs[2] = { frameCache.GetSrvY(), frameCache.GetSrvUV() };
        context->PSSetShaderResources(0, 2, srvs);
    }

#if defined(RADIOIFY_ENABLE_GPU_TIMING)
    {
        Microsoft::WRL::ComPtr<ID3D11Query> qDisjoint, qStart, qEnd;
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ID3D11Device* gpuDevice = getSharedGpuDevice();
        if (gpuDevice) {
            gpuDevice->CreateQuery(&qd, qDisjoint.GetAddressOf());
            qd.Query = D3D11_QUERY_TIMESTAMP;
            gpuDevice->CreateQuery(&qd, qStart.GetAddressOf());
            gpuDevice->CreateQuery(&qd, qEnd.GetAddressOf());

            context->Begin(qDisjoint.Get());
            context->End(qStart.Get());
            context->Draw(4, 0);
            DrawOverlay(ui);
            context->End(qEnd.Get());
            context->End(qDisjoint.Get());

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            UINT64 t1 = 0;
            UINT64 t2 = 0;
            if (context->GetData(qDisjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                context->GetData(qStart.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                context->GetData(qEnd.Get(), &t2, sizeof(t2), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                !disjoint.Disjoint) {
                double gpu_ms = (double)(t2 - t1) / (double)disjoint.Frequency * 1000.0;
                fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present GPU draw+overlay time %.3f ms\n", now_ms().c_str(), thread_id_str().c_str(), gpu_ms);
            }
        } else {
            context->Draw(4, 0);
            DrawOverlay(ui);
        }
    }
#else
    context->Draw(4, 0);
    DrawOverlay(ui);
#endif
    DrawPictureInPictureBorder(context.Get());
    frameCache.MarkFrameInFlight(context.Get());

#if RADIOIFY_ENABLE_TIMING_LOG
    fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present about to Present()\n", now_ms().c_str(), thread_id_str().c_str());
#endif
    lock.unlock();
    if (!swapChain) return;
    const VideoWindowPresentArgs presentArgs =
        liveVideoWindowPresentArgs(presentInterval, nonBlocking);
    HRESULT presHr =
        swapChain->Present(presentArgs.syncInterval, presentArgs.flags);
    frameCache.SignalFrameLatencyFence(context.Get());
    if (videoWindowPresentSkipped(presHr)) {
#if RADIOIFY_ENABLE_TIMING_LOG
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present skipped (0x%08X)\n", now_ms().c_str(), thread_id_str().c_str(), static_cast<unsigned int>(presHr));
#endif
        return;
    }
#if RADIOIFY_ENABLE_TIMING_LOG
    if (FAILED(presHr)) {
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present Present() FAILED 0x%08X\n", now_ms().c_str(), thread_id_str().c_str(), static_cast<unsigned int>(presHr));
    } else {
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::Present Present() OK\n", now_ms().c_str(), thread_id_str().c_str());
    }
#endif
}

void VideoWindow::DrawOverlay(const WindowUiState& ui) {
    bool showOverlay = ui.overlayAlpha > 0.01f || !ui.debugLines.empty();
    const bool hasAssScript =
        static_cast<bool>(ui.subtitleAssScript) && !ui.subtitleAssScript->empty();
    const bool hasPlaintextSubtitleCues = std::any_of(
        ui.subtitleCues.begin(), ui.subtitleCues.end(),
        [](const WindowUiState::SubtitleCue& cue) {
            return !cue.assStyled && !cue.text.empty();
        });
    bool showSubtitle =
        ui.subtitleAlpha > 0.01f && (hasPlaintextSubtitleCues || hasAssScript);
    if (!hasAssScript && !ui.subtitleRenderError.empty()) {
        setSubtitleRenderError({});
    }
    if (!showOverlay && !showSubtitle) return;

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context || !m_constantBuffer) return;

    // UI overlay is window-space: render it on the full client viewport,
    // not the letterboxed video viewport.
    D3D11_VIEWPORT overlayViewport = {
        0.0f,
        0.0f,
        static_cast<float>(std::max(1, m_width)),
        static_cast<float>(std::max(1, m_height)),
        0.0f,
        1.0f
    };
    context->RSSetViewports(1, &overlayViewport);
    
    context->PSSetShader(m_uiShader.Get(), NULL, 0);
    context->VSSetShader(m_vertexShader.Get(), NULL, 0);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
    if (m_uiBlendState) {
        const float blendFactor[4] = {0.f, 0.f, 0.f, 0.f};
        context->OMSetBlendState(m_uiBlendState.Get(), blendFactor, 0xffffffffu);
    }
    
    float subtitleTopNorm = 0.0f;
    float subtitleHeightNorm = 0.0f;
    float subtitleLeftNorm = 0.0f;
    float subtitleWidthNorm = 0.0f;
    bool drawOverlayTextGrid = false;
    D3D11_VIEWPORT overlayTextGridViewport{};
    int viewportX = std::clamp(static_cast<int>(std::lround(m_viewportX)),
                               0, std::max(0, m_width - 1));
    int viewportY = std::clamp(static_cast<int>(std::lround(m_viewportY)),
                               0, std::max(0, m_height - 1));
    int viewportW = std::clamp(static_cast<int>(std::lround(m_viewportW)),
                               1, std::max(1, m_width - viewportX));
    int viewportH = std::clamp(static_cast<int>(std::lround(m_viewportH)),
                               1, std::max(1, m_height - viewportY));
    if (viewportW <= 1 || viewportH <= 1) {
        viewportX = 0;
        viewportY = 0;
        viewportW = std::max(1, m_width);
        viewportH = std::max(1, m_height);
    }

    if (showOverlay) {
        const SIZE cellSize = TextGridCellSize();
        const int cellWidth = std::max(1, static_cast<int>(cellSize.cx));
        const int cellHeight = std::max(1, static_cast<int>(cellSize.cy));
        const int cols =
            playback_overlay::overlayCellCountForPixels(std::max(1, m_width),
                                                        cellWidth);
        const int rows =
            playback_overlay::overlayCellCountForPixels(std::max(1, m_height),
                                                        cellHeight);
        if (buildWindowOverlayTextGrid(ui, cols, rows,
                                       m_windowOverlayTextGrid)) {
            const int textPxW =
                std::min(m_width, m_windowOverlayTextGrid.cols * cellWidth);
            const int textPxH =
                std::min(m_height, m_windowOverlayTextGrid.rows * cellHeight);
            overlayTextGridViewport = D3D11_VIEWPORT{
                0.0f, 0.0f, static_cast<float>(textPxW),
                static_cast<float>(textPxH), 0.0f, 1.0f};
            drawOverlayTextGrid = textPxW > 0 && textPxH > 0;
        }
    }

    if (showSubtitle) {
        const CaptionStyleProfile baseCaptionStyle = getWindowsCaptionStyleProfile();
        const int canvasW = std::max(1, viewportW);
        const int canvasH = std::max(1, viewportH);
        if (!m_subtitleTexture || m_subtitleWidth != canvasW ||
            m_subtitleHeight != canvasH) {
            m_subtitleWidth = canvasW;
            m_subtitleHeight = canvasH;
            m_subtitleTexture.Reset();
            m_subtitleSrv.Reset();

            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = static_cast<UINT>(canvasW);
            texDesc.Height = static_cast<UINT>(canvasH);
            texDesc.MipLevels = 1;
            texDesc.ArraySize = 1;
            texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_DEFAULT;
            texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            if (SUCCEEDED(device->CreateTexture2D(
                    &texDesc, NULL, &m_subtitleTexture))) {
                device->CreateShaderResourceView(
                    m_subtitleTexture.Get(), NULL, &m_subtitleSrv);
            }
        }

        if (m_subtitleTexture) {
            std::vector<uint8_t> canvas(
                static_cast<size_t>(canvasW) * static_cast<size_t>(canvasH) * 4u, 0u);
            std::vector<RECT> occupiedRects;
            std::string assRenderErrorText = ui.subtitleRenderError;
            const bool useAssScript =
                static_cast<bool>(ui.subtitleAssScript) && !ui.subtitleAssScript->empty();
            if (useAssScript) {
                const AssRenderResult assResult =
                    renderAssSubtitlesToCanvas(ui.subtitleAssScript,
                                               ui.subtitleAssFonts,
                                               ui.subtitleClockUs, canvasW, canvasH,
                                               &canvas);
                if (assResult.status == AssRenderStatus::error_init_or_parse) {
                    assRenderErrorText = assResult.errorMessage;
                    if (assRenderErrorText.empty()) {
                        assRenderErrorText =
                            "Unknown ASS renderer error while parsing or rendering.";
                    }
                    setSubtitleRenderError(assRenderErrorText);
                } else {
                    assRenderErrorText.clear();
                    setSubtitleRenderError({});
                }
            } else if (!assRenderErrorText.empty()) {
                assRenderErrorText.clear();
                setSubtitleRenderError({});
            }

            auto rectsOverlap = [](const RECT& a, const RECT& b) {
                return a.left < b.right && a.right > b.left && a.top < b.bottom &&
                       a.bottom > b.top;
            };

            auto blendBitmapIntoCanvas = [&](const std::vector<uint8_t>& srcBitmap,
                                             int srcW, int srcH, int dstX0,
                                             int dstY0) {
                if (srcBitmap.empty() || srcW <= 0 || srcH <= 0) return;
                for (int y = 0; y < srcH; ++y) {
                    const int dstY = dstY0 + y;
                    if (dstY < 0 || dstY >= canvasH) continue;
                    for (int x = 0; x < srcW; ++x) {
                        const int dstX = dstX0 + x;
                        if (dstX < 0 || dstX >= canvasW) continue;

                        const size_t srcIdx =
                            (static_cast<size_t>(y) * static_cast<size_t>(srcW) +
                             static_cast<size_t>(x)) *
                            4u;
                        const uint8_t srcA8 = srcBitmap[srcIdx + 3];
                        if (srcA8 == 0) continue;
                        const float srcA = static_cast<float>(srcA8) / 255.0f;

                        const size_t dstIdx =
                            (static_cast<size_t>(dstY) * static_cast<size_t>(canvasW) +
                             static_cast<size_t>(dstX)) *
                            4u;
                        const float dstA =
                            static_cast<float>(canvas[dstIdx + 3]) / 255.0f;
                        const float outA = srcA + dstA * (1.0f - srcA);
                        if (outA <= 0.0001f) continue;

                        for (int c = 0; c < 3; ++c) {
                            const float srcC =
                                static_cast<float>(srcBitmap[srcIdx + static_cast<size_t>(c)]) /
                                255.0f;
                            const float dstC =
                                static_cast<float>(canvas[dstIdx + static_cast<size_t>(c)]) /
                                255.0f;
                            const float outC =
                                (srcC * srcA + dstC * dstA * (1.0f - srcA)) / outA;
                            canvas[dstIdx + static_cast<size_t>(c)] = static_cast<uint8_t>(
                                std::lround(255.0f * std::clamp(outC, 0.0f, 1.0f)));
                        }
                        canvas[dstIdx + 3] = static_cast<uint8_t>(
                            std::lround(255.0f * std::clamp(outA, 0.0f, 1.0f)));
                    }
                }
            };

            for (const auto& cue : ui.subtitleCues) {
                if (cue.assStyled) continue;
                if (cue.text.empty()) continue;

                CaptionStyleProfile cueStyle = baseCaptionStyle;
                cueStyle.sizeScale = std::clamp(
                    baseCaptionStyle.sizeScale * std::max(0.40f, cue.sizeScale), 0.35f,
                    3.0f);
                const std::wstring cueTextWide = utf8ToWide(cue.text);
                if (cueTextWide.empty()) continue;
                const std::wstring cueFontNameWide = utf8ToWide(cue.fontName);

                SubtitleBitmapLayout layout{};
                if (!computeSubtitleLayout(
                        cueTextWide, canvasW, canvasH, cueStyle, cueFontNameWide,
                        std::clamp(cue.scaleX, 0.40f, 3.5f), cue.bold, cue.italic,
                        cue.underline, &layout)) {
                    continue;
                }

                int align = std::clamp(cue.alignment, 1, 9);
                const int alignCol = ((align - 1) % 3) + 1;  // 1:left, 2:center, 3:right
                const int alignRow = ((align - 1) / 3) + 1;  // 1:bottom, 2:middle, 3:top

                int marginL = std::max(
                    0, static_cast<int>(std::lround(cue.marginLNorm * canvasW)));
                int marginR = std::max(
                    0, static_cast<int>(std::lround(cue.marginRNorm * canvasW)));
                int marginV = std::max(
                    0, static_cast<int>(std::lround(cue.marginVNorm * canvasH)));
                if (!cue.hasPosition && marginV == 0) {
                    marginV = std::max(2, layout.fontPx / 6);
                }

                int anchorX = canvasW / 2;
                int anchorY = canvasH - marginV;
                if (cue.hasPosition) {
                    anchorX = static_cast<int>(std::lround(cue.posX * canvasW));
                    anchorY = static_cast<int>(std::lround(cue.posY * canvasH));
                } else {
                    if (alignCol == 1) {
                        anchorX = marginL;
                    } else if (alignCol == 2) {
                        anchorX = canvasW / 2;
                    } else {
                        anchorX = canvasW - marginR;
                    }

                    if (alignRow == 1) {
                        anchorY = canvasH - marginV;
                    } else if (alignRow == 2) {
                        anchorY = canvasH / 2;
                    } else {
                        anchorY = marginV;
                    }
                }

                int drawX = anchorX;
                if (alignCol == 2) drawX -= layout.width / 2;
                if (alignCol == 3) drawX -= layout.width;

                int drawY = anchorY;
                if (alignRow == 1) drawY -= layout.height;
                if (alignRow == 2) drawY -= layout.height / 2;

                drawX = std::clamp(drawX, 0, std::max(0, canvasW - layout.width));
                drawY = std::clamp(drawY, 0, std::max(0, canvasH - layout.height));

                RECT rect{drawX, drawY, drawX + layout.width, drawY + layout.height};
                if (!cue.hasPosition) {
                    const int moveStep = std::max(2, layout.fontPx / 3);
                    for (int tries = 0; tries < 64; ++tries) {
                        bool overlaps = false;
                        for (const RECT& occupied : occupiedRects) {
                            if (rectsOverlap(rect, occupied)) {
                                overlaps = true;
                                break;
                            }
                        }
                        if (!overlaps) break;

                        if (alignRow == 1) {
                            drawY -= moveStep;
                        } else if (alignRow == 3) {
                            drawY += moveStep;
                        } else {
                            drawY += moveStep;
                        }
                        drawY =
                            std::clamp(drawY, 0, std::max(0, canvasH - layout.height));
                        rect = RECT{drawX, drawY, drawX + layout.width,
                                    drawY + layout.height};
                    }
                }

                std::vector<uint8_t> cueBitmap;
                if (!renderSubtitleTextToBitmap(
                        cue.text, layout, cueStyle, cueFontNameWide,
                        std::clamp(cue.scaleX, 0.40f, 3.5f), cue.bold, cue.italic,
                        cue.underline, cueBitmap) ||
                    cueBitmap.empty()) {
                    continue;
                }

                blendBitmapIntoCanvas(cueBitmap, layout.width, layout.height, drawX,
                                      drawY);

                occupiedRects.push_back(rect);
            }

            if (useAssScript && !assRenderErrorText.empty()) {
                CaptionStyleProfile errorStyle = baseCaptionStyle;
                errorStyle.sizeScale =
                    std::clamp(baseCaptionStyle.sizeScale * 0.82f, 0.45f, 2.4f);
                errorStyle.textR = 255;
                errorStyle.textG = 224;
                errorStyle.textB = 224;
                errorStyle.textAlpha = 1.0f;
                errorStyle.backgroundR = 16;
                errorStyle.backgroundG = 16;
                errorStyle.backgroundB = 16;
                errorStyle.backgroundAlpha = 0.84f;
                errorStyle.fontEffect = 4;  // outline

                const std::string errorText =
                    "ASS subtitle render error: " + assRenderErrorText;
                const std::wstring errorWide = utf8ToWide(errorText);
                SubtitleBitmapLayout errorLayout{};
                if (!errorWide.empty() &&
                    computeSubtitleLayout(errorWide, canvasW, canvasH, errorStyle,
                                          std::wstring(), 1.0f, false, false,
                                          false, &errorLayout)) {
                    std::vector<uint8_t> errorBitmap;
                    if (renderSubtitleTextToBitmap(errorText, errorLayout, errorStyle,
                                                   std::wstring(), 1.0f, false,
                                                   false, false, errorBitmap)) {
                        const int errorX = std::clamp(
                            (canvasW - errorLayout.width) / 2, 0,
                            std::max(0, canvasW - errorLayout.width));
                        const int errorY = std::clamp(
                            std::max(4, canvasH / 24), 0,
                            std::max(0, canvasH - errorLayout.height));
                        blendBitmapIntoCanvas(errorBitmap, errorLayout.width,
                                              errorLayout.height, errorX, errorY);
                    }
                }
            }

            D3D11_BOX box{0, 0, 0, static_cast<UINT>(canvasW),
                          static_cast<UINT>(canvasH), 1};
            context->UpdateSubresource(m_subtitleTexture.Get(), 0, &box,
                                       canvas.data(), canvasW * 4, 0);

            subtitleHeightNorm =
                static_cast<float>(canvasH) / std::max(1, m_height);
            subtitleWidthNorm =
                static_cast<float>(canvasW) / std::max(1, m_width);
            subtitleLeftNorm =
                static_cast<float>(viewportX) / std::max(1, m_width);
            subtitleTopNorm =
                static_cast<float>(viewportY) / std::max(1, m_height);
        }
    }

    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            ShaderConstants sc{};
            sc.progress = ui.progress;
            sc.overlayAlpha = 0.0f;
            sc.isPaused = ui.isPaused ? 1 : 0;
            sc.volPct = (uint32_t)std::clamp(ui.volPct, 0, 100);
            sc.textTop = 0.0f;
            sc.textHeight = 0.0f;
            sc.textLeft = 0.0f;
            sc.textWidth = 0.0f;
            sc.subtitleTop = subtitleTopNorm;
            sc.subtitleHeight = subtitleHeightNorm;
            sc.subtitleLeft = subtitleLeftNorm;
            sc.subtitleWidth = subtitleWidthNorm;
            sc.subtitleAlpha =
                showSubtitle ? std::clamp(ui.subtitleAlpha, 0.0f, 1.0f) : 0.0f;
            FillOutputColorConstants(sc);
            std::memcpy(mapped.pData, &sc, sizeof(ShaderConstants));
            context->Unmap(m_constantBuffer.Get(), 0);
        }
    }

    if (showSubtitle) {
        ID3D11ShaderResourceView* srvs[5] = {
            nullptr, nullptr, nullptr, nullptr, m_subtitleSrv.Get()};
        context->PSSetShaderResources(0, 5, srvs);
        context->Draw(4, 0);
    }
    if (drawOverlayTextGrid) {
        DrawGpuTextGridFrame(device, context.Get(), m_windowOverlayTextGrid,
                             overlayTextGridViewport);
    }
    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);

    if (showSubtitle) {
        ID3D11ShaderResourceView* nullSRVs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
        context->PSSetShaderResources(0, 5, nullSRVs);
    }
}

void VideoWindow::PresentOverlay(GpuVideoFrameCache& frameCache, const WindowUiState& ui,
                                 bool nonBlocking) {
    std::unique_lock<std::recursive_mutex> lock(getSharedGpuMutex());
#if RADIOIFY_ENABLE_TIMING_LOG
    fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay enter (wnd=%p swap=%p visible=%d)\n", now_ms().c_str(), thread_id_str().c_str(), (void*)m_hWnd, (void*)m_swapChain.Get(), m_hWnd ? IsWindowVisible(m_hWnd) : 0);
#endif
    if (!m_hWnd || !m_swapChain || !IsWindowVisible(m_hWnd)) {
#if RADIOIFY_ENABLE_TIMING_LOG
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay early exit: window/swap not ready\n", now_ms().c_str(), thread_id_str().c_str());
#endif
        return;
    }
    if (!frameCache.HasFrame()) {
#if RADIOIFY_ENABLE_TIMING_LOG
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay early exit: no frame in cache\n", now_ms().c_str(), thread_id_str().c_str());
#endif
        return;
    }

    // Refresh window dimensions to avoid stale size during fullscreen transitions
    RECT rect;
    if (GetClientRect(m_hWnd, &rect)) {
        m_width = rect.right - rect.left;
        m_height = rect.bottom - rect.top;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain = m_swapChain;
    UINT presentInterval = m_presentInterval.load(std::memory_order_relaxed);

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context || !m_renderTargetView || !m_constantBuffer) return;

    m_videoWidth = frameCache.GetDisplayWidth();
    m_videoHeight = frameCache.GetDisplayHeight();

    float clearColor[4] = { 0, 0, 0, 1 };
    context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
    context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), NULL);

    UpdateViewport(m_width, m_height);
    D3D11_VIEWPORT viewport = { m_viewportX, m_viewportY, m_viewportW, m_viewportH, 0.0f, 1.0f };
    context->RSSetViewports(1, &viewport);

    context->IASetInputLayout(NULL);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(m_vertexShader.Get(), NULL, 0);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    // Video path
    context->PSSetShader(m_pixelShader.Get(), NULL, 0);
    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            ShaderConstants sc{};
            sc.isFullRange = frameCache.IsFullRange() ? 1 : 0;
            sc.yuvMatrix = (uint32_t)frameCache.GetMatrix();
            sc.yuvTransfer = (uint32_t)frameCache.GetTransfer();
            sc.bitDepth = (uint32_t)frameCache.GetBitDepth();
            sc.hasRGBA = frameCache.IsRgba() ? 1u : 0u;
            sc.rotationQuarterTurns =
                (uint32_t)(frameCache.GetRotationQuarterTurns() & 3);
            FillOutputColorConstants(sc);
            std::memcpy(mapped.pData, &sc, sizeof(ShaderConstants));
            context->Unmap(m_constantBuffer.Get(), 0);
        }
    }

    if (frameCache.IsRgba()) {
        ID3D11ShaderResourceView* srvs[3] = { nullptr, nullptr, frameCache.GetSrvRGBA() };
        context->PSSetShaderResources(0, 3, srvs);
    } else {
        ID3D11ShaderResourceView* srvs[2] = { frameCache.GetSrvY(), frameCache.GetSrvUV() };
        context->PSSetShaderResources(0, 2, srvs);
    }

    context->Draw(4, 0);
    DrawOverlay(ui);
    DrawPictureInPictureBorder(context.Get());
    frameCache.MarkFrameInFlight(context.Get());

#if RADIOIFY_ENABLE_TIMING_LOG
    fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay about to Present()\n", now_ms().c_str(), thread_id_str().c_str());
#endif
    lock.unlock();
    if (!swapChain) return;
    const VideoWindowPresentArgs presentArgs =
        liveVideoWindowPresentArgs(presentInterval, nonBlocking);
    HRESULT presHr =
        swapChain->Present(presentArgs.syncInterval, presentArgs.flags);
    frameCache.SignalFrameLatencyFence(context.Get());
    if (videoWindowPresentSkipped(presHr)) {
#if RADIOIFY_ENABLE_TIMING_LOG
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay skipped (0x%08X)\n", now_ms().c_str(), thread_id_str().c_str(), static_cast<unsigned int>(presHr));
#endif
        return;
    }
#if RADIOIFY_ENABLE_TIMING_LOG
    if (FAILED(presHr)) {
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay Present() FAILED 0x%08X\n", now_ms().c_str(), thread_id_str().c_str(), static_cast<unsigned int>(presHr));
    } else {
        fprintf(stderr, "[%s] [tid=%s] VideoWindow::PresentOverlay Present() OK\n", now_ms().c_str(), thread_id_str().c_str());
    }
#endif
}

void VideoWindow::PresentBackbuffer() {
    std::unique_lock<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd || !m_swapChain || !IsWindowVisible(m_hWnd)) return;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain = m_swapChain;
    UINT presentInterval = m_presentInterval.load(std::memory_order_relaxed);
    lock.unlock();
    if (!swapChain) return;
    const VideoWindowPresentArgs presentArgs =
        liveVideoWindowPresentArgs(presentInterval, false);
    swapChain->Present(presentArgs.syncInterval, presentArgs.flags);
}
