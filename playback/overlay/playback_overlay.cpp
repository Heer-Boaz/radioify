#include "playback_overlay.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include "playback_subtitle_effects.h"
#include "ui_helpers.h"

namespace playback_overlay {
namespace {

int countVisibleChars(const std::string& text) {
  std::string filtered;
  filtered.reserve(text.size());
  for (char c : text) {
    if (c == '\r' || c == '\n') continue;
    filtered.push_back(c);
  }
  return utf8DisplayWidth(filtered);
}

std::string fitCellText(const std::string& text, int width) {
  if (width <= 0) return {};
  std::string filtered;
  filtered.reserve(text.size());
  for (char c : text) {
    if (c == '\r' || c == '\n') continue;
    filtered.push_back(c);
  }

  const int displayWidth = utf8DisplayWidth(filtered);
  if (displayWidth > width) {
    return utf8TakeDisplayWidth(filtered, width);
  }
  if (displayWidth < width) {
    filtered.append(static_cast<size_t>(width - displayWidth), ' ');
  }
  return filtered;
}

struct PendingOverlayCellControl {
  std::string text;
  int controlIndex = -1;
  int x = 0;
  int line = 0;
  int width = 0;
  bool active = false;
  bool hovered = false;
};

std::vector<PendingOverlayCellControl> wrapOverlayControls(
    const std::vector<OverlayCellControlInput>& controls, int width,
    int* outLineCount) {
  const int contentInset = width > 2 ? 1 : 0;
  const int maxLineWidth = std::max(1, width - contentInset * 2);
  std::vector<PendingOverlayCellControl> out;
  out.reserve(controls.size());

  int cursor = 0;
  int line = 0;
  for (const OverlayCellControlInput& control : controls) {
    const int controlWidth =
        std::min(maxLineWidth,
                 std::max(1, control.width > 0
                                  ? control.width
                                  : utf8DisplayWidth(control.text)));
    const int gap = cursor > 0 ? 2 : 0;
    if (cursor > 0 && cursor + gap + controlWidth > maxLineWidth) {
      ++line;
      cursor = 0;
    } else {
      cursor += gap;
    }

    PendingOverlayCellControl item;
    item.text = fitCellText(control.text, controlWidth);
    item.controlIndex = control.controlIndex;
    item.x = contentInset + cursor;
    item.line = line;
    item.width = controlWidth;
    item.active = control.active;
    item.hovered = control.hovered;
    out.push_back(std::move(item));
    cursor += controlWidth;
  }

  if (outLineCount) {
    *outLineCount = out.empty() ? 0 : line + 1;
  }
  return out;
}

std::string overlayTitleWithDebugLines(const std::vector<std::string>& debugLines,
                                       const std::string& title) {
  std::string out;
  for (const std::string& line : debugLines) {
    if (line.empty()) continue;
    if (!out.empty()) out.push_back('\n');
    out += line;
  }
  if (!out.empty()) out.push_back('\n');
  out += " " + title;
  return out;
}

uint32_t overlayGpuRgb(const Color& color) {
  return gpuTextGridRgb(color.r, color.g, color.b);
}

GpuTextGridCell overlayGpuCell(wchar_t ch, const Style& style,
                               uint32_t flags = 0) {
  return GpuTextGridCell{static_cast<uint32_t>(ch),
                         overlayGpuRgb(style.fg),
                         overlayGpuRgb(style.bg), flags};
}

std::wstring overlayUtf8ToWide(const std::string& text) {
  if (text.empty()) return {};

#ifdef _WIN32
  int needed =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                 static_cast<int>(text.size()), nullptr, 0);
  }
  if (needed > 0) {
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const int written =
        MultiByteToWideChar(CP_UTF8, 0, text.data(),
                            static_cast<int>(text.size()), out.data(),
                            needed);
    if (written > 0) {
      out.resize(static_cast<size_t>(written));
      out.erase(std::remove_if(out.begin(), out.end(),
                               [](wchar_t ch) {
                                 return ch == L'\r' || ch == L'\n';
                               }),
                out.end());
      return out;
    }
  }
#endif

  std::wstring out;
  out.reserve(text.size());
  for (unsigned char ch : text) {
    if (ch == '\r' || ch == '\n') continue;
    out.push_back(static_cast<wchar_t>(ch));
  }
  return out;
}
}  // namespace

