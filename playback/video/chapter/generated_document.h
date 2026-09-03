#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "playback/video/chapter/backend.h"

namespace playback_video_chapters {

// Generated only after chapter planning has established a bounded chapter.
// The model describes that immutable interval; it never proposes, repairs,
// sorts, or removes timeline boundaries in this stage.
struct GeneratedChapterMetadata {
  std::string title;
  std::string summary;
};

struct GeneratedChapterPlanEntry {
  std::int64_t startUs = 0;
};

struct GeneratedChapter {
  std::int64_t startUs = 0;
  std::string title;
  std::string summary;
};

// Complete private inference artifact. It is not publishable until
// materializeGeneratedDocument validates model-selected sampler-owned
// timestamps and the exact timeline partition.
struct GeneratedDocument {
  std::string overview;
  std::vector<GeneratedChapter> chapters;
};

std::string generatedFrameCaptionsGrammar(std::size_t captionCount);
bool parseGeneratedFrameCaptions(std::string_view json,
                                 std::size_t expectedCount,
                                 std::vector<std::string> *captions,
                                 std::string *error = nullptr);

// Parses Chapter-Llama's native, training-aligned `hh:mm:ss - Title` output.
// The model is trained to emit approximate timestamps, so each in-range time
// is deterministically snapped to the nearest sampler-owned interval. An
// optional matching ordinal emitted by the adapter is presentation syntax and
// is removed; ordering and range violations are rejected rather than sorted.
bool parseChapterLlamaPlan(std::string_view output, std::int64_t durationUs,
                           const std::vector<std::int64_t> &boundaryAnchorsUs,
                           std::vector<GeneratedChapterPlanEntry> *plan,
                           std::string *error = nullptr);

std::string generatedChapterMetadataGrammar();
bool parseGeneratedChapterMetadata(std::string_view json,
                                   GeneratedChapterMetadata *metadata,
                                   std::string *error = nullptr);

std::string generatedOverviewGrammar();
bool parseGeneratedOverview(std::string_view json, std::string *overview,
                            std::string *error = nullptr);

// Maps selected interval identities to dense-scan-owned boundary timestamps.
// Invalid model output is rejected, never sorted, deduplicated, truncated or
// repaired.
AnalysisResult materializeGeneratedDocument(
    const GeneratedDocument &document, std::int64_t durationUs,
    const std::vector<std::int64_t> &boundaryAnchorsUs);

} // namespace playback_video_chapters
