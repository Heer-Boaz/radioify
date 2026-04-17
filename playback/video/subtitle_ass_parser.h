#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "subtitle_manager.h"

struct AssTransformInfo {
  int startMs = 0;
  int endMs = 0;
  float accel = 1.0f;
  float fontSize = 0.0f;
  float scaleX = 0.0f;
  float scaleY = 0.0f;
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

struct AssOverrideInfo {
  int alignment = -1;
  bool hasPosition = false;
  float posX = 0.0f;
  float posY = 0.0f;
  bool hasMove = false;
  float moveStartX = 0.0f;
  float moveStartY = 0.0f;
  float moveEndX = 0.0f;
  float moveEndY = 0.0f;
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
  float clipX1 = 0.0f;
  float clipY1 = 0.0f;
  float clipX2 = 0.0f;
  float clipY2 = 0.0f;
  float fontSize = 0.0f;
  float scaleX = 0.0f;
  float scaleY = 0.0f;
  bool hasFontName = false;
  std::string fontName;
  bool hasBold = false;
  bool bold = false;
  bool hasItalic = false;
  bool italic = false;
  bool hasUnderline = false;
  bool underline = false;
  bool hasPrimaryColor = false;
  bool hasPrimaryAlpha = false;
  uint8_t primaryR = 255;
  uint8_t primaryG = 255;
  uint8_t primaryB = 255;
  float primaryAlpha = 1.0f;
  bool hasBackColor = false;
  bool hasBackAlpha = false;
  uint8_t backR = 0;
  uint8_t backG = 0;
  uint8_t backB = 0;
  float backAlpha = 0.55f;
  std::vector<AssTransformInfo> transforms;
};

int clampAssAlignment(int alignment);

bool parseAssColorValue(std::string_view token, uint8_t* outR, uint8_t* outG,
                        uint8_t* outB, float* outAlpha);
bool parseAssAlphaValue(std::string_view token, int* outAlpha);

void appendSubtitleTransforms(const std::vector<AssTransformInfo>& source,
                              float baseFontSize, SubtitleCue* cue);
void parseAssOverridesFromText(const std::string& rawText,
                               AssOverrideInfo* out);

std::string stripSubtitleMarkup(const std::string& in);
