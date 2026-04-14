#include "consolescreen.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "asciiart.h"
#include "browser_grid_index.h"
#include "consoleinput.h"
#include "playback/playback_media_artwork_catalog.h"
#include "playback/playback_media_metadata_catalog.h"
#include "videodecoder.h"

static std::wstring utf8ToWide(const std::string& text) {
  if (text.empty()) return {};
  int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    std::wstring fallback;
    fallback.reserve(text.size());
    for (unsigned char c : text) {
      fallback.push_back(static_cast<wchar_t>(c));
    }
    return fallback;
  }
  std::wstring out(needed, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), out.data(), needed);
  return out;
}

static bool wideToUtf8(const std::wstring& text, std::string& out) {
  if (text.empty()) {
    out.clear();
    return true;
  }
  int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                   static_cast<int>(text.size()), nullptr, 0,
                                   nullptr, nullptr);
  if (needed <= 0) return false;
  out.resize(static_cast<size_t>(needed));
  int written = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
      static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
  if (written <= 0) return false;
  if (written != needed) out.resize(static_cast<size_t>(written));
  return true;
}

static std::string pathToUtf8(const std::filesystem::path& p) {
#ifdef _WIN32
  auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
#else
  return p.string();
#endif
}

static size_t utf8Next(const std::string& s, size_t i) {
  if (i >= s.size()) return s.size();
  unsigned char c = static_cast<unsigned char>(s[i]);
  if ((c & 0x80) == 0x00) return i + 1;
  if ((c & 0xE0) == 0xC0) return std::min(s.size(), i + 2);
  if ((c & 0xF0) == 0xE0) return std::min(s.size(), i + 3);
  if ((c & 0xF8) == 0xF0) return std::min(s.size(), i + 4);
  return i + 1;
}

static int utf8CodepointCount(const std::string& s) {
  int count = 0;
  for (size_t i = 0; i < s.size();) {
    i = utf8Next(s, i);
    count++;
  }
  return count;
}

static std::string utf8Take(const std::string& s, int count) {
  if (count <= 0) return "";
  size_t i = 0;
  int c = 0;
  for (; i < s.size() && c < count; ++c) {
    i = utf8Next(s, i);
  }
  return s.substr(0, i);
}

static std::string fitLine(const std::string& s, int width) {
  if (width <= 0) return "";
  int count = utf8CodepointCount(s);
  if (count <= width) return s;
  if (width <= 1) return utf8Take(s, width);
  return utf8Take(s, width - 1) + "~";
}

