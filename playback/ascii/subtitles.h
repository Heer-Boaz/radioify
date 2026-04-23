#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "asciiart.h"
#include "consolescreen.h"
#include "subtitle_font_attachments.h"
#include "videowindow.h"

namespace playback_ascii_subtitles {

struct RenderInput {
  ConsoleScreen* screen = nullptr;
  const AsciiArt* art = nullptr;
  int width = 0;
  int height = 0;
  int maxHeight = 0;
  int artTop = 0;
  bool allowFrame = false;
  bool overlayVisible = false;
  int overlayReservedLines = 0;
  std::string subtitleText;
  const std::vector<WindowUiState::SubtitleCue>* subtitleCues = nullptr;
  std::shared_ptr<const std::string> assScript;
  std::shared_ptr<const SubtitleFontAttachmentList> assFonts;
  int64_t subtitleClockUs = 0;
  Style baseStyle{};
  Style accentStyle{};
  Style dimStyle{};
};

void renderAsciiSubtitles(const RenderInput& input);

}  // namespace playback_ascii_subtitles
