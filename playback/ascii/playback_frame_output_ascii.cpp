#include "playback_frame_output.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "subtitle_caption_style.h"
#include "unicode_display_width.h"

namespace {

void emitWarning(
    const playback_frame_output::LogLineWriter& sink,
    const std::string& message) {
  if (sink) {
    sink(message);
  }
}

void setFailure(playback_frame_output::AsciiModePrepareInput& input,
                const std::string& message,
                const std::string& detail) {
  if (input.renderFailed) {
    *input.renderFailed = true;
  }
  if (input.renderFailMessage) {
    *input.renderFailMessage = message;
  }
  if (input.renderFailDetail) {
    *input.renderFailDetail = detail;
  }
  if (input.haveFrame) {
    *input.haveFrame = false;
  }
}

bool logAsciiRendererStartup(const VideoFrame& frame,
                            const AsciiArt& art,
                            const std::function<void(const std::string&)>&
                                timingSink) {
  static bool logged = false;
  if (logged) {
    return false;
  }
  logged = true;
  if (!timingSink) return false;

  const char* formatName = "";
  switch (frame.format) {
    case VideoPixelFormat::HWTexture:
      formatName = "hwtexture";
      break;
    case VideoPixelFormat::NV12:
      formatName = "nv12";
      break;
    case VideoPixelFormat::P010:
      formatName = "p010";
      break;
    case VideoPixelFormat::RGB32:
      formatName = "rgba";
      break;
    case VideoPixelFormat::ARGB32:
      formatName = "rgba";
      break;
    default:
      formatName = "unknown";
      break;
  }
  char inputBuf[256];
  std::snprintf(inputBuf, sizeof(inputBuf),
                "video_renderer_input format=%s in=%dx%d out=%dx%d",
                formatName, frame.width, frame.height, art.width, art.height);
  timingSink(std::string(inputBuf));
  return true;
}

bool updateFrameCache(GpuVideoFrameCache& cache, ID3D11Device* device,
                     ID3D11DeviceContext* context, const VideoFrame& frame) {
  if (frame.format == VideoPixelFormat::HWTexture) {
    if (!frame.hwTexture) {
      return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    frame.hwTexture->GetDesc(&desc);
    const bool is10Bit = (desc.Format == DXGI_FORMAT_P010);
    return cache.Update(device, context, frame.hwTexture.Get(),
                        frame.hwTextureArrayIndex, frame.width, frame.height,
                        frame.fullRange, frame.yuvMatrix, frame.yuvTransfer,
                        is10Bit ? 10 : 8, frame.rotationQuarterTurns);
  }

  if (frame.format == VideoPixelFormat::NV12 ||
      frame.format == VideoPixelFormat::P010) {
    if (frame.stride <= 0 || frame.planeHeight <= 0 || frame.yuv.empty()) {
      return false;
    }
    const size_t strideBytes = static_cast<size_t>(frame.stride);
    const size_t planeHeight = static_cast<size_t>(frame.planeHeight);
    const size_t yBytes = strideBytes * planeHeight;
    if (planeHeight == 0 || strideBytes == 0 ||
        yBytes / strideBytes != planeHeight) {
      return false;
    }
    return cache.UpdateNV12(
        device, context, frame.yuv.data(), frame.stride, frame.planeHeight,
        frame.width, frame.height, frame.fullRange, frame.yuvMatrix,
        frame.yuvTransfer, frame.format == VideoPixelFormat::P010 ? 10 : 8,
        frame.rotationQuarterTurns);
  }

  if (frame.format == VideoPixelFormat::RGB32 ||
      frame.format == VideoPixelFormat::ARGB32) {
    if (frame.rgba.empty()) {
      return false;
    }
    const int stride = frame.stride > 0 ? frame.stride : frame.width * 4;
    return cache.Update(device, context, frame.rgba.data(), stride, frame.width,
                        frame.height, frame.rotationQuarterTurns);
  }

  return false;
}

bool cpuRenderFallback(const playback_frame_output::AsciiModePrepareInput& input) {
  if (input.frame->format == VideoPixelFormat::NV12 ||
      input.frame->format == VideoPixelFormat::P010) {
    bool is10Bit = input.frame->format == VideoPixelFormat::P010;
    return renderAsciiArtFromYuv(
        input.frame->yuv.data(), input.frame->width, input.frame->height,
        input.frame->stride, input.frame->planeHeight,
        is10Bit ? YuvFormat::P010 : YuvFormat::NV12, input.frame->fullRange,
        input.frame->yuvMatrix, input.frame->yuvTransfer, input.art->width,
        input.art->height, *input.art);
  }

  if (input.frame->format == VideoPixelFormat::RGB32 ||
      input.frame->format == VideoPixelFormat::ARGB32) {
    return renderAsciiArtFromRgba(input.frame->rgba.data(), input.frame->width,
                                  input.frame->height, input.art->width,
                                  input.art->height, *input.art,
                                  input.frame->format == VideoPixelFormat::ARGB32);
  }

  return false;
}

std::pair<int, int> computeAsciiOutputSizeWithInset(int maxWidth,
                                                    int maxHeight,
                                                    int srcW,
                                                    int srcH,
                                                    int horizontalInset) {
  int maxOutW = std::max(1, maxWidth - std::max(0, horizontalInset));
  AsciiArtLayout fitted =
      fitAsciiArtLayout(srcW, srcH, maxOutW, std::max(1, maxHeight));
  return std::pair<int, int>(fitted.width, fitted.height);
}

}  // namespace

namespace playback_frame_output {

std::pair<int, int> computeAsciiPlaybackTargetSize(int width, int height,
                                                  int srcW, int srcH,
                                                  bool showStatusLine) {
  int headerLines = showStatusLine ? 1 : 0;
  const int footerLines = 0;
  int maxHeight = std::max(1, height - headerLines - footerLines);
  int maxOutW = std::max(1, width - 8);
  AsciiArtLayout fitted =
      fitAsciiArtLayout(srcW, srcH, maxOutW, std::max(1, maxHeight));
  int outW = fitted.width;
  int outH = fitted.height;
  int targetW = std::max(2, outW * 2);
  int targetH = std::max(4, outH * 4);
  if (targetW & 1) ++targetW;
  if (targetH & 1) ++targetH;
  if (srcW > 0) targetW = std::min(targetW, srcW & ~1);
  if (srcH > 0) targetH = std::min(targetH, srcH & ~1);
  targetW = std::max(2, targetW);
  targetH = std::max(4, targetH);

  const int kMaxDecodeWidth = 1024;
  const int kMaxDecodeHeight = 768;
  if (targetW > kMaxDecodeWidth || targetH > kMaxDecodeHeight) {
    double scaleW = static_cast<double>(kMaxDecodeWidth) / targetW;
    double scaleH = static_cast<double>(kMaxDecodeHeight) / targetH;
    double scale = std::min(scaleW, scaleH);
    targetW = static_cast<int>(std::lround(targetW * scale));
    targetH = static_cast<int>(std::lround(targetH * scale));
    targetW = std::min(targetW, kMaxDecodeWidth);
    targetH = std::min(targetH, kMaxDecodeHeight);
    targetW &= ~1;
    targetH &= ~1;
    targetW = std::max(2, targetW);
    targetH = std::max(4, targetH);
  }
  return std::pair<int, int>(targetW, targetH);
}

std::pair<int, int> computeAsciiOutputSize(int maxWidth, int maxHeight,
                                          int srcW, int srcH) {
  return computeAsciiOutputSizeWithInset(maxWidth, maxHeight, srcW, srcH, 8);
}

std::pair<int, int> computeTightAsciiOutputSize(int maxWidth, int maxHeight,
                                                int srcW, int srcH) {
  return computeAsciiOutputSizeWithInset(maxWidth, maxHeight, srcW, srcH, 0);
}

bool prepareAsciiModeFrame(AsciiModePrepareInput& input) {
  if (!input.allowFrame) {
    if (input.cachedWidth) *input.cachedWidth = -1;
    if (input.cachedMaxHeight) *input.cachedMaxHeight = -1;
    if (input.cachedFrameWidth) *input.cachedFrameWidth = -1;
    if (input.cachedFrameHeight) *input.cachedFrameHeight = -1;
    return false;
  }
  if (!(input.frameChanged || input.clearHistory || input.sizeChanged)) {
    return false;
  }
  if (!input.frame || !input.art || !input.gpuRenderer || !input.frameCache) {
    setFailure(input, "Invalid frame rendering context.",
               "Missing frame, art, renderer, or frame cache.");
    return false;
  }
  if (input.frame->width <= 0 || input.frame->height <= 0) {
    emitWarning(input.warningSink, "Skipping video frame with invalid dimensions.");
    if (input.haveFrame) {
      *input.haveFrame = false;
    }
    return false;
  }

  auto [outW, outH] = input.computeAsciiOutputSize(
      input.width, input.maxHeight,
      ((input.frame->rotationQuarterTurns & 1) != 0) ? input.frame->height
                                                    : input.frame->width,
      ((input.frame->rotationQuarterTurns & 1) != 0) ? input.frame->width
                                                    : input.frame->height);
  const int prevArtWidth = input.art->width;
  const int prevArtHeight = input.art->height;
  input.art->width = outW;
  input.art->height = outH;

  bool cacheUpdated = false;
  bool renderFromCache = false;
  bool keepPreviousFrame = false;
  bool hadCachedFrame = false;
  std::string gpuErr;

  try {
    ID3D11Device* device = getSharedGpuDevice();
    if (!device) {
      setFailure(input, "GPU device unavailable.",
                 "Shared GPU device was not initialized.");
      return false;
    }
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) {
      setFailure(input, "GPU context unavailable.",
                 "Failed to acquire D3D11 immediate context.");
      return false;
    }

    const auto start = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
      hadCachedFrame = input.frameCache->HasFrame();
      if (input.clearHistory) {
        input.gpuRenderer->ClearHistory();
      }
      cacheUpdated = updateFrameCache(*input.frameCache, device, context.Get(),
                                     *input.frame);
      if (cacheUpdated) {
        renderFromCache = input.gpuRenderer->RenderFromCache(*input.frameCache,
                                                            *input.art, &gpuErr);
        if (!renderFromCache && hadCachedFrame) {
          keepPreviousFrame = true;
        }
      } else if (hadCachedFrame) {
        keepPreviousFrame = true;
      }
    }
    const auto end = std::chrono::steady_clock::now();

