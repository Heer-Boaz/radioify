#include "playback_subtitles.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace {

struct EmbeddedSubtitleCue {
  int64_t startUs = 0;
  int64_t endUs = 0;
  std::string text;
};

constexpr int64_t kUsPerMs = 1000;
constexpr int64_t kDefaultCueDurationUs = 2'000'000;
constexpr int64_t kMinValidTimestampUs = 0;

std::string trimLeft(std::string_view s) {
  size_t p = 0;
  while (p < s.size() &&
         std::isspace(static_cast<unsigned char>(s[p])) != 0) {
    ++p;
  }
  return std::string(s.substr(p));
}

std::string trimRight(std::string_view s) {
  if (s.empty()) return std::string();
  size_t p = s.size();
  while (p > 0 &&
         std::isspace(static_cast<unsigned char>(s[p - 1])) != 0) {
    --p;
  }
  return std::string(s.substr(0, p));
}

std::string trim(std::string_view s) {
  return trimRight(trimLeft(s));
}

bool startsWithIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() < b.size()) return false;
  for (size_t i = 0; i < b.size(); ++i) {
    char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
    char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
    if (ca != cb) return false;
  }
  return true;
}

std::string toLowerAscii(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

std::string normalizeLanguageLabel(const std::string& language) {
  if (language.empty()) return "Default";
  std::string label;
  label.reserve(language.size());
  for (char ch : language) {
    if (ch == '_') ch = '-';
    label.push_back(ch);
  }
  for (char& ch : label) {
    if (ch >= 'a' && ch <= 'z') {
      ch = static_cast<char>(ch - 'a' + 'A');
    }
  }
  return label;
}

std::string subtitleOptionLabel(const playback_subtitles::SubtitleOption& option,
                                size_t index) {
  std::string label;
  if (!option.language.empty()) {
    label = normalizeLanguageLabel(option.language);
  }
  if (label.empty() && !option.isEmbedded) {
    std::string stem = option.path.stem().string();
    if (!stem.empty()) {
      label = stem;
    }
  }
  if (label.empty()) {
    label = "Track " + std::to_string(index + 1);
  }
  return label;
}

std::string formatSrtTimestamp(int64_t timeUs) {
  int64_t safeUs = std::max<int64_t>(kMinValidTimestampUs, timeUs);
  int64_t totalMs = safeUs / kUsPerMs;
  int64_t ms = totalMs % 1000;
  int64_t sec = (totalMs / 1000) % 60;
  int64_t min = (totalMs / 60000) % 60;
  int64_t h = totalMs / 3600000;
  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << h << ":" << std::setw(2) << min
      << ":" << std::setw(2) << sec << "," << std::setw(3) << ms;
  return out.str();
}

std::string stripEmbeddedText(std::string_view raw) {
  std::string_view source = raw;
  while (!source.empty() &&
         std::isspace(static_cast<unsigned char>(source.front())) != 0) {
    source.remove_prefix(1);
  }
  if (startsWithIgnoreCase(source, "dialogue:")) {
    int commaCount = 0;
    for (size_t i = 0; i < source.size(); ++i) {
      if (source[i] != ',') continue;
      ++commaCount;
      if (commaCount >= 9) {
        source = source.substr(i + 1);
        break;
      }
    }
  }

  std::string out;
  out.reserve(source.size());
  bool inAssTag = false;
  bool inHtmlTag = false;
  for (size_t i = 0; i < source.size(); ++i) {
    char c = source[i];
    if (inAssTag) {
      if (c == '}') inAssTag = false;
      continue;
    }
    if (inHtmlTag) {
      if (c == '>') inHtmlTag = false;
      continue;
    }

    if (c == '{') {
      inAssTag = true;
      continue;
    }
    if (c == '<') {
      inHtmlTag = true;
      continue;
    }
    if (c == '\\' && i + 1 < source.size() &&
        (source[i + 1] == 'N' || source[i + 1] == 'n')) {
      out.push_back('\n');
      ++i;
      continue;
    }
    if (c == '\\' && i + 1 < source.size() &&
        (source[i + 1] == 'h' || source[i + 1] == 'H')) {
      out.push_back(' ');
      ++i;
      continue;
    }
    if (c == '\r' || c == '\t') continue;
    out.push_back(c);
  }

  std::string merged;
  std::istringstream lines(out);
  std::string line;
  bool firstLine = true;
  while (std::getline(lines, line)) {
    line = trim(line);
    if (line.empty()) continue;
    if (!firstLine) merged.push_back('\n');
    merged += line;
    firstLine = false;
  }
  return merged;
}

void fixEmbeddedCueOrdering(std::vector<EmbeddedSubtitleCue>& cues) {
  if (cues.empty()) return;
  std::sort(cues.begin(), cues.end(),
            [](const EmbeddedSubtitleCue& a, const EmbeddedSubtitleCue& b) {
              if (a.startUs != b.startUs) return a.startUs < b.startUs;
              if (a.endUs != b.endUs) return a.endUs < b.endUs;
              return a.text < b.text;
            });
  for (size_t i = 0; i < cues.size(); ++i) {
    if (cues[i].startUs < 0) cues[i].startUs = 0;
    if (cues[i].endUs <= cues[i].startUs) {
      cues[i].endUs = cues[i].startUs + kDefaultCueDurationUs;
    }
    if (i + 1 < cues.size() && cues[i + 1].startUs > cues[i].startUs &&
        cues[i + 1].startUs < cues[i].endUs) {
      cues[i].endUs = cues[i + 1].startUs;
    }
    if (cues[i].startUs == cues[i].endUs) {
      cues[i].endUs = cues[i].startUs + kDefaultCueDurationUs;
    }
  }
}

std::string streamLanguage(const AVStream* stream) {
  if (!stream || !stream->metadata) return {};
  if (const AVDictionaryEntry* lang =
          av_dict_get(stream->metadata, "language", nullptr, 0)) {
    if (lang->value && lang->value[0] != '\0') return lang->value;
  }
  return {};
}

int64_t streamTimestampUs(const AVPacket& packet, const AVRational& tb) {
  int64_t pts = packet.pts;
  if (pts == AV_NOPTS_VALUE) pts = packet.dts;
  if (pts == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
  return av_rescale_q(pts, tb, AVRational{1, 1000000});
}

bool writeEmbeddedCuesToSrt(const std::vector<EmbeddedSubtitleCue>& cues,
                            const std::filesystem::path& path,
                            std::string* error) {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    if (error) *error = "Failed to write temporary subtitle file: " + path.string();
    return false;
  }
  for (size_t i = 0; i < cues.size(); ++i) {
    const auto& cue = cues[i];
    out << (i + 1) << "\n";
    out << formatSrtTimestamp(cue.startUs) << " --> "
        << formatSrtTimestamp(cue.endUs) << "\n";
    out << cue.text << "\n\n";
  }
  if (!out.good()) {
    if (error) *error = "Failed to write temporary subtitle file: " + path.string();
    return false;
  }
  return true;
}

bool appendEmbeddedCuesFromSubtitle(const AVSubtitle& sub, int64_t fallbackBaseUs,
                                   const AVRational& streamTimeBase,
                                   std::vector<EmbeddedSubtitleCue>& out) {
  (void)streamTimeBase;
  int64_t baseUs = AV_NOPTS_VALUE;
  if (sub.pts != AV_NOPTS_VALUE) {
    baseUs =
        av_rescale_q(sub.pts, AVRational{1, AV_TIME_BASE}, AVRational{1, 1000000});
  }
  if (baseUs == AV_NOPTS_VALUE) {
    baseUs = fallbackBaseUs;
  }
  if (baseUs == AV_NOPTS_VALUE) return false;

  int64_t startMs = sub.start_display_time;
  int64_t endMs = sub.end_display_time;
  if (startMs < 0) startMs = 0;
  if (endMs <= startMs) {
    endMs = startMs + (kDefaultCueDurationUs / kUsPerMs);
  }

  bool added = false;
  for (unsigned i = 0; i < sub.num_rects; ++i) {
    AVSubtitleRect* rect = sub.rects[i];
    if (!rect) continue;
    const char* raw = nullptr;
    if (rect->type == SUBTITLE_ASS && rect->ass) {
      raw = rect->ass;
    } else if (rect->type == SUBTITLE_TEXT && rect->text) {
      raw = rect->text;
    }
    if (!raw) continue;
    std::string text = stripEmbeddedText(raw);
    if (text.empty()) continue;
    EmbeddedSubtitleCue cue;
    cue.startUs = baseUs + startMs * kUsPerMs;
    cue.endUs = baseUs + endMs * kUsPerMs;
    if (cue.endUs <= cue.startUs) {
      cue.endUs = cue.startUs + kDefaultCueDurationUs;
    }
    cue.text = std::move(text);
    out.push_back(std::move(cue));
    added = true;
  }
  return added;
}

bool decodeEmbeddedSubtitleStream(AVFormatContext* format, AVStream* stream,
                                 std::vector<EmbeddedSubtitleCue>& out,
                                 std::string* error) {
  if (!format || !stream || !stream->codecpar) return false;
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) {
    if (error) *error = "Unsupported embedded subtitle codec.";
    return false;
  }

  AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
  if (!codecCtx) {
    if (error) *error = "Failed to allocate embedded subtitle decoder.";
    return false;
  }
  if (avcodec_parameters_to_context(codecCtx, stream->codecpar) < 0) {
    avcodec_free_context(&codecCtx);
    if (error) *error = "Failed to initialize embedded subtitle decoder.";
    return false;
  }
  if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
    avcodec_free_context(&codecCtx);
    if (error) *error = "Failed to open embedded subtitle decoder.";
    return false;
  }

  AVPacket* packet = av_packet_alloc();
  if (!packet) {
    avcodec_free_context(&codecCtx);
    if (error) *error = "Failed to allocate packet for subtitle decode.";
    return false;
  }

  const int streamIndex = stream->index;
  while (av_read_frame(format, packet) >= 0) {
    if (packet->stream_index != streamIndex) {
      av_packet_unref(packet);
      continue;
    }
    int got = 0;
    AVSubtitle sub{};
    int64_t fallbackUs = streamTimestampUs(*packet, stream->time_base);
    int decodeRes = avcodec_decode_subtitle2(codecCtx, &sub, &got, packet);
    if (decodeRes >= 0 && got == 1) {
      appendEmbeddedCuesFromSubtitle(sub, fallbackUs, stream->time_base, out);
    }
    avsubtitle_free(&sub);
    av_packet_unref(packet);
  }

  // Flush decoder.
  // Some FFmpeg builds dereference the packet pointer unconditionally in
  // avcodec_decode_subtitle2(), so do not pass nullptr here.
  AVPacket flushPacket{};
  flushPacket.data = nullptr;
  flushPacket.size = 0;
  flushPacket.pts = AV_NOPTS_VALUE;
  flushPacket.dts = AV_NOPTS_VALUE;
  while (true) {
    AVSubtitle sub{};
    int got = 0;
    int decodeRes =
        avcodec_decode_subtitle2(codecCtx, &sub, &got, &flushPacket);
    if (decodeRes < 0 || got == 0) {
      avsubtitle_free(&sub);
      break;
    }
    appendEmbeddedCuesFromSubtitle(sub, AV_NOPTS_VALUE, stream->time_base, out);
    avsubtitle_free(&sub);
  }

  av_packet_free(&packet);
  avcodec_close(codecCtx);
  avcodec_free_context(&codecCtx);
  return !out.empty();
}

