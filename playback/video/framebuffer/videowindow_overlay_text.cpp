#include "videowindow_overlay_text.h"

#include "terminal_font.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
    std::wstring overlayUtf8ToWideLine(const std::string& text) {
        std::string clean;
        clean.reserve(text.size());
        for (char ch : text) {
            if (ch != '\r' && ch != '\n') clean.push_back(ch);
        }
        if (clean.empty()) return {};

        int needed =
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, clean.data(),
                                static_cast<int>(clean.size()), nullptr, 0);
        if (needed <= 0) {
            needed = MultiByteToWideChar(CP_UTF8, 0, clean.data(),
                                         static_cast<int>(clean.size()),
                                         nullptr, 0);
        }
        if (needed <= 0) {
            std::wstring fallback;
            fallback.reserve(clean.size());
            for (unsigned char ch : clean) {
                fallback.push_back(static_cast<wchar_t>(ch));
            }
            return fallback;
        }

        std::wstring out(static_cast<size_t>(needed), L'\0');
        int written = MultiByteToWideChar(CP_UTF8, 0, clean.data(),
                                          static_cast<int>(clean.size()),
                                          out.data(), needed);
        if (written <= 0) return {};
        return out;
    }

    void composePixel(std::vector<uint8_t>& pixels, int width, int height,
                      int x, int y, uint8_t r, uint8_t g, uint8_t b,
                      uint8_t a) {
        if (x < 0 || x >= width || y < 0 || y >= height || a == 0) return;
        const size_t idx =
            (static_cast<size_t>(y) * static_cast<size_t>(width) +
             static_cast<size_t>(x)) *
            4u;
        const float srcA = static_cast<float>(a) / 255.0f;
        const float dstA = static_cast<float>(pixels[idx + 3]) / 255.0f;
        const float outA = srcA + dstA * (1.0f - srcA);
        if (outA <= 0.0f) return;
        auto blend = [&](uint8_t src, uint8_t dst) -> uint8_t {
            const float v =
                (static_cast<float>(src) * srcA +
                 static_cast<float>(dst) * dstA * (1.0f - srcA)) /
                outA;
            return static_cast<uint8_t>(std::clamp(std::lround(v), 0l, 255l));
        };
        pixels[idx + 0] = blend(b, pixels[idx + 0]);
        pixels[idx + 1] = blend(g, pixels[idx + 1]);
        pixels[idx + 2] = blend(r, pixels[idx + 2]);
        pixels[idx + 3] =
            static_cast<uint8_t>(std::clamp(std::lround(outA * 255.0f), 0l, 255l));
    }
}

