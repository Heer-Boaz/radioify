#include "playback/video/chapter/generated_document.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <optional>
#include <utility>

#include "core/utf8.h"

namespace playback_video_chapters {
namespace {

constexpr std::size_t kMaximumObservationBytes = 1200;

void setError(std::string* error, std::string value) {
  if (error) *error = std::move(value);
}

std::optional<std::string> normalizedGeneratedText(const nlohmann::json& value,
                                                   std::size_t maximumBytes,
                                                   std::string_view fieldName,
                                                   std::string* error) {
  if (!value.is_string()) {
    setError(error,
             "The generated " + std::string(fieldName) + " is not text.");
    return std::nullopt;
  }
  const std::string source = value.get<std::string>();
  if (!isValidUtf8(source)) {
    setError(error, "The generated " + std::string(fieldName) +
                        " is not valid UTF-8.");
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
    if (pendingSpace) normalized.push_back(' ');
    pendingSpace = false;
    normalized.push_back(static_cast<char>(ch));
  }
  if (normalized.empty()) {
    setError(error, "The generated " + std::string(fieldName) + " is empty.");
    return std::nullopt;
  }
  if (normalized.size() > maximumBytes) {
    setError(error, "The generated " + std::string(fieldName) +
                        " exceeds its published size limit.");
    return std::nullopt;
  }
  return normalized;
}

std::size_t wordCount(std::string_view value) {
  std::size_t count = 0;
  bool inWord = false;
  for (unsigned char ch : value) {
    if (std::isspace(ch)) {
      inWord = false;
    } else if (!inWord) {
      ++count;
      inWord = true;
    }
  }
  return count;
}

bool requireCompleteText(std::string_view value, std::size_t maximumWords,
                         std::string_view fieldName, std::string* error) {
  const char final = value.empty() ? '\0' : value.back();
  if ((final != '.' && final != '!' && final != '?') ||
      wordCount(value) > maximumWords) {
    setError(error, "The generated " + std::string(fieldName) +
                        " is not concise complete text.");
    return false;
  }
  return true;
}

std::optional<nlohmann::json> parseObject(std::string_view json,
                                          std::size_t expectedFields,
                                          std::string* error) {
  try {
    nlohmann::json document = nlohmann::json::parse(json.begin(), json.end());
    if (!document.is_object() || document.size() != expectedFields) {
      setError(error, "The chapter engine returned an invalid JSON object.");
      return std::nullopt;
    }
    return document;
  } catch (const nlohmann::json::exception&) {
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

}  // namespace

std::string generatedObservationGrammar() {
  return "root ::= \"{\" ws \"\\\"observation\\\"\" ws \":\" ws "
         "\"\\\"\" string-char{1,600} \"\\\"\" ws \"}\" ws\n" +
         stringRules();
}

bool parseGeneratedObservation(std::string_view json, std::string* observation,
                               std::string* error) {
  if (error) error->clear();
  if (!observation) {
    setError(error, "The observation destination is missing.");
    return false;
  }
  const std::optional<nlohmann::json> document = parseObject(json, 1, error);
  if (!document || !document->contains("observation")) {
    if (document) setError(error, "The observation field is missing.");
    return false;
  }
  const std::optional<std::string> parsed =
      normalizedGeneratedText((*document)["observation"],
                              kMaximumObservationBytes, "observation", error);
  if (!parsed) return false;
  if (wordCount(*parsed) > 50) {
    setError(error, "The generated observation is not concise.");
    return false;
  }
  *observation = *parsed;
  return true;
}

static std::string integerAlternatives(std::size_t first, std::size_t last) {
  std::string values = "(";
  for (std::size_t value = first; value <= last; ++value) {
    if (value != first) values += " | ";
    values += "\"" + std::to_string(value) + "\"";
  }
  values += ")";
  return values;
}

std::string generatedSegmentationPlanGrammar(std::size_t maximumChapters) {
  if (maximumChapters < kMinimumAutomaticChapterCount) return {};
  return "root ::= \"{\" ws \"\\\"progression\\\"\" ws \":\" ws "
         "\"\\\"\" string-char{1,1000} \"\\\"\" ws \",\" ws "
         "\"\\\"chapter_count\\\"\" ws \":\" ws chapter-count ws \"}\" ws\n"
         "chapter-count ::= " +
         integerAlternatives(kMinimumAutomaticChapterCount, maximumChapters) +
         "\n" + stringRules();
}

bool parseGeneratedSegmentationPlan(std::string_view json,
                                    std::size_t maximumChapters,
                                    GeneratedSegmentationPlan* plan,
                                    std::string* error) {
  if (error) error->clear();
  if (!plan || maximumChapters < kMinimumAutomaticChapterCount) {
    setError(error, "The segmentation-plan destination is missing.");
    return false;
  }
  const std::optional<nlohmann::json> document = parseObject(json, 2, error);
  if (!document || !document->contains("progression") ||
      !document->contains("chapter_count") ||
      !(*document)["chapter_count"].is_number_unsigned()) {
    if (document) setError(error, "The segmentation-plan fields are invalid.");
    return false;
  }
  const std::optional<std::string> progression =
      normalizedGeneratedText((*document)["progression"],
                              kMaximumAutomaticOverviewBytes,
                              "timeline progression", error);
  if (!progression || wordCount(*progression) > 100) {
    if (progression) {
      setError(error, "The generated timeline progression is not concise.");
    }
    return false;
  }
  const std::size_t count = (*document)["chapter_count"].get<std::size_t>();
  if (count < kMinimumAutomaticChapterCount || count > maximumChapters) {
    setError(error, "The generated chapter count is outside policy.");
    return false;
  }
  plan->progression = *progression;
  plan->chapterCount = count;
  return true;
}

std::string generatedChangePointScoreGrammar() {
  return "root ::= \"{\" ws \"\\\"score\\\"\" ws \":\" ws score ws \"}\" ws\n"
         "score ::= " +
         integerAlternatives(0, 100) + "\n" + stringRules();
}

bool parseGeneratedChangePointScore(std::string_view json,
                                    GeneratedChangePointScore* score,
                                    std::string* error) {
  if (error) error->clear();
  if (!score) {
    setError(error, "The change-point score destination is missing.");
    return false;
  }
  const std::optional<nlohmann::json> document = parseObject(json, 1, error);
  if (!document || !document->contains("score") ||
      !(*document)["score"].is_number_unsigned()) {
    if (document) setError(error, "The change-point fields are invalid.");
    return false;
  }
  const std::uint32_t parsed = (*document)["score"].get<std::uint32_t>();
  if (parsed > 100) {
    setError(error, "The generated change-point score is outside policy.");
    return false;
  }
  score->score = parsed;
  return true;
}

bool selectGeneratedBoundaries(
    const std::vector<GeneratedChangePointScore>& scores,
    std::size_t chapterCount, std::vector<std::size_t>* startFrames,
    std::string* error) {
  if (error) error->clear();
  if (!startFrames || scores.empty() ||
      scores.size() + 1 > kMaximumAutomaticChapterCount ||
      chapterCount < kMinimumAutomaticChapterCount ||
      chapterCount > scores.size() + 1) {
    setError(error, "The change-point selection dimensions are invalid.");
    return false;
  }

  // Boundary localization models conventionally produce one confidence per
  // candidate. Choose the maximum-confidence fixed-cardinality partition.
  // When quantized confidences tie, use only a structural prior: the lower
  // squared deviation from evenly covered sample cells wins. Semantic score
  // always remains the primary objective.
  const std::size_t required = chapterCount - 1;
  const std::uint32_t limit = std::uint32_t{1} << scores.size();
  bool found = false;
  std::uint64_t bestSemanticScore = 0;
  std::uint64_t bestBalancePenalty = 0;
  std::vector<std::size_t> selected;
  for (std::uint32_t mask = 0; mask < limit; ++mask) {
    std::size_t selectedCount = 0;
    for (std::uint32_t bits = mask; bits; bits >>= 1u) {
      selectedCount += bits & 1u;
    }
    if (selectedCount != required) continue;

    std::uint64_t semanticScore = 0;
    std::uint64_t balancePenalty = 0;
    std::size_t previousStart = 1;
    std::vector<std::size_t> candidate;
    candidate.reserve(required);
    for (std::size_t index = 0; index < scores.size(); ++index) {
      if ((mask & (std::uint32_t{1} << index)) == 0) continue;
      semanticScore += scores[index].score;
      const std::size_t start = index + 2;
      const std::int64_t scaledWidth =
          static_cast<std::int64_t>((start - previousStart) * chapterCount);
      const std::int64_t delta =
          scaledWidth - static_cast<std::int64_t>(scores.size() + 1);
      balancePenalty += static_cast<std::uint64_t>(delta * delta);
      previousStart = start;
      candidate.push_back(index);
    }
    const std::int64_t scaledWidth = static_cast<std::int64_t>(
        (scores.size() + 2 - previousStart) * chapterCount);
    const std::int64_t delta =
        scaledWidth - static_cast<std::int64_t>(scores.size() + 1);
    balancePenalty += static_cast<std::uint64_t>(delta * delta);

    if (!found || semanticScore > bestSemanticScore ||
        (semanticScore == bestSemanticScore &&
         balancePenalty < bestBalancePenalty)) {
      found = true;
      bestSemanticScore = semanticScore;
      bestBalancePenalty = balancePenalty;
      selected = std::move(candidate);
    }
  }
  if (!found) {
    setError(error, "The change-point partition could not be ranked.");
    return false;
  }
  for (const std::size_t index : selected) {
    if (scores[index].score != 0) continue;
    setError(error,
             "The chapter plan requires boundaries without semantic "
             "change evidence.");
    return false;
  }

  std::vector<std::size_t> starts = {1};
  starts.reserve(chapterCount);
  for (const std::size_t scoreIndex : selected) {
    starts.push_back(scoreIndex + 2);
  }
  *startFrames = std::move(starts);
  return true;
}

std::string generatedChapterMetadataGrammar() {
  return "root ::= \"{\" ws \"\\\"title\\\"\" ws \":\" ws title \",\" "
         "ws \"\\\"summary\\\"\" ws \":\" ws summary \"}\" ws\n"
         "title ::= \"\\\"\" string-char{1,80} \"\\\"\" ws\n"
         "summary ::= \"\\\"\" string-char{1,600} \"\\\"\" ws\n" +
         stringRules();
}

bool parseGeneratedChapterMetadata(std::string_view json,
                                   GeneratedChapterMetadata* metadata,
                                   std::string* error) {
  if (error) error->clear();
  if (!metadata) {
    setError(error, "The chapter-metadata destination is missing.");
    return false;
  }
  const std::optional<nlohmann::json> document = parseObject(json, 2, error);
  if (!document || !document->contains("title") ||
      !document->contains("summary")) {
    if (document) setError(error, "The chapter metadata fields are missing.");
    return false;
  }
  const std::optional<std::string> title = normalizedGeneratedText(
      (*document)["title"], kMaximumAutomaticTitleBytes, "chapter title",
      error);
  if (!title) return false;
  const std::size_t titleWords = wordCount(*title);
  if (titleWords > 8) {
    setError(error, "The generated chapter title is not a concise phrase.");
    return false;
  }
  const std::optional<std::string> summary = normalizedGeneratedText(
      (*document)["summary"], kMaximumAutomaticSummaryBytes,
      "chapter summary", error);
  if (!summary) return false;
  if (!requireCompleteText(*summary, 40, "chapter summary", error)) {
    return false;
  }
  metadata->title = *title;
  metadata->summary = *summary;
  return true;
}

std::string generatedOverviewGrammar() {
  return "root ::= \"{\" ws \"\\\"overview\\\"\" ws \":\" ws "
         "\"\\\"\" string-char{1,600} \"\\\"\" ws \"}\" ws\n" +
         stringRules();
}

bool parseGeneratedOverview(std::string_view json, std::string* overview,
                            std::string* error) {
  if (error) error->clear();
  if (!overview) {
    setError(error, "The overview destination is missing.");
    return false;
  }
  const std::optional<nlohmann::json> document = parseObject(json, 1, error);
  if (!document || !document->contains("overview")) {
    if (document) setError(error, "The overview field is missing.");
    return false;
  }
  const std::optional<std::string> parsed = normalizedGeneratedText(
      (*document)["overview"], kMaximumAutomaticOverviewBytes, "overview",
      error);
  if (!parsed) return false;
  if (!requireCompleteText(*parsed, 50, "overview", error)) return false;
  *overview = *parsed;
  return true;
}

AnalysisResult materializeGeneratedDocument(
    const GeneratedDocument& document, std::int64_t durationUs,
    const std::vector<std::int64_t>& sampleTimesUs) {
  if (durationUs <= 0 || sampleTimesUs.size() < kMinimumAutomaticChapterCount ||
      sampleTimesUs.size() > kMaximumAutomaticChapterCount ||
      document.chapters.size() < kMinimumAutomaticChapterCount ||
      document.chapters.size() > sampleTimesUs.size()) {
    return invalid("The generated chapter artifact has invalid dimensions.");
  }
  if (sampleTimesUs.front() != 0) {
    return invalid("The sampled chapter evidence has no video-start frame.");
  }
  for (std::size_t index = 0; index < sampleTimesUs.size(); ++index) {
    if (sampleTimesUs[index] < 0 || sampleTimesUs[index] >= durationUs ||
        (index > 0 && sampleTimesUs[index] <= sampleTimesUs[index - 1])) {
      return invalid("The sampled chapter evidence is not chronological.");
    }
  }

  AnalysisResult result;
  result.overview = document.overview;
  result.chapters.reserve(document.chapters.size());
  std::size_t previousFrame = 0;
  for (std::size_t index = 0; index < document.chapters.size(); ++index) {
    const GeneratedChapter& generated = document.chapters[index];
    if (generated.startFrame == 0 ||
        generated.startFrame > sampleTimesUs.size() ||
        (index == 0 && generated.startFrame != 1) ||
        (index > 0 && generated.startFrame <= previousFrame)) {
      return invalid(
          "Generated chapter frames must begin at one and strictly increase.");
    }
    Chapter chapter;
    chapter.id = static_cast<std::uint64_t>(index + 1);
    chapter.startUs = sampleTimesUs[generated.startFrame - 1];
    chapter.title = generated.metadata.title;
    chapter.summary = generated.metadata.summary;
    result.chapters.push_back(std::move(chapter));
    previousFrame = generated.startFrame;
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

}  // namespace playback_video_chapters
