#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playback_video_chapters {

// Non-owning input to Chapter-Llama's published timestamped-text protocol.
// Callers retain the backing strings for the duration of a build call.
struct ChapterPromptEvidence {
  std::int64_t timeUs = 0;
  std::string_view text;
};

// Builds the exact HwwwH/MiniCPM-V-2 chat turn used by Chapter-Llama's
// published frame-caption extractor. The caller supplies llama.cpp's image
// marker in place of the extractor's (<image>./</image>) placeholder.
std::optional<std::string>
buildChapterLlamaMiniCpmV2CaptionPrompt(std::string_view imageMarker,
                                       std::size_t maximumBytes);

// Builds the exact single-modality frame-selector prompt used by PromptASR.
std::optional<std::string> buildChapterLlamaSpeechPrompt(
    std::int64_t durationUs,
    const std::vector<ChapterPromptEvidence> &asrEvidence,
    std::size_t maximumBytes);

// Builds the exact interleaved PromptCaptionsASR input. The reference
// implementation performs a stable sort over ASR followed by Caption records,
// so ASR deliberately precedes Caption when timestamps are equal.
std::optional<std::string> buildChapterLlamaCaptionAsrPrompt(
    std::int64_t durationUs,
    const std::vector<ChapterPromptEvidence> &captionEvidence,
    const std::vector<ChapterPromptEvidence> &asrEvidence,
    std::size_t maximumBytes);

// Prevents media-derived text from becoming a Llama control-token delimiter.
// Ordinary transcript and caption text remains byte-for-byte unchanged.
std::string escapeChapterLlamaEvidence(std::string_view text);

} // namespace playback_video_chapters