bool renderWindowOverlayTextToBitmap(
    const std::string& topLine,
    const std::vector<WindowUiState::ControlButton>& controlButtons,
    const std::string& fallbackControlsLine,
    const std::string& progressSuffix,
    int width,
    int height,
    std::vector<uint8_t>& outPixels,
    bool centerAlign,
    int padX,
    int padY) {
    outPixels.clear();
    if (width <= 0 || height <= 0) return false;

    struct ControlSegment {
        int charStart = 0;
        int width = 0;
        bool active = false;
        bool hovered = false;
    };

    const std::wstring line0 = overlayUtf8ToWideLine(topLine);
    std::wstring line1;
    const std::wstring line2 = overlayUtf8ToWideLine(progressSuffix);
    std::vector<ControlSegment> segments;
    if (!controlButtons.empty()) {
        int cursor = 0;
        for (size_t i = 0; i < controlButtons.size(); ++i) {
            if (i > 0) {
                line1 += L"  ";
                cursor += 2;
            }
            std::wstring text = overlayUtf8ToWideLine(controlButtons[i].text);
            const int widthChars = static_cast<int>(text.size());
            segments.push_back(
                ControlSegment{cursor, widthChars, controlButtons[i].active,
                               controlButtons[i].hovered});
            line1 += text;
            cursor += widthChars;
        }
    } else {
        line1 = overlayUtf8ToWideLine(fallbackControlsLine);
    }

    int lineCount = 1;
    if (!line1.empty()) ++lineCount;
    if (!line2.empty()) ++lineCount;

    int maxChars = static_cast<int>(line0.size());
    if (!line1.empty())
        maxChars = std::max(maxChars, static_cast<int>(line1.size()));
    if (maxChars <= 0) {
        outPixels.assign(static_cast<size_t>(width) *
                             static_cast<size_t>(height) * 4u,
                         static_cast<uint8_t>(0));
        return true;
    }

    const int glyphW = 5;
    const int glyphH = 7;
    const int spacing = 1;
    const int lineSpacing = 1;
    const int totalGlyphH = lineCount * glyphH + (lineCount - 1) * lineSpacing;
    const int maxScaleVert = std::max(1, height / std::max(1, totalGlyphH));
    const int maxScaleHoriz =
        std::max(1, width / std::max(1, maxChars * (glyphW + spacing)));
    const int maxCap = std::max(3, height / 80);
    int scale = std::min({maxScaleVert, maxScaleHoriz, maxCap});
    if (scale < 1) scale = 1;

    const int charAdvance = (glyphW + spacing) * scale;
    const int totalTextWidth = maxChars * charAdvance;
    const int totalTextHeight = totalGlyphH * scale;
    const int startX =
        centerAlign ? std::max(0, (width - totalTextWidth) / 2)
                    : std::max(0, padX);
    const int startY =
        centerAlign ? std::max(0, (height - totalTextHeight) / 2)
                    : std::max(0, padY);

    outPixels.assign(static_cast<size_t>(width) *
                         static_cast<size_t>(height) * 4u,
                     static_cast<uint8_t>(0));

    HDC maskDC = CreateCompatibleDC(nullptr);
    if (!maskDC) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* maskBits = nullptr;
    HBITMAP maskBmp =
        CreateDIBSection(maskDC, &bmi, DIB_RGB_COLORS, &maskBits, nullptr, 0);
    if (!maskBmp || !maskBits) {
        if (maskBmp) DeleteObject(maskBmp);
        DeleteDC(maskDC);
        return false;
    }

    HGDIOBJ oldMaskBmp = SelectObject(maskDC, maskBmp);
    std::memset(maskBits, 0,
                static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    SetBkMode(maskDC, TRANSPARENT);
    SetTextColor(maskDC, RGB(255, 255, 255));

    const int fontH = fitRadioifyTerminalFontHeightToCell(
        maskDC, std::max(1, glyphH * scale), charAdvance,
        std::max(1, glyphH * scale));
    HFONT font = createRadioifyTerminalFont(fontH);
    const bool ownsFont = font != nullptr;
    if (!font) {
        font = static_cast<HFONT>(GetStockObject(SYSTEM_FIXED_FONT));
    }
    HGDIOBJ oldFont = SelectObject(maskDC, font);

    auto fillRect = [&](int x0, int y0, int x1, int y1, uint8_t r, uint8_t g,
                        uint8_t b, uint8_t a) {
        const int rx0 = std::max(0, x0);
        const int ry0 = std::max(0, y0);
        const int rx1 = std::min(width, x1);
        const int ry1 = std::min(height, y1);
        for (int y = ry0; y < ry1; ++y) {
            for (int x = rx0; x < rx1; ++x) {
                const size_t idx =
                    (static_cast<size_t>(y) * static_cast<size_t>(width) +
                     static_cast<size_t>(x)) *
                    4u;
                outPixels[idx + 0] = b;
                outPixels[idx + 1] = g;
                outPixels[idx + 2] = r;
                outPixels[idx + 3] = a;
            }
        }
    };

    auto clearMaskRect = [&](const RECT& rc) {
        uint8_t* bits = static_cast<uint8_t*>(maskBits);
        const int x0 = std::max(0, static_cast<int>(rc.left));
        const int y0 = std::max(0, static_cast<int>(rc.top));
        const int x1 = std::min(width, static_cast<int>(rc.right));
        const int y1 = std::min(height, static_cast<int>(rc.bottom));
        for (int y = y0; y < y1; ++y) {
            std::memset(bits + (static_cast<size_t>(y) *
                                    static_cast<size_t>(width) +
                                static_cast<size_t>(x0)) *
                                   4u,
                        0, static_cast<size_t>(x1 - x0) * 4u);
        }
    };

    auto drawGlyph = [&](wchar_t ch, const RECT& rc, uint8_t r, uint8_t g,
                         uint8_t b, uint8_t a) {
        if (ch == L' ') return;
        clearMaskRect(rc);
        RECT drawRc = rc;
        DrawTextW(maskDC, &ch, 1, &drawRc,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        const uint8_t* bits = static_cast<const uint8_t*>(maskBits);
        const int x0 = std::max(0, static_cast<int>(rc.left));
        const int y0 = std::max(0, static_cast<int>(rc.top));
        const int x1 = std::min(width, static_cast<int>(rc.right));
        const int y1 = std::min(height, static_cast<int>(rc.bottom));
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const size_t idx =
                    (static_cast<size_t>(y) * static_cast<size_t>(width) +
                     static_cast<size_t>(x)) *
                    4u;
                const uint8_t coverage =
                    std::max({bits[idx + 0], bits[idx + 1], bits[idx + 2]});
                if (coverage == 0) continue;
                composePixel(outPixels, width, height, x, y, r, g, b,
                             static_cast<uint8_t>(
                                 (static_cast<unsigned int>(coverage) * a) /
                                 255u));
            }
        }
    };

    auto drawLine = [&](const std::wstring& lineText, int lineIndex,
                        bool withStyledButtons, bool rightAlign) {
        if (lineText.empty()) return;
        const int lineWidth = static_cast<int>(lineText.size()) * charAdvance;
        int lineX = startX;
        if (centerAlign) {
            lineX = startX + std::max(0, (totalTextWidth - lineWidth) / 2);
        } else if (rightAlign) {
            lineX = std::max(padX, width - padX - lineWidth);
        }
        const int lineY = startY + lineIndex * (glyphH + lineSpacing) * scale;
        const int lineH = glyphH * scale;

        if (withStyledButtons && !segments.empty()) {
            for (const auto& seg : segments) {
                uint8_t bgR = 36, bgG = 36, bgB = 36, bgA = 190;
                if (seg.active) {
                    bgR = 64;
                    bgG = 48;
                    bgB = 24;
                    bgA = 215;
                }
                if (seg.hovered) {
                    bgR = 226;
                    bgG = 226;
                    bgB = 226;
                    bgA = 235;
                }
                fillRect(lineX + seg.charStart * charAdvance, lineY,
                         lineX + (seg.charStart + seg.width) * charAdvance,
                         lineY + lineH, bgR, bgG, bgB, bgA);
            }
        }

        for (int i = 0; i < static_cast<int>(lineText.size()); ++i) {
            uint8_t fgR = 235, fgG = 235, fgB = 235, fgA = 255;
            if (withStyledButtons && !segments.empty()) {
                for (const auto& seg : segments) {
                    if (i >= seg.charStart && i < seg.charStart + seg.width) {
                        if (seg.hovered) {
                            fgR = 18;
                            fgG = 18;
                            fgB = 18;
                        } else if (seg.active) {
                            fgR = 255;
                            fgG = 220;
                            fgB = 135;
                        } else {
                            fgR = 222;
                            fgG = 222;
                            fgB = 222;
                        }
                        break;
                    }
                }
            }
            RECT rc{lineX + i * charAdvance, lineY,
                    lineX + (i + 1) * charAdvance, lineY + lineH};
            drawGlyph(lineText[static_cast<size_t>(i)], rc, fgR, fgG, fgB,
                      fgA);
        }
    };

    int lineIndex = 0;
    drawLine(line0, lineIndex++, false, false);
    if (!line1.empty()) {
        drawLine(line1, lineIndex++, true, false);
    }
    if (!line2.empty()) {
        drawLine(line2, lineIndex, false, true);
    }

    if (oldFont) SelectObject(maskDC, oldFont);
    if (ownsFont && font) DeleteObject(font);
    if (oldMaskBmp) SelectObject(maskDC, oldMaskBmp);
    DeleteObject(maskBmp);
    DeleteDC(maskDC);
    return true;
}

