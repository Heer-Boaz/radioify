#include "playback_media_metadata_catalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include "gmeaudio.h"
#include "media_formats.h"
#include "playback_track_catalog.h"
#include "ui_helpers.h"
#include "vgmaudio.h"
#include "video/ascii/asciiart.h"
#include "video/framebuffer/text_grid_bitmap_renderer.h"

namespace {

constexpr int kPosterCols = 34;
constexpr int kPosterRows = 18;
constexpr int kArtworkBitmapWidth = 680;
constexpr int kArtworkBitmapHeight = 680;
constexpr int kArtworkMaxCols = 48;
constexpr int kArtworkMaxRows = 48;

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

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string metadataValue(const AVDictionary* metadata, const char* key) {
  if (!metadata || !key || key[0] == '\0') {
    return {};
  }
  const AVDictionaryEntry* entry = av_dict_get(metadata, key, nullptr, 0);
  if (entry && entry->value && entry->value[0] != '\0') {
    return trimAscii(entry->value);
  }

  const std::string keyLower = lowercase(key);
  entry = nullptr;
  while ((entry = av_dict_get(metadata, "", entry, AV_DICT_IGNORE_SUFFIX))) {
    if (!entry->key || !entry->value || entry->value[0] == '\0') {
      continue;
    }
    if (lowercase(entry->key) == keyLower) {
      return trimAscii(entry->value);
    }
  }
  return {};
}

template <size_t N>
std::string firstMetadataValue(const AVDictionary* primary,
                               const AVDictionary* secondary,
                               const std::array<const char*, N>& keys) {
  for (const char* key : keys) {
    std::string value = metadataValue(primary, key);
    if (!value.empty()) {
      return value;
    }
    value = metadataValue(secondary, key);
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

uint32_t parseTrackNumber(const std::string& value) {
  if (value.empty()) {
    return 0;
  }
  uint32_t parsed = 0;
  bool haveDigit = false;
  for (unsigned char ch : value) {
    if (!std::isdigit(ch)) {
      break;
    }
    haveDigit = true;
    parsed = parsed * 10u + static_cast<uint32_t>(ch - '0');
  }
  return haveDigit ? parsed : 0u;
}

void applyTrackCatalogFallback(const std::filesystem::path& path, int trackIndex,
                               PlaybackMediaDisplayInfo* out) {
  if (!out || trackIndex < 0) {
    return;
  }

  std::vector<TrackEntry> tracks;
  std::string unusedError;
  if (!listPlaybackTracks(normalizePlaybackTrackPath(path), &tracks,
                          &unusedError)) {
    return;
  }

  const TrackEntry* track = findPlaybackTrack(tracks, trackIndex);
  if (!track) {
    return;
  }

  if (!track->title.empty()) {
    out->title = track->title;
  }
  out->trackNumber = static_cast<uint32_t>(trackIndex + 1);
}

void applyGmeMetadata(const std::filesystem::path& path, int trackIndex,
                      PlaybackMediaDisplayInfo* out) {
  if (!out) {
    return;
  }

  Music_Emu* emu = nullptr;
  if (gme_open_file(path.string().c_str(), &emu, gme_info_only) != nullptr ||
      !emu) {
    applyTrackCatalogFallback(path, trackIndex, out);
    return;
  }

  if (trackIndex < 0) {
    trackIndex = 0;
  }
  if (trackIndex >= gme_track_count(emu)) {
    trackIndex = std::max(0, gme_track_count(emu) - 1);
  }

  gme_info_t* info = nullptr;
  if (gme_track_info(emu, &info, trackIndex) == nullptr && info) {
    if (info->song && info->song[0] != '\0') {
      out->title = trimAscii(info->song);
    }
    if (info->author && info->author[0] != '\0') {
      out->artist = trimAscii(info->author);
    }
    if (info->game && info->game[0] != '\0') {
      out->albumTitle = trimAscii(info->game);
    }
    if (info->system && info->system[0] != '\0') {
      out->subtitle = trimAscii(info->system);
    }
    out->trackNumber = static_cast<uint32_t>(trackIndex + 1);
    gme_free_info(info);
  } else {
    applyTrackCatalogFallback(path, trackIndex, out);
  }

  gme_delete(emu);
}

void applyVgmMetadata(const std::filesystem::path& path,
                      PlaybackMediaDisplayInfo* out) {
  if (!out) {
    return;
  }

  std::vector<VgmMetadataEntry> metadata;
  std::string unusedError;
  if (!vgmReadMetadata(path, &metadata, &unusedError)) {
    applyTrackCatalogFallback(path, 0, out);
    return;
  }

  auto findMeta = [&](const char* key) -> std::string {
    for (const auto& entry : metadata) {
      if (entry.key == key) {
        return trimAscii(entry.value);
      }
    }
    return {};
  };

  if (out->title.empty()) {
    out->title = findMeta("Title");
  }
  if (out->title.empty()) {
    out->title = findMeta("Title (JPN)");
  }
  if (out->albumTitle.empty()) {
    out->albumTitle = findMeta("Game");
  }
  if (out->albumTitle.empty()) {
    out->albumTitle = findMeta("Game (JPN)");
  }
  if (out->subtitle.empty()) {
    out->subtitle = findMeta("System");
  }
  if (out->subtitle.empty()) {
    out->subtitle = findMeta("System (JPN)");
  }
  if (out->artist.empty()) {
    out->artist = findMeta("Artist");
  }
  if (out->artist.empty()) {
    out->artist = findMeta("Artist (JPN)");
  }
}

void applyEmulatedMetadata(const PlaybackMediaDisplayRequest& request,
                           PlaybackMediaDisplayInfo* out) {
  if (!out) {
    return;
  }

  if (isGmeExt(request.file)) {
    applyGmeMetadata(request.file, request.trackIndex, out);
    return;
  }
  if (isVgmExt(request.file)) {
    applyVgmMetadata(request.file, out);
    return;
  }
  applyTrackCatalogFallback(request.file, request.trackIndex, out);
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
                                       PlaybackMediaArtwork* out) {
  if (!out) {
    return false;
  }

  AsciiArt art;
  std::string unusedError;
  if (!renderAsciiArt(file, kArtworkMaxCols, kArtworkMaxRows, art,
                      &unusedError)) {
    return false;
  }
  return buildBitmapArtworkFromAscii(art, out);
}

bool tryBuildAsciiArtworkFromEncodedBytes(const uint8_t* bytes, size_t size,
                                          PlaybackMediaArtwork* out) {
  if (!out) {
    return false;
  }

  AsciiArt art;
  std::string unusedError;
  if (!renderAsciiArtFromEncodedImageBytes(bytes, size, kArtworkMaxCols,
                                           kArtworkMaxRows, art,
                                           &unusedError)) {
    return false;
  }
  return buildBitmapArtworkFromAscii(art, out);
}

bool tryResolveSidecarArtwork(const std::filesystem::path& file,
                              PlaybackMediaArtwork* out) {
  if (!out) {
    return false;
  }

  const std::filesystem::path dir = file.parent_path();
  if (dir.empty()) {
    return false;
  }

  const std::string stem = lowercase(file.stem().string());
  static constexpr std::array<const char*, 4> exts = {
      ".jpg", ".jpeg", ".png", ".bmp"};
  static constexpr std::array<const char*, 4> names = {
      "cover", "folder", "front", "album"};

  std::vector<std::filesystem::path> candidates;
  candidates.reserve(exts.size() * (names.size() + 1));
  for (const char* name : names) {
    for (const char* ext : exts) {
      candidates.push_back(dir / (std::string(name) + ext));
    }
  }
  if (!stem.empty()) {
    for (const char* ext : exts) {
      candidates.push_back(dir / (stem + ext));
    }
  }

  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
      continue;
    }
    if (tryBuildAsciiArtworkFromImageFile(candidate, out)) {
      return true;
    }
  }
  return false;
}

bool tryResolveEmbeddedArtwork(AVFormatContext* fmt, PlaybackMediaArtwork* out) {
  if (!fmt || !out) {
    return false;
  }

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
            pic.data, static_cast<size_t>(pic.size), out)) {
      return true;
    }
  }
  return false;
}

