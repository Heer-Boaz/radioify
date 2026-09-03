#include "playback/video/chapter/generated_document.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <nlohmann/json.hpp>
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

std::optional<std::string> normalizedText(const nlohmann::json &value,
                                          std::size_t maximumBytes,
                                          std::string_view field,
                                          std::string *error) {
  if (!value.is_string()) {
    setError(error, "The generated " + std::string(field) + " is not text.");
    return std::nullopt;
  }
  const std::string source = value.get<std::string>();
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

std::optional<nlohmann::json> parseObject(std::string_view source,
                                          std::size_t expectedFields,
                                          std::string *error) {
  try {
    nlohmann::json document =
        nlohmann::json::parse(source.begin(), source.end());
    if (!document.is_object() || document.size() != expectedFields) {
      setError(error, "The chapter engine returned an invalid JSON object.");
      return std::nullopt;
    }
    return document;
  } catch (const nlohmann::json::exception &) {
    setError(error, "The chapter engine did not return valid JSON.");
    return std::nullopt;
  }
}

std::string stringRules() {
  return "string-char ::= [^\"\\\\\\x7F\\x00-\\x1F] | \"\\\\\" "
         "([\"\\\\bfnrt] | \"u\" [0-9a-fA-F]{4})\n"
         "ws ::= | \" \" | \"\\n\" [ \\t]{0,20}\n";
}

AnalysisResult invalid(std::string detail) {
  AnalysisResult result;
  result.detail = std::move(detail);
  return result;
}

} // namespace

std::string generatedFrameCaptionsGrammar(std::size_t captionCount) {
  if (captionCount == 0 || captionCount > 6)
    return {};
  std::ostringstream grammar;
  grammar << "root ::= \"{\" ws \"\\\"captions\\\"\" ws \":\" ws "
             "\"[\" ws caption";
  for (std::size_t index = 1; index < captionCount; ++index)
    grammar << " ws \",\" ws caption";
  grammar << " ws \"]\" ws \"}\" ws\n"
             "caption ::= \"\\\"\" string-char+ \"\\\"\" ws\n"
          << stringRules();
  return grammar.str();
}

bool parseGeneratedFrameCaptions(std::string_view json,
                                 std::size_t expectedCount,
                                 std::vector<std::string> *captions,
                                 std::string *error) {
  if (error)
    error->clear();
  if (!captions) {
    setError(error, "The frame-caption destination is missing.");
    return false;
  }
  captions->clear();
  const std::optional<nlohmann::json> document = parseObject(json, 1, error);
  if (!document || !document->contains("captions") ||
      !(*document)["captions"].is_array() ||
      (*document)["captions"].size() != expectedCount || expectedCount == 0 ||
      expectedCount > 6) {
    if (document)
      setError(error,
               "The frame-caption array does not match the supplied frames.");
    return false;
  }
  captions->reserve(expectedCount);
  for (const nlohmann::json &value : (*document)["captions"]) {
    const std::optional<std::string> parsed = normalizedText(
        value, kMaximumAutomaticCaptionBytes, "frame caption", error);
    if (!parsed) {
      captions->clear();
      return false;
    }
    captions->push_back(*parsed);
  }
  return true;
}