PlaybackOverlayState buildPlaybackOverlayState(
    const PlaybackOverlayInputs& inputs) {
  PlaybackOverlayState state;
  state.windowTitle = inputs.windowTitle;
  state.audioOk = inputs.audioOk;
  state.playPauseAvailable = inputs.playPauseAvailable;
  state.audioSupports50HzToggle = inputs.audioSupports50HzToggle;
  state.canPlayPrevious = inputs.canPlayPrevious;
  state.canPlayNext = inputs.canPlayNext;
  state.radioEnabled = inputs.radioEnabled;
  state.hz50Enabled = inputs.hz50Enabled;
  state.canCycleAudioTracks = inputs.canCycleAudioTracks;
  state.activeAudioTrackLabel = inputs.activeAudioTrackLabel;
  state.hasSubtitles = inputs.hasSubtitles;
  state.subtitlesEnabled = inputs.subtitlesEnabled;
  state.activeSubtitleLabel =
      (inputs.subtitleManager && inputs.hasSubtitles)
          ? inputs.subtitleManager->activeTrackLabel()
          : "N/A";
  state.subtitleClockUs = inputs.subtitleClockUs;
  state.seekingOverlay = inputs.seekingOverlay;
  state.displaySec = inputs.displaySec;
  state.totalSec = inputs.totalSec;
  state.volPct = inputs.volPct;
  state.overlayVisible = inputs.overlayVisible;
  state.paused = inputs.paused;
  state.audioFinished = inputs.audioFinished;
  state.pictureInPictureAvailable = inputs.pictureInPictureAvailable;
  state.pictureInPictureActive = inputs.pictureInPictureActive;
  state.subtitleRenderError = inputs.subtitleRenderError;
  state.screenWidth = inputs.screenWidth;
  state.screenHeight = inputs.screenHeight;
  state.windowWidth = inputs.windowWidth;
  state.windowHeight = inputs.windowHeight;
  state.artTop = inputs.artTop;
  state.progressBarX = inputs.progressBarX;
  state.progressBarY = inputs.progressBarY;
  state.progressBarWidth = inputs.progressBarWidth;
  state.debugLines = inputs.debugLines;

  if (inputs.subtitleManager) {
    state.subtitleText = buildSubtitleText(*inputs.subtitleManager,
                                           inputs.subtitlesEnabled,
                                           inputs.seekingOverlay,
                                           inputs.subtitleClockUs,
                                           inputs.hasSubtitles);
    state.subtitleCues = collectSubtitleCues(*inputs.subtitleManager,
                                             inputs.subtitlesEnabled,
                                             inputs.seekingOverlay,
                                             inputs.subtitleClockUs,
                                             inputs.hasSubtitles);
    if (inputs.subtitlesEnabled && !inputs.seekingOverlay &&
        inputs.subtitleClockUs >= 0 && inputs.hasSubtitles) {
      if (const SubtitleTrack* activeTrack =
              inputs.subtitleManager->activeTrack()) {
        state.subtitleAssScript = activeTrack->assScript;
        state.subtitleAssFonts = activeTrack->assFonts;
      }
    }
  }

  return state;
}

std::vector<WindowUiState::SubtitleCue> collectSubtitleCues(
    const SubtitleManager& subtitleManager, bool subtitlesEnabled,
    bool seekingOverlay, int64_t clockUs, bool hasSubtitles) {
  std::vector<WindowUiState::SubtitleCue> out;
  if (!subtitlesEnabled || seekingOverlay || clockUs < 0 || !hasSubtitles) {
    return out;
  }
  const SubtitleTrack* activeTrack = subtitleManager.activeTrack();
  if (!activeTrack) {
    return out;
  }

  std::vector<const SubtitleCue*> active;
  activeTrack->cuesAt(clockUs, &active);
  if (active.empty()) return out;
  out.reserve(active.size());
  for (const SubtitleCue* cue : active) {
    if (!cue) continue;
    const float fadeOpacity = subtitleFadeOpacity(*cue, clockUs);
    if (fadeOpacity <= 0.001f) continue;
    const bool hasRenderableAss = cue->assStyled && !cue->rawText.empty();
    if (cue->text.empty() && !hasRenderableAss) continue;
    WindowUiState::SubtitleCue item;
    item.text = cue->text;
    item.rawText = cue->rawText;
    item.textRuns.reserve(cue->textRuns.size());
    for (const SubtitleTextRun& run : cue->textRuns) {
      if (run.text.empty()) continue;
      WindowUiState::SubtitleCue::TextRun itemRun;
      itemRun.text = run.text;
      itemRun.hasPrimaryColor = run.hasPrimaryColor;
      itemRun.primaryColor = Color{run.primaryR, run.primaryG, run.primaryB};
      itemRun.primaryAlpha = run.primaryAlpha;
      itemRun.hasBackColor = run.hasBackColor;
      itemRun.backColor = Color{run.backR, run.backG, run.backB};
      itemRun.backAlpha = run.backAlpha;
      item.textRuns.push_back(std::move(itemRun));
    }
    item.sizeScale = std::clamp(cue->sizeScale, 0.40f, 3.0f);
    item.scaleX = std::clamp(cue->scaleX, 0.40f, 3.5f);
    item.scaleY = std::clamp(cue->scaleY, 0.40f, 3.5f);
    item.fontName = cue->fontName;
    item.bold = cue->bold;
    item.italic = cue->italic;
    item.underline = cue->underline;
    item.assStyled = cue->assStyled;
    item.hasPrimaryColor = cue->hasPrimaryColor;
    item.primaryColor = Color{cue->primaryR, cue->primaryG, cue->primaryB};
    item.primaryAlpha = cue->primaryAlpha;
    item.hasBackColor = cue->hasBackColor;
    item.backColor = Color{cue->backR, cue->backG, cue->backB};
    item.backAlpha = cue->backAlpha;
    item.startUs = cue->startUs;
    item.endUs = cue->endUs;
    item.alignment = cue->alignment;
    item.layer = cue->layer;
    item.hasPosition = cue->hasPosition;
    item.posX = cue->posXNorm;
    item.posY = cue->posYNorm;
    item.hasClip = cue->hasClip;
    item.inverseClip = cue->inverseClip;
    item.clipX1 = cue->clipX1Norm;
    item.clipY1 = cue->clipY1Norm;
    item.clipX2 = cue->clipX2Norm;
    item.clipY2 = cue->clipY2Norm;
    if (cue->hasMove) {
      item.hasPosition = true;
      const double elapsedMs =
          static_cast<double>(std::max<int64_t>(0, clockUs - cue->startUs)) /
          1000.0;
      double t = 0.0;
      if (cue->moveEndMs > cue->moveStartMs) {
        t = (elapsedMs - static_cast<double>(cue->moveStartMs)) /
            static_cast<double>(cue->moveEndMs - cue->moveStartMs);
      } else if (elapsedMs >= static_cast<double>(cue->moveStartMs)) {
        t = 1.0;
      }
      t = std::clamp(t, 0.0, 1.0);
      item.posX =
          static_cast<float>(cue->moveStartXNorm +
                             (cue->moveEndXNorm - cue->moveStartXNorm) * t);
      item.posY =
          static_cast<float>(cue->moveStartYNorm +
                             (cue->moveEndYNorm - cue->moveStartYNorm) * t);
    }
    item.marginVNorm = cue->marginVNorm;
    item.marginLNorm = cue->marginLNorm;
    item.marginRNorm = cue->marginRNorm;
    applySubtitleTimelineEffects(&item, *cue, clockUs, fadeOpacity);
    out.push_back(std::move(item));
  }

  std::stable_sort(out.begin(), out.end(),
                   [](const WindowUiState::SubtitleCue& a,
                      const WindowUiState::SubtitleCue& b) {
                     if (a.layer != b.layer) return a.layer < b.layer;
                     if (a.sizeScale != b.sizeScale)
                       return a.sizeScale > b.sizeScale;
                     return a.text < b.text;
                   });
  return out;
}