bool tryResolveTaggedArtwork(const PlaybackMediaDisplayRequest& request,
                             PlaybackMediaDisplayInfo* out,
                             std::string* error) {
  if (!out) {
    return false;
  }

  AVFormatContext* fmt = nullptr;
  const int openErr =
      avformat_open_input(&fmt, request.file.string().c_str(), nullptr, nullptr);
  if (openErr < 0) {
    setError(error, "Failed to open media metadata: " + ffmpegError(openErr));
    return false;
  }

  const int infoErr = avformat_find_stream_info(fmt, nullptr);
  if (infoErr < 0) {
    avformat_close_input(&fmt);
    setError(error, "Failed to read media metadata: " + ffmpegError(infoErr));
    return false;
  }

  int bestStream = -1;
  if (request.isVideo) {
    bestStream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr,
                                     0);
  } else {
    bestStream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr,
                                     0);
  }

  const AVDictionary* formatMetadata = fmt->metadata;
  const AVDictionary* streamMetadata = nullptr;
  if (bestStream >= 0 && bestStream < static_cast<int>(fmt->nb_streams) &&
      fmt->streams[bestStream]) {
    streamMetadata = fmt->streams[bestStream]->metadata;
  }

  const std::string title = firstMetadataValue(
      formatMetadata, streamMetadata, std::array<const char*, 2>{"title", "TITLE"});
  if (!title.empty()) {
    out->title = title;
  }

  const std::string artist =
      firstMetadataValue(formatMetadata, streamMetadata,
                         std::array<const char*, 4>{"artist", "composer",
                                                     "performer", "ARTIST"});
  if (!artist.empty()) {
    out->artist = artist;
  }

  const std::string album =
      firstMetadataValue(formatMetadata, streamMetadata,
                         std::array<const char*, 3>{"album", "show", "ALBUM"});
  if (!album.empty()) {
    out->albumTitle = album;
  }

  const std::string albumArtist =
      firstMetadataValue(formatMetadata, streamMetadata,
                         std::array<const char*, 3>{"album_artist",
                                                     "album artist",
                                                     "ALBUMARTIST"});
  if (!albumArtist.empty()) {
    out->albumArtist = albumArtist;
  }

  const std::string subtitle =
      firstMetadataValue(formatMetadata, streamMetadata,
                         std::array<const char*, 4>{"comment", "description",
                                                     "synopsis", "genre"});
  if (!subtitle.empty()) {
    out->subtitle = subtitle;
  }

  const std::string track =
      firstMetadataValue(formatMetadata, streamMetadata,
                         std::array<const char*, 4>{"track", "tracknumber",
                                                     "TRACKNUMBER", "TRACK"});
  if (!track.empty()) {
    out->trackNumber = parseTrackNumber(track);
  }

  if (!tryResolveEmbeddedArtwork(fmt, &out->artwork)) {
    tryResolveSidecarArtwork(request.file, &out->artwork);
  }

  avformat_close_input(&fmt);
  return true;
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