    const auto durMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           end - start)
                           .count();
    if (durMs > 50 && input.timingSink) {
      char buf[256];
      std::snprintf(
          buf, sizeof(buf), "video_render_slow pts_us=%lld dur_ms=%lld",
          static_cast<long long>(input.frame->timestamp100ns / 10),
          static_cast<long long>(durMs));
      input.timingSink(std::string(buf));
    }

    bool asciiOk = false;
    if (renderFromCache) {
      asciiOk = true;
      if (input.timingSink && logAsciiRendererStartup(*input.frame, *input.art,
                                                     input.timingSink)) {
        char buf[256];
        std::string details = input.gpuRenderer->lastNv12TextureDetail();
        if (details.empty()) {
          details = std::string("path=") + input.gpuRenderer->lastNv12TexturePath();
        }
        const char* fmt = "unknown";
        switch (input.frame->format) {
          case VideoPixelFormat::HWTexture:
            fmt = "hwtexture";
            break;
          case VideoPixelFormat::NV12:
            fmt = "nv12";
            break;
          case VideoPixelFormat::P010:
            fmt = "p010";
            break;
          case VideoPixelFormat::RGB32:
          case VideoPixelFormat::ARGB32:
            fmt = "rgba";
            break;
          default:
            fmt = "unsupported";
            break;
        }
        std::snprintf(buf, sizeof(buf),
                      "video_renderer gpu_active=1 format=%s %s", fmt,
                      details.c_str());
        input.timingSink(std::string(buf));
      }
    } else if (keepPreviousFrame) {
      input.art->width = prevArtWidth;
      input.art->height = prevArtHeight;
      asciiOk = true;
    } else if (input.allowAsciiCpuFallback ||
               input.frame->format == VideoPixelFormat::NV12 ||
               input.frame->format == VideoPixelFormat::P010 ||
               input.frame->format == VideoPixelFormat::RGB32 ||
               input.frame->format == VideoPixelFormat::ARGB32) {
      if (!cacheUpdated) {
        emitWarning(input.warningSink,
                    "GPU cache update failed; falling back to CPU ASCII path.");
      } else if (!gpuErr.empty()) {
        emitWarning(
            input.warningSink,
            std::string("GPU ASCII render failed; falling back to CPU path: ") +
                gpuErr);
      } else {
        emitWarning(input.warningSink,
                    "GPU ASCII render failed; falling back to CPU path.");
      }
      asciiOk = cpuRenderFallback(input);
    } else {
      emitWarning(input.warningSink,
                  gpuErr.empty() ? "GPU renderer temporarily unavailable."
                                 : gpuErr);
      if (input.haveFrame) {
        *input.haveFrame = false;
      }
      return false;
    }

