#include "playback/video/transcript/document.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#include "core/file_output.h"
#include "playback/video/transcript/cue_semantics.h"
#include "runtime_helpers.h"

namespace playback_video_transcript {
namespace {

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

bool parseSrtTimestamp(const std::string& value, int64_t* timestampUs) {
  if (!timestampUs || value.size() < 12) return false;
  const size_t firstColon = value.find(':');
  const size_t secondColon =
      firstColon == std::string::npos ? std::string::npos
                                      : value.find(':', firstColon + 1);
  const size_t fraction =
      secondColon == std::string::npos
          ? std::string::npos
          : value.find_first_of(",.", secondColon + 1);
  if (firstColon < 2 || secondColon == std::string::npos ||
      secondColon != firstColon + 3 || fraction != secondColon + 3 ||
      fraction + 4 != value.size()) {
    return false;
  }
  const auto parsePart = [&](size_t begin, size_t end,
                             int64_t* part) -> bool {
    if (!part || begin >= end) return false;
    int64_t parsed = 0;
    for (size_t index = begin; index < end; ++index) {
      if (value[index] < '0' || value[index] > '9' ||
          parsed > ((std::numeric_limits<int64_t>::max)() - 9) / 10) {
        return false;
      }
      parsed = parsed * 10 + (value[index] - '0');
    }
    *part = parsed;
    return true;
  };
  int64_t hours = 0;
  int64_t minutes = 0;
  int64_t seconds = 0;
  int64_t milliseconds = 0;
  if (!parsePart(0, firstColon, &hours) ||
      !parsePart(firstColon + 1, secondColon, &minutes) ||
      !parsePart(secondColon + 1, fraction, &seconds) ||
      !parsePart(fraction + 1, value.size(), &milliseconds) || minutes >= 60 ||
      seconds >= 60) {
    return false;
  }
  const int64_t maximumMilliseconds =
      (std::numeric_limits<int64_t>::max)() / 1000;
  constexpr int64_t kMillisecondsPerHour = 3'600'000;
  if (hours > maximumMilliseconds / kMillisecondsPerHour) return false;
  const int64_t hourMilliseconds = hours * kMillisecondsPerHour;
  const int64_t remainingMilliseconds =
      (minutes * 60 + seconds) * 1000 + milliseconds;
  if (hourMilliseconds > maximumMilliseconds - remainingMilliseconds) {
    return false;
  }
  const int64_t totalMilliseconds =
      hourMilliseconds + remainingMilliseconds;
  *timestampUs = totalMilliseconds * 1000;
  return true;
}

bool parseSrtTimingLine(const std::string& line, int64_t* startUs,
                        int64_t* endUs) {
  constexpr const char* kArrow = "-->";
  const size_t arrow = line.find(kArrow);
  if (arrow == std::string::npos) return false;
  const auto trim = [](std::string value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
      value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
      value.pop_back();
    }
    return value;
  };
  const std::string start = trim(line.substr(0, arrow));
  std::string end = trim(line.substr(arrow + 3));
  const size_t endOfTimestamp = end.find_first_of(" \t");
  if (endOfTimestamp != std::string::npos) end.resize(endOfTimestamp);
  return parseSrtTimestamp(start, startUs) &&
         parseSrtTimestamp(end, endUs) && *endUs > *startUs;
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

}  // namespace

bool readIndexedTranscript(const std::filesystem::path& inputPath,
                           std::vector<Segment>* segments,
                           std::string* error) {
  if (error) error->clear();
  if (segments) segments->clear();
  if (inputPath.empty() || !segments) {
    setError(error, "Transcript input or destination is empty.");
    return false;
  }
  std::ifstream input(inputPath, std::ios::binary);
  if (!input) {
    setError(error, "Could not open transcript: " +
                        toUtf8String(inputPath.filename()));
    return false;
  }

  std::vector<std::string> block;
  const auto consumeBlock = [&]() {
    if (block.empty()) return;
    size_t timingIndex = block.size();
    int64_t startUs = 0;
    int64_t endUs = 0;
    for (size_t index = 0; index < block.size(); ++index) {
      if (parseSrtTimingLine(block[index], &startUs, &endUs)) {
        timingIndex = index;
        break;
      }
    }
    if (timingIndex == block.size()) {
      block.clear();
      return;
    }
    std::string text;
    for (size_t index = timingIndex + 1; index < block.size(); ++index) {
      const std::string normalized =
          normalizeTranscriptCueText(block[index]);
      if (normalized.empty()) continue;
      if (!text.empty()) text.push_back(' ');
      text += normalized;
    }
    if (isMeaningfulTranscriptText(text)) {
      segments->push_back({startUs, endUs, std::move(text)});
    }
    block.clear();
  };

  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) {
      consumeBlock();
    } else {
      block.push_back(std::move(line));
    }
  }
  consumeBlock();
  if (!input.eof() && input.fail()) {
    segments->clear();
    setError(error, "Could not finish reading transcript: " +
                        toUtf8String(inputPath.filename()));
    return false;
  }
  if (segments->empty()) {
    setError(error, "Transcript contains no readable speech cues.");
    return false;
  }
  std::stable_sort(segments->begin(), segments->end(),
                   [](const Segment& lhs, const Segment& rhs) {
                     if (lhs.startUs != rhs.startUs) {
                       return lhs.startUs < rhs.startUs;
                     }
                     return lhs.endUs < rhs.endUs;
                   });
  return true;
}

bool writeIndexedTranscript(const std::filesystem::path& outputPath,
                            const std::vector<Segment>& segments,
                            TranscriptPublishMode publishMode,
                            std::string* error,
                            const TranscriptOutputCommitStarted&
                                outputCommitStarted) {
  if (error) error->clear();
  if (outputPath.empty() || outputPath.filename().empty()) {
    setError(error, "Transcript output path is empty.");
    return false;
  }
  {
    std::error_code ec;
    if (publishMode == TranscriptPublishMode::CreateNew &&
        std::filesystem::exists(outputPath, ec)) {
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
    cue.text = normalizeTranscriptCueText(cue.text);
    if (!isTranscriptSpeechCue(cue.text)) continue;
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

  const file_output::PublishMode outputMode =
      publishMode == TranscriptPublishMode::ReplaceExisting
          ? file_output::PublishMode::ReplaceExisting
          : file_output::PublishMode::CreateNew;
  std::optional<file_output::Transaction> transaction =
      file_output::Transaction::begin(outputPath, outputMode, error);
  if (!transaction) return false;

  {
    std::ofstream output(transaction->temporaryPath(),
                         std::ios::binary | std::ios::trunc);
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
      setError(error, "Could not finish writing transcript: " +
                          toUtf8String(outputPath));
      return false;
    }
  }

  if (outputCommitStarted && !outputCommitStarted()) {
    setError(error, "Transcript cancelled before publication.");
    return false;
  }
  return transaction->publish(error);
}

}  // namespace playback_video_transcript
