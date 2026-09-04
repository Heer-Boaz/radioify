#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "playback/video/chapter/backend.h"

namespace playback_video_chapters {

struct GeneratedChapter {
  std::int64_t startUs = 0;
  std::string title;
};

using GeneratedChapterPlanEntry = GeneratedChapter;

// Complete private inference artifact. It is not publishable until
// materializeGeneratedDocument validates the model-selected timestamps and
// the exact timeline partition.
struct GeneratedDocument {
  std::vector<GeneratedChapter> chapters;
};

// Normalizes the unconstrained observation returned by the independently
// invoked visual captioner before it enters the bounded Chapter-Llama prompt.
bool normalizeGeneratedFrameCaption(std::string_view text,
                                    std::string *caption,
                                    std::string *error = nullptr);

// Parses Chapter-Llama's native, training-aligned `hh:mm:ss - Title` output.
// Timestamps are planner output, not image identities: the published pipeline
// does not snap them to sampled frames or to a separate cut detector. Ordering,
// range, and non-protocol text violations are rejected rather than repaired.
bool parseChapterLlamaPlan(std::string_view output, std::int64_t durationUs,
                           std::vector<GeneratedChapterPlanEntry> *plan,
                           std::string *error = nullptr);

// Validates planner timestamps and builds the exact source partition. Invalid
// model output is rejected, never sorted, deduplicated, truncated or repaired.
AnalysisResult materializeGeneratedDocument(
    const GeneratedDocument &document, std::int64_t durationUs);

} // namespace playback_video_chapters
