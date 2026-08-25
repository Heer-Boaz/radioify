#include "playback/video/transcript/document.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

#include "runtime_helpers.h"

namespace playback_video_transcript {
namespace {

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

std::string normalizedCueText(const std::string& text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool pendingSpace = false;
  for (unsigned char byte : text) {
    const bool asciiWhitespace =
        byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' ||
        byte == '\f' || byte == '\v';
    if (asciiWhitespace) {
      pendingSpace = !normalized.empty();
      continue;
    }
    if (pendingSpace) {
      normalized.push_back(' ');
      pendingSpace = false;
    }
    normalized.push_back(static_cast<char>(byte));
  }
  return normalized;
}

bool hasTranscriptContent(const std::string& text) {
  for (unsigned char byte : text) {
    if (byte >= 0x80 || (byte >= '0' && byte <= '9') ||
        (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) {
      return true;
    }
  }
  return false;
}

bool isSilencePlaceholder(const std::string& text) {
  std::string lowercase;
  lowercase.reserve(text.size());
  for (unsigned char byte : text) {
    lowercase.push_back(static_cast<char>(
        byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte));
  }
  return lowercase == "[blank_audio]" || lowercase == "[blank audio]" ||
         lowercase == "(blank audio)" || lowercase == "[silence]" ||
         lowercase == "(silence)";
}

std::string srtTimestamp(int64_t timestampUs) {
  const int64_t totalMs = std::max<int64_t>(0, timestampUs) / 1000;
  const int64_t milliseconds = totalMs % 1000;
  const int64_t totalSeconds = totalMs / 1000;
  const int64_t seconds = totalSeconds % 60;
  const int64_t totalMinutes = totalSeconds / 60;
  const int64_t minutes = totalMinutes % 60;
  const int64_t hours = totalMinutes / 60;

  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2)
      << minutes << ':' << std::setw(2) << seconds << ',' << std::setw(3)
      << milliseconds;
  return out.str();
}

std::filesystem::path temporarySiblingPath(
    const std::filesystem::path& outputPath) {
  const uint64_t stamp = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  for (uint32_t attempt = 0; attempt < 100; ++attempt) {
    std::filesystem::path candidate = outputPath;
    candidate += ".radioify-" + std::to_string(stamp) + "-" +
                 std::to_string(attempt) + ".tmp";
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec) && !ec) return candidate;
  }
  return {};
}

bool publishFile(const std::filesystem::path& source,
                 const std::filesystem::path& destination,
                 std::string* error) {
#ifdef _WIN32
  if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  setError(error, "Could not publish transcript (Windows error " +
                      std::to_string(GetLastError()) + ").");
  return false;
#else
  std::error_code existsError;
  if (std::filesystem::exists(destination, existsError) || existsError) {
    setError(error, "Transcript destination already exists.");
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(source, destination, ec);
  if (!ec) return true;
  setError(error, "Could not publish transcript: " + ec.message());
  return false;
#endif
}

}  // namespace

bool isMeaningfulTranscriptText(const std::string& text) {
  const std::string normalized = normalizedCueText(text);
  return !normalized.empty() && hasTranscriptContent(normalized) &&
         !isSilencePlaceholder(normalized);
}

std::filesystem::path defaultTranscriptPath(
    const std::filesystem::path& videoPath) {
  if (videoPath.filename().empty()) return {};
  std::filesystem::path output = videoPath.parent_path() / videoPath.stem();
  output += ".transcript.srt";
  return output;
}

std::filesystem::path availableTranscriptPath(
    const std::filesystem::path& videoPath) {
  const std::filesystem::path preferred = defaultTranscriptPath(videoPath);
  if (preferred.empty()) return {};
  std::error_code ec;
  if (!std::filesystem::exists(preferred, ec) && !ec) return preferred;

  const std::filesystem::path prefix =
      videoPath.parent_path() / videoPath.stem();
  for (uint32_t suffix = 2; suffix < 10000; ++suffix) {
    std::filesystem::path candidate = prefix;
    candidate += ".transcript." + std::to_string(suffix) + ".srt";
    ec.clear();
    if (!std::filesystem::exists(candidate, ec) && !ec) return candidate;
  }
  return {};
}

bool writeIndexedTranscript(const std::filesystem::path& outputPath,
                            const std::vector<Segment>& segments,
                            std::string* error) {
  if (error) error->clear();
  if (outputPath.empty() || outputPath.filename().empty()) {
    setError(error, "Transcript output path is empty.");
    return false;
  }
  {
    std::error_code ec;
    if (std::filesystem::exists(outputPath, ec)) {
      setError(error, "Transcript already exists: " +
                          toUtf8String(outputPath.filename()));
      return false;
    }
    if (ec) {
      setError(error, "Could not inspect transcript destination: " +
                          ec.message());
      return false;
    }
  }

  std::vector<Segment> cues;
  cues.reserve(segments.size());
  for (const Segment& segment : segments) {
    Segment cue = segment;
    cue.text = normalizedCueText(cue.text);
    if (!isMeaningfulTranscriptText(cue.text)) continue;
    cue.startUs = std::max<int64_t>(0, cue.startUs);
    cue.endUs = std::max(cue.startUs + 1000, cue.endUs);
    cues.push_back(std::move(cue));
  }
  if (cues.empty()) {
    setError(error, "No speech was detected; no transcript was written.");
    return false;
  }
  std::stable_sort(cues.begin(), cues.end(),
                   [](const Segment& lhs, const Segment& rhs) {
                     if (lhs.startUs != rhs.startUs) {
                       return lhs.startUs < rhs.startUs;
                     }
                     return lhs.endUs < rhs.endUs;
                   });

  const std::filesystem::path parent = outputPath.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    const bool isDirectory = std::filesystem::is_directory(parent, ec);
    if (ec || !isDirectory) {
      setError(error, "Transcript directory does not exist: " +
                          toUtf8String(parent));
      return false;
    }
  }

  const std::filesystem::path temporaryPath =
      temporarySiblingPath(outputPath);
  if (temporaryPath.empty()) {
    setError(error, "Could not reserve a temporary transcript file.");
    return false;
  }

  {
    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output) {
      setError(error, "Could not create transcript: " +
                          toUtf8String(outputPath));
      return false;
    }
    for (size_t index = 0; index < cues.size(); ++index) {
      output << (index + 1) << "\r\n"
             << srtTimestamp(cues[index].startUs) << " --> "
             << srtTimestamp(cues[index].endUs) << "\r\n"
             << cues[index].text << "\r\n\r\n";
    }
    output.flush();
    if (!output) {
      output.close();
      std::error_code ignored;
      std::filesystem::remove(temporaryPath, ignored);
      setError(error, "Could not finish writing transcript: " +
                          toUtf8String(outputPath));
      return false;
    }
  }

  if (!publishFile(temporaryPath, outputPath, error)) {
    std::error_code ignored;
    std::filesystem::remove(temporaryPath, ignored);
    return false;
  }
  return true;
}

}  // namespace playback_video_transcript
