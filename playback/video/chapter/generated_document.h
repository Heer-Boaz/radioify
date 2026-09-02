#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "playback/video/chapter/backend.h"

namespace playback_video_chapters {

struct GeneratedChapterMetadata {
  std::string title;
  std::string summary;
};

struct GeneratedChapter {
  std::size_t startFrame = 0;
  GeneratedChapterMetadata metadata;
};

// Private global planning result. The progression makes the model reason over
// the complete timeline before it commits to a bounded chapter count; neither
// field is published directly.
struct GeneratedSegmentationPlan {
  std::string progression;
  std::size_t chapterCount = 0;
};

struct GeneratedChangePointScore {
  std::uint32_t score = 0;
};

// Complete private inference artifact. It is not publishable until
// materializeGeneratedDocument maps model-selected frame identities onto the
// sampler-owned timeline and validates the resulting partition.
struct GeneratedDocument {
  std::string overview;
  std::vector<GeneratedChapter> chapters;
};

std::string generatedObservationGrammar();
bool parseGeneratedObservation(std::string_view json, std::string* observation,
                               std::string* error = nullptr);

std::string generatedSegmentationPlanGrammar(std::size_t maximumChapters);
bool parseGeneratedSegmentationPlan(std::string_view json,
                                    std::size_t maximumChapters,
                                    GeneratedSegmentationPlan* plan,
                                    std::string* error = nullptr);

std::string generatedChangePointScoreGrammar();
bool parseGeneratedChangePointScore(std::string_view json,
                                    GeneratedChangePointScore* score,
                                    std::string* error = nullptr);
bool selectGeneratedBoundaries(
    const std::vector<GeneratedChangePointScore>& scores,
    std::size_t chapterCount, std::vector<std::size_t>* startFrames,
    std::string* error = nullptr);

std::string generatedChapterMetadataGrammar();
bool parseGeneratedChapterMetadata(std::string_view json,
                                   GeneratedChapterMetadata* metadata,
                                   std::string* error = nullptr);

std::string generatedOverviewGrammar();
bool parseGeneratedOverview(std::string_view json, std::string* overview,
                            std::string* error = nullptr);

// Maps selected frame identities to authoritative sample timestamps. Invalid
// model output is rejected, never sorted, deduplicated, truncated or repaired.
AnalysisResult materializeGeneratedDocument(
    const GeneratedDocument& document, std::int64_t durationUs,
    const std::vector<std::int64_t>& sampleTimesUs);

}  // namespace playback_video_chapters
