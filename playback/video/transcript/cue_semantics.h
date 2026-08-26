#pragma once

#include <string>

namespace playback_video_transcript {

std::string normalizeTranscriptCueText(const std::string& text);

// Rejects empty/model-placeholder cues before they can reach a sidecar or
// downstream analysis.
bool isMeaningfulTranscriptText(const std::string& text);

// True only for an entire square-bracketed SDH/sound cue. Parentheses are not
// classified here: Whisper also uses them for spoken foreign-language text,
// so treating every parenthetical cue as non-speech loses real dialogue.
bool isTranscriptSoundAnnotation(const std::string& text);

}  // namespace playback_video_transcript
