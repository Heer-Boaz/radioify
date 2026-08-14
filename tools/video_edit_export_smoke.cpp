#include "playback/video/edit/export.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/runtime_helpers.h"

namespace {

bool parseRange(const std::wstring& text,
                playback_video_edit::SourceRange* range) {
  if (!range) return false;
  const size_t separator = text.find(L':');
  if (separator == std::wstring::npos) return false;
  try {
    range->startUs = std::stoll(text.substr(0, separator));
    range->endUs = std::stoll(text.substr(separator + 1));
  } catch (...) {
    return false;
  }
  return range->startUs >= 0 && range->endUs > range->startUs;
}

bool probeOutput(const std::filesystem::path& path, int64_t expectedDurationUs,
                 bool expectedAudio, std::string* error) {
  AVFormatContext* format = nullptr;
  const std::string pathUtf8 = toUtf8String(path);
  int result = avformat_open_input(&format, pathUtf8.c_str(), nullptr, nullptr);
  if (result >= 0) result = avformat_find_stream_info(format, nullptr);
  bool video = false;
  bool audio = false;
  if (result >= 0) {
    for (unsigned index = 0; index < format->nb_streams; ++index) {
      const AVMediaType type = format->streams[index]->codecpar->codec_type;
      video = video || type == AVMEDIA_TYPE_VIDEO;
      audio = audio || type == AVMEDIA_TYPE_AUDIO;
    }
  }
  const int64_t durationUs = format ? format->duration : 0;
  avformat_close_input(&format);
  if (result < 0) {
    if (error) *error = "exported MP4 could not be probed";
    return false;
  }
  if (!video || durationUs <= 0) {
    if (error) *error = "exported MP4 has no playable video timeline";
    return false;
  }
  if (audio != expectedAudio) {
    if (error) {
      *error = expectedAudio ? "exported MP4 lost the selected audio stream"
                             : "exported MP4 unexpectedly gained audio";
    }
    return false;
  }
  const int64_t durationDifference =
      durationUs >= expectedDurationUs ? durationUs - expectedDurationUs
                                       : expectedDurationUs - durationUs;
  if (durationDifference > 150'000) {
    if (error) {
      *error = "export duration differs from the edit list by " +
               std::to_string(durationDifference) + " us";
    }
    return false;
  }
  std::cout << "probe duration_us=" << durationUs << " video=1 audio="
            << (audio ? 1 : 0) << '\n';
  return true;
}

bool selectInputStreams(const std::filesystem::path& path, int* videoIndex,
                        int* audioIndex) {
  AVFormatContext* format = nullptr;
  const std::string pathUtf8 = toUtf8String(path);
  int result = avformat_open_input(&format, pathUtf8.c_str(), nullptr, nullptr);
  if (result >= 0) result = avformat_find_stream_info(format, nullptr);
  int video = -1;
  int audio = -1;
  if (result >= 0) {
    video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video >= 0) {
      audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, video,
                                  nullptr, 0);
    }
  }
  avformat_close_input(&format);
  if (video < 0) return false;
  if (videoIndex) *videoIndex = video;
  if (audioIndex) *audioIndex = audio;
  return true;
}

std::set<std::filesystem::path> temporarySiblings(
    const std::filesystem::path& destination) {
  std::set<std::filesystem::path> paths;
  const std::wstring prefix =
      destination.filename().wstring() + L".radioify-part-";
  std::error_code error;
  for (const auto& entry :
       std::filesystem::directory_iterator(destination.parent_path(), error)) {
    if (error) break;
    const std::wstring filename = entry.path().filename().wstring();
    if (filename.rfind(prefix, 0) == 0) paths.insert(entry.path());
  }
  return paths;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) {
    std::cerr << "usage: video_edit_export_smoke <input> <output> "
                 "[--cancel] <start_us:end_us> [range...]\n";
    return 2;
  }
  playback_video_edit::ExportRequest request;
  request.sourcePath = argv[1];
  request.destinationPath = argv[2];
  if (!selectInputStreams(request.sourcePath, &request.videoStreamIndex,
                          &request.audioStreamIndex)) {
    std::cerr << "input streams could not be selected\n";
    return 2;
  }
  const bool cancelTest = std::wstring(argv[3]) == L"--cancel";
  const int firstRange = cancelTest ? 4 : 3;
  if (firstRange >= argc) {
    std::cerr << "at least one range is required\n";
    return 2;
  }
  for (int index = firstRange; index < argc; ++index) {
    playback_video_edit::SourceRange range;
    if (!parseRange(argv[index], &range)) {
      std::cerr << "invalid range\n";
      return 2;
    }
    request.keptRanges.push_back(range);
  }
  if (!std::is_sorted(request.keptRanges.begin(), request.keptRanges.end(),
                      [](const auto& lhs, const auto& rhs) {
                        return lhs.startUs < rhs.startUs;
                      })) {
    std::cerr << "ranges must be source ordered\n";
    return 2;
  }
  int64_t expectedDurationUs = 0;
  for (const playback_video_edit::SourceRange& range : request.keptRanges) {
    expectedDurationUs += range.durationUs();
  }
  const bool expectedAudio = request.audioStreamIndex >= 0;

  const std::set<std::filesystem::path> temporaryFilesBefore =
      temporarySiblings(request.destinationPath);
  playback_video_edit::Exporter exporter;
  if (!exporter.start(std::move(request))) {
    std::cerr << "export worker did not start\n";
    return 1;
  }
  if (cancelTest) exporter.cancel();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    exporter.consumeChanged();
    const playback_video_edit::ExportSnapshot snapshot = exporter.snapshot();
    if (!snapshot.finished()) continue;
    if (cancelTest) {
      const bool clean =
          snapshot.state == playback_video_edit::ExportState::Cancelled &&
          !std::filesystem::exists(snapshot.destinationPath) &&
          temporarySiblings(snapshot.destinationPath) == temporaryFilesBefore;
      if (!clean) {
        std::cerr << "cancelled export published or leaked a partial file\n";
        return 1;
      }
      std::cout << "cancelled cleanly\n";
      return 0;
    }
    if (snapshot.state != playback_video_edit::ExportState::Succeeded) {
      std::cerr << "export failed: " << snapshot.error << '\n';
      return 1;
    }
    std::cout << "encoder=" << snapshot.videoEncoder << '\n';
    std::string probeError;
    if (!probeOutput(snapshot.destinationPath, expectedDurationUs,
                     expectedAudio, &probeError)) {
      std::cerr << probeError << '\n';
      return 1;
    }
    return 0;
  }
}
