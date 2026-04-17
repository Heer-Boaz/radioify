#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "subtitle_font_attachments.h"

enum class SubtitleAssRenderStatus {
  NoGlyph,
  WithGlyph,
  Error,
};

struct SubtitleAssRenderResult {
  SubtitleAssRenderStatus status = SubtitleAssRenderStatus::NoGlyph;
  std::string errorMessage;
};

SubtitleAssRenderResult renderAssSubtitlesToBgraCanvas(
    const std::shared_ptr<const std::string>& assScript,
    const std::shared_ptr<const SubtitleFontAttachmentList>& assFonts,
    int64_t clockUs, int canvasW, int canvasH,
    std::vector<uint8_t>* outCanvas);