static bool sameColor(const Color& a, const Color& b) {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

struct PaletteEntry {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

constexpr std::array<PaletteEntry, 16> kConsolePalette = {{
    {0, 0, 0},
    {0, 0, 128},
    {0, 128, 0},
    {0, 128, 128},
    {128, 0, 0},
    {128, 0, 128},
    {128, 128, 0},
    {192, 192, 192},
    {128, 128, 128},
    {0, 0, 255},
    {0, 255, 0},
    {0, 255, 255},
    {255, 0, 0},
    {255, 0, 255},
    {255, 255, 0},
    {255, 255, 255},
}};

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t r5 = static_cast<uint16_t>(r >> 3);
  uint16_t g6 = static_cast<uint16_t>(g >> 2);
  uint16_t b5 = static_cast<uint16_t>(b >> 3);
  return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

const std::array<uint8_t, 65536> kConsoleColorFrom565 = []() {
  std::array<uint8_t, 65536> table{};
  for (int i = 0; i < 65536; ++i) {
    uint8_t r5 = static_cast<uint8_t>((i >> 11) & 0x1F);
    uint8_t g6 = static_cast<uint8_t>((i >> 5) & 0x3F);
    uint8_t b5 = static_cast<uint8_t>(i & 0x1F);
    uint8_t r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    uint8_t g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    uint8_t b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
    int best = 0;
    int bestDist = std::numeric_limits<int>::max();
    for (int p = 0; p < static_cast<int>(kConsolePalette.size()); ++p) {
      int dr = static_cast<int>(r) - static_cast<int>(kConsolePalette[p].r);
      int dg = static_cast<int>(g) - static_cast<int>(kConsolePalette[p].g);
      int db = static_cast<int>(b) - static_cast<int>(kConsolePalette[p].b);
      int dist = dr * dr + dg * dg + db * db;
      if (dist < bestDist) {
        bestDist = dist;
        best = p;
      }
    }
    table[static_cast<size_t>(i)] = static_cast<uint8_t>(best);
  }
  return table;
}();

inline uint8_t consoleColorIndex(const Color& c) {
  return kConsoleColorFrom565[rgb565(c.r, c.g, c.b)];
}

inline void appendByte(std::wstring& out, unsigned value) {
  if (value >= 100) {
    out.push_back(static_cast<wchar_t>(L'0' + (value / 100)));
    value %= 100;
    out.push_back(static_cast<wchar_t>(L'0' + (value / 10)));
    out.push_back(static_cast<wchar_t>(L'0' + (value % 10)));
    return;
  }
  if (value >= 10) {
    out.push_back(static_cast<wchar_t>(L'0' + (value / 10)));
    out.push_back(static_cast<wchar_t>(L'0' + (value % 10)));
    return;
  }
  out.push_back(static_cast<wchar_t>(L'0' + value));
}

inline void appendNumber(std::wstring& out, int value) {
  if (value <= 0) {
    out.push_back(L'0');
    return;
  }
  wchar_t buf[16];
  const int bufSize = static_cast<int>(sizeof(buf) / sizeof(buf[0]));
  int len = 0;
  while (value > 0 && len < bufSize) {
    buf[len++] = static_cast<wchar_t>(L'0' + (value % 10));
    value /= 10;
  }
  for (int i = len - 1; i >= 0; --i) {
    out.push_back(buf[i]);
  }
}

inline void appendCursorPos(std::wstring& out, int x, int y) {
  out.append(L"\x1b[");
  appendNumber(out, y);
  out.push_back(L';');
  appendNumber(out, x);
  out.push_back(L'H');
}

inline void appendColorSeq(std::wstring& out, const Color& c, bool fg) {
  if (fg) {
    out.append(L"\x1b[38;2;");
  } else {
    out.append(L"\x1b[48;2;");
  }
  appendByte(out, c.r);
  out.push_back(L';');
  appendByte(out, c.g);
  out.push_back(L';');
  appendByte(out, c.b);
  out.push_back(L'm');
}

struct ThumbnailCell {
  wchar_t ch = L' ';
  Color fg{255, 255, 255};
  Color bg{0, 0, 0};
  bool hasBg = false;
};

struct Thumbnail {
  bool ok = false;
  int width = 0;
  int height = 0;
  std::vector<ThumbnailCell> cells;
};

enum class ThumbState {
  Pending,
  Ready,
  Failed,
};

struct ThumbEntry {
  ThumbState state = ThumbState::Pending;
  std::shared_ptr<Thumbnail> thumb;
};

struct ThumbJob {
  std::string key;
  std::filesystem::path path;
  bool isImage = false;
  bool isVideo = false;
  bool isAudio = false;
  int trackIndex = -1;
  int width = 0;
  int height = 0;
  uint64_t generation = 0;
};

struct ThumbCacheState {
  ThumbCacheState() {
    thumbnailReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  ~ThumbCacheState() {
    if (thumbnailReadyEvent) {
      CloseHandle(thumbnailReadyEvent);
      thumbnailReadyEvent = nullptr;
    }
  }

  int thumbW = 0;
  int thumbH = 0;
  uint64_t generation = 1;
  std::unordered_map<std::string, ThumbEntry> entries;
  std::deque<ThumbJob> queue;
  std::mutex mutex;
  std::condition_variable cv;
  bool workerStarted = false;
  HANDLE thumbnailReadyEvent = nullptr;
};

static ThumbCacheState& thumbCache() {
  static ThumbCacheState cache;
  return cache;
}

static std::string thumbnailCacheKey(const FileEntry& entry) {
  std::string key = pathToUtf8(entry.path);
  if (entry.trackIndex >= 0) {
    key += "#track=" + std::to_string(entry.trackIndex);
  }
  return key;
}

static std::string fitName(const std::string& name, int colWidth) {
  int maxLen = colWidth - 2;
  if (maxLen <= 1) return name.empty() ? " " : utf8Take(name, 1);
  if (utf8CodepointCount(name) <= maxLen) return name;
  return utf8Take(name, maxLen - 1) + "~";
}

static void drawCenteredText(ConsoleScreen& screen, int x, int y, int width,
                             int height, const std::string& text,
                             const Style& style) {
  if (width <= 0 || height <= 0 || text.empty()) return;
  int textWidth = utf8CodepointCount(text);
  if (textWidth <= 0) return;
  int cx = x + std::max(0, (width - textWidth) / 2);
  int cy = y + height / 2;
  screen.writeText(cx, cy, text, style);
}

static void drawScrollBar(ConsoleScreen& screen, int x, int width, int top,
                          int height, int scrollRow, int totalRows,
                          int visibleRows, const Style& trackStyle,
                          const Style& thumbStyle) {
  if (x < 0 || width <= 0 || height <= 0 || totalRows <= visibleRows) return;
  int maxScroll = std::max(0, totalRows - visibleRows);
  if (maxScroll <= 0) return;
  int barHeight = height;
  int thumbHeight = std::max(1, static_cast<int>(
      (static_cast<int64_t>(visibleRows) * barHeight + totalRows - 1) /
      totalRows));
  if (thumbHeight > barHeight) thumbHeight = barHeight;
  int thumbTravel = barHeight - thumbHeight;
  int scroll = std::clamp(scrollRow, 0, maxScroll);
  int thumbTop = top;
  if (thumbTravel > 0) {
    thumbTop += static_cast<int>(
        (static_cast<int64_t>(scroll) * thumbTravel + maxScroll / 2) /
        maxScroll);
  }
  for (int col = 0; col < width; ++col) {
    for (int y = 0; y < barHeight; ++y) {
      screen.writeChar(x + col, top + y, L'\u2591', trackStyle);
    }
    for (int y = 0; y < thumbHeight; ++y) {
      screen.writeChar(x + col, thumbTop + y, L'\u2588', thumbStyle);
    }
  }
}

static void assignThumbnailFromAscii(const AsciiArt& art, Thumbnail& out) {
  out.ok = true;
  out.width = art.width;
  out.height = art.height;
  out.cells.resize(art.cells.size());
  for (size_t i = 0; i < art.cells.size(); ++i) {
    const auto& cell = art.cells[i];
    auto& dst = out.cells[i];
    dst.ch = cell.ch;
    dst.fg = cell.fg;
    dst.bg = cell.bg;
    dst.hasBg = cell.hasBg;
  }
}

static bool renderImageThumbnail(const std::filesystem::path& file,
                                 int maxWidth, int maxHeight, Thumbnail& out,
                                 std::string* error) {
  AsciiArt art;
  if (!renderAsciiArt(file, maxWidth, maxHeight, art, error)) return false;
  assignThumbnailFromAscii(art, out);
  return true;
}

static void computeVideoThumbTarget(int thumbWidth, int thumbHeight, int& outW,
                                    int& outH) {
  outW = std::max(2, thumbWidth * 2);
  outH = std::max(4, thumbHeight * 4);
  if (outW & 1) ++outW;
  if (outH & 1) ++outH;
}

static bool renderVideoThumbnail(const std::filesystem::path& file,
                                 int maxWidth, int maxHeight, Thumbnail& out,
                                 std::string* error) {
  if (maxWidth <= 0 || maxHeight <= 0) return false;
  VideoDecoder decoder;
  std::string initError;
  if (!decoder.init(file, &initError, true, false)) {
    if (!decoder.init(file, &initError, false, false)) {
      if (error && !initError.empty()) *error = initError;
      return false;
    }
  }

  int targetW = 0;
  int targetH = 0;
  computeVideoThumbTarget(maxWidth, maxHeight, targetW, targetH);
  decoder.setTargetSize(targetW, targetH, nullptr);

  VideoFrame frame;
  VideoReadInfo info;
  for (int i = 0; i < 20; ++i) {
    if (!decoder.readFrame(frame, &info, true)) {
      if (decoder.atEnd()) break;
      continue;
    }

    AsciiArt art;
    bool ok = false;
    if ((frame.format == VideoPixelFormat::NV12 ||
         frame.format == VideoPixelFormat::P010) &&
        !frame.yuv.empty()) {
      YuvFormat yuvFormat = (frame.format == VideoPixelFormat::P010)
                                ? YuvFormat::P010
                                : YuvFormat::NV12;
      ok = renderAsciiArtFromYuv(
          frame.yuv.data(), frame.width, frame.height, frame.stride,
          frame.planeHeight, yuvFormat, frame.fullRange, frame.yuvMatrix,
          frame.yuvTransfer, maxWidth, maxHeight, art);
    } else if (!frame.rgba.empty()) {
      ok = renderAsciiArtFromRgba(frame.rgba.data(), frame.width, frame.height,
                                  maxWidth, maxHeight, art, true);
    }
    if (ok) {
      assignThumbnailFromAscii(art, out);
      return true;
    }
  }
  if (error && !initError.empty()) *error = initError;
  return false;
}

static bool renderAudioThumbnail(const std::filesystem::path& file,
                                 int trackIndex, int maxWidth, int maxHeight,
                                 Thumbnail& out, std::string* error) {
  PlaybackMediaDisplayRequest request;
  request.file = file;
  request.trackIndex = trackIndex;
  request.isVideo = false;

  PlaybackMediaDisplayInfo info;
  std::string metadataError;
  resolvePlaybackMediaDisplayInfo(request, &info, &metadataError);

  AsciiArt art;
  std::string artworkError;
  if (!resolvePlaybackMediaArtworkAscii(request, info, maxWidth, maxHeight,
                                        &art, &artworkError)) {
    if (error) {
      *error = !artworkError.empty() ? artworkError : metadataError;
    }
    return false;
  }

  assignThumbnailFromAscii(art, out);
  return true;
}

static void thumbWorkerLoop() {
  ThumbCacheState& cache = thumbCache();
  for (;;) {
    ThumbJob job;
    {
      std::unique_lock<std::mutex> lock(cache.mutex);
      cache.cv.wait(lock, [&]() { return !cache.queue.empty(); });
      job = std::move(cache.queue.front());
      cache.queue.pop_front();
    }

    Thumbnail thumb;
    bool ok = false;
    std::string error;
    if (job.isImage) {
      ok = renderImageThumbnail(job.path, job.width, job.height, thumb, &error);
    } else if (job.isVideo) {
      ok = renderVideoThumbnail(job.path, job.width, job.height, thumb, &error);
    } else if (job.isAudio) {
      ok = renderAudioThumbnail(job.path, job.trackIndex, job.width,
                                job.height, thumb, &error);
    }

    {
      std::lock_guard<std::mutex> lock(cache.mutex);
      if (job.generation != cache.generation) {
        continue;
      }
      auto it = cache.entries.find(job.key);
      if (it != cache.entries.end()) {
        if (ok) {
          it->second.state = ThumbState::Ready;
          it->second.thumb = std::make_shared<Thumbnail>(std::move(thumb));
        } else {
          it->second.state = ThumbState::Failed;
          it->second.thumb.reset();
        }
      }
    }
    if (cache.thumbnailReadyEvent) {
      SetEvent(cache.thumbnailReadyEvent);
    }
  }
}

static void startThumbWorkerLocked(ThumbCacheState& cache) {
  if (cache.workerStarted) return;
  cache.workerStarted = true;
  std::thread(thumbWorkerLoop).detach();
}

HANDLE browserThumbnailWakeHandle() {
  return thumbCache().thumbnailReadyEvent;
}

bool consumeBrowserThumbnailWake() {
  ThumbCacheState& cache = thumbCache();
  if (!cache.thumbnailReadyEvent) return false;
  DWORD state = WaitForSingleObject(cache.thumbnailReadyEvent, 0);
  if (state != WAIT_OBJECT_0) return false;
  ResetEvent(cache.thumbnailReadyEvent);
  return true;
}

GridLayout buildLayout(const BrowserState& state, int width, int listHeight) {
  GridLayout layout;
  layout.names.reserve(state.entries.size());
  bool hasSectionHeaders = std::any_of(state.entries.begin(), state.entries.end(),
                                       [](const FileEntry& entry) {
                                         return entry.isSectionHeader;
                                       });
  int maxName = 0;
  for (const auto& e : state.entries) {
    std::string name = e.name;
    if (e.isDir && name != "..") name += "/";
    layout.names.push_back(name);
    maxName = std::max(maxName, utf8CodepointCount(name) + 2);
  }

  constexpr int kThumbMinWidth = 20;
  constexpr int kThumbTargetWidth = 32;
  constexpr int kThumbMinHeight = 8;
  constexpr int kThumbMaxHeight = 24;
  constexpr int kThumbLabelRows = 1;
  constexpr int kPreviewMinWidth = 24;
  constexpr int kPreviewMinHeight = 8;
  constexpr int kPreviewGap = 2;
  constexpr int kListMinWidth = 20;
  constexpr int kScrollBarWidth = 2;

  layout.showThumbs =
      state.viewMode == BrowserState::ViewMode::Thumbnails &&
      (listHeight >= kThumbMinHeight + kThumbLabelRows &&
       width >= kThumbMinWidth + 2);
  layout.listWidth = width;
  layout.showPreview = false;
  layout.previewX = 0;
  layout.previewWidth = 0;
  layout.previewHeight = 0;
  if (layout.showThumbs) {
    layout.thumbHeight =
        std::clamp(listHeight - kThumbLabelRows, kThumbMinHeight,
                   kThumbMaxHeight);
    layout.cellHeight = layout.thumbHeight + kThumbLabelRows;
    int minColWidth = std::max(8, maxName + 2);
    int maxThumbWidth = std::max(1, width - 2);
    int targetThumbWidth = std::min(kThumbTargetWidth, maxThumbWidth);
    if (targetThumbWidth < kThumbMinWidth &&
        maxThumbWidth >= kThumbMinWidth) {
      targetThumbWidth = kThumbMinWidth;
    }
    int desiredColWidth = std::max(minColWidth, targetThumbWidth + 2);
    layout.colWidth = std::min(width, desiredColWidth);
    layout.colWidth = std::max(layout.colWidth, 6);
    layout.thumbWidth = std::max(1, layout.colWidth - 2);
  } else {
    int listWidth = width;
    if (state.viewMode == BrowserState::ViewMode::ListPreview &&
        width >= kListMinWidth + kPreviewMinWidth + kPreviewGap &&
        listHeight >= kPreviewMinHeight) {
      int baseListWidth = std::max(kListMinWidth, maxName + 3);
      int maxListWidth = width - kPreviewMinWidth - kPreviewGap;
      if (maxListWidth >= kListMinWidth) {
        int listCandidate = std::min(baseListWidth, maxListWidth);
        int previewWidth = width - listCandidate - kPreviewGap;
        if (previewWidth >= kPreviewMinWidth) {
          layout.showPreview = true;
          layout.previewWidth = previewWidth;
          layout.previewHeight = std::max(kPreviewMinHeight, listHeight);
          layout.previewX = listCandidate + kPreviewGap;
          listWidth = listCandidate;
        }
      }
    }
    layout.listWidth = std::max(1, listWidth);
    layout.cellHeight = 1;
    layout.thumbWidth = 0;
    layout.thumbHeight = 0;
    layout.colWidth =
        std::min(layout.listWidth, std::max(8, maxName + 3));
  }

  int safeRows = std::max(1, listHeight / std::max(1, layout.cellHeight));
  int maxCols = std::max(1, layout.listWidth / std::max(1, layout.colWidth));
  int cols = std::max(
      1,
      std::min(maxCols, static_cast<int>((state.entries.size() + safeRows - 1) /
                                         safeRows)));
  int totalRows = browserGridTotalRowsForEntryCount(
      state.viewMode, static_cast<int>(state.entries.size()), safeRows, cols);
  int visibleRows = std::min(safeRows, totalRows);

  if (hasSectionHeaders) {
    layout.showThumbs = false;
    layout.showPreview = false;
    layout.previewX = 0;
    layout.previewWidth = 0;
    layout.previewHeight = 0;
    layout.listWidth = std::max(1, width);
    layout.cellHeight = 1;
    layout.thumbWidth = 0;
    layout.thumbHeight = 0;
    layout.colWidth = layout.listWidth;
    layout.cols = 1;
    layout.totalRows = static_cast<int>(state.entries.size());
    layout.rowsVisible = std::min(std::max(1, safeRows), layout.totalRows);
    if (layout.rowsVisible <= 0) layout.rowsVisible = 1;
    layout.showScrollBar = (layout.totalRows > layout.rowsVisible);
    layout.scrollBarWidth = 0;
    layout.scrollBarX = -1;
    if (layout.showScrollBar) {
      int barWidth = std::min(kScrollBarWidth, std::max(1, width));
      layout.scrollBarWidth = barWidth;
      layout.scrollBarX = std::max(0, width - barWidth);
    }
    return layout;
  }

  layout.rowsVisible = visibleRows;
  layout.totalRows = totalRows;
  layout.cols = cols;
  layout.showScrollBar = (totalRows > visibleRows);
  layout.scrollBarWidth = 0;
  layout.scrollBarX = -1;
  if (layout.showScrollBar) {
    int barWidth = std::min(kScrollBarWidth, std::max(1, width));
    int listUsedWidth = std::max(1, layout.colWidth) * cols;
    layout.scrollBarWidth = barWidth;
    if (layout.showPreview) {
      layout.scrollBarX = std::max(0, layout.previewX - barWidth);
    } else if (listUsedWidth + barWidth <= width) {
      layout.scrollBarX = listUsedWidth;
    } else {
      layout.scrollBarX = std::max(0, width - barWidth);
    }
    if (layout.scrollBarX + barWidth > width) {
      layout.scrollBarX = std::max(0, width - barWidth);
    }
  }
  return layout;
}

void drawBrowserEntries(ConsoleScreen& screen, const BrowserState& browser,
                        const GridLayout& layout, int listTop, int listHeight,
                        const Style& baseStyle, const Style& normalStyle,
                        const Style& dirStyle, const Style& highlightStyle,
                        const Style& dimStyle,
                        bool (*isImage)(const std::filesystem::path&),
                        bool (*isVideo)(const std::filesystem::path&),
                        bool (*isAudio)(const std::filesystem::path&)) {
  ThumbCacheState& cache = thumbCache();
  if (browser.entries.empty()) {
    screen.writeText(2, listTop, "(no supported files)", dimStyle);
    return;
  }

  int desiredThumbW = 0;
  int desiredThumbH = 0;
  if (layout.showThumbs) {
    desiredThumbW = layout.thumbWidth;
    desiredThumbH = layout.thumbHeight;
  } else if (layout.showPreview) {
    desiredThumbW = layout.previewWidth;
    desiredThumbH = layout.previewHeight;
  }
  if (desiredThumbW > 0 && desiredThumbH > 0) {
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.thumbW != desiredThumbW || cache.thumbH != desiredThumbH) {
      cache.thumbW = desiredThumbW;
      cache.thumbH = desiredThumbH;
      cache.generation++;
      cache.entries.clear();
      cache.queue.clear();
    }
  }

  auto entryPrefix = [&](const FileEntry& entry) -> std::string {
    if (entry.isDir) return "\xF0\x9F\x93\x81 ";
    if (entry.trackIndex >= 0) return "\xE2\x99\xAA ";
    if (isAudio && isAudio(entry.path)) return "\xE2\x99\xAA ";
    if (isVideo && isVideo(entry.path)) return "\xE2\x96\xB6 ";
    if (isImage && isImage(entry.path)) return "\xE2\x96\xA1 ";
    return "\xC2\xB7 ";
  };

  constexpr int kThumbJobsPerFrame = 4;
  constexpr size_t kMaxThumbQueue = 64;
  int enqueueBudget =
      layout.showThumbs ? kThumbJobsPerFrame : (layout.showPreview ? 1 : 0);

  struct ThumbLookup {
    std::shared_ptr<Thumbnail> thumb;
    bool pending = false;
  };

  auto fetchThumb = [&](const FileEntry& entry, int width, int height,
                        bool wantImage, bool wantVideo,
                        bool wantAudio) -> ThumbLookup {
    ThumbLookup result;
    if (!wantImage && !wantVideo && !wantAudio) return result;
    if (width <= 0 || height <= 0) return result;
    std::string key = thumbnailCacheKey(entry);
    {
      std::lock_guard<std::mutex> lock(cache.mutex);
      auto it = cache.entries.find(key);
      if (it != cache.entries.end()) {
        if (it->second.state == ThumbState::Ready) {
          result.thumb = it->second.thumb;
        } else if (it->second.state == ThumbState::Pending) {
          result.pending = true;
        }
        return result;
      }
      if (enqueueBudget <= 0) return result;
      if (cache.queue.size() >= kMaxThumbQueue) return result;
      ThumbEntry entryState;
      entryState.state = ThumbState::Pending;
      cache.entries.emplace(key, entryState);
      ThumbJob job;
      job.key = key;
      job.path = entry.path;
      job.isImage = wantImage;
      job.isVideo = wantVideo;
      job.isAudio = wantAudio;
      job.trackIndex = entry.trackIndex;
      job.width = width;
      job.height = height;
      job.generation = cache.generation;
      cache.queue.push_back(std::move(job));
      startThumbWorkerLocked(cache);
      cache.cv.notify_one();
      enqueueBudget--;
      result.pending = true;
    }
    return result;
  };

  if (!layout.showThumbs) {
    for (int r = 0; r < layout.rowsVisible; ++r) {
      int y = listTop + r;
      int logicalRow = r + browser.scrollRow;
      if (logicalRow >= layout.totalRows) continue;
      for (int c = 0; c < layout.cols; ++c) {
        int idx = browserGridEntryIndex(layout, browser.viewMode, logicalRow, c,
                                        static_cast<int>(browser.entries.size()));
        if (idx < 0 || idx >= static_cast<int>(browser.entries.size())) continue;
        const auto& entry = browser.entries[static_cast<size_t>(idx)];
        bool isSelected = (idx == browser.selected);

        if (entry.isSectionHeader) {
          std::string cell = fitName("[" + entry.name + "]", layout.colWidth);
          int cellWidth = utf8CodepointCount(cell);
          if (cellWidth < layout.colWidth) {
            cell.append(static_cast<size_t>(layout.colWidth - cellWidth), ' ');
          } else if (cellWidth > layout.colWidth) {
            cell = utf8Take(cell, layout.colWidth);
          }
          Style headerStyle = dimStyle;
          if (isSelected) {
            headerStyle = dimStyle;
          }
          screen.writeText(c * layout.colWidth, y, cell, headerStyle);
          continue;
        }

        std::string cell = fitName(entryPrefix(entry) +
                                       layout.names[static_cast<size_t>(idx)],
                                   layout.colWidth);
        int cellWidth = utf8CodepointCount(cell);
        if (cellWidth < layout.colWidth) {
          cell.append(static_cast<size_t>(layout.colWidth - cellWidth), ' ');
        } else if (cellWidth > layout.colWidth) {
          cell = utf8Take(cell, layout.colWidth);
        }
        Style attr =
            isSelected ? highlightStyle : (entry.isDir ? dirStyle : normalStyle);
        screen.writeText(c * layout.colWidth, y, cell, attr);
      }
    }
    if (layout.showPreview && !browser.entries.empty()) {
      int idx = std::clamp(
          browser.selected, 0,
          static_cast<int>(browser.entries.size()) - 1);
      const auto& entry = browser.entries[static_cast<size_t>(idx)];
      bool img = !entry.isDir && isImage && isImage(entry.path);
      bool vid = !entry.isDir && isVideo && isVideo(entry.path);
      bool aud = !entry.isDir && isAudio && isAudio(entry.path);
      int previewW = std::max(1, layout.previewWidth);
      int previewH = std::max(1, layout.previewHeight);
      int previewX = layout.previewX;
      int previewY = listTop + std::max(0, (listHeight - previewH) / 2);

      ThumbLookup lookup = fetchThumb(entry, previewW, previewH, img, vid, aud);
      const Thumbnail* thumb = lookup.thumb.get();
      if (thumb && thumb->ok && thumb->width > 0 && thumb->height > 0) {
        int artW = std::min(thumb->width, previewW);
        int artH = std::min(thumb->height, previewH);
        int artX = previewX + std::max(0, (previewW - artW) / 2);
        int artY = previewY + std::max(0, (previewH - artH) / 2);
        for (int y = 0; y < artH; ++y) {
          for (int x = 0; x < artW; ++x) {
            const auto& cell =
                thumb->cells[static_cast<size_t>(y * thumb->width + x)];
            Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
            screen.writeChar(artX + x, artY + y, cell.ch, cellStyle);
          }
        }
      } else {
        std::string placeholder;
        if (entry.isDir) {
          placeholder = "\xF0\x9F\x93\x81";
        } else if (img) {
          placeholder = "\xE2\x96\xA1";
        } else if (vid) {
          placeholder = "\xE2\x96\xB6";
        } else if (aud) {
          placeholder = "\xE2\x99\xAA";
        } else {
          placeholder = "\xC2\xB7";
        }
        if ((img || vid || aud) && lookup.pending) {
          placeholder = "...";
        }
        placeholder = fitLine(placeholder, previewW);
        drawCenteredText(screen, previewX, previewY, previewW, previewH,
                         placeholder, dimStyle);
      }
    }
    if (layout.showScrollBar) {
      drawScrollBar(screen, layout.scrollBarX, layout.scrollBarWidth, listTop,
                    listHeight, browser.scrollRow, layout.totalRows,
                    layout.rowsVisible, dimStyle, normalStyle);
    }
    return;
  }

  for (int r = 0; r < layout.rowsVisible; ++r) {
    int logicalRow = r + browser.scrollRow;
    if (logicalRow >= layout.totalRows) continue;
    int cellTop = listTop + r * layout.cellHeight;
    for (int c = 0; c < layout.cols; ++c) {
      int idx = logicalRow * layout.cols + c;
      if (idx >= static_cast<int>(browser.entries.size())) continue;
      const auto& entry = browser.entries[static_cast<size_t>(idx)];
      bool isSelected = (idx == browser.selected);
      int cellLeft = c * layout.colWidth;
      int thumbX = cellLeft + 1;
      int thumbY = cellTop;
      int thumbW = std::max(1, layout.thumbWidth);
      int thumbH = std::max(1, layout.thumbHeight);
      bool img = !entry.isDir && isImage && isImage(entry.path);
      bool vid = !entry.isDir && isVideo && isVideo(entry.path);
      bool aud = !entry.isDir && isAudio && isAudio(entry.path);

      ThumbLookup lookup = fetchThumb(entry, thumbW, thumbH, img, vid, false);
      const Thumbnail* thumb = lookup.thumb.get();
      if (thumb && thumb->ok && thumb->width > 0 && thumb->height > 0) {
        int artW = std::min(thumb->width, thumbW);
        int artH = std::min(thumb->height, thumbH);
        int artX = thumbX + std::max(0, (thumbW - artW) / 2);
        int artY = thumbY + std::max(0, (thumbH - artH) / 2);
        for (int y = 0; y < artH; ++y) {
          for (int x = 0; x < artW; ++x) {
            const auto& cell =
                thumb->cells[static_cast<size_t>(y * thumb->width + x)];
            Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
            screen.writeChar(artX + x, artY + y, cell.ch, cellStyle);
          }
        }
      } else {
        std::string placeholder;
        if (entry.isDir) {
          placeholder = "\xF0\x9F\x93\x81";
        } else if (img) {
          placeholder = "\xE2\x96\xA1";
        } else if (vid) {
          placeholder = "\xE2\x96\xB6";
        } else if (aud) {
          placeholder = "\xE2\x99\xAA";
        } else {
          placeholder = "\xC2\xB7";
        }
        if ((img || vid) && lookup.pending) {
          placeholder = "...";
        }
        placeholder = fitLine(placeholder, thumbW);
        drawCenteredText(screen, thumbX, thumbY, thumbW, thumbH, placeholder,
                         dimStyle);
      }

      int labelY = cellTop + layout.thumbHeight;
      if (labelY < listTop + listHeight) {
        std::string label = fitName(entryPrefix(entry) +
                                        layout.names[static_cast<size_t>(idx)],
                                    layout.colWidth);
        int labelWidth = utf8CodepointCount(label);
        int labelX =
            cellLeft + std::max(0, (layout.colWidth - labelWidth) / 2);
        Style labelStyle =
            isSelected ? highlightStyle : (entry.isDir ? dirStyle : normalStyle);
        if (isSelected) {
          screen.writeRun(cellLeft, labelY, layout.colWidth, L' ', labelStyle);
        }
        screen.writeText(labelX, labelY, label, labelStyle);
      }
    }
  }
  if (layout.showScrollBar) {
    drawScrollBar(screen, layout.scrollBarX, layout.scrollBarWidth, listTop,
                  listHeight, browser.scrollRow, layout.totalRows,
                  layout.rowsVisible, dimStyle, normalStyle);
  }
}

BreadcrumbLine buildBreadcrumbLine(const std::filesystem::path& dir, int width) {
  BreadcrumbLine line;
  const std::string prefix = "  Path: ";
  const int prefixLen = utf8CodepointCount(prefix);
  if (width <= 0) return line;
  if (prefixLen >= width) {
    line.text = fitLine(prefix, width);
    return line;
  }

  struct Item {
    std::string label;
    std::filesystem::path path;
  };
  std::vector<Item> items;
  std::filesystem::path cur;
  std::filesystem::path root = dir.root_path();
#ifdef _WIN32
  items.push_back(Item{"This PC", {}});
  if (!root.empty()) {
    std::string rootLabel = pathToUtf8(dir.root_name());
    if (rootLabel.empty()) rootLabel = pathToUtf8(root);
    if (!rootLabel.empty() && (rootLabel.back() == '\\' || rootLabel.back() == '/')) {
      rootLabel.pop_back();
    }
    if (rootLabel.empty()) rootLabel = "\\";
    cur = root;
    items.push_back(Item{rootLabel, cur});
  }
#else
  if (!root.empty()) {
    cur = root;
    items.push_back(Item{"/", cur});
  }
#endif
  for (const auto& part : dir.relative_path()) {
    cur /= part;
    items.push_back(Item{pathToUtf8(part), cur});
  }
  if (items.empty()) {
    items.push_back(Item{pathToUtf8(dir), dir});
  }

  const std::string sep = " > ";
  const int sepLen = utf8CodepointCount(sep);
  const std::string ellipsis = "... > ";
  const int ellipsisLen = utf8CodepointCount(ellipsis);

  const int avail = width - prefixLen;
  std::vector<Item> visibleRev;
  int used = 0;
  int index = static_cast<int>(items.size()) - 1;
  for (; index >= 0; --index) {
    int labelLen = utf8CodepointCount(items[index].label);
    int add = labelLen + (visibleRev.empty() ? 0 : sepLen);
    if (used + add <= avail) {
      visibleRev.push_back(items[index]);
      used += add;
    } else {
      break;
    }
  }

  bool skipped = index >= 0;
  if (visibleRev.empty()) {
    Item last = items.back();
    last.label = fitLine(last.label, avail);
    visibleRev.push_back(last);
    used = utf8CodepointCount(last.label);
    skipped = items.size() > 1;
  }

  int ellipsisUse = (skipped && used + ellipsisLen <= avail) ? ellipsisLen : 0;
  std::vector<Item> visible(visibleRev.rbegin(), visibleRev.rend());

  std::string text = prefix;
  int x = prefixLen;
  if (ellipsisUse) {
    text += ellipsis;
    x += ellipsisLen;
  }
  for (size_t i = 0; i < visible.size(); ++i) {
    Item item = visible[i];
    int remaining = width - x;
    if (remaining <= 0) break;
    int labelLen = utf8CodepointCount(item.label);
    if (labelLen > remaining) {
      item.label = fitLine(item.label, remaining);
      labelLen = utf8CodepointCount(item.label);
    }
    Breadcrumb crumb;
    crumb.startX = x;
    crumb.endX = x + labelLen;
    crumb.path = item.path;
    line.crumbs.push_back(crumb);

    text += item.label;
    x += labelLen;
    if (i + 1 < visible.size()) {
      if (x + sepLen > width) break;
      text += sep;
      x += sepLen;
    }
  }
  line.text = text;
  if (line.text.empty()) {
    std::string fallback = fitLine(pathToUtf8(dir), width - prefixLen);
    line.text = prefix + fallback;
  }
  if (line.crumbs.empty()) {
    int total = utf8CodepointCount(line.text);
    if (total > prefixLen) {
      Breadcrumb crumb;
      crumb.startX = prefixLen;
      crumb.endX = std::min(width, total);
      crumb.path = dir;
      line.crumbs.push_back(crumb);
    }
  }
  return line;
}

bool hitTestBreadcrumb(const BreadcrumbLine& line, int x, int y, int lineY, std::filesystem::path* outPath) {
  int index = breadcrumbIndexAt(line, x, y, lineY);
  if (index < 0) return false;
  if (outPath) *outPath = line.crumbs[static_cast<size_t>(index)].path;
  return true;
}

int breadcrumbIndexAt(const BreadcrumbLine& line, int x, int y, int lineY) {
  if (y != lineY) return -1;
  for (size_t i = 0; i < line.crumbs.size(); ++i) {
    const auto& crumb = line.crumbs[i];
    if (x >= crumb.startX && x < crumb.endX) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool ConsoleScreen::init() {
  out_ = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out_ == INVALID_HANDLE_VALUE) return false;
  if (!GetConsoleMode(out_, &originalMode_)) return false;
  originalOutputCp_ = GetConsoleOutputCP();
  useUtf8Output_ = false;
  DWORD mode = originalMode_;
  mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT;
  SetConsoleMode(out_, mode);
  CONSOLE_CURSOR_INFO cursor{};
  if (GetConsoleCursorInfo(out_, &cursor)) {
    originalCursor_ = cursor;
    cursor.bVisible = FALSE;
    SetConsoleCursorInfo(out_, &cursor);
  }
  if (useUtf8Output_) {
    SetConsoleOutputCP(CP_UTF8);
  }
  std::wstring seq = L"\x1b[?1049h\x1b[H\x1b[?25l";
  if (!writeOutput(seq)) {
    reportWriteError(L"init");
    altScreen_ = false;
    hasPrev_ = false;
    updateSize();
    return true;
  }
  altScreen_ = true;
  hasPrev_ = false;
  updateSize();
  return true;
}

void ConsoleScreen::restore() {
  if (out_ != INVALID_HANDLE_VALUE) {
    if (altScreen_) {
      std::wstring seq = L"\x1b[0m\x1b[?25h\x1b[?1049l";
      if (!writeOutput(seq)) {
        reportWriteError(L"restore");
      }
      altScreen_ = false;
    }
    if (useUtf8Output_ && originalOutputCp_ != 0) {
      SetConsoleOutputCP(originalOutputCp_);
    }
    SetConsoleMode(out_, originalMode_);
    if (originalCursor_.dwSize != 0) {
      SetConsoleCursorInfo(out_, &originalCursor_);
    }
  }
}

void ConsoleScreen::updateSize() {
  int oldW = width_;
  int oldH = height_;
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(out_, &info)) {
    width_ = 80;
    height_ = 25;
  } else {
    width_ = info.srWindow.Right - info.srWindow.Left + 1;
    height_ = info.srWindow.Bottom - info.srWindow.Top + 1;
  }
  if (width_ < 1) width_ = 1;
  if (height_ < 1) height_ = 1;
  size_t needed = static_cast<size_t>(width_ * height_);
  bool dimsChanged = (width_ != oldW || height_ != oldH);
  if (dimsChanged || buffer_.size() != needed) {
    buffer_.assign(needed, {});
  }
  if (dimsChanged || prevBuffer_.size() != needed) {
    prevBuffer_.assign(needed, {});
    hasPrev_ = false;
  }
  if (dimsChanged) {
    outputFailed_ = false;
    outputErrorReported_ = false;
  }
}

int ConsoleScreen::width() const { return width_; }
int ConsoleScreen::height() const { return height_; }

void ConsoleScreen::clear(const Style& style) {
  Cell cell{};
  cell.ch = ' ';
  cell.fg = style.fg;
  cell.bg = style.bg;
  std::fill(buffer_.begin(), buffer_.end(), cell);
}

void ConsoleScreen::writeText(int x, int y, const std::string& text, const Style& style) {
  if (y < 0 || y >= height_) return;
  if (x >= width_) return;
  int pos = y * width_ + std::max(0, x);
  int start = std::max(0, x);
  int maxLen = width_ - start;
  std::wstring wide = utf8ToWide(text);
  int limit = std::min(maxLen, static_cast<int>(wide.size()));
  for (int i = 0; i < limit; ++i) {
    wchar_t ch = wide[static_cast<size_t>(i)];
    if (i == limit - 1 && ch >= 0xD800 && ch <= 0xDBFF) {
      break;
    }
    buffer_[pos + i].ch = ch;
    buffer_[pos + i].fg = style.fg;
    buffer_[pos + i].bg = style.bg;
  }
}

void ConsoleScreen::writeRun(int x, int y, int len, wchar_t ch, const Style& style) {
  if (y < 0 || y >= height_) return;
  if (x >= width_) return;
  int start = std::max(0, x);
  int maxLen = std::min(len, width_ - start);
  int pos = y * width_ + start;
  for (int i = 0; i < maxLen; ++i) {
    buffer_[pos + i].ch = ch;
    buffer_[pos + i].fg = style.fg;
    buffer_[pos + i].bg = style.bg;
  }
}

void ConsoleScreen::writeChar(int x, int y, wchar_t ch, const Style& style) {
  if (y < 0 || y >= height_) return;
  if (x < 0 || x >= width_) return;
  int pos = y * width_ + x;
  buffer_[pos].ch = ch;
  buffer_[pos].fg = style.fg;
  buffer_[pos].bg = style.bg;
}

void ConsoleScreen::setFastOutput(bool enabled) {
  if (fastOutput_ == enabled) return;
  fastOutput_ = enabled;
  hasPrev_ = false;
  outputFailed_ = false;
  outputErrorReported_ = false;
}

void ConsoleScreen::setAlwaysFullRedraw(bool enabled) {
  if (alwaysFullRedraw_ == enabled) return;
  alwaysFullRedraw_ = enabled;
  hasPrev_ = false;
}

bool ConsoleScreen::fastOutput() const { return fastOutput_; }

bool ConsoleScreen::snapshot(std::vector<ScreenCell>& outCells, int& outWidth,
                             int& outHeight) const {
  outWidth = width_;
  outHeight = height_;
  if (width_ <= 0 || height_ <= 0 || buffer_.empty()) {
    outCells.clear();
    return false;
  }
  outCells.resize(buffer_.size());
  for (size_t i = 0; i < buffer_.size(); ++i) {
    const Cell& src = buffer_[i];
    ScreenCell& dst = outCells[i];
    dst.ch = src.ch;
    dst.fg = src.fg;
    dst.bg = src.bg;
  }
  return true;
}

bool ConsoleScreen::writeOutput(const std::wstring& text) {
  if (out_ == INVALID_HANDLE_VALUE) return false;
  if (text.empty()) return true;
  if (outputFailed_) return false;
  if (useUtf8Output_) {
    if (!wideToUtf8(text, drawUtf8Buffer_)) {
      outputError_ = GetLastError();
      outputFailed_ = true;
      return false;
    }
    DWORD written = 0;
    if (!WriteFile(out_, drawUtf8Buffer_.data(),
                   static_cast<DWORD>(drawUtf8Buffer_.size()), &written,
                   nullptr)) {
      outputError_ = GetLastError();
      outputFailed_ = true;
      return false;
    }
    if (written != drawUtf8Buffer_.size()) {
      outputError_ = ERROR_WRITE_FAULT;
      outputFailed_ = true;
      return false;
    }
    return true;
  }
  DWORD written = 0;
  if (!WriteConsoleW(out_, text.c_str(), static_cast<DWORD>(text.size()),
                     &written, nullptr)) {
    outputError_ = GetLastError();
    outputFailed_ = true;
    return false;
  }
  if (written != text.size()) {
    outputError_ = ERROR_WRITE_FAULT;
    outputFailed_ = true;
    return false;
  }
  return true;
}

void ConsoleScreen::reportWriteError(const wchar_t* context) {
  if (!outputFailed_ || outputErrorReported_ || out_ == INVALID_HANDLE_VALUE) {
    return;
  }
  outputErrorReported_ = true;
  std::wstring msg;
  msg.append(L"\x1b[0m\r\n[console] output failed in ");
  msg.append(context ? context : L"draw");
  msg.append(L" (err=");
  appendNumber(msg, static_cast<int>(outputError_));
  msg.append(L")\r\n");
  DWORD written = 0;
  WriteConsoleW(out_, msg.c_str(), static_cast<DWORD>(msg.size()), &written,
                nullptr);
}

void ConsoleScreen::drawFast() {
  if (out_ == INVALID_HANDLE_VALUE) return;
  if (outputFailed_) {
    reportWriteError(L"drawFast");
    return;
  }
  size_t needed = static_cast<size_t>(width_ * height_);
  if (buffer_.size() < needed) {
    updateSize();
    if (buffer_.size() < needed) return;
  }
  if (fastBuffer_.size() != needed) {
    fastBuffer_.resize(needed);
  }
  for (size_t i = 0; i < needed; ++i) {
    const Cell& cell = buffer_[i];
    CHAR_INFO& outCell = fastBuffer_[i];
    outCell.Char.UnicodeChar = cell.ch ? cell.ch : L' ';
    uint8_t fg = consoleColorIndex(cell.fg);
    uint8_t bg = consoleColorIndex(cell.bg);
    outCell.Attributes = static_cast<WORD>(fg | (bg << 4));
  }
  COORD bufSize{static_cast<SHORT>(width_), static_cast<SHORT>(height_)};
  COORD bufPos{0, 0};
  SMALL_RECT region{0, 0, static_cast<SHORT>(width_ - 1),
                    static_cast<SHORT>(height_ - 1)};
  WriteConsoleOutputW(out_, fastBuffer_.data(), bufSize, bufPos, &region);
}

void ConsoleScreen::draw() {
  if (out_ == INVALID_HANDLE_VALUE) return;
  if (fastOutput_) {
    drawFast();
    return;
  }
  if (outputFailed_) {
    reportWriteError(L"draw");
    return;
  }
  size_t needed = static_cast<size_t>(width_ * height_);
  if (buffer_.size() < needed) {
    updateSize();
    if (buffer_.size() < needed) return;
  }
  if (prevBuffer_.size() != needed) {
    prevBuffer_.assign(needed, {});
    hasPrev_ = false;
  }
  bool fullRedraw = !hasPrev_ || alwaysFullRedraw_;
  std::wstring& out = drawBuffer_;
  out.clear();
  out.reserve(static_cast<size_t>(width_ * height_ * 2 + height_ * 32));

  auto sameCell = [](const Cell& a, const Cell& b) {
    return a.ch == b.ch && sameColor(a.fg, b.fg) && sameColor(a.bg, b.bg);
  };

  if (fullRedraw) {
    out.append(L"\x1b[H");
    Color curFg{};
    Color curBg{};
    bool hasColor = false;

    for (int y = 0; y < height_; ++y) {
      int rowStart = y * width_;
      for (int x = 0; x < width_; ++x) {
        const Cell& cell = buffer_[rowStart + x];
        bool fgDiff = !hasColor || !sameColor(cell.fg, curFg);
        bool bgDiff = !hasColor || !sameColor(cell.bg, curBg);
        if (fgDiff) {
          appendColorSeq(out, cell.fg, true);
        }
        if (bgDiff) {
          appendColorSeq(out, cell.bg, false);
        }
        if (fgDiff || bgDiff) {
          curFg = cell.fg;
          curBg = cell.bg;
          hasColor = true;
        }
        out.push_back(cell.ch ? cell.ch : L' ');
      }
      if (y < height_ - 1) {
        out.append(L"\r\n");
      }
    }
    out.append(L"\x1b[0m");
    if (!writeOutput(out)) {
      reportWriteError(L"draw");
      return;
    }
    prevBuffer_ = buffer_;
    hasPrev_ = true;
    return;
  }

  bool wrote = false;
  Color curFg{};
  Color curBg{};
  bool hasColor = false;
  for (int y = 0; y < height_; ++y) {
    size_t rowStart = static_cast<size_t>(y) * width_;
    int x = 0;
    while (x < width_) {
      while (x < width_ &&
             sameCell(buffer_[rowStart + x], prevBuffer_[rowStart + x])) {
        ++x;
      }
      if (x >= width_) break;
      int spanStart = x;
      while (x < width_ &&
             !sameCell(buffer_[rowStart + x], prevBuffer_[rowStart + x])) {
        ++x;
      }
      int spanEnd = x;
      appendCursorPos(out, spanStart + 1, y + 1);

      for (int i = spanStart; i < spanEnd; ++i) {
        const Cell& cell = buffer_[rowStart + i];
        bool fgDiff = !hasColor || !sameColor(cell.fg, curFg);
        bool bgDiff = !hasColor || !sameColor(cell.bg, curBg);
        if (fgDiff) {
          appendColorSeq(out, cell.fg, true);
        }
        if (bgDiff) {
          appendColorSeq(out, cell.bg, false);
        }
        if (fgDiff || bgDiff) {
          curFg = cell.fg;
          curBg = cell.bg;
          hasColor = true;
        }
        out.push_back(cell.ch ? cell.ch : L' ');
        prevBuffer_[rowStart + i] = cell;
      }
      wrote = true;
    }
  }
  if (wrote) {
    out.append(L"\x1b[0m");
    if (!writeOutput(out)) {
      reportWriteError(L"draw");
      return;
    }
  }
}