bool collectFileSubtitleCandidates(const std::filesystem::path& videoPath,
                                 std::vector<playback_subtitles::SubtitleOption>& out) {
  std::error_code ec;
  std::filesystem::path baseDir = videoPath.parent_path();
  if (baseDir.empty()) baseDir = ".";
  std::string videoStem = videoPath.stem().string();
  if (!std::filesystem::exists(baseDir, ec) ||
      !std::filesystem::is_directory(baseDir, ec)) {
    return false;
  }

  const char* extes[] = {".srt", ".vtt"};
  for (const auto& entry : std::filesystem::directory_iterator(baseDir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec) || ec) continue;
    std::filesystem::path p = entry.path();
    std::string ext = toLowerAscii(p.extension().string());
    bool supported = false;
    for (const char* e : extes) {
      if (ext == e) {
        supported = true;
        break;
      }
    }
    if (!supported) continue;
    std::string stem = p.stem().string();
    if (stem.size() > videoStem.size() &&
        stem[videoStem.size()] != '.' &&
        stem[videoStem.size()] != '-') {
      continue;
    }
    if (!startsWithIgnoreCase(stem, videoStem)) continue;
    playback_subtitles::SubtitleOption option;
    option.path = p;
    if (stem.size() > videoStem.size()) {
      option.language = stem.substr(videoStem.size() + 1);
    }
    out.push_back(std::move(option));
  }
  return !out.empty();
}

}  // namespace

