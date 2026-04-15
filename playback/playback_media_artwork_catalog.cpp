#include "playback_media_artwork_catalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include "media_artwork_sidecar.h"
#include "runtime_helpers.h"
#include "ui_helpers.h"
#include "video/ascii/asciiart.h"
#include "video/framebuffer/text_grid_bitmap_renderer.h"

namespace {

constexpr int kArtworkBitmapWidth = 680;
constexpr int kArtworkBitmapHeight = 680;
constexpr int kArtworkMaxCols = 48;
constexpr int kArtworkMaxRows = 48;
constexpr int kPosterMinCols = 16;
constexpr int kPosterMaxCols = 34;
constexpr int kPosterMinRows = 10;
constexpr int kPosterMaxRows = 18;

std::string trimAscii(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

void setError(std::string* error, const std::string& message) {
  if (error) {
    *error = message;
  }
}

std::string ffmpegError(int err) {
  char buf[256];
  buf[0] = '\0';
  av_strerror(err, buf, sizeof(buf));
  return std::string(buf);
}

std::string fallbackMediaTitle(const std::filesystem::path& file) {
  std::string title = toUtf8String(file.stem());
  if (!title.empty()) {
    return title;
  }
  title = toUtf8String(file.filename());
  if (!title.empty()) {
    return title;
  }
  return toUtf8String(file);
}

std::wstring utf8ToWideLossy(const std::string& text) {
#ifdef _WIN32
  if (text.empty()) {
    return {};
  }
  int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                   static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    std::wstring fallback;
    fallback.reserve(text.size());
    for (unsigned char ch : text) {
      fallback.push_back(static_cast<wchar_t>(ch));
    }
    return fallback;
  }
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), out.data(), needed);
  return out;
#else
  return std::wstring(text.begin(), text.end());
#endif
}

void fillPosterRect(AsciiArt* art, int x0, int y0, int x1, int y1,
                    const Color& bg) {
  if (!art) {
    return;
  }
  for (int y = std::max(0, y0); y < std::min(art->height, y1); ++y) {
    for (int x = std::max(0, x0); x < std::min(art->width, x1); ++x) {
      auto& cell = art->cells[static_cast<size_t>(y) *
                                  static_cast<size_t>(art->width) +
                              static_cast<size_t>(x)];
      cell.bg = bg;
      cell.hasBg = true;
    }
  }
}

void putPosterChar(AsciiArt* art, int x, int y, wchar_t ch, const Color& fg,
                   const Color& bg) {
  if (!art || x < 0 || x >= art->width || y < 0 || y >= art->height) {
    return;
  }
  auto& cell = art->cells[static_cast<size_t>(y) *
                              static_cast<size_t>(art->width) +
                          static_cast<size_t>(x)];
  cell.ch = ch;
  cell.fg = fg;
  cell.bg = bg;
  cell.hasBg = true;
}

void putPosterText(AsciiArt* art, int x, int y, const std::string& text,
                   const Color& fg, const Color& bg) {
  const std::wstring wide = utf8ToWideLossy(text);
  int col = x;
  for (wchar_t ch : wide) {
    if (!art || col >= art->width) {
      break;
    }
    putPosterChar(art, col++, y, ch, fg, bg);
  }
}

std::vector<std::string> wrapPosterLines(const std::string& text, int width,
                                         int maxLines) {
  std::vector<std::string> lines;
  if (text.empty() || width <= 0 || maxLines <= 0) {
    return lines;
  }

  std::string remaining = trimAscii(text);
  while (!remaining.empty() && static_cast<int>(lines.size()) < maxLines) {
    if (utf8CodepointCount(remaining) <= width ||
        static_cast<int>(lines.size()) == maxLines - 1) {
      lines.push_back(fitLine(remaining, width));
      break;
    }

    int cut = width;
    int bestSpace = -1;
    const int codepoints = utf8CodepointCount(remaining);
    for (int i = 0; i < width && i < codepoints; ++i) {
      if (utf8Slice(remaining, i, 1) == " ") {
        bestSpace = i;
      }
    }
    if (bestSpace > 0) {
      cut = bestSpace;
    }

    std::string line = trimAscii(utf8Slice(remaining, 0, cut));
    if (line.empty()) {
      line = fitLine(remaining, width);
      remaining.clear();
    } else {
      remaining = trimAscii(
          utf8Slice(remaining, cut + (bestSpace > 0 ? 1 : 0),
                    std::max(0, codepoints - cut)));
    }
    lines.push_back(line);
  }

  return lines;
}

std::string posterFooterLabel(const PlaybackMediaDisplayRequest& request,
                              const PlaybackMediaDisplayInfo& info) {
  std::string mediaKind = request.isVideo ? "VIDEO" : "AUDIO";
  if (request.trackIndex >= 0) {
    return mediaKind + "  TRACK " +
           std::to_string(info.trackNumber != 0
                              ? info.trackNumber
                              : static_cast<uint32_t>(request.trackIndex + 1));
  }
  return mediaKind;
}

