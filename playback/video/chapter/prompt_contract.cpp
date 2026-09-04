#include "playback/video/chapter/prompt_contract.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "core/utf8.h"

namespace playback_video_chapters {
namespace {

enum class Modality : std::uint8_t { Asr, Caption };

struct ProtocolLine {
  std::int64_t timeUs = 0;
  Modality modality = Modality::Asr;
  std::string_view text;
};

std::string formatTimestamp(std::int64_t timeUs) {
  const std::int64_t totalSeconds = timeUs / 1'000'000;
  const std::int64_t hours = totalSeconds / 3600;
  const std::int64_t minutes = (totalSeconds / 60) % 60;
  const std::int64_t seconds = totalSeconds % 60;
  std::ostringstream formatted;
  formatted << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2)
            << minutes << ':' << std::setw(2) << seconds;
  return formatted.str();
}

bool validEvidence(std::int64_t durationUs,
                   const std::vector<ChapterPromptEvidence> &evidence,
                   bool allowEmptyText = false) {
  std::int64_t previousUs = -1;
  for (const ChapterPromptEvidence &item : evidence) {
    if (item.timeUs < 0 || item.timeUs >= durationUs ||
        item.timeUs < previousUs || (!allowEmptyText && item.text.empty()) ||
        !isValidUtf8(item.text)) {
      return false;
    }
    previousUs = item.timeUs;
  }
  return true;
}

void appendBasePrompt(std::ostringstream *prompt, std::int64_t durationUs,
                      std::string_view task) {
  *prompt << "Given the complete transcript of a video of duration "
          << formatTimestamp(durationUs) << ", " << task << '\n'
          << "Identify the approximate start time of each chapter in the "
             "format 'hh:mm:ss - Title'. Ensure each chapter entry is on a "
             "new line. Focus on significant topic changes that would merit "
             "a new chapter in a video, but do not provide summaries of the "
             "chapters.\nHere is the transcript to analyze:\n";
}

void appendLine(std::ostringstream *prompt, const ProtocolLine &line) {
  *prompt << (line.modality == Modality::Caption ? "Caption " : "ASR ")
          << formatTimestamp(line.timeUs) << ": "
          << escapeChapterLlamaEvidence(line.text);
}

} // namespace

std::optional<std::string>
buildChapterLlamaMiniCpmV2CaptionPrompt(std::string_view imageMarker,
                                       std::size_t maximumBytes) {
  if (imageMarker.empty() || maximumBytes == 0 ||
      !isValidUtf8(imageMarker)) {
    return std::nullopt;
  }
  std::string prompt;
  prompt.reserve(imageMarker.size() + 61);
  prompt += "<user>";
  prompt += imageMarker;
  prompt += "\nWhat is the content of this image?<AI>";
  if (prompt.size() > maximumBytes)
    return std::nullopt;
  return prompt;
}

std::string escapeChapterLlamaEvidence(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (index + 1 < text.size() && text[index] == '<' &&
        text[index + 1] == '|') {
      escaped += "< |";
      ++index;
    } else if (index + 1 < text.size() && text[index] == '|' &&
               text[index + 1] == '>') {
      escaped += "| >";
      ++index;
    } else {
      escaped.push_back(text[index]);
    }
  }
  return escaped;
}

std::optional<std::string> buildChapterLlamaSpeechPrompt(
    std::int64_t durationUs,
    const std::vector<ChapterPromptEvidence> &asrEvidence,
    std::size_t maximumBytes) {
  if (durationUs <= 0 || maximumBytes == 0 || asrEvidence.empty() ||
      !validEvidence(durationUs, asrEvidence)) {
    return std::nullopt;
  }
  std::ostringstream prompt;
  appendBasePrompt(
      &prompt, durationUs,
      "segment the text into distinct chapters based on thematic shifts or "
      "changes in topics.");
  for (const ChapterPromptEvidence &item : asrEvidence) {
    prompt << formatTimestamp(item.timeUs) << ": "
           << escapeChapterLlamaEvidence(item.text) << '\n';
  }
  std::string result = prompt.str();
  if (result.size() > maximumBytes)
    return std::nullopt;
  return result;
}

std::optional<std::string> buildChapterLlamaCaptionAsrPrompt(
    std::int64_t durationUs,
    const std::vector<ChapterPromptEvidence> &captionEvidence,
    const std::vector<ChapterPromptEvidence> &asrEvidence,
    std::size_t maximumBytes) {
  if (durationUs <= 0 || maximumBytes == 0 || captionEvidence.empty() ||
      asrEvidence.empty() ||
      !validEvidence(durationUs, captionEvidence, true) ||
      !validEvidence(durationUs, asrEvidence)) {
    return std::nullopt;
  }

  std::vector<ProtocolLine> lines;
  lines.reserve(asrEvidence.size() + captionEvidence.size());
  // Python's stable sorted(asr_data + captions_data, key=timestamp) retains
  // this modality order on ties; it is part of the trained input protocol.
  for (const ChapterPromptEvidence &item : asrEvidence)
    lines.push_back({item.timeUs, Modality::Asr, item.text});
  for (const ChapterPromptEvidence &item : captionEvidence)
    lines.push_back({item.timeUs, Modality::Caption, item.text});
  std::stable_sort(lines.begin(), lines.end(),
                   [](const ProtocolLine &left, const ProtocolLine &right) {
                     return left.timeUs < right.timeUs;
                   });

  std::ostringstream prompt;
  appendBasePrompt(
      &prompt, durationUs,
      "use the provided captions and ASR transcript to identify distinct "
      "chapters based on content shifts.");
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (index > 0)
      prompt << '\n';
    appendLine(&prompt, lines[index]);
  }
  std::string result = prompt.str();
  if (result.size() > maximumBytes)
    return std::nullopt;
  return result;
}

} // namespace playback_video_chapters