std::string buildSubtitleText(const SubtitleManager& subtitleManager,
                             bool subtitlesEnabled, bool seekingOverlay,
                             int64_t clockUs, bool hasSubtitles) {
  if (!subtitlesEnabled || seekingOverlay || clockUs < 0 || !hasSubtitles) {
    return {};
  }
  const SubtitleTrack* activeTrack = subtitleManager.activeTrack();
  if (!activeTrack) {
    return {};
  }
  std::vector<const SubtitleCue*> active;
  activeTrack->cuesAt(clockUs, &active);
  if (active.empty()) return {};

  std::stable_sort(active.begin(), active.end(),
                   [](const SubtitleCue* a, const SubtitleCue* b) {
                     if (!a || !b) return a < b;
                     if (a->layer != b->layer) return a->layer < b->layer;
                     if (a->sizeScale != b->sizeScale)
                       return a->sizeScale > b->sizeScale;
                     if (a->startUs != b->startUs) return a->startUs < b->startUs;
                     return a->text < b->text;
                   });

  std::string merged;
  for (const SubtitleCue* cue : active) {
    if (!cue || cue->text.empty()) continue;
    if (!merged.empty()) merged.push_back('\n');
    merged += cue->text;
  }
  return merged;
}

OverlayControlSpec makeOverlayTextControlSpec(OverlayControlId id,
                                              const std::string& label,
                                              bool active) {
  BracketButtonLabels labels = makeBracketButtonLabels(label);
  OverlayControlSpec spec;
  spec.id = id;
  spec.normalText = std::move(labels.normal);
  spec.hoverText = std::move(labels.hover);
  spec.width = labels.width;
  spec.active = active;
  return spec;
}

std::vector<OverlayCellControlInput> buildOverlayCellControlInputs(
    const std::vector<OverlayControlSpec>& specs, int hoverIndex) {
  std::vector<OverlayCellControlInput> controls;
  controls.reserve(specs.size());
  for (size_t i = 0; i < specs.size(); ++i) {
    const bool hovered = static_cast<int>(i) == hoverIndex;
    OverlayCellControlInput control;
    control.text = hovered ? specs[i].hoverText : specs[i].normalText;
    control.width = specs[i].width;
    control.active = specs[i].active;
    control.hovered = hovered;
    control.controlIndex = static_cast<int>(i);
    controls.push_back(std::move(control));
  }
  return controls;
}

bool dispatchOverlayControl(OverlayControlId id,
                            const OverlayControlActions& actions) {
  auto invoke = [](const std::function<bool()>& action) {
    return action ? action() : false;
  };
  switch (id) {
    case OverlayControlId::Previous:
      return invoke(actions.previous);
    case OverlayControlId::PlayPause:
      return invoke(actions.playPause);
    case OverlayControlId::Next:
      return invoke(actions.next);
    case OverlayControlId::Radio:
      return invoke(actions.radio);
    case OverlayControlId::Hz50:
      return invoke(actions.hz50);
    case OverlayControlId::AudioTrack:
      return invoke(actions.audioTrack);
    case OverlayControlId::Subtitles:
      return invoke(actions.subtitles);
    case OverlayControlId::PictureInPicture:
      return invoke(actions.pictureInPicture);
  }
  return false;
}