bool buildAsciiPosterArtwork(const PlaybackMediaDisplayRequest& request,
                             const PlaybackMediaDisplayInfo& info,
                             int maxWidth,
                             int maxHeight,
                             AsciiArt* out) {
  if (!out || maxWidth <= 0 || maxHeight <= 0) {
    return false;
  }

  const int cols = std::clamp(maxWidth, kPosterMinCols, kPosterMaxCols);
  const int rows = std::clamp(maxHeight, kPosterMinRows, kPosterMaxRows);
  out->width = cols;
  out->height = rows;
  out->cells.assign(static_cast<size_t>(cols) * static_cast<size_t>(rows), {});

  const Color background{10, 16, 28};
  const Color panel{18, 28, 44};
  const Color accent{255, 214, 120};
  const Color soft{216, 227, 241};
  const Color dim{124, 150, 178};

  fillPosterRect(out, 0, 0, cols, rows, background);
  fillPosterRect(out, 1, 1, cols - 1, rows - 1, panel);

  for (int x = 1; x < cols - 1; ++x) {
    putPosterChar(out, x, 1, L'-', accent, panel);
    putPosterChar(out, x, rows - 2, L'-', accent, panel);
  }
  for (int y = 1; y < rows - 1; ++y) {
    putPosterChar(out, 1, y, L'|', accent, panel);
    putPosterChar(out, cols - 2, y, L'|', accent, panel);
  }
  putPosterChar(out, 1, 1, L'+', accent, panel);
  putPosterChar(out, cols - 2, 1, L'+', accent, panel);
  putPosterChar(out, 1, rows - 2, L'+', accent, panel);
  putPosterChar(out, cols - 2, rows - 2, L'+', accent, panel);

  const int contentX = cols >= 24 ? 4 : 2;
  const int contentWidth = std::max(6, cols - contentX - 3);
  const bool showBrand = rows >= 12 && cols >= 20;
  int footerY = std::max(2, rows - 4);
  int row = showBrand ? 3 : 2;
  if (showBrand) {
    putPosterText(out, contentX, row, "RADIOIFY", accent, panel);
    row += 2;
  }

  const std::string title =
      info.title.empty() ? fallbackMediaTitle(request.file) : info.title;
  const int titleLines = rows >= 16 ? 4 : (rows >= 13 ? 3 : 2);
  for (const auto& line : wrapPosterLines(title, contentWidth, titleLines)) {
    if (row >= footerY) {
      break;
    }
    putPosterText(out, contentX, row++, line, soft, panel);
  }

  if (row < footerY) {
    ++row;
  }

  const int artistLines = rows >= 14 ? 2 : 1;
  for (const auto& line : wrapPosterLines(info.artist, contentWidth,
                                          artistLines)) {
    if (row >= footerY) {
      break;
    }
    putPosterText(out, contentX, row++, line, accent, panel);
  }

  const std::string secondary =
      info.albumTitle.empty() ? info.subtitle : info.albumTitle;
  const int secondaryLines = rows >= 15 ? 2 : 1;
  for (const auto& line : wrapPosterLines(secondary, contentWidth,
                                          secondaryLines)) {
    if (row >= footerY) {
      break;
    }
    putPosterText(out, contentX, row++, line, dim, panel);
  }

  putPosterText(out, contentX, footerY, posterFooterLabel(request, info), dim,
                panel);
  return true;
}

bool buildBitmapArtworkFromAscii(const AsciiArt& art,
                                 PlaybackMediaArtwork* outArtwork) {
  if (!outArtwork || art.width <= 0 || art.height <= 0 ||
      art.cells.size() !=
          static_cast<size_t>(art.width) * static_cast<size_t>(art.height)) {
    return false;
  }

  std::vector<ScreenCell> cells(art.cells.size());
  for (size_t i = 0; i < art.cells.size(); ++i) {
    const auto& src = art.cells[i];
    auto& dst = cells[i];
    dst.ch = src.ch;
    dst.fg = src.fg;
    dst.bg = src.hasBg ? src.bg : Color{};
  }

  std::vector<uint8_t> pixels;
  if (!renderScreenGridToBitmap(cells.data(), art.width, art.height,
                                kArtworkBitmapWidth, kArtworkBitmapHeight,
                                &pixels)) {
    return false;
  }

  outArtwork->kind = PlaybackMediaArtwork::Kind::Bgra32;
  outArtwork->width = kArtworkBitmapWidth;
  outArtwork->height = kArtworkBitmapHeight;
  outArtwork->bytes = std::move(pixels);
  return true;
}

