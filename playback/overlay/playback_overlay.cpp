#include "playback_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "ui_helpers.h"

namespace playback_overlay {
namespace {

int countVisibleChars(const std::string& text) {
  int count = 0;
  for (char c : text) {
    if (c == '\r' || c == '\n') continue;
    ++count;
  }
  return count;
}

std::pair<std::string, std::string> makeLabels(const std::string& text) {
  return std::pair<std::string, std::string>(" [" + text + "] ",
                                             "[ " + text + " ]");
}
}  // namespace

PlaybackOverlayState buildPlaybackOverlayState(
    const PlaybackOverlayInputs& inputs) {
  PlaybackOverlayState state;
  state.windowTitle = inputs.windowTitle;
  state.audioOk = inputs.audioOk;
  state.audioSupports50HzToggle = inputs.audioSupports50HzToggle;
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
  state.subtitleRenderError = inputs.subtitleRenderError;
  state.screenWidth = inputs.screenWidth;
  state.screenHeight = inputs.screenHeight;
  state.windowWidth = inputs.windowWidth;
  state.windowHeight = inputs.windowHeight;
  state.artTop = inputs.artTop;
  state.progressBarX = inputs.progressBarX;
  state.progressBarY = inputs.progressBarY;
  state.progressBarWidth = inputs.progressBarWidth;

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
    const bool hasRenderableAss = cue->assStyled && !cue->rawText.empty();
    if (cue->text.empty() && !hasRenderableAss) continue;
    WindowUiState::SubtitleCue item;
    item.text = cue->text;
    item.rawText = cue->rawText;
    item.sizeScale = std::clamp(cue->sizeScale, 0.40f, 3.0f);
    item.scaleX = std::clamp(cue->scaleX, 0.40f, 3.5f);
    item.scaleY = std::clamp(cue->scaleY, 0.40f, 3.5f);
    item.fontName = cue->fontName;
    item.bold = cue->bold;
    item.italic = cue->italic;
    item.underline = cue->underline;
    item.assStyled = cue->assStyled;
    item.startUs = cue->startUs;
    item.endUs = cue->endUs;
    item.alignment = cue->alignment;
    item.layer = cue->layer;
    item.hasPosition = cue->hasPosition;
    item.posX = cue->posXNorm;
    item.posY = cue->posYNorm;
    item.marginVNorm = cue->marginVNorm;
    item.marginLNorm = cue->marginLNorm;
    item.marginRNorm = cue->marginRNorm;
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

std::vector<OverlayControlSpec> buildOverlayControlSpecs(
    const PlaybackOverlayState& state, int hoverIndex) {
  std::vector<OverlayControlSpec> out;
  auto addSpec = [&](OverlayControlSpec spec) {
    out.push_back(std::move(spec));
  };

  OverlayControlSpec radio{};
  radio.id = OverlayControlId::Radio;
  radio.active = state.radioEnabled;
  auto radioLabels = makeLabels(radio.active ? "Radio: AM" : "Radio: Dry");
  radio.normalText = radioLabels.first;
  radio.hoverText = radioLabels.second;
  radio.width =
      std::max(countVisibleChars(radio.normalText),
               countVisibleChars(radio.hoverText));
  addSpec(std::move(radio));

  if (state.audioOk && state.audioSupports50HzToggle) {
    OverlayControlSpec hz50{};
    hz50.id = OverlayControlId::Hz50;
    hz50.active = state.hz50Enabled;
    auto hzLabels = makeLabels(hz50.active ? "50Hz: 50" : "50Hz: Auto");
    hz50.normalText = hzLabels.first;
    hz50.hoverText = hzLabels.second;
    hz50.width =
        std::max(countVisibleChars(hz50.normalText),
                 countVisibleChars(hz50.hoverText));
    addSpec(std::move(hz50));
  }

  OverlayControlSpec audioTrack{};
  audioTrack.id = OverlayControlId::AudioTrack;
  std::string audioLabel = "Audio: N/A";
  if (state.canCycleAudioTracks && state.audioOk) {
    std::string activeAudio = state.activeAudioTrackLabel;
    if (activeAudio.empty()) activeAudio = "N/A";
    if (utf8CodepointCount(activeAudio) > 14) {
      activeAudio = utf8Take(activeAudio, 14);
    }
    audioLabel = "Audio: " + activeAudio;
  }
  auto audioLabels = makeLabels(audioLabel);
  audioTrack.normalText = audioLabels.first;
  audioTrack.hoverText = audioLabels.second;
  audioTrack.active = state.audioOk && state.canCycleAudioTracks;
  audioTrack.width =
      std::max(countVisibleChars(audioTrack.normalText),
               countVisibleChars(audioTrack.hoverText));
  addSpec(std::move(audioTrack));

  OverlayControlSpec subtitles{};
  subtitles.id = OverlayControlId::Subtitles;
  subtitles.active = state.hasSubtitles;
  std::string subtitleLabel = "Subs: N/A";
  if (state.hasSubtitles) {
    if (!state.subtitlesEnabled) {
      subtitleLabel = "Subs: Off";
    } else {
      std::string activeSubtitle = state.activeSubtitleLabel;
      if (activeSubtitle.empty()) activeSubtitle = "N/A";
      if (utf8CodepointCount(activeSubtitle) > 14) {
        activeSubtitle = utf8Take(activeSubtitle, 14);
      }
      subtitleLabel = "Subs: " + activeSubtitle;
    }
  }
  auto subtitleLabels = makeLabels(subtitleLabel);
  subtitles.normalText = subtitleLabels.first;
  subtitles.hoverText = subtitleLabels.second;
  subtitles.width =
      std::max(countVisibleChars(subtitles.normalText),
               countVisibleChars(subtitles.hoverText));
  addSpec(std::move(subtitles));

  int cursor = 0;
  for (size_t i = 0; i < out.size(); ++i) {
    auto& spec = out[i];
    if (i > 0) cursor += 2;
    spec.charStart = cursor;
    bool hovered = static_cast<int>(i) == hoverIndex;
    spec.renderText = hovered ? spec.hoverText : spec.normalText;
    int textWidth = countVisibleChars(spec.renderText);
    if (textWidth < spec.width) {
      spec.renderText.append(static_cast<size_t>(spec.width - textWidth), ' ');
    } else if (textWidth > spec.width) {
      spec.renderText = utf8Take(spec.renderText, spec.width);
    }
    cursor += spec.width;
  }
  return out;
}

std::string buildOverlayControlsText(const PlaybackOverlayState& state,
                                     int hoverIndex) {
  std::vector<OverlayControlSpec> specs = buildOverlayControlSpecs(state, hoverIndex);
  std::string line;
  for (size_t i = 0; i < specs.size(); ++i) {
    if (i > 0) line += "  ";
    line += specs[i].renderText;
  }
  return line;
}

std::string buildWindowOverlayProgressSuffix(
    const PlaybackOverlayState& state) {
  std::string status = "\xE2\x96\xB6";  // ▶
  if (state.audioFinished || state.paused) {
    status = state.audioFinished ? "\xE2\x96\xA0" : "\xE2\x8F\xB8";
  }
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
  return timeLabel + " " + status + volStr;
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

static bool isWindowProgressHit(const PlaybackOverlayState& state,
                                const MouseEvent& mouse) {
  if (state.windowWidth <= 0 || state.windowHeight <= 0) {
    return false;
  }
  float mouseWinX = static_cast<float>(mouse.pos.X) /
                    static_cast<float>(state.windowWidth);
  float mouseWinY = static_cast<float>(mouse.pos.Y) /
                    static_cast<float>(state.windowHeight);
  const float barYTop = 0.95f;
  const float barYBot = 1.0f;
  const float barXLeft = 0.02f;
  const float barXRight = 0.98f;
  return mouseWinX >= barXLeft && mouseWinX <= barXRight &&
         mouseWinY >= barYTop && mouseWinY <= barYBot;
}

static bool isTerminalProgressHit(const PlaybackOverlayState& state,
                                  const MouseEvent& mouse) {
  int screenHeight = std::max(10, state.screenHeight);
  int barY = state.progressBarY >= 0 ? state.progressBarY : (screenHeight - 1);
  if (mouse.pos.Y != barY) {
    return false;
  }
  if (state.progressBarWidth > 0 && state.progressBarX >= 0) {
    return mouse.pos.X >= state.progressBarX &&
           mouse.pos.X < state.progressBarX + state.progressBarWidth;
  }
  return mouse.pos.X >= 0;
}

bool isProgressHit(const PlaybackOverlayState& state, const MouseEvent& mouse) {
  bool windowEvent = (mouse.control & 0x80000000) != 0;
  return windowEvent ? isWindowProgressHit(state, mouse)
                     : isTerminalProgressHit(state, mouse);
}

int terminalOverlayControlAt(const PlaybackOverlayState& state,
                            const MouseEvent& mouse) {
  if (!state.overlayVisible) return -1;
  std::vector<OverlayControlSpec> specs = buildOverlayControlSpecs(state, -1);
  int controlsY = std::max(0, std::max(10, state.screenHeight) - 3);
  if (mouse.pos.Y != controlsY) return -1;
  int relX = mouse.pos.X - 1;
  if (relX < 0) return -1;
  for (size_t i = 0; i < specs.size(); ++i) {
    const auto& spec = specs[i];
    if (relX >= spec.charStart && relX < spec.charStart + spec.width) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int windowOverlayControlAt(const PlaybackOverlayState& state,
                          const MouseEvent& mouse) {
  if (!state.overlayVisible) return -1;
  if (state.windowWidth <= 0 || state.windowHeight <= 0) return -1;
  std::vector<OverlayControlSpec> specs = buildOverlayControlSpecs(state, -1);
  if (specs.empty()) return -1;

  std::string topLine = buildWindowOverlayTopLine(state);
  std::string controlsLine = buildOverlayControlsText(state, -1);
  int maxChars = std::max(utf8CodepointCount(topLine),
                          utf8CodepointCount(controlsLine));
  if (maxChars <= 0) return -1;
  std::string progressLine = buildWindowOverlayProgressSuffix(state);
  int linePxH = std::clamp(static_cast<int>(std::round(state.windowHeight * 0.045f)),
                           12, 36);
  int lineCount = 1 + (controlsLine.empty() ? 0 : 1) +
                  (progressLine.empty() ? 0 : 1);
  int textPxH =
      std::clamp(lineCount * linePxH + std::max(0, lineCount - 1) * 2, 14, 96);
  int textPxW =
      std::clamp(static_cast<int>(std::lround(state.windowWidth * 0.96)), 1,
                 state.windowWidth);
  int totalGlyphH = lineCount * 7 + (lineCount - 1);
  int maxScaleVert = std::max(1, textPxH / std::max(1, totalGlyphH));
  int maxScaleHoriz = std::max(1, textPxW / std::max(1, maxChars * 6));
  int maxCap = std::max(3, textPxH / 80);
  int scale = std::min({maxScaleVert, maxScaleHoriz, maxCap});
  if (scale < 1) scale = 1;
  int charAdvance = 6 * scale;
  float textHeightNorm = static_cast<float>(textPxH) / state.windowHeight;
  float textWidthNorm = static_cast<float>(textPxW) / state.windowWidth;
  float textLeftNorm = 0.02f;
  if (textLeftNorm + textWidthNorm > 1.0f) {
    textLeftNorm = std::max(0.0f, 1.0f - textWidthNorm);
  }
  float textTopNorm = 0.95f - textHeightNorm;
  int textLeftPx = static_cast<int>(std::round(textLeftNorm * state.windowWidth));
  int textTopPx = static_cast<int>(std::round(textTopNorm * state.windowHeight));
  int controlsLineIndex = controlsLine.empty() ? -1 : 1;
  if (controlsLineIndex < 0) return -1;
  int controlsY0 = textTopPx + 1 + controlsLineIndex * (7 + 1) * scale;
  int controlsY1 = controlsY0 + 7 * scale;
  if (mouse.pos.Y < controlsY0 || mouse.pos.Y >= controlsY1) return -1;
  for (size_t i = 0; i < specs.size(); ++i) {
    const auto& spec = specs[i];
    int x0 = textLeftPx + 1 + spec.charStart * charAdvance;
    int x1 = x0 + spec.width * charAdvance;
    if (mouse.pos.X >= x0 && mouse.pos.X < x1) {
      return static_cast<int>(i);
    }
  }
  return -1;
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
  ui.controls = buildOverlayControlsText(state, hoverIndex);
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

}  // namespace playback_overlay