std::vector<OverlayControlSpec> buildOverlayControlSpecs(
    const PlaybackOverlayState& state, int hoverIndex,
    const OverlayControlSpecOptions& options) {
  std::vector<OverlayControlSpec> out;
  auto addSpec = [&](OverlayControlSpec spec) {
    out.push_back(std::move(spec));
  };

  if (state.canPlayPrevious) {
    addSpec(makeOverlayTextControlSpec(OverlayControlId::Previous, "<<",
                                       false));
  }
  if (state.playPauseAvailable) {
    addSpec(makeOverlayTextControlSpec(OverlayControlId::PlayPause, "pause",
                                       state.paused));
  }
  if (state.canPlayNext) {
    addSpec(makeOverlayTextControlSpec(OverlayControlId::Next, ">>", false));
  }

  if (options.includeRadio) {
    addSpec(makeOverlayTextControlSpec(OverlayControlId::Radio, "Radio",
                                       state.radioEnabled));
  }

  if (state.audioOk && state.audioSupports50HzToggle) {
    addSpec(makeOverlayTextControlSpec(OverlayControlId::Hz50, "50Hz",
                                       state.hz50Enabled));
  }

  if (options.includeAudioTrack) {
    std::string audioLabel = "Audio: N/A";
    if (state.canCycleAudioTracks && state.audioOk) {
      std::string activeAudio = state.activeAudioTrackLabel;
      if (activeAudio.empty()) activeAudio = "N/A";
      if (utf8DisplayWidth(activeAudio) > 14) {
        activeAudio = utf8TakeDisplayWidth(activeAudio, 14);
      }
      audioLabel = "Audio: " + activeAudio;
    }
    addSpec(makeOverlayTextControlSpec(
        OverlayControlId::AudioTrack, audioLabel,
        state.audioOk && state.canCycleAudioTracks));
  }

  if (options.includeSubtitles) {
    const bool subtitlesActive = state.hasSubtitles && state.subtitlesEnabled;
    std::string subtitleLabel = "Subs";
    if (subtitlesActive) {
      std::string activeSubtitle = state.activeSubtitleLabel;
      if (!activeSubtitle.empty() && activeSubtitle != "N/A") {
        if (utf8DisplayWidth(activeSubtitle) > 14) {
          activeSubtitle = utf8TakeDisplayWidth(activeSubtitle, 14);
        }
        subtitleLabel += ": " + activeSubtitle;
      }
    }
    addSpec(makeOverlayTextControlSpec(OverlayControlId::Subtitles,
                                       subtitleLabel, subtitlesActive));
  }

  if (options.includePictureInPicture && state.pictureInPictureAvailable) {
    addSpec(makeOverlayTextControlSpec(OverlayControlId::PictureInPicture,
                                       "PiP",
                                       state.pictureInPictureActive));
  }

  for (size_t i = 0; i < out.size(); ++i) {
    auto& spec = out[i];
    bool hovered = static_cast<int>(i) == hoverIndex;
    spec.renderText = hovered ? spec.hoverText : spec.normalText;
    int textWidth = countVisibleChars(spec.renderText);
    if (textWidth < spec.width) {
      spec.renderText.append(static_cast<size_t>(spec.width - textWidth), ' ');
    } else if (textWidth > spec.width) {
      spec.renderText = utf8TakeDisplayWidth(spec.renderText, spec.width);
    }
  }
  return out;
}

std::vector<OverlayControlSpec> buildOverlayControlSpecs(
    const PlaybackOverlayState& state, int hoverIndex) {
  return buildOverlayControlSpecs(state, hoverIndex,
                                  OverlayControlSpecOptions{});
}

