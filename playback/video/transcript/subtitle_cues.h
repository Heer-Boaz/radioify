#pragma once

#include <vector>

#include "playback/video/transcript/cue.h"
#include "playback/video/transcript/whisper_engine.h"

namespace playback_video_transcript {

// Converts word-aligned inference output into speech-only presentation cues.
// Recognition owns token timing; this policy owns readable grouping, sound-
// annotation exclusion, and a bounded display tail. It never invents speech
// positions from text length.
std::vector<Segment> buildSubtitleCues(
    const std::vector<RecognizedSegment>& recognition);

// Resolves chunk-overlap seams after all recognized chunks have been merged.
// Cue ordering is stable and simultaneous text is preserved without allowing
// an older cue to remain visible underneath its successor.
void finalizeSubtitleCueTimeline(std::vector<Segment>* cues);

}  // namespace playback_video_transcript
