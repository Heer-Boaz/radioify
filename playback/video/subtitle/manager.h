#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "font_attachments.h"

struct SubtitleTextRun {
  std::string text;
  bool hasPrimaryColor = false;
  uint8_t primaryR = 255;
  uint8_t primaryG = 255;
  uint8_t primaryB = 255;
  float primaryAlpha = 1.0f;
  bool hasBackColor = false;
  uint8_t backR = 0;
  uint8_t backG = 0;
  uint8_t backB = 0;
  float backAlpha = 0.55f;
};

struct SubtitleTransform {
  int startMs = 0;
  int endMs = 0;
  float accel = 1.0f;
  bool hasSizeScale = false;
  float sizeScale = 1.0f;
  bool hasScaleX = false;
  float scaleX = 1.0f;
  bool hasScaleY = false;
  float scaleY = 1.0f;
  bool hasPrimaryColor = false;
  uint8_t primaryR = 255;
  uint8_t primaryG = 255;
  uint8_t primaryB = 255;
  bool hasPrimaryAlpha = false;
  float primaryAlpha = 1.0f;
  bool hasBackColor = false;
  uint8_t backR = 0;
  uint8_t backG = 0;
  uint8_t backB = 0;
  bool hasBackAlpha = false;
  float backAlpha = 0.55f;
};

struct SubtitleCue {
  int64_t startUs = 0;
  int64_t endUs = 0;
  std::string text;
  std::string rawText;
  std::vector<SubtitleTextRun> textRuns;
  std::vector<SubtitleTransform> transforms;
  int layer = 0;
  int alignment = 2;  // ASS grid: 1..9, default bottom-center.
  float sizeScale = 1.0f;
  float scaleX = 1.0f;
  float scaleY = 1.0f;
  std::string fontName;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool assStyled = false;
  bool hasPrimaryColor = false;
  uint8_t primaryR = 255;
  uint8_t primaryG = 255;
  uint8_t primaryB = 255;
  float primaryAlpha = 1.0f;
  bool hasBackColor = false;
  uint8_t backR = 0;
  uint8_t backG = 0;
  uint8_t backB = 0;
  float backAlpha = 0.55f;
  bool hasPosition = false;
  float posXNorm = 0.5f;
  float posYNorm = 0.9f;
  bool hasMove = false;
  float moveStartXNorm = 0.5f;
  float moveStartYNorm = 0.9f;
  float moveEndXNorm = 0.5f;
  float moveEndYNorm = 0.9f;
  int moveStartMs = 0;
  int moveEndMs = 0;
  bool hasSimpleFade = false;
  int fadeInMs = 0;
  int fadeOutMs = 0;
  bool hasComplexFade = false;
  int fadeAlpha1 = 0;
  int fadeAlpha2 = 0;
  int fadeAlpha3 = 0;
  int fadeT1Ms = 0;
  int fadeT2Ms = 0;
  int fadeT3Ms = 0;
  int fadeT4Ms = 0;
  bool hasClip = false;
  bool inverseClip = false;
  float clipX1Norm = 0.0f;
  float clipY1Norm = 0.0f;
  float clipX2Norm = 1.0f;
  float clipY2Norm = 1.0f;
  float marginVNorm = 0.0f;
  float marginLNorm = 0.0f;
  float marginRNorm = 0.0f;
};

struct SubtitleTrack {
  enum class SourceKind : uint8_t {
    Sidecar,
    Embedded,
  };

  std::string label;
  std::filesystem::path sourcePath;
  SourceKind sourceKind = SourceKind::Sidecar;
  // Normalized lower-case primary language subtag when the container or
  // sidecar name declares one (for example "en" for en-US or eng).
  std::string language;
  int embeddedStreamIndex = -1;
  bool textTrack = true;
  bool forced = false;
  bool hearingImpaired = false;
  bool commentary = false;
  bool signsOrSongs = false;
  bool defaultDisposition = false;
  std::vector<SubtitleCue> cues;
  std::shared_ptr<const std::string> assScript;
  std::shared_ptr<const SubtitleFontAttachmentList> assFonts;
  mutable size_t lastCueIndex = 0;

  void cuesAt(int64_t clockUs, std::vector<const SubtitleCue*>* out) const;
  const SubtitleCue* cueAt(int64_t clockUs) const;
  void resetLookup() const;
};

class SubtitleManager {
 public:
  using CancellationCheck = std::function<bool()>;

  void loadForVideo(const std::filesystem::path& videoPath,
                    const CancellationCheck& cancellation = {});

  size_t trackCount() const;
  size_t selectableTrackCount() const;
  bool selectFirstTrackWithCues();
  bool selectTrackForFile(const std::filesystem::path& sourcePath);
  size_t activeTrackIndex() const;
  const SubtitleTrack* activeTrack() const;
  const std::vector<SubtitleTrack>& tracks() const { return tracks_; }
  std::string activeTrackLabel() const;
  bool isActiveLastCueTrack() const;
  bool cycleLanguage();

 private:
  std::string makeUniqueLabel(const std::string& rawLabel) const;

  std::vector<SubtitleTrack> tracks_;
  size_t activeTrack_ = 0;
};