namespace playback_subtitles {

bool SubtitleManager::load(const std::filesystem::path& videoPath,
                          std::string* error) {
  options_.clear();
  activeIndex_.store(-1, std::memory_order_relaxed);
  if (error) {
    error->clear();
  }

  std::vector<SubtitleOption> discovered;
  collectFileSubtitleCandidates(videoPath, discovered);

  std::string embeddedError;
  if (!loadEmbeddedTracks(videoPath, discovered, &embeddedError)) {
    if (discovered.empty() && !embeddedError.empty() && error) {
      *error = embeddedError;
    }
  }

  std::string lastLoadError;
  bool anyLoaded = false;
  for (auto& option : discovered) {
    if (option.loaded && !option.track.empty()) {
      options_.push_back(std::move(option));
      anyLoaded = true;
      continue;
    }
    SubtitleTrack track;
    std::string loadError;
    if (!track.loadFromFile(option.path, &loadError)) {
      if (loadError.empty()) {
        continue;
      }
      if (!loadError.empty()) {
        lastLoadError = std::move(loadError);
      }
      continue;
    }
    if (track.empty()) continue;
    option.track = std::move(track);
    option.loaded = true;
    options_.push_back(std::move(option));
    anyLoaded = true;
  }

  if (!options_.empty()) {
    std::sort(options_.begin(), options_.end(),
              [](const SubtitleOption& a, const SubtitleOption& b) {
                if (a.isEmbedded != b.isEmbedded) return a.isEmbedded;
                if (a.isDefault != b.isDefault) return a.isDefault;
                bool aDefault = a.language.empty();
                bool bDefault = b.language.empty();
                if (aDefault != bDefault) return aDefault;
                std::string aLang = toLowerAscii(a.language);
                std::string bLang = toLowerAscii(b.language);
                if (aLang == bLang) {
                  return toLowerAscii(a.path.filename().string()) <
                         toLowerAscii(b.path.filename().string());
                }
                return aLang < bLang;
              });
    activeIndex_.store(0, std::memory_order_relaxed);
  }

  if (!anyLoaded) {
    if (error) {
      if (!lastLoadError.empty()) {
        *error = lastLoadError;
      } else if (!embeddedError.empty()) {
        *error = embeddedError;
      } else {
        *error = "No matching subtitle files found.";
      }
    }
    return false;
  }

  return true;
}

bool SubtitleManager::hasSubtitles() const {
  return !options_.empty();
}

const std::vector<SubtitleOption>& SubtitleManager::tracks() const {
  return options_;
}

const SubtitleTrack* SubtitleManager::activeTrack() const {
  int activeIndex = activeIndex_.load(std::memory_order_relaxed);
  if (activeIndex < 0 || activeIndex >= static_cast<int>(options_.size())) {
    return nullptr;
  }
  return &options_[static_cast<size_t>(activeIndex)].track;
}

const SubtitleOption* SubtitleManager::activeOption() const {
  int activeIndex = activeIndex_.load(std::memory_order_relaxed);
  if (activeIndex < 0 || activeIndex >= static_cast<int>(options_.size())) {
    return nullptr;
  }
  return &options_[static_cast<size_t>(activeIndex)];
}

size_t SubtitleManager::trackCount() const {
  return options_.size();
}

bool SubtitleManager::canCycle() const {
  return trackCount() > 1;
}

size_t SubtitleManager::activeTrackIndex() const {
  int activeIndex = activeIndex_.load(std::memory_order_relaxed);
  if (activeIndex < 0 || activeIndex >= static_cast<int>(options_.size())) {
    return static_cast<size_t>(-1);
  }
  return static_cast<size_t>(activeIndex);
}

bool SubtitleManager::cycleLanguage() {
  if (!canCycle()) return false;
  int activeIndex = activeIndex_.load(std::memory_order_relaxed);
  if (activeIndex < 0 || activeIndex >= static_cast<int>(options_.size())) {
    activeIndex_.store(0, std::memory_order_relaxed);
    return true;
  }
  activeIndex = (activeIndex + 1) % static_cast<int>(options_.size());
  activeIndex_.store(activeIndex, std::memory_order_relaxed);
  return true;
}

std::string SubtitleManager::activeTrackLabel() const {
  size_t activeIndex = activeTrackIndex();
  if (activeIndex == static_cast<size_t>(-1) || activeIndex >= options_.size()) {
    return "N/A";
  }
  return subtitleOptionLabel(options_[activeIndex], activeIndex);
}

std::string SubtitleManager::activeLanguageLabel() const {
  return activeTrackLabel();
}

bool SubtitleManager::loadEmbeddedTracks(const std::filesystem::path& videoPath,
                                        std::vector<SubtitleOption>& out,
                                        std::string* error) const {
  AVFormatContext* format = nullptr;
  if (avformat_open_input(&format, videoPath.string().c_str(), nullptr, nullptr) <
      0) {
    if (error) {
      *error = "No matching subtitle files found.";
    }
    return false;
  }

  bool loadedAny = false;
  std::string lastEmbeddedError;
  if (avformat_find_stream_info(format, nullptr) < 0) {
    if (error) {
      *error = "No matching subtitle streams found.";
    }
    avformat_close_input(&format);
    return false;
  }

  for (unsigned i = 0; i < format->nb_streams; ++i) {
    AVStream* stream = format->streams[i];
    if (!stream || !stream->codecpar) continue;
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;

    int64_t rewindTarget =
        (format->start_time != AV_NOPTS_VALUE) ? format->start_time : 0;
    if (avformat_seek_file(format, -1, INT64_MIN, rewindTarget, INT64_MAX,
                           AVSEEK_FLAG_BACKWARD) < 0) {
      av_seek_frame(format, -1, 0, AVSEEK_FLAG_BACKWARD);
    }
    avformat_flush(format);

    std::vector<EmbeddedSubtitleCue> cues;
    std::string decodeError;
    if (!decodeEmbeddedSubtitleStream(format, stream, cues, &decodeError)) {
      if (!decodeError.empty()) {
        lastEmbeddedError = std::move(decodeError);
      }
      continue;
    }
    fixEmbeddedCueOrdering(cues);
    if (cues.empty()) continue;

    std::string baseName = videoPath.stem().string();
    if (baseName.empty()) baseName = "radioify";
    auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();
    std::filesystem::path tempPath =
        std::filesystem::temp_directory_path() / (baseName + "_" + std::to_string(stamp) +
                                                  "_" + std::to_string(i) +
                                                  ".srt");
    std::string writeError;
    if (!writeEmbeddedCuesToSrt(cues, tempPath, &writeError)) {
      if (!writeError.empty()) {
        lastEmbeddedError = std::move(writeError);
      }
      continue;
    }

    SubtitleTrack track;
    if (!track.loadFromFile(tempPath, &decodeError)) {
      std::filesystem::remove(tempPath);
      if (!decodeError.empty()) {
        lastEmbeddedError = std::move(decodeError);
      }
      continue;
    }
    std::filesystem::remove(tempPath);
    if (track.empty()) continue;

    SubtitleOption option;
    option.path = videoPath;
    option.language = streamLanguage(stream);
    if (option.language.empty()) {
      option.language = std::string("track_") + std::to_string(i);
    }
    option.isDefault = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
    option.isEmbedded = true;
    option.track = std::move(track);
    option.loaded = true;
    out.push_back(std::move(option));
    loadedAny = true;
  }
  avformat_close_input(&format);
  if (loadedAny) {
    if (error) error->clear();
    return true;
  }
  if (!lastEmbeddedError.empty() && error) {
    *error = lastEmbeddedError;
  }
  return false;
}

}  // namespace playback_subtitles