OverlayCellLayout layoutOverlayCells(const OverlayCellLayoutInput& input) {
  OverlayCellLayout layout;
  layout.width = std::max(1, input.width);

  int controlLineCount = 0;
  std::vector<PendingOverlayCellControl> pending =
      wrapOverlayControls(input.controls, layout.width, &controlLineCount);
  const bool hasSuffix = !input.suffix.empty();
  const int contentInset = layout.width > 2 ? 1 : 0;
  const int progressWidth = std::max(1, layout.width - contentInset * 2);
  const int reservedRowsAboveProgress =
      std::max(0, input.reservedRowsAboveProgress);
  std::vector<std::string> titleLines = wrapLine(input.title, layout.width);
  if (titleLines.empty()) titleLines.emplace_back();
  auto placeTitleLines = [&](int topY, int firstLine, int lineCount) {
    layout.titleLines.clear();
    layout.titleX = 0;
    layout.titleY = -1;
    layout.titleText.clear();
    if (lineCount <= 0) return;
    layout.titleLines.reserve(static_cast<size_t>(lineCount));
    for (int i = 0; i < lineCount; ++i) {
      const int lineIndex = firstLine + i;
      OverlayCellTextLine line;
      line.x = 0;
      line.y = topY + i;
      line.text = titleLines[static_cast<size_t>(lineIndex)];
      layout.titleLines.push_back(std::move(line));
    }
    layout.titleX = layout.titleLines.front().x;
    layout.titleY = layout.titleLines.front().y;
    layout.titleText = layout.titleLines.front().text;
  };

  if (input.height > 0) {
    layout.height = input.height;
    layout.progressBarX = contentInset;
    layout.progressBarY = layout.height - 1;
    layout.progressBarWidth = progressWidth;
    const int firstContentYAboveFooter =
        layout.progressBarY - reservedRowsAboveProgress;
    if (hasSuffix) {
      layout.suffixText = fitLine(input.suffix, layout.width);
      layout.suffixY = firstContentYAboveFooter - 1;
      layout.suffixX =
          std::max(0, layout.width - utf8DisplayWidth(layout.suffixText));
    }

    const int controlsBottom =
        (layout.suffixY >= 0 ? layout.suffixY : firstContentYAboveFooter) - 1;
    const int controlsTop =
        pending.empty() ? controlsBottom
                        : controlsBottom - (controlLineCount - 1);
    for (const PendingOverlayCellControl& item : pending) {
      OverlayCellControlLayoutItem placed;
      placed.text = item.text;
      placed.controlIndex = item.controlIndex;
      placed.x = item.x;
      placed.y = controlsTop + item.line;
      placed.width = item.width;
      placed.active = item.active;
      placed.hovered = item.hovered;
      layout.controls.push_back(std::move(placed));
    }

    const int titleBaseY =
        pending.empty()
            ? ((layout.suffixY >= 0 ? layout.suffixY : firstContentYAboveFooter) -
               1)
            : (controlsTop - 1);
    const int titleSlots = std::max(0, titleBaseY + 1);
    const int titleLineCount = std::min(
        static_cast<int>(titleLines.size()), titleSlots);
    const int firstTitleLine =
        static_cast<int>(titleLines.size()) - titleLineCount;
    placeTitleLines(titleBaseY - titleLineCount + 1, firstTitleLine,
                    titleLineCount);
  } else {
    const int titleLineCount = static_cast<int>(titleLines.size());
    layout.height =
        titleLineCount + controlLineCount + (hasSuffix ? 1 : 0) +
        reservedRowsAboveProgress + 1;
    placeTitleLines(0, 0, titleLineCount);

    const int controlsTop = titleLineCount;
    for (const PendingOverlayCellControl& item : pending) {
      OverlayCellControlLayoutItem placed;
      placed.text = item.text;
      placed.controlIndex = item.controlIndex;
      placed.x = item.x;
      placed.y = controlsTop + item.line;
      placed.width = item.width;
      placed.active = item.active;
      placed.hovered = item.hovered;
      layout.controls.push_back(std::move(placed));
    }

    if (hasSuffix) {
      layout.suffixText = fitLine(input.suffix, layout.width);
      layout.suffixY = controlsTop + controlLineCount;
      layout.suffixX =
          std::max(0, layout.width - utf8DisplayWidth(layout.suffixText));
    }
    layout.progressBarX = contentInset;
    layout.progressBarY = layout.height - 1;
    layout.progressBarWidth = progressWidth;
  }

  layout.topY = layout.progressBarY;
  auto useTop = [&](int y) {
    if (y != -1) layout.topY = std::min(layout.topY, y);
  };
  for (const auto& line : layout.titleLines) {
    useTop(line.y);
  }
  useTop(layout.suffixY);
  for (const auto& item : layout.controls) {
    useTop(item.y);
  }
  return layout;
}

OverlayCellLayout layoutOverlayControlCells(
    const std::vector<OverlayCellControlInput>& controls, int width) {
  OverlayCellLayout layout;
  layout.width = std::max(1, width);

  int controlLineCount = 0;
  std::vector<PendingOverlayCellControl> pending =
      wrapOverlayControls(controls, layout.width, &controlLineCount);
  layout.height = controlLineCount;
  layout.progressBarX = -1;
  layout.progressBarY = -1;
  layout.progressBarWidth = 0;
  layout.topY = pending.empty() ? -1 : 0;

  layout.controls.reserve(pending.size());
  for (const PendingOverlayCellControl& item : pending) {
    OverlayCellControlLayoutItem placed;
    placed.text = item.text;
    placed.controlIndex = item.controlIndex;
    placed.x = item.x;
    placed.y = item.line;
    placed.width = item.width;
    placed.active = item.active;
    placed.hovered = item.hovered;
    layout.controls.push_back(std::move(placed));
  }
  return layout;
}

OverlayCellViewportLayout layoutOverlayCellViewport(
    const OverlayCellLayoutInput& input, int windowWidth, int windowHeight,
    int cellPixelWidth, int cellPixelHeight) {
  OverlayCellViewportLayout geometry;
  geometry.cellWidth = std::max(1, cellPixelWidth);
  geometry.cellHeight = std::max(1, cellPixelHeight);
  geometry.layout = layoutOverlayCells(input);

  if (windowWidth <= 0 || windowHeight <= 0) {
    return geometry;
  }

  const int textPxW = std::min(windowWidth, geometry.layout.width *
                                              geometry.cellWidth);
  const int textPxH = std::min(windowHeight, geometry.layout.height *
                                               geometry.cellHeight);
  geometry.leftPx = std::clamp(
      static_cast<int>(std::lround(windowWidth * 0.02)), 0,
      std::max(0, windowWidth - textPxW));
  geometry.topPx = std::clamp(
      static_cast<int>(std::lround(windowHeight * 0.95)) - textPxH, 0,
      std::max(0, windowHeight - textPxH));
  return geometry;
}

OverlayCellLayout layoutPlaybackOverlayCells(
    const PlaybackOverlayState& state, int width, int height,
    int hoverIndex) {
  std::vector<OverlayControlSpec> specs =
      buildOverlayControlSpecs(state, hoverIndex);

  OverlayCellLayoutInput input;
  input.width = width;
  input.height = height;
  input.title =
      overlayTitleWithDebugLines(state.debugLines,
                                 buildWindowOverlayTopLine(state));
  input.suffix = buildWindowOverlayProgressSuffix(state);
  input.controls = buildOverlayCellControlInputs(specs, hoverIndex);
  return layoutOverlayCells(input);
}

