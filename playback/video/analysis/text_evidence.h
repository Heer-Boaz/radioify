#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class SubtitleManager;
struct SubtitleTrack;

namespace playback_video_analysis {

struct TextCue {
  std::int64_t startUs = 0;
  std::int64_t endUs = 0;
  std::string text;
};

struct TextEvidence {
  std::string language;
  std::string label;
  // Content-addressed cache input over the exact timestamped text consumed by
  // the planner. Storage paths, mtimes and presentation selection are not
  // semantic inference inputs and therefore do not participate.
  std::string identity;
  std::vector<TextCue> cues;
};

// Selects an explicitly English, complete-dialogue text track using container
// disposition, accessibility role and stable track order. Forced,
// signs/songs and commentary tracks are not complete-dialogue evidence; SDH
// remains valid. No language or completeness is guessed from cue contents.
std::optional<TextEvidence>
selectEnglishTextEvidence(const SubtitleManager &subtitles,
                          const std::filesystem::path &videoPath);
std::optional<TextEvidence>
selectEnglishTextEvidence(const std::vector<SubtitleTrack> &tracks,
                          const std::filesystem::path &videoPath);

// Loads Radioify's persisted English ASR artifact independently of subtitle
// presentation state. Editing analysis may publish this sidecar after the
// session's subtitle tracks were discovered, so the analysis owner must not
// depend on a UI-manager reload to consume its own prerequisite.
// Like other subtitle documents, the SRT is associated by filename and read
// as stored. A different recognition model does not invalidate a document.
std::optional<TextEvidence>
loadGeneratedEnglishTextEvidence(const std::filesystem::path &videoPath,
                                 std::string *error = nullptr);

} // namespace playback_video_analysis
