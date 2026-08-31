#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "playback/video/transcript/cue.h"

namespace playback_video_analysis {

// Resolves optional transcript evidence for scene analysis. No sidecar is a
// valid visual-only input; once a sidecar exists, an unreadable document is a
// hard error so callers never cache a silently degraded result.
bool loadSceneTranscriptEvidence(
    const std::filesystem::path& transcriptPath,
    std::vector<playback_video_transcript::Segment>* segments,
    std::string* error);

}  // namespace playback_video_analysis