OverlayCellLayout layoutWindowOverlayCells(const WindowUiState& ui, int width,
                                           int height) {
  OverlayCellLayoutInput input;
  input.width = width;
  input.height = height;
  input.title = overlayTitleWithDebugLines(ui.debugLines, ui.title);
  input.suffix = ui.progressSuffix;
  input.controls.reserve(ui.controlButtons.size());
  for (size_t i = 0; i < ui.controlButtons.size(); ++i) {
    OverlayCellControlInput control;
    control.text = ui.controlButtons[i].text;
    control.active = ui.controlButtons[i].active;
    control.hovered = ui.controlButtons[i].hovered;
    control.controlIndex = static_cast<int>(i);
    input.controls.push_back(std::move(control));
  }
  return layoutOverlayCells(input);
}

int overlayCellControlAt(const OverlayCellLayout& layout, int cellX,
                         int cellY) {
  for (const OverlayCellControlLayoutItem& item : layout.controls) {
    if (cellY != item.y) continue;
    if (cellX >= item.x && cellX < item.x + item.width) {
      return item.controlIndex;
    }
  }
  return -1;
}

std::string buildWindowOverlayProgressSuffix(
    const PlaybackOverlayState& state) {
  auto formatTime = [](double s) -> std::string {
    if (!(s >= 0.0) || !std::isfinite(s)) return "--:--";
    int total = static_cast<int>(std::llround(s));
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int sec = total % 60;
    char buf[64];
    if (h > 0)
      std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else
      std::snprintf(buf, sizeof(buf), "%02d:%02d", m, sec);
    return std::string(buf);
  };
  std::string timeLabel = state.totalSec > 0.0
                              ? (formatTime(state.displaySec) + " / " +
                                 formatTime(state.totalSec))
                              : formatTime(state.displaySec);
  std::string volStr = " Vol: " + std::to_string(state.volPct) + "%";
  return timeLabel + volStr;
}

std::string buildWindowOverlayTopLine(const PlaybackOverlayState& state) {
  return state.windowTitle;
}

bool isBackMousePressed(const MouseEvent& mouse) {
  const DWORD backMask = RIGHTMOST_BUTTON_PRESSED |
                         FROM_LEFT_2ND_BUTTON_PRESSED |
                         FROM_LEFT_3RD_BUTTON_PRESSED |
                         FROM_LEFT_4TH_BUTTON_PRESSED;
  return (mouse.buttonState & backMask) != 0;
}

static bool terminalOverlayProgressRatioAt(const PlaybackOverlayState& state,
                                           const MouseEvent& mouse,
                                           double* outRatio) {
  if (!state.overlayVisible) return false;
  OverlayCellLayout layout = layoutPlaybackOverlayCells(
      state, state.screenWidth, state.screenHeight, -1);
  ProgressBarHitTestInput hit;
  hit.x = mouse.pos.X;
  hit.y = mouse.pos.Y;
  hit.barX = layout.progressBarX;
  hit.barY = layout.progressBarY;
  hit.barWidth = layout.progressBarWidth;
  return progressBarRatioAt(hit, outRatio);
}

static bool windowOverlayProgressRatioAt(const PlaybackOverlayState& state,
                                         const MouseEvent& mouse,
                                         int cellPixelWidth,
                                         int cellPixelHeight,
                                         double* outRatio) {
  if (!state.overlayVisible) return false;
  if (state.windowWidth <= 0 || state.windowHeight <= 0) return false;
  const int cellWidth = std::max(1, cellPixelWidth);
  const int cellHeight = std::max(1, cellPixelHeight);
  const int cols = overlayCellCountForPixels(state.windowWidth, cellWidth);
  const int rows = overlayCellCountForPixels(state.windowHeight, cellHeight);
  OverlayCellLayout layout =
      layoutPlaybackOverlayCells(state, cols, rows, -1);
  if (layout.progressBarWidth <= 0) return false;
  const double unitWidth =
      static_cast<double>(std::min(state.windowWidth, cols * cellWidth)) /
      static_cast<double>(std::max(1, cols));
  const double unitHeight =
      static_cast<double>(std::min(state.windowHeight, rows * cellHeight)) /
      static_cast<double>(std::max(1, rows));

  ProgressBarHitTestInput hit;
  hit.x = mouse.pos.X;
  hit.y = mouse.pos.Y;
  hit.barX = layout.progressBarX;
  hit.barY = layout.progressBarY;
  hit.barWidth = layout.progressBarWidth;
  hit.unitWidth = unitWidth;
  hit.unitHeight = unitHeight;
  return progressBarRatioAt(hit, outRatio);
}

bool overlayProgressRatioAt(const PlaybackOverlayState& state,
                            const MouseEvent& mouse, int cellPixelWidth,
                            int cellPixelHeight, double* outRatio) {
  const bool windowEvent = isWindowMouseEvent(mouse);
  return windowEvent ? windowOverlayProgressRatioAt(
                           state, mouse, cellPixelWidth, cellPixelHeight,
                           outRatio)
                     : terminalOverlayProgressRatioAt(state, mouse, outRatio);
}

