#include "playback/video/chapter/generated_document.h"

#include <cctype>
#include <optional>
#include <sstream>
#include <utility>

#include "core/utf8.h"

namespace playback_video_chapters {
namespace {

void setError(std::string *error, std::string value) {
  if (error)
    *error = std::move(value);
}

std::optional<std::string> normalizedText(std::string_view source,
                                          std::size_t maximumBytes,
                                          std::string_view field,
                                          std::string *error) {
  if (!isValidUtf8(source)) {
    setError(error,
             "The generated " + std::string(field) + " is not valid UTF-8.");
    return std::nullopt;
  }

  std::string normalized;
  normalized.reserve(source.size());
  bool pendingSpace = false;
  for (unsigned char ch : source) {
    if (std::isspace(ch)) {
      pendingSpace = !normalized.empty();
      continue;
    }
    if (ch < 0x20 || ch == 0x7f) {
      setError(error, "The generated " + std::string(field) +
                          " contains a control character.");
      return std::nullopt;
    }
    if (pendingSpace)
      normalized.push_back(' ');
    pendingSpace = false;
    normalized.push_back(static_cast<char>(ch));
  }
  if (normalized.empty() || normalized.size() > maximumBytes) {
    setError(error, "The generated " + std::string(field) +
                        " is empty or exceeds its storage limit.");
    return std::nullopt;
  }
  return normalized;
}

AnalysisResult invalid(std::string detail) {
  AnalysisResult result;
  result.detail = std::move(detail);
  return result;
}

} // namespace

bool normalizeGeneratedFrameCaption(std::string_view text,
                                    std::string *caption,
                                    std::string *error) {
  if (error)
    error->clear();
  if (!caption) {
    setError(error, "The frame-caption destination is missing.");
    return false;
  }
  caption->clear();
  const std::optional<std::string> parsed = normalizedText(
      text, kMaximumAutomaticCaptionBytes, "frame caption", error);
  if (!parsed)
    return false;
  *caption = *parsed;
  return true;
}

bool parseChapterLlamaPlan(std::string_view output, std::int64_t durationUs,
                           std::vector<GeneratedChapterPlanEntry> *plan,
                           std::string *error) {
  if (error)
    error->clear();
  if (!plan) {
    setError(error, "The Chapter-Llama plan destination is missing.");
    return false;
  }
  plan->clear();
  if (durationUs <= 0) {
    setError(error, "The Chapter-Llama video duration is invalid.");
    return false;
  }

  std::istringstream lines{std::string(output)};
  std::string line;
  while (std::getline(lines, line)) {
    const std::size_t first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
      continue;
    const std::size_t last = line.find_last_not_of(" \t\r\n");
    line = line.substr(first, last - first + 1);
    const std::size_t separator = line.find(" - ");
    if (separator == std::string::npos || separator != 8 || line[2] != ':' ||
        line[5] != ':') {
      setError(error, "Chapter-Llama returned a line outside its chapter protocol.");
      plan->clear();
      return false;
    }
    for (const std::size_t index : {0u, 1u, 3u, 4u, 6u, 7u}) {
      if (!std::isdigit(static_cast<unsigned char>(line[index]))) {
        setError(error, "A Chapter-Llama timestamp is malformed.");
        plan->clear();
        return false;
      }
    }
    const std::int64_t hours = (line[0] - '0') * 10 + (line[1] - '0');
    const std::int64_t minutes = (line[3] - '0') * 10 + (line[4] - '0');
    const std::int64_t seconds = (line[6] - '0') * 10 + (line[7] - '0');
    if (minutes >= 60 || seconds >= 60) {
      setError(error, "A Chapter-Llama timestamp is malformed.");
      plan->clear();
      return false;
    }
    const std::int64_t proposedUs =
        (hours * 3600 + minutes * 60 + seconds) * 1'000'000;
    if (proposedUs < 0 || proposedUs >= durationUs) {
      setError(error, "A Chapter-Llama timestamp is outside the video.");
      plan->clear();
      return false;
    }

    const std::int64_t startUs = proposedUs;
    if (!plan->empty() && startUs <= plan->back().startUs) {
      setError(error, "Chapter-Llama timestamps must strictly increase.");
      plan->clear();
      return false;
    }
    if (plan->size() >= kMaximumAutomaticChapterCount) {
      setError(error, "Chapter-Llama returned more chapters than the product "
                      "limit permits.");
      plan->clear();
      return false;
    }
    std::string title = line.substr(separator + 3);
    const std::optional<std::string> normalized =
        normalizedText(title, kMaximumAutomaticTitleBytes, "chapter title",
                       error);
    if (!normalized) {
      plan->clear();
      return false;
    }
    plan->push_back({startUs, *normalized});
  }
  if (plan->size() < kMinimumAutomaticChapterCount ||
      plan->front().startUs != 0) {
    setError(error, "Chapter-Llama did not return a complete navigation plan.");
    plan->clear();
    return false;
  }
  return true;
}

AnalysisResult materializeGeneratedDocument(
    const GeneratedDocument &document, std::int64_t durationUs) {
  if (durationUs <= 0 ||
      document.chapters.size() < kMinimumAutomaticChapterCount ||
      document.chapters.size() > kMaximumAutomaticChapterCount) {
    return invalid("The generated chapter artifact has invalid dimensions.");
  }

  AnalysisResult result;
  result.chapters.reserve(document.chapters.size());
  std::int64_t previousStartUs = -1;
  for (std::size_t index = 0; index < document.chapters.size(); ++index) {
    const GeneratedChapter &generated = document.chapters[index];
    if (generated.startUs < 0 || generated.startUs >= durationUs ||
        (index == 0 && generated.startUs != 0) ||
        (index > 0 && generated.startUs <= previousStartUs)) {
      return invalid(
          "Generated chapter timestamps must begin at zero and strictly "
          "increase.");
    }
    Chapter chapter;
    chapter.id = static_cast<std::uint64_t>(index + 1);
    chapter.startUs = generated.startUs;
    chapter.title = generated.title;
    result.chapters.push_back(std::move(chapter));
    previousStartUs = generated.startUs;
  }
  for (std::size_t index = 0; index < result.chapters.size(); ++index) {
    result.chapters[index].endUs = index + 1 < result.chapters.size()
                                       ? result.chapters[index + 1].startUs
                                       : durationUs;
  }

  std::string validationError;
  if (!validateAutomaticAnalysis(durationUs, result.chapters,
                                 &validationError)) {
    return invalid(std::move(validationError));
  }
  result.status = OperationStatus::Succeeded;
  return result;
}

} // namespace playback_video_chapters
