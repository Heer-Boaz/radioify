#include "playback/video/transcript/cue_semantics.h"

#include <string>

namespace playback_video_transcript {
namespace {

bool hasTranscriptContent(const std::string& text) {
  for (const unsigned char byte : text) {
    if (byte >= 0x80 || (byte >= '0' && byte <= '9') ||
        (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) {
      return true;
    }
  }
  return false;
}

bool isSilencePlaceholder(const std::string& text) {
  std::string lowercase;
  lowercase.reserve(text.size());
  for (const unsigned char byte : text) {
    lowercase.push_back(static_cast<char>(
        byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte));
  }
  return lowercase == "[blank_audio]" || lowercase == "[blank audio]" ||
         lowercase == "(blank audio)" || lowercase == "[silence]" ||
         lowercase == "(silence)";
}

}  // namespace

std::string normalizeTranscriptCueText(const std::string& text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool pendingSpace = false;
  for (const unsigned char byte : text) {
    const bool asciiWhitespace =
        byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' ||
        byte == '\f' || byte == '\v';
    if (asciiWhitespace) {
      pendingSpace = !normalized.empty();
      continue;
    }
    if (pendingSpace) {
      normalized.push_back(' ');
      pendingSpace = false;
    }
    normalized.push_back(static_cast<char>(byte));
  }
  return normalized;
}

bool isMeaningfulTranscriptText(const std::string& text) {
  const std::string normalized = normalizeTranscriptCueText(text);
  return !normalized.empty() && hasTranscriptContent(normalized) &&
         !isSilencePlaceholder(normalized);
}

bool isTranscriptSoundAnnotation(const std::string& text) {
  const std::string normalized = normalizeTranscriptCueText(text);
  return normalized.size() >= 2 && normalized.front() == '[' &&
         normalized.back() == ']';
}

}  // namespace playback_video_transcript