int terminalOverlayControlAt(const PlaybackOverlayState& state,
                            const MouseEvent& mouse) {
  if (!state.overlayVisible) return -1;
  OverlayCellLayout layout = layoutPlaybackOverlayCells(
      state, state.screenWidth, state.screenHeight, -1);
  return overlayCellControlAt(layout, mouse.pos.X, mouse.pos.Y);
}

int windowOverlayControlAt(const PlaybackOverlayState& state,
                          const MouseEvent& mouse, int cellPixelWidth,
                          int cellPixelHeight) {
  if (!state.overlayVisible) return -1;
  if (state.windowWidth <= 0 || state.windowHeight <= 0) return -1;
  const int cellWidth = std::max(1, cellPixelWidth);
  const int cellHeight = std::max(1, cellPixelHeight);
  const int cols = overlayCellCountForPixels(state.windowWidth, cellWidth);
  const int rows = overlayCellCountForPixels(state.windowHeight, cellHeight);
  OverlayCellLayout layout =
      layoutPlaybackOverlayCells(state, cols, rows, -1);
  const int gridPixelWidth = std::min(state.windowWidth, cols * cellWidth);
  const int gridPixelHeight = std::min(state.windowHeight, rows * cellHeight);
  if (mouse.pos.X < 0 || mouse.pos.Y < 0 || mouse.pos.X >= gridPixelWidth ||
      mouse.pos.Y >= gridPixelHeight) {
    return -1;
  }
  const int cellX = std::clamp(
      static_cast<int>((static_cast<int64_t>(mouse.pos.X) * cols) /
                       std::max(1, gridPixelWidth)),
      0, cols - 1);
  const int cellY = std::clamp(
      static_cast<int>((static_cast<int64_t>(mouse.pos.Y) * rows) /
                       std::max(1, gridPixelHeight)),
      0, rows - 1);
  return overlayCellControlAt(layout, cellX, cellY);
}

WindowUiState buildWindowUiState(const PlaybackOverlayState& state,
                                int hoverIndex) {
  WindowUiState ui;
  ui.progress =
      (state.totalSec > 0.0 && std::isfinite(state.totalSec))
          ? static_cast<float>(std::clamp(state.displaySec / state.totalSec, 0.0, 1.0))
          : 0.0f;
  ui.overlayAlpha = state.overlayVisible ? 1.0f : 0.0f;
  ui.isPaused = state.paused;
  ui.title = state.windowTitle;
  std::vector<OverlayControlSpec> controlSpecs =
      buildOverlayControlSpecs(state, hoverIndex);
  ui.progressSuffix = buildWindowOverlayProgressSuffix(state);
  ui.controlButtons.clear();
  ui.controlButtons.reserve(controlSpecs.size());
  for (size_t i = 0; i < controlSpecs.size(); ++i) {
    WindowUiState::ControlButton btn;
    btn.text = controlSpecs[i].renderText;
    btn.active = controlSpecs[i].active;
    btn.hovered = static_cast<int>(i) == hoverIndex;
    ui.controlButtons.push_back(std::move(btn));
  }
  ui.subtitleClockUs = state.subtitleClockUs;
  ui.subtitleAssScript = state.subtitleAssScript;
  ui.subtitleAssFonts = state.subtitleAssFonts;
  ui.subtitleRenderError = state.subtitleRenderError;
  ui.subtitleCues = state.subtitleCues;
  ui.displaySec = state.displaySec;
  ui.totalSec = state.totalSec;
  ui.volPct = state.volPct;
  ui.subtitle = state.subtitleText;
  ui.subtitleAlpha =
      (state.subtitleCues.empty() && !state.subtitleAssScript) ? 0.0f : 1.0f;
  return ui;
}

namespace {

class ScreenOverlayTarget {
 public:
  ScreenOverlayTarget(ConsoleScreen& screen, int minY, int maxY)
      : screen_(screen),
        width_(screen.width()),
        firstY_(std::max(0, minY)),
        lastY_(std::min(screen.height(), maxY)) {}

  bool isDrawable() const { return width_ > 0 && firstY_ < lastY_; }

  bool rowVisible(int y) const { return y >= firstY_ && y < lastY_; }

  void writeText(int x, int y, const std::string& text,
                 const Style& style) {
    if (!rowVisible(y) || text.empty() || x >= width_) return;
    const int drawX = std::max(0, x);
    const int available = width_ - drawX;
    if (available <= 0) return;
    std::string clipped =
        x < 0 ? utf8SliceDisplayWidth(text, -x, available) : text;
    if (utf8DisplayWidth(clipped) > available) {
      clipped = utf8TakeDisplayWidth(clipped, available);
    }
    screen_.writeText(drawX, y, clipped, style);
  }

  void writeControlText(const std::string& text, int y, int x, int width,
                        const Style& style) {
    if (width <= 0) return;
    writeText(x, y, text, style);
  }

  void writeChar(int x, int y, wchar_t ch, const Style& style) {
    if (!rowVisible(y) || x < 0 || x >= width_) return;
    screen_.writeChar(x, y, ch, style);
  }