void fillPosterRect(std::vector<ScreenCell>* cells, int x0, int y0,
                    int x1, int y1, const Color& bg) {
  if (!cells) {
    return;
  }
  for (int y = std::max(0, y0); y < std::min(kPosterRows, y1); ++y) {
    for (int x = std::max(0, x0); x < std::min(kPosterCols, x1); ++x) {
      auto& cell = (*cells)[static_cast<size_t>(y) * kPosterCols +
                            static_cast<size_t>(x)];
      cell.bg = bg;
    }
  }
}

void putPosterChar(std::vector<ScreenCell>* cells, int x, int y,
                   wchar_t ch, const Color& fg, const Color& bg) {
  if (!cells || x < 0 || x >= kPosterCols || y < 0 || y >= kPosterRows) {
    return;
  }
  auto& cell = (*cells)[static_cast<size_t>(y) * kPosterCols +
                        static_cast<size_t>(x)];
  cell.ch = ch;
  cell.fg = fg;
  cell.bg = bg;
}

void putPosterText(std::vector<ScreenCell>* cells, int x, int y,
                   const std::string& text, const Color& fg, const Color& bg) {
  const std::wstring wide = utf8ToWideLossy(text);
  int col = x;
  for (wchar_t ch : wide) {
    if (col >= kPosterCols) {
      break;
    }
    putPosterChar(cells, col++, y, ch, fg, bg);
  }
}

