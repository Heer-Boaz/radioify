#include "playback/video/transcript/whisper_model_config.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace playback_video_transcript {

std::optional<WhisperAlignmentPreset> parseWhisperAlignmentPreset(
    std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char byte) {
                   return static_cast<char>(std::tolower(byte));
                 });
  if (normalized == "none" || normalized == "off") {
    return WhisperAlignmentPreset::None;
  }
  if (normalized == "tiny.en") return WhisperAlignmentPreset::TinyEn;
  if (normalized == "tiny") return WhisperAlignmentPreset::Tiny;
  if (normalized == "base.en") return WhisperAlignmentPreset::BaseEn;
  if (normalized == "base") return WhisperAlignmentPreset::Base;
  if (normalized == "small.en") return WhisperAlignmentPreset::SmallEn;
  if (normalized == "small") return WhisperAlignmentPreset::Small;
  if (normalized == "medium.en") return WhisperAlignmentPreset::MediumEn;
  if (normalized == "medium") return WhisperAlignmentPreset::Medium;
  if (normalized == "large.v1" || normalized == "large-v1") {
    return WhisperAlignmentPreset::LargeV1;
  }
  if (normalized == "large.v2" || normalized == "large-v2") {
    return WhisperAlignmentPreset::LargeV2;
  }
  if (normalized == "large.v3" || normalized == "large-v3") {
    return WhisperAlignmentPreset::LargeV3;
  }
  if (normalized == "large.v3.turbo" ||
      normalized == "large-v3-turbo") {
    return WhisperAlignmentPreset::LargeV3Turbo;
  }
  return std::nullopt;
}

std::optional<std::string> normalizeWhisperLanguageOverride(
    std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char byte) {
                   return static_cast<char>(std::tolower(byte));
                 });
  if (normalized.empty() || normalized == "auto") return std::string{};
  if (normalized.size() != 2 ||
      std::any_of(normalized.begin(), normalized.end(), [](char byte) {
        return byte < 'a' || byte > 'z';
      })) {
    return std::nullopt;
  }
  return normalized;
}

}  // namespace playback_video_transcript
