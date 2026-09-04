#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class SubtitleManager;
struct SubtitleTrack;

namespace playback_video_chapters {

struct TextCue {
  std::int64_t startUs = 0;
  std::int64_t endUs = 0;
  std::string text;
};

struct TextEvidence {
  std::string language;
  std::string label;
  // Stable cache input describing the exact sidecar or embedded stream and
  // its containing media identity. It deliberately does not depend on the
  // currently selected presentation track.
  std::string identity;
  std::vector<TextCue> cues;
};

// Selects the best complete English text track. Full dialogue is preferred
// over forced/signs/songs/commentary tracks; SDH remains a valid fallback.
// No language is guessed from dialogue text.
std::optional<TextEvidence>
selectEnglishTextEvidence(const SubtitleManager &subtitles,
                          const std::filesystem::path &videoPath);
std::optional<TextEvidence>
selectEnglishTextEvidence(const std::vector<SubtitleTrack> &tracks,
                          const std::filesystem::path &videoPath);

// Loads Radioify's persisted English ASR artifact independently of subtitle
// presentation state. Chapter analysis may publish this sidecar after the
// session's subtitle tracks were discovered, so the analysis owner must not
// depend on a UI-manager reload to consume its own prerequisite.
std::optional<TextEvidence>
loadGeneratedEnglishTextEvidence(const std::filesystem::path &videoPath,
                                 const std::string &producerIdentity,
                                 std::string *error = nullptr);

// Returns bounded dialogue around a sampled time for the VLM prompt.
std::string textNear(const TextEvidence &evidence, std::int64_t centerUs,
                     std::int64_t radiusUs, std::size_t maxBytes);

// Assigns each cue by its midpoint to one half-open timeline interval. This is
// used for sampled video evidence so adjacent samples never receive the same
// broad transcript window.
std::string textInInterval(const TextEvidence &evidence, std::int64_t startUs,
                           std::int64_t endUs, std::size_t maxBytes);

} // namespace playback_video_chapters