std::vector<std::string> wrapPosterLines(const std::string& text,
                                         int width,
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
    for (int i = 0; i < width && i < utf8CodepointCount(remaining); ++i) {
      const std::string slice = utf8Slice(remaining, i, 1);
      if (slice == " ") {
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
                    std::max(0, utf8CodepointCount(remaining) - cut)));
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
                             PlaybackMediaArtwork* outArtwork) {
  if (!outArtwork) {
    return false;
  }

  const Color background{10, 16, 28};
  const Color panel{18, 28, 44};
  const Color accent{255, 214, 120};
  const Color soft{216, 227, 241};
  const Color dim{124, 150, 178};

  std::vector<ScreenCell> cells(
      static_cast<size_t>(kPosterCols) * static_cast<size_t>(kPosterRows));
  fillPosterRect(&cells, 0, 0, kPosterCols, kPosterRows, background);
  fillPosterRect(&cells, 1, 1, kPosterCols - 1, kPosterRows - 1, panel);

  for (int x = 1; x < kPosterCols - 1; ++x) {
    putPosterChar(&cells, x, 1, L'-', accent, panel);
    putPosterChar(&cells, x, kPosterRows - 2, L'-', accent, panel);
  }
  for (int y = 1; y < kPosterRows - 1; ++y) {
    putPosterChar(&cells, 1, y, L'|', accent, panel);
    putPosterChar(&cells, kPosterCols - 2, y, L'|', accent, panel);
  }
  putPosterChar(&cells, 1, 1, L'+', accent, panel);
  putPosterChar(&cells, kPosterCols - 2, 1, L'+', accent, panel);
  putPosterChar(&cells, 1, kPosterRows - 2, L'+', accent, panel);
  putPosterChar(&cells, kPosterCols - 2, kPosterRows - 2, L'+', accent, panel);

  putPosterText(&cells, 4, 3, "RADIOIFY", accent, panel);

  const int textWidth = kPosterCols - 8;
  int row = 5;
  for (const auto& line :
       wrapPosterLines(info.title.empty() ? fallbackMediaTitle(request.file)
                                          : info.title,
                       textWidth, 4)) {
    putPosterText(&cells, 4, row++, line, soft, panel);
  }
  row += 1;
  for (const auto& line : wrapPosterLines(info.artist, textWidth, 2)) {
    putPosterText(&cells, 4, row++, line, accent, panel);
  }
  for (const auto& line :
       wrapPosterLines(info.albumTitle.empty() ? info.subtitle : info.albumTitle,
                       textWidth, 2)) {
    putPosterText(&cells, 4, row++, line, dim, panel);
  }

  putPosterText(&cells, 4, kPosterRows - 4,
                posterFooterLabel(request, info), dim, panel);

  std::vector<uint8_t> pixels;
  if (!renderScreenGridToBitmap(cells.data(), kPosterCols, kPosterRows,
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

}  // namespace

bool resolvePlaybackMediaDisplayInfo(const PlaybackMediaDisplayRequest& request,
                                     PlaybackMediaDisplayInfo* out,
                                     std::string* error) {
  if (!out) {
    setError(error, "Playback media metadata output was null.");
    return false;
  }

  *out = PlaybackMediaDisplayInfo{};
  out->title = fallbackMediaTitle(request.file);

  std::string metadataError;
  const bool isEmulated = isGmeExt(request.file) || isGsfExt(request.file) ||
                          isVgmExt(request.file) || isKssExt(request.file) ||
                          isPsfExt(request.file);

  if (isEmulated) {
    applyEmulatedMetadata(request, out);
  } else {
    tryResolveTaggedArtwork(request, out, &metadataError);
  }

  if (out->title.empty()) {
    out->title = fallbackMediaTitle(request.file);
  }
  if (request.trackIndex >= 0 && out->trackNumber == 0) {
    out->trackNumber = static_cast<uint32_t>(request.trackIndex + 1);
  }
  if (out->subtitle.empty() && !out->albumTitle.empty() && request.isVideo) {
    out->subtitle = out->albumTitle;
  }
  if (out->subtitle.empty() && !out->artist.empty() && request.isVideo) {
    out->subtitle = out->artist;
  }

  if (out->artwork.kind == PlaybackMediaArtwork::Kind::None) {
    buildAsciiPosterArtwork(request, *out, &out->artwork);
  }

  if (!metadataError.empty() && error) {
    *error = metadataError;
  }
  return true;
}