    if (!asciiOk) {
      setFailure(input, "Failed to render video frame.", "");
      return false;
    }
  } catch (...) {
    setFailure(input, "Failed to render video frame.", "");
    return false;
  }

  if (input.cachedWidth) *input.cachedWidth = input.width;
  if (input.cachedMaxHeight) *input.cachedMaxHeight = input.maxHeight;
  if (input.cachedFrameWidth) *input.cachedFrameWidth = input.frame->width;
  if (input.cachedFrameHeight) *input.cachedFrameHeight = input.frame->height;
  return true;
}

void renderAsciiModeContent(ConsoleScreen& screen, const AsciiArt& art, int width,
                           int height, int maxHeight, int artTop,
                           const std::string& waitingLabel, bool allowFrame,
                           const Style& baseStyle, bool overlayVisible,
                           int overlayReservedLines,
                           const std::string& subtitleText,
                           const Style& accentStyle, const Style& dimStyle) {
  const int artWidth = std::min(art.width, width);
  const int artHeight = std::min(art.height, maxHeight);
  const int availableHeight = height - artTop;
  const int visibleArtHeight = std::min(artHeight, availableHeight);
  const int artX = std::max(0, (width - artWidth) / 2);
  const size_t expectedCellCount = static_cast<size_t>(std::max(0, art.width)) *
                                   static_cast<size_t>(std::max(0, art.height));
  const bool canDrawArt =
      allowFrame && artWidth > 0 && artHeight > 0 &&
      art.cells.size() >= expectedCellCount;

  if (canDrawArt && visibleArtHeight > 0) {
    for (int y = 0; y < visibleArtHeight; ++y) {
      for (int x = 0; x < artWidth; ++x) {
        const auto& cell = art.cells[static_cast<size_t>(y * art.width + x)];
        Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
        screen.writeChar(artX + x, artTop + y, cell.ch, cellStyle);
      }
    }
  } else if (!allowFrame) {
    screen.writeText(0, artTop, fitLine(waitingLabel, width), dimStyle);
  }

  if (subtitleText.empty() || width <= 2 || height <= artTop + 1) {
    return;
  }

  const CaptionStyleProfile captionStyle = getWindowsCaptionStyleProfile();
  auto blendColor = [](const Color& src, const Color& dst, float alpha) -> Color {
    const float a = std::clamp(alpha, 0.0f, 1.0f);
    Color out;
    out.r = static_cast<uint8_t>(
        std::lround(static_cast<float>(src.r) * a +
                    static_cast<float>(dst.r) * (1.0f - a)));
    out.g = static_cast<uint8_t>(
        std::lround(static_cast<float>(src.g) * a +
                    static_cast<float>(dst.g) * (1.0f - a)));
    out.b = static_cast<uint8_t>(
        std::lround(static_cast<float>(src.b) * a +
                    static_cast<float>(dst.b) * (1.0f - a)));
    return out;
  };

  const Color targetText{
      captionStyle.textR, captionStyle.textG, captionStyle.textB};
  const Color targetBg{
      captionStyle.backgroundR, captionStyle.backgroundG, captionStyle.backgroundB};
  const Color blendedText =
      blendColor(targetText, baseStyle.bg, captionStyle.textAlpha);
  const Color blendedBg =
      blendColor(targetBg, baseStyle.bg, captionStyle.backgroundAlpha);

  Style subtitleTextStyle = accentStyle;
  subtitleTextStyle.fg = blendedText;
  subtitleTextStyle.bg = blendedBg;
  Style subtitleBoxStyle = subtitleTextStyle;
  subtitleBoxStyle.fg = blendedBg;

  auto trimAscii = [](std::string s) {
    size_t begin = 0;
    while (begin < s.size() &&
           (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r')) {
      ++begin;
    }
    size_t end = s.size();
    while (end > begin &&
           (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) {
      --end;
    }
    return s.substr(begin, end - begin);
  };

  auto wrapSubtitle = [&](const std::string& text, int maxChars,
                          int maxLines) -> std::vector<std::string> {
    std::vector<std::string> lines;
    if (maxChars <= 0 || maxLines <= 0) return lines;
    size_t start = 0;
    while (start <= text.size() && static_cast<int>(lines.size()) < maxLines) {
      size_t end = text.find('\n', start);
      std::string raw =
          (end == std::string::npos) ? text.substr(start)
                                     : text.substr(start, end - start);
      raw = trimAscii(raw);
      if (raw.empty()) {
        if (end == std::string::npos) break;
        start = end + 1;
        continue;
      }

      std::string remaining = raw;
      while (!remaining.empty() &&
             static_cast<int>(lines.size()) < maxLines) {
        int charCount = utf8DisplayWidth(remaining);
        if (charCount <= maxChars) {
          lines.push_back(remaining);
          break;
        }
        std::string chunk = utf8TakeDisplayWidth(remaining, maxChars);
        size_t split = chunk.find_last_of(" \t");
        if (split == std::string::npos || split < chunk.size() / 3) {
          split = chunk.size();
        }
        std::string line = trimAscii(remaining.substr(0, split));
        if (line.empty()) {
          line = chunk;
          split = chunk.size();
        }
        lines.push_back(line);
        if (split >= remaining.size()) {
          remaining.clear();
        } else {
          remaining = trimAscii(remaining.substr(split));
        }
      }

      if (end == std::string::npos) break;
      start = end + 1;
    }
    return lines;
  };

  const int subtitleAreaX = (allowFrame && artWidth > 0) ? artX : 0;
  const int subtitleAreaW =
      (allowFrame && artWidth > 0) ? artWidth : std::max(1, width);
  // VLC-like wrapping behavior: fit to available subtitle area width.
  const int maxSubtitleChars = std::max(8, subtitleAreaW - 4);
  int maxSubtitleLines = overlayVisible ? 2 : 3;
  std::vector<std::string> lines =
      wrapSubtitle(subtitleText, maxSubtitleChars, maxSubtitleLines);
  if (lines.empty()) return;

  const int artBottomY =
      (allowFrame && visibleArtHeight > 0)
          ? (artTop + visibleArtHeight - 1)
          : (height - 1);
  const int overlayTopY =
      overlayVisible ? (height - std::max(5, overlayReservedLines)) : height;
  int subtitleBottomY = std::min(artBottomY - 1, overlayTopY - 1);
  subtitleBottomY = std::max(artTop, subtitleBottomY);

  int y = subtitleBottomY - static_cast<int>(lines.size()) + 1;
  if (y < artTop) {
    y = artTop;
  }
  for (const std::string& rawLine : lines) {
    if (y >= height) break;
    std::string line = rawLine;
    if (utf8DisplayWidth(line) > subtitleAreaW - 2) {
      line = utf8TakeDisplayWidth(line, std::max(1, subtitleAreaW - 2));
    }
    int lineWidth = utf8DisplayWidth(line);
    int x = subtitleAreaX + std::max(0, (subtitleAreaW - lineWidth) / 2);
    if (captionStyle.backgroundAlpha > 0.01f) {
      const int pad = 1;
      int bgX = std::max(0, x - pad);
      int bgW = std::min(width - bgX, lineWidth + pad * 2);
      if (bgW > 0) {
        screen.writeRun(bgX, y, bgW, L' ', subtitleBoxStyle);
      }
    }
    screen.writeText(x, y, line, subtitleTextStyle);
    ++y;
  }
}

}  // namespace playback_frame_output