 private:
  ConsoleScreen& screen_;
  int width_ = 0;
  int firstY_ = 0;
  int lastY_ = 0;
};

class GpuTextGridOverlayTarget {
 public:
  GpuTextGridOverlayTarget(GpuTextGridFrame& frame, int cols, int rows,
                           const Style& baseStyle)
      : frame_(frame) {
    frame_.cols = std::max(1, cols);
    frame_.rows = std::max(1, rows);
    const GpuTextGridCell transparentSpace = overlayGpuCell(
        L' ', baseStyle, kGpuTextGridCellFlagTransparentBg);
    const size_t cellCount =
        static_cast<size_t>(frame_.cols) * static_cast<size_t>(frame_.rows);
    frame_.cells.assign(cellCount, transparentSpace);
  }

  bool isDrawable() const { return frame_.cols > 0 && frame_.rows > 0; }

  bool rowVisible(int y) const { return y >= 0 && y < frame_.rows; }

  void writeText(int x, int y, const std::string& text,
                 const Style& style) {
    if (!rowVisible(y) || text.empty() || x >= frame_.cols) return;
    const int drawX = std::max(0, x);
    const int available = frame_.cols - drawX;
    if (available <= 0) return;
    const std::string clipped =
        x < 0 ? utf8SliceDisplayWidth(text, -x, available)
              : utf8TakeDisplayWidth(text, available);
    const std::wstring wide = overlayUtf8ToWide(clipped);
    int dstX = drawX;
    for (size_t i = 0; i < wide.size() && dstX < frame_.cols; ++i, ++dstX) {
      frame_.cells[static_cast<size_t>(y * frame_.cols + dstX)] =
          overlayGpuCell(wide[i], style);
    }
  }

  void writeControlText(const std::string& text, int y, int x, int width,
                        const Style& style) {
    if (!rowVisible(y) || width <= 0 || x >= frame_.cols) return;
    const int startX = std::max(0, x);
    const int endX = std::min(frame_.cols, x + width);
    if (startX >= endX) return;
    const std::string clipped =
        x < 0 ? utf8SliceDisplayWidth(text, -x, endX - startX)
              : utf8TakeDisplayWidth(text, endX - startX);
    const std::wstring wide = overlayUtf8ToWide(clipped);
    for (int dstX = startX; dstX < endX; ++dstX) {
      const int srcX = dstX - startX;
      const wchar_t ch =
          srcX >= 0 && srcX < static_cast<int>(wide.size())
              ? wide[static_cast<size_t>(srcX)]
              : L' ';
      frame_.cells[static_cast<size_t>(y * frame_.cols + dstX)] =
          overlayGpuCell(ch, style);
    }
  }

  void writeChar(int x, int y, wchar_t ch, const Style& style) {
    if (!rowVisible(y) || x < 0 || x >= frame_.cols) return;
    frame_.cells[static_cast<size_t>(y * frame_.cols + x)] =
        overlayGpuCell(ch, style);
  }

 private:
  GpuTextGridFrame& frame_;
};

template <typename Target>
void renderOverlayToTarget(Target& target, const OverlayCellLayout& layout,
                           const OverlayRenderStyles& styles,
                           double progress) {
  if (!target.isDrawable()) return;

  for (const auto& item : layout.controls) {
    Style style = item.active ? styles.accentStyle : styles.baseStyle;
    if (item.hovered) {
      style = {style.bg, style.fg};
    }
    target.writeControlText(item.text, item.y, item.x, item.width, style);
  }

  for (const auto& titleLine : layout.titleLines) {
    target.writeText(titleLine.x, titleLine.y, titleLine.text,
                     styles.accentStyle);
  }

  if (layout.progressBarY >= 0 && layout.progressBarWidth > 0 &&
      target.rowVisible(layout.progressBarY)) {
    const int leftFrameX = layout.progressBarX - 1;
    const int rightFrameX = layout.progressBarX + layout.progressBarWidth;
    target.writeChar(leftFrameX, layout.progressBarY, L'|',
                     styles.progressFrameStyle);
    auto barCells = renderProgressBarCells(
        std::clamp(progress, 0.0, 1.0), layout.progressBarWidth,
        styles.progressEmptyStyle, styles.progressStart, styles.progressEnd);
    for (int i = 0; i < layout.progressBarWidth; ++i) {
      const auto& cell = barCells[static_cast<size_t>(i)];
      target.writeChar(layout.progressBarX + i, layout.progressBarY, cell.ch,
                       cell.style);
    }
    target.writeChar(rightFrameX, layout.progressBarY, L'|',
                     styles.progressFrameStyle);
  }

  target.writeText(layout.suffixX, layout.suffixY, layout.suffixText,
                   styles.baseStyle);
}

}  // namespace

void renderOverlayToScreen(ConsoleScreen& screen,
                           const OverlayCellLayout& layout,
                           const OverlayRenderStyles& styles,
                           double progress,
                           int minY,
                           int maxY) {
  ScreenOverlayTarget target(screen, minY, maxY);
  renderOverlayToTarget(target, layout, styles, progress);
}

bool renderOverlayToGpuTextGrid(const OverlayCellLayout& layout,
                                const OverlayRenderStyles& styles,
                                double progress,
                                GpuTextGridFrame& outFrame) {
  GpuTextGridOverlayTarget target(outFrame, layout.width, layout.height,
                                  styles.baseStyle);
  renderOverlayToTarget(target, layout, styles, progress);
  return true;
}

}  // namespace playback_overlay
