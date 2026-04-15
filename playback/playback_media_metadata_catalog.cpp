#include "playback_media_metadata_catalog.h"
#include "playback_media_artwork_catalog.h"

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
#include "runtime_helpers.h"
#include "ui_helpers.h"
#include "vgmaudio.h"

namespace {

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
  const std::string pathUtf8 = toUtf8String(path);
  if (gme_open_file(pathUtf8.c_str(), &emu, gme_info_only) != nullptr ||
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

bool tryResolveTaggedMetadata(const PlaybackMediaDisplayRequest& request,
                              PlaybackMediaDisplayInfo* out,
                              std::string* error) {
  if (!out) {
    return false;
  }

  AVFormatContext* fmt = nullptr;
  const std::string pathUtf8 = toUtf8String(request.file);
  const int openErr =
      avformat_open_input(&fmt, pathUtf8.c_str(), nullptr, nullptr);
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

  avformat_close_input(&fmt);
  return true;
}

}  // namespace

bool resolvePlaybackMediaDisplayInfo(const PlaybackMediaDisplayRequest& request,
                                     const PlaybackMediaDisplayResolveOptions& options,
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
    tryResolveTaggedMetadata(request, out, &metadataError);
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

  if (options.includeArtwork) {
    resolvePlaybackMediaArtworkBitmap(request, *out, &out->artwork, nullptr);
  }

  if (!metadataError.empty() && error) {
    *error = metadataError;
  }
  return true;
}
