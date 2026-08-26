#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace playback_video_transcript {

// Radioify keeps whisper.cpp's public enum behind the engine boundary. Model
// selection owns this value; the engine only translates the validated preset
// when it creates the Whisper context.
enum class WhisperAlignmentPreset : uint8_t {
  None,
  TinyEn,
  Tiny,
  BaseEn,
  Base,
  SmallEn,
  Small,
  MediumEn,
  Medium,
  LargeV1,
  LargeV2,
  LargeV3,
  LargeV3Turbo,
};

// Parses whisper.cpp's documented DTW preset names. std::nullopt means the
// configuration value is invalid; "none" deliberately disables DTW.
std::optional<WhisperAlignmentPreset> parseWhisperAlignmentPreset(
    std::string_view value);

}  // namespace playback_video_transcript