bool parseChapterLlamaPlan(std::string_view output, std::int64_t durationUs,
                           const std::vector<std::int64_t> &boundaryAnchorsUs,
                           std::vector<GeneratedChapterPlanEntry> *plan,
                           std::string *error) {
  if (error)
    error->clear();
  if (!plan) {
    setError(error, "The Chapter-Llama plan destination is missing.");
    return false;
  }
  plan->clear();
  if (durationUs <= 0 ||
      boundaryAnchorsUs.size() < kMinimumAutomaticEvidenceSampleCount ||
      boundaryAnchorsUs.size() > kMaximumAutomaticEvidenceFrameCount ||
      boundaryAnchorsUs.front() != 0 ||
      !std::is_sorted(boundaryAnchorsUs.begin(), boundaryAnchorsUs.end()) ||
      std::adjacent_find(boundaryAnchorsUs.begin(), boundaryAnchorsUs.end()) !=
          boundaryAnchorsUs.end() ||
      boundaryAnchorsUs.back() >= durationUs) {
    setError(error, "The Chapter-Llama evidence timeline is invalid.");
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
      continue;
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

    auto after = std::lower_bound(boundaryAnchorsUs.begin(),
                                  boundaryAnchorsUs.end(), proposedUs);
    auto match = after;
    if (after == boundaryAnchorsUs.end()) {
      match = std::prev(boundaryAnchorsUs.end());
    } else if (after != boundaryAnchorsUs.begin()) {
      const auto before = std::prev(after);
      if (proposedUs - *before <= *after - proposedUs)
        match = before;
    }
    const std::int64_t startUs = *match;
    if (!plan->empty() && startUs <= plan->back().startUs) {
      setError(error, "Two Chapter-Llama timestamps map to the same or an "
                      "earlier sampler-owned frame.");
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
    const std::string ordinal = std::to_string(plan->size() + 1) + ". ";
    if (title.compare(0, ordinal.size(), ordinal) == 0) {
      title.erase(0, ordinal.size());
    }
    const std::optional<std::string> normalized =
        normalizedText(nlohmann::json(title), kMaximumAutomaticTitleBytes,
                       "chapter title", error);
    if (!normalized) {
      plan->clear();
      return false;
    }
    plan->push_back({startUs});
  }
  if (plan->size() < kMinimumAutomaticChapterCount ||
      plan->front().startUs != 0) {
    setError(error, "Chapter-Llama did not return a complete navigation plan.");
    plan->clear();
    return false;
  }
  return true;
}

std::string generatedChapterMetadataGrammar() {
  return "root ::= \"{\" ws \"\\\"title\\\"\" ws \":\" ws title ws "
         "\",\" ws \"\\\"summary\\\"\" ws \":\" ws summary \"}\" ws\n"
         "title ::= \"\\\"\" string-char{1,120} \"\\\"\" ws\n"
         "summary ::= \"\\\"\" string-char+ \"\\\"\" ws\n" +
         stringRules();
}

bool parseGeneratedChapterMetadata(std::string_view json,
                                   GeneratedChapterMetadata *metadata,
                                   std::string *error) {
  if (error)
    error->clear();
  if (!metadata) {
    setError(error, "The generated-metadata destination is missing.");
    return false;
  }
  const std::optional<nlohmann::json> document = parseObject(json, 2, error);
  if (!document || !document->contains("title") ||
      !document->contains("summary")) {
    if (document)
      setError(error, "The generated chapter metadata is invalid.");
    return false;
  }
  const std::optional<std::string> parsedTitle =
      normalizedText((*document)["title"], kMaximumAutomaticTitleBytes,
                     "chapter title", error);
  const std::optional<std::string> parsedSummary =
      normalizedText((*document)["summary"], kMaximumAutomaticSummaryBytes,
                     "chapter summary", error);
  if (!parsedTitle || !parsedSummary)
    return false;
  metadata->title = *parsedTitle;
  metadata->summary = *parsedSummary;
  return true;
}

std::string generatedOverviewGrammar() {
  return "root ::= \"{\" ws \"\\\"overview\\\"\" ws \":\" ws "
         "overview \"}\" ws\n"
         "overview ::= \"\\\"\" string-char+ \"\\\"\" ws\n" +
         stringRules();
}

bool parseGeneratedOverview(std::string_view json, std::string *overview,
                            std::string *error) {
  if (error)
    error->clear();
  if (!overview) {
    setError(error, "The generated-overview destination is missing.");
    return false;
  }
  const std::optional<nlohmann::json> document = parseObject(json, 1, error);
  if (!document || !document->contains("overview")) {
    if (document)
      setError(error, "The generated overview is invalid.");
    return false;
  }
  const std::optional<std::string> parsed =
      normalizedText((*document)["overview"], kMaximumAutomaticOverviewBytes,
                     "overview", error);
  if (!parsed)
    return false;
  *overview = *parsed;
  return true;
}

AnalysisResult materializeGeneratedDocument(
    const GeneratedDocument &document, std::int64_t durationUs,
    const std::vector<std::int64_t> &boundaryAnchorsUs) {
  if (durationUs <= 0 ||
      boundaryAnchorsUs.size() < kMinimumAutomaticEvidenceSampleCount ||
      boundaryAnchorsUs.size() > kMaximumAutomaticEvidenceFrameCount ||
      document.chapters.size() < kMinimumAutomaticChapterCount ||
      document.chapters.size() > kMaximumAutomaticChapterCount ||
      document.chapters.size() > boundaryAnchorsUs.size()) {
    return invalid("The generated chapter artifact has invalid dimensions.");
  }
  if (boundaryAnchorsUs.front() != 0) {
    return invalid("The chapter evidence has no video-start interval.");
  }
  for (std::size_t index = 0; index < boundaryAnchorsUs.size(); ++index) {
    if (boundaryAnchorsUs[index] < 0 ||
        boundaryAnchorsUs[index] >= durationUs ||
        (index > 0 &&
         boundaryAnchorsUs[index] <= boundaryAnchorsUs[index - 1])) {
      return invalid("The chapter evidence intervals are not chronological.");
    }
  }

  AnalysisResult result;
  result.overview = document.overview;
  result.chapters.reserve(document.chapters.size());
  std::int64_t previousStartUs = -1;
  for (std::size_t index = 0; index < document.chapters.size(); ++index) {
    const GeneratedChapter &generated = document.chapters[index];
    if (!std::binary_search(boundaryAnchorsUs.begin(), boundaryAnchorsUs.end(),
                            generated.startUs) ||
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
    chapter.summary = generated.summary;
    result.chapters.push_back(std::move(chapter));
    previousStartUs = generated.startUs;
  }
  for (std::size_t index = 0; index < result.chapters.size(); ++index) {
    result.chapters[index].endUs = index + 1 < result.chapters.size()
                                       ? result.chapters[index + 1].startUs
                                       : durationUs;
  }

  std::string validationError;
  if (!validateAutomaticAnalysis(durationUs, result.overview, result.chapters,
                                 &validationError)) {
    return invalid(std::move(validationError));
  }
  result.status = OperationStatus::Succeeded;
  return result;
}

} // namespace playback_video_chapters