bool tryBuildAsciiArtworkFromImageFile(const std::filesystem::path& file,
                                       int maxWidth,
                                       int maxHeight,
                                       AsciiArt* out) {
  if (!out) {
    return false;
  }
  std::string unusedError;
  return renderAsciiArt(file, maxWidth, maxHeight, *out, &unusedError);
}

bool tryBuildAsciiArtworkFromEncodedBytes(const uint8_t* bytes, size_t size,
                                          int maxWidth,
                                          int maxHeight,
                                          AsciiArt* out) {
  if (!out) {
    return false;
  }
  std::string unusedError;
  return renderAsciiArtFromEncodedImageBytes(bytes, size, maxWidth, maxHeight,
                                             *out, &unusedError);
}

bool tryResolveSidecarArtwork(const std::filesystem::path& file, int maxWidth,
                              int maxHeight, AsciiArt* out) {
  if (!out) {
    return false;
  }

  std::vector<std::filesystem::path> candidates;
  appendMediaArtworkSidecarCandidates(file, &candidates);

  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
      continue;
    }
    if (tryBuildAsciiArtworkFromImageFile(candidate, maxWidth, maxHeight,
                                          out)) {
      return true;
    }
  }
  return false;
}

bool tryResolveEmbeddedArtwork(const std::filesystem::path& file, int maxWidth,
                               int maxHeight, AsciiArt* out,
                               std::string* error) {
  if (!out) {
    return false;
  }

  AVFormatContext* fmt = nullptr;
  const std::string pathUtf8 = toUtf8String(file);
  const int openErr =
      avformat_open_input(&fmt, pathUtf8.c_str(), nullptr, nullptr);
  if (openErr < 0) {
    setError(error, "Failed to open media artwork: " + ffmpegError(openErr));
    return false;
  }

  const int infoErr = avformat_find_stream_info(fmt, nullptr);
  if (infoErr < 0) {
    avformat_close_input(&fmt);
    setError(error, "Failed to read media artwork: " + ffmpegError(infoErr));
    return false;
  }

  bool resolved = false;
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    AVStream* stream = fmt->streams[i];
    if (!stream || !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      continue;
    }
    const AVPacket& pic = stream->attached_pic;
    if (!pic.data || pic.size <= 0) {
      continue;
    }
    if (tryBuildAsciiArtworkFromEncodedBytes(
            pic.data, static_cast<size_t>(pic.size), maxWidth, maxHeight,
            out)) {
      resolved = true;
      break;
    }
  }

  avformat_close_input(&fmt);
  return resolved;
}

}  // namespace

bool resolvePlaybackMediaArtworkAscii(const PlaybackMediaDisplayRequest& request,
                                      const PlaybackMediaDisplayInfo& info,
                                      int maxWidth,
                                      int maxHeight,
                                      AsciiArt* out,
                                      std::string* error) {
  if (!out) {
    setError(error, "Playback media artwork ASCII output was null.");
    return false;
  }

  *out = AsciiArt{};
  if (maxWidth <= 0 || maxHeight <= 0) {
    setError(error, "Playback media artwork ASCII target was invalid.");
    return false;
  }

  std::string artworkError;
  if (!request.isVideo) {
    if (tryResolveEmbeddedArtwork(request.file, maxWidth, maxHeight, out,
                                  &artworkError)) {
      return true;
    }
    if (tryResolveSidecarArtwork(request.file, maxWidth, maxHeight, out)) {
      return true;
    }
  } else {
    if (tryResolveEmbeddedArtwork(request.file, maxWidth, maxHeight, out,
                                  &artworkError)) {
      return true;
    }
    if (tryResolveSidecarArtwork(request.file, maxWidth, maxHeight, out)) {
      return true;
    }
  }
  if (buildAsciiPosterArtwork(request, info, maxWidth, maxHeight, out)) {
    return true;
  }

  if (!artworkError.empty()) {
    setError(error, artworkError);
  }
  return false;
}

bool resolvePlaybackMediaArtworkBitmap(
    const PlaybackMediaDisplayRequest& request,
    const PlaybackMediaDisplayInfo& info,
    PlaybackMediaArtwork* out,
    std::string* error) {
  if (!out) {
    setError(error, "Playback media artwork bitmap output was null.");
    return false;
  }

  *out = PlaybackMediaArtwork{};
  AsciiArt art;
  std::string asciiError;
  if (!resolvePlaybackMediaArtworkAscii(request, info, kArtworkMaxCols,
                                        kArtworkMaxRows, &art, &asciiError)) {
    if (!asciiError.empty()) {
      setError(error, asciiError);
    }
    return false;
  }
  if (!buildBitmapArtworkFromAscii(art, out)) {
    setError(error, "Failed to rasterize playback media artwork.");
    return false;
  }
  return true;
}
