#include "overlay.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include "playback/video/edit/overlay_model.h"
#include "playback/video/image.h"
#include "subtitle_effects.h"
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
  OverlayControlId id = OverlayControlId::Radio;
  std::string text;
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
    item.id = control.id;
    item.text = fitCellText(control.text, controlWidth);
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
  state.radioLabel = inputs.radioLabel;
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
  state.overlayVisible = inputs.osd.controlsVisible;
  state.transientMessage = inputs.osd.message;
  state.paused = inputs.paused;
  state.pictureInPictureAvailable = inputs.pictureInPictureAvailable;
  state.pictureInPictureActive = inputs.pictureInPictureActive;
  state.subtitleRenderError = inputs.subtitleRenderError;
  state.debugLines = inputs.debugLines;
  state.contextMenu = inputs.contextMenu;
  state.videoEdit = inputs.videoEdit;
  state.videoEditExport = inputs.videoEditExport;
  state.videoEditPrompt = inputs.videoEditPrompt;
  state.chromeVisible = state.overlayVisible || !state.debugLines.empty() ||
                        state.videoEdit.active ||
                        state.videoEditPrompt !=
                            playback_video_edit::Prompt::None ||
                        state.videoEditExport.running();

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
    const std::vector<OverlayControlSpec>& specs, int hoverControlToken) {
  std::vector<OverlayCellControlInput> controls;
  controls.reserve(specs.size());
  for (size_t i = 0; i < specs.size(); ++i) {
    const bool hovered =
        overlayControlToken(specs[i].id) == hoverControlToken;
    OverlayCellControlInput control;
    control.id = specs[i].id;
    control.text = hovered ? specs[i].hoverText : specs[i].normalText;
    control.width = specs[i].width;
    control.active = specs[i].active;
    control.hovered = hovered;
    controls.push_back(std::move(control));
  }
  return controls;
}

bool dispatchOverlayControl(OverlayControlId id,
                            const OverlayControlActions& actions) {
  auto invoke = [](const std::function<bool()>& action) {
    return action ? action() : false;
  };
  const auto invokeEdit = [&](playback_video_edit::Command command) {
    return actions.videoEdit ? actions.videoEdit(command) : false;
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
    case OverlayControlId::EditMarkIn:
      return invokeEdit(playback_video_edit::Command::MarkIn);
    case OverlayControlId::EditMarkOut:
      return invokeEdit(playback_video_edit::Command::MarkOut);
    case OverlayControlId::EditRippleDelete:
      return invokeEdit(playback_video_edit::Command::RippleDelete);
    case OverlayControlId::EditTrim:
      return invokeEdit(playback_video_edit::Command::Trim);
    case OverlayControlId::EditUndo:
      return invokeEdit(playback_video_edit::Command::Undo);
    case OverlayControlId::EditRedo:
      return invokeEdit(playback_video_edit::Command::Redo);
    case OverlayControlId::EditReset:
      return invokeEdit(playback_video_edit::Command::Reset);
    case OverlayControlId::EditExport:
      return invokeEdit(playback_video_edit::Command::Export);
    case OverlayControlId::EditLeave:
      return invokeEdit(playback_video_edit::Command::RequestClose);
    case OverlayControlId::EditConfirmPrompt:
      return invokeEdit(playback_video_edit::Command::ConfirmPrompt);
    case OverlayControlId::EditCancelPrompt:
      return invokeEdit(playback_video_edit::Command::CancelPrompt);
    case OverlayControlId::EditDiscardAndExit:
      return invoke(actions.confirmPendingExit);
    case OverlayControlId::EditCancelExit:
      return invoke(actions.cancelPendingExit);
  }
  return false;
}

std::vector<OverlayControlSpec> buildOverlayControlSpecs(
    const PlaybackOverlayState& state, int hoverControlToken,
    const OverlayControlSpecOptions& options) {
  std::vector<OverlayControlSpec> out;
  auto addSpec = [&](OverlayControlSpec spec) {
    out.push_back(std::move(spec));
  };
  const auto finishSpecs = [&]() {
    for (size_t i = 0; i < out.size(); ++i) {
      auto& spec = out[i];
      const bool hovered =
          overlayControlToken(spec.id) == hoverControlToken;
      spec.renderText = hovered ? spec.hoverText : spec.normalText;
      const int textWidth = countVisibleChars(spec.renderText);
      if (textWidth < spec.width) {
        spec.renderText.append(static_cast<size_t>(spec.width - textWidth), ' ');
      } else if (textWidth > spec.width) {
        spec.renderText = utf8TakeDisplayWidth(spec.renderText, spec.width);
      }
    }
  };

  if (state.videoEditPrompt == playback_video_edit::Prompt::LeaveEditMode) {
    addSpec(makeOverlayTextControlSpec(OverlayControlId::EditConfirmPrompt,
                                       "Leave", true));
    addSpec(makeOverlayTextControlSpec(OverlayControlId::EditCancelPrompt,
                                       "Cancel", false));
    finishSpecs();
    return out;
  }

  if (state.videoEditPrompt == playback_video_edit::Prompt::DiscardEdits) {
    addSpec(makeOverlayTextControlSpec(OverlayControlId::EditConfirmPrompt,
                                       "Discard", false));
    addSpec(makeOverlayTextControlSpec(OverlayControlId::EditCancelPrompt,
                                       "Cancel", true));
    finishSpecs();
    return out;
  }

  if (state.videoEditPrompt == playback_video_edit::Prompt::LeavePlayback) {
    addSpec(makeOverlayTextControlSpec(
        OverlayControlId::EditExport,
        state.videoEditExport.running() ? "Wait" : "Export",
        state.videoEditExport.running()));
    addSpec(makeOverlayTextControlSpec(OverlayControlId::EditDiscardAndExit,
                                       "Discard", false));
    addSpec(makeOverlayTextControlSpec(OverlayControlId::EditCancelExit,
                                       "Stay", true));
    finishSpecs();
    return out;
  }

  if (state.videoEdit.active) {
    const bool hasSelection =
        state.videoEdit.inTimelineUs && state.videoEdit.outTimelineUs &&
        *state.videoEdit.outTimelineUs > *state.videoEdit.inTimelineUs;
    if (state.playPauseAvailable) {
      addSpec(makeOverlayTextControlSpec(OverlayControlId::PlayPause,
                                         state.paused ? "Play" : "Pause",
                                         state.paused));
    }
    addSpec(makeOverlayTextControlSpec(OverlayControlId::EditMarkIn, "In",
                                       false));
    addSpec(makeOverlayTextControlSpec(OverlayControlId::EditMarkOut, "Out",
                                       false));
    if (hasSelection) {
      addSpec(makeOverlayTextControlSpec(OverlayControlId::EditRippleDelete,
                                         "Delete", false));
      addSpec(makeOverlayTextControlSpec(OverlayControlId::EditTrim, "Trim",
                                         false));
    }
    if (state.videoEdit.canUndo) {
      addSpec(makeOverlayTextControlSpec(OverlayControlId::EditUndo, "Undo",
                                         false));
    }
    if (state.videoEdit.canRedo) {
      addSpec(makeOverlayTextControlSpec(OverlayControlId::EditRedo, "Redo",
                                         false));
    }
    if (state.videoEdit.hasEdits) {
      addSpec(makeOverlayTextControlSpec(OverlayControlId::EditReset, "Reset",
                                         false));
    }
    if (state.videoEdit.hasUnexportedChanges ||
        state.videoEditExport.running()) {
      addSpec(makeOverlayTextControlSpec(
          OverlayControlId::EditExport,
          state.videoEditExport.running() ? "Cancel" : "Export",
          state.videoEditExport.running()));
    }
    addSpec(makeOverlayTextControlSpec(OverlayControlId::EditLeave, "Leave",
                                       false));
    if (options.includePictureInPicture && state.pictureInPictureAvailable) {
      addSpec(makeOverlayTextControlSpec(OverlayControlId::PictureInPicture,
                                         "PiP",
                                         state.pictureInPictureActive));
    }
    finishSpecs();
    return out;
  }

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
    addSpec(makeOverlayTextControlSpec(OverlayControlId::Radio,
                                       state.radioLabel,
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

  finishSpecs();
  return out;
}

std::vector<OverlayControlSpec> buildOverlayControlSpecs(
    const PlaybackOverlayState& state, int hoverControlToken) {
  return buildOverlayControlSpecs(state, hoverControlToken,
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
      placed.id = item.id;
      placed.text = item.text;
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
      placed.id = item.id;
      placed.text = item.text;
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
    placed.id = item.id;
    placed.text = item.text;
    placed.x = item.x;
    placed.y = item.line;
    placed.width = item.width;
    placed.active = item.active;
    placed.hovered = item.hovered;
    layout.controls.push_back(std::move(placed));
  }
  return layout;
}

OverlayCellLayout layoutPlaybackOverlayCells(
    const PlaybackOverlayState& state, int width, int height,
    int hoverControlToken) {
  std::vector<OverlayControlSpec> specs =
      buildOverlayControlSpecs(state, hoverControlToken);

  OverlayCellLayoutInput input;
  input.width = width;
  input.height = height;
  input.title =
      overlayTitleWithDebugLines(state.debugLines,
                                 buildWindowOverlayTopLine(state));
  input.suffix = buildWindowOverlayProgressSuffix(state);
  input.reservedRowsAboveProgress =
      (state.videoEdit.active ||
       state.videoEditPrompt != playback_video_edit::Prompt::None ||
       state.videoEditExport.running())
          ? 1
          : 0;
  input.controls = buildOverlayCellControlInputs(specs, hoverControlToken);
  return layoutOverlayCells(input);
}

OverlayCellLayout layoutWindowOverlayCells(const WindowUiState& ui, int width,
                                           int height) {
  OverlayCellLayoutInput input;
  input.width = width;
  input.height = height;
  input.title = overlayTitleWithDebugLines(ui.debugLines, ui.title);
  input.suffix = ui.progressSuffix;
  input.reservedRowsAboveProgress =
      (ui.videoEdit.active ||
       ui.videoEditPrompt != playback_video_edit::Prompt::None ||
       ui.videoEditExport.running())
          ? 1
          : 0;
  input.controls.reserve(ui.controlButtons.size());
  for (size_t i = 0; i < ui.controlButtons.size(); ++i) {
    OverlayCellControlInput control;
    control.id = ui.controlButtons[i].id;
    control.text = ui.controlButtons[i].text;
    control.active = ui.controlButtons[i].active;
    control.hovered = ui.controlButtons[i].hovered;
    input.controls.push_back(std::move(control));
  }
  return layoutOverlayCells(input);
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

WindowUiState buildWindowUiState(const PlaybackOverlayState& state,
                                int hoverControlToken) {
  WindowUiState ui;
  ui.progress =
      (state.totalSec > 0.0 && std::isfinite(state.totalSec))
          ? static_cast<float>(std::clamp(state.displaySec / state.totalSec, 0.0, 1.0))
          : 0.0f;
  ui.overlayAlpha = state.overlayVisible ? 1.0f : 0.0f;
  ui.chromeVisible = state.chromeVisible;
  ui.isPaused = state.paused;
  ui.title = state.windowTitle;
  ui.transientMessage = state.transientMessage;
  std::vector<OverlayControlSpec> controlSpecs =
      buildOverlayControlSpecs(state, hoverControlToken);
  ui.progressSuffix = buildWindowOverlayProgressSuffix(state);
  ui.controlButtons.clear();
  ui.controlButtons.reserve(controlSpecs.size());
  for (size_t i = 0; i < controlSpecs.size(); ++i) {
    WindowUiState::ControlButton btn;
    btn.id = controlSpecs[i].id;
    btn.text = controlSpecs[i].renderText;
    btn.active = controlSpecs[i].active;
    btn.hovered =
        overlayControlToken(controlSpecs[i].id) == hoverControlToken;
    ui.controlButtons.push_back(std::move(btn));
  }
  ui.subtitleClockUs = state.subtitleClockUs;
  ui.subtitleAssScript = state.subtitleAssScript;
  ui.subtitleAssFonts = state.subtitleAssFonts;
  ui.subtitleRenderError = state.subtitleRenderError;
  ui.subtitleCues = state.subtitleCues;
  ui.displaySec = state.displaySec;
  ui.volPct = state.volPct;
  ui.subtitle = state.subtitleText;
  ui.subtitleAlpha =
      (state.subtitleCues.empty() && !state.subtitleAssScript) ? 0.0f : 1.0f;
  ui.contextMenu = state.contextMenu;
  ui.videoEdit = state.videoEdit;
  ui.videoEditExport = state.videoEditExport;
  ui.videoEditPrompt = state.videoEditPrompt;
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
  int width() const { return width_; }
  int height() const { return lastY_; }

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
      : frame_(frame),
        transparentSpace_(overlayGpuCell(
            L' ', baseStyle, kGpuTextGridCellFlagTransparentBg)) {
    frame_.cols = std::max(1, cols);
    frame_.rows = std::max(1, rows);
    const size_t cellCount =
        static_cast<size_t>(frame_.cols) * static_cast<size_t>(frame_.rows);
    frame_.cells.assign(cellCount, transparentSpace_);
  }

  bool isDrawable() const { return frame_.cols > 0 && frame_.rows > 0; }
  int width() const { return frame_.cols; }
  int height() const { return frame_.rows; }

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

  void clearRect(int x, int y, int width, int height) {
    const int left = std::clamp(x, 0, frame_.cols);
    const int top = std::clamp(y, 0, frame_.rows);
    const int right = std::clamp(x + std::max(0, width), 0, frame_.cols);
    const int bottom = std::clamp(y + std::max(0, height), 0, frame_.rows);
    for (int row = top; row < bottom; ++row) {
      for (int column = left; column < right; ++column) {
        frame_.cells[static_cast<size_t>(row * frame_.cols + column)] =
            transparentSpace_;
      }
    }
  }

 private:
  GpuTextGridFrame& frame_;
  GpuTextGridCell transparentSpace_;
};

OverlayCellTextLine layoutTransientMessageLine(const std::string& message,
                                               int width, int height) {
  OverlayCellTextLine line;
  const int safeWidth = std::max(1, width);
  const int horizontalInset = safeWidth > 2 ? 1 : 0;
  const int availableWidth =
      std::max(1, safeWidth - horizontalInset * 2);
  line.text = " " + message + " ";
  if (utf8DisplayWidth(line.text) > availableWidth) {
    line.text = utf8TakeDisplayWidth(line.text, availableWidth);
  }
  line.x = std::max(horizontalInset,
                    safeWidth - horizontalInset -
                        utf8DisplayWidth(line.text));
  line.y = std::min(1, std::max(0, height - 1));
  return line;
}

template <typename Target>
void renderVideoEditTimelineToTarget(
    Target& target, const OverlayCellLayout& layout,
    const OverlayRenderStyles& styles, double progress,
    const playback_video_edit::EditSnapshot& edit,
    const playback_video_edit::ExportProgress* editExport,
    playback_video_edit::Prompt editPrompt) {
  if ((!edit.active && editPrompt == playback_video_edit::Prompt::None &&
       !(editExport && editExport->running())) ||
      layout.progressBarY < 0 || layout.progressBarWidth <= 0 ||
      !target.rowVisible(layout.progressBarY)) {
    return;
  }

  const int width = layout.progressBarWidth;
  const playback_video_edit::OverlayModel model =
      playback_video_edit::buildOverlayModel(edit, editExport,
                                              editPrompt, width,
                                              progress);
  const Style keptStyle{styles.progressStart, styles.progressEmptyStyle.bg};
  const Style selectedStyle{styles.accentStyle.bg, styles.accentStyle.fg};
  const Style cutStyle{{255, 145, 96}, styles.progressEmptyStyle.bg};
  const Style playheadStyle{styles.baseStyle.bg, styles.baseStyle.fg};

  for (int cell = 0; cell < static_cast<int>(model.cells.size()); ++cell) {
    const playback_video_edit::TimelineCellKind kind =
        model.cells[static_cast<size_t>(cell)];
    const bool selected =
        kind == playback_video_edit::TimelineCellKind::Selected;
    const wchar_t glyph = selected ? L'=' : L'─';
    const Style& style = selected ? selectedStyle : keptStyle;
    target.writeChar(layout.progressBarX + cell, layout.progressBarY, glyph,
                     style);
  }

  for (const int cutCell : model.cutCells) {
    target.writeChar(layout.progressBarX + cutCell, layout.progressBarY, L'┆',
                     cutStyle);
  }

  if (!model.cells.empty()) {
    target.writeChar(layout.progressBarX + model.playheadCell,
                     layout.progressBarY, L'│', playheadStyle);
  }
  if (edit.active && model.inCell) {
    target.writeChar(layout.progressBarX + *model.inCell,
                     layout.progressBarY, L'I',
                     styles.accentStyle);
  }
  if (edit.active && model.outCell) {
    target.writeChar(layout.progressBarX + *model.outCell,
                     layout.progressBarY, L'O',
                     styles.accentStyle);
  }

  const int statusY = layout.progressBarY - 1;
  if (!target.rowVisible(statusY)) return;
  const std::string status =
      utf8TakeDisplayWidth(model.status, target.width());
  target.writeText(0, statusY, status, styles.accentStyle);
}

template <typename Target>
void renderTransientMessageToTarget(Target& target,
                                    const std::string& message,
                                    const Style& style) {
  if (!target.isDrawable()) return;
  const OverlayCellTextLine line = layoutTransientMessageLine(
      message, target.width(), target.height());
  target.writeText(line.x, line.y, line.text, style);
}

template <typename Target>
void renderContextMenuToTarget(Target& target,
                               const ContextMenuCellLayout& layout,
                               const OverlayRenderStyles& styles) {
  if (!target.isDrawable() || !layout.drawable()) return;

  const int left = layout.x;
  const int right = layout.x + layout.width - 1;
  const int top = layout.y;
  const int bottom = layout.y + layout.height - 1;
  for (int x = left + 1; x < right; ++x) {
    target.writeChar(x, top, L'─', styles.accentStyle);
    target.writeChar(x, bottom, L'─', styles.accentStyle);
  }
  target.writeChar(left, top, L'┌', styles.accentStyle);
  target.writeChar(right, top, L'┐', styles.accentStyle);
  target.writeChar(left, bottom, L'└', styles.accentStyle);
  target.writeChar(right, bottom, L'┘', styles.accentStyle);

  const Style selectedStyle{styles.accentStyle.bg, styles.accentStyle.fg};
  for (const ContextMenuCellItem& item : layout.items) {
    const Style& rowStyle = item.selected ? selectedStyle : styles.baseStyle;
    const std::string row = fitCellText(" " + item.text, item.width);
    target.writeControlText(row, item.y, item.x, item.width, rowStyle);
    target.writeChar(left, item.y, L'│', styles.accentStyle);
    target.writeChar(right, item.y, L'│', styles.accentStyle);
  }
}

template <typename Target>
void renderOverlayToTarget(Target& target, const OverlayCellLayout& layout,
                           const OverlayRenderStyles& styles,
                           double progress,
                           const playback_video_edit::EditSnapshot* videoEdit,
                           const playback_video_edit::ExportProgress*
                               videoEditExport,
                           playback_video_edit::Prompt videoEditPrompt) {
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

  if (videoEdit) {
    renderVideoEditTimelineToTarget(target, layout, styles, progress,
                                    *videoEdit, videoEditExport,
                                    videoEditPrompt);
  }

  target.writeText(layout.suffixX, layout.suffixY, layout.suffixText,
                   styles.baseStyle);
}

template <typename Target>
void renderTimelinePreviewTimestampToTarget(
    Target& target,
    const playback_video_timeline_preview::CellLayout& layout,
    const OverlayRenderStyles& styles) {
  if (!target.isDrawable() || !layout.drawable()) return;

  const int availableLabelWidth = std::max(0, layout.outerWidth - 4);
  const std::string label =
      utf8TakeDisplayWidth(layout.label, availableLabelWidth);
  const int labelWidth = utf8DisplayWidth(label);
  const int labelX =
      layout.outerX + std::max(2, (layout.outerWidth - labelWidth) / 2);
  target.writeText(labelX, layout.labelY, label, styles.accentStyle);
}

template <typename Target>
void renderTimelinePreviewChromeToTarget(
    Target& target,
    const playback_video_timeline_preview::CellLayout& layout,
    const OverlayRenderStyles& styles) {
  if (!target.isDrawable() || !layout.drawable()) return;

  const int left = layout.outerX;
  const int right = layout.outerX + layout.outerWidth - 1;
  const int top = layout.outerY;
  const int bottom = layout.outerY + layout.outerHeight - 1;
  for (int x = left + 1; x < right; ++x) {
    target.writeChar(x, top, L'─', styles.accentStyle);
    target.writeChar(x, bottom, L'─', styles.accentStyle);
  }
  for (int y = top + 1; y < bottom; ++y) {
    target.writeChar(left, y, L'│', styles.accentStyle);
    target.writeChar(right, y, L'│', styles.accentStyle);
  }
  target.writeChar(left, top, L'┌', styles.accentStyle);
  target.writeChar(right, top, L'┐', styles.accentStyle);
  target.writeChar(left, bottom, L'└', styles.accentStyle);
  target.writeChar(right, bottom, L'┘', styles.accentStyle);

  renderTimelinePreviewTimestampToTarget(target, layout, styles);
}

}  // namespace

void renderOverlayToScreen(ConsoleScreen& screen,
                           const OverlayCellLayout& layout,
                           const OverlayRenderStyles& styles,
                           double progress,
                           const playback_video_edit::EditSnapshot* videoEdit,
                           const playback_video_edit::ExportProgress*
                               videoEditExport,
                           playback_video_edit::Prompt videoEditPrompt,
                           int minY,
                           int maxY) {
  ScreenOverlayTarget target(screen, minY, maxY);
  renderOverlayToTarget(target, layout, styles, progress, videoEdit,
                        videoEditExport, videoEditPrompt);
}

void renderTransientMessageToScreen(ConsoleScreen& screen,
                                    const std::string& message,
                                    const Style& style) {
  ScreenOverlayTarget target(screen, 0, screen.height());
  renderTransientMessageToTarget(target, message, style);
}

void renderContextMenuToScreen(ConsoleScreen& screen,
                               const ContextMenuCellLayout& layout,
                               const OverlayRenderStyles& styles) {
  ScreenOverlayTarget target(screen, 0, screen.height());
  renderContextMenuToTarget(target, layout, styles);
}

void renderTimelinePreviewChromeToScreen(
    ConsoleScreen& screen,
    const playback_video_timeline_preview::CellLayout& layout,
    const OverlayRenderStyles& styles) {
  ScreenOverlayTarget target(screen, 0, screen.height());
  renderTimelinePreviewChromeToTarget(target, layout, styles);
}

void renderTimelinePreviewTimestampToScreen(
    ConsoleScreen& screen,
    const playback_video_timeline_preview::CellLayout& layout,
    const OverlayRenderStyles& styles) {
  ScreenOverlayTarget target(screen, 0, screen.height());
  renderTimelinePreviewTimestampToTarget(target, layout, styles);
}

bool renderWindowUiToGpuTextGrid(const WindowUiState& ui,
                                 const OverlayCellLayout& overlayLayout,
                                 int cellPixelWidth, int cellPixelHeight,
                                 TimelinePreviewPresentation previewPresentation,
                                 const OverlayRenderStyles& styles,
                                 GpuTextGridFrame& outFrame) {
  GpuTextGridOverlayTarget target(outFrame, overlayLayout.width,
                                  overlayLayout.height, styles.baseStyle);
  bool rendered = false;
  if (ui.chromeVisible) {
    renderOverlayToTarget(target, overlayLayout, styles, ui.progress,
                          &ui.videoEdit, &ui.videoEditExport,
                          ui.videoEditPrompt);
    rendered = true;
  }
  if (ui.timelinePreview.hoverActive) {
    const bool imageReady =
        previewPresentation ==
            TimelinePreviewPresentation::ImageAndTimestamp &&
        ui.timelinePreview.hasImage();
    const playback_video_image::RgbaImage* previewSurface =
        imageReady ? &ui.timelinePreview.image->surface : nullptr;
    const int sourceWidth =
        previewSurface ? static_cast<int>(previewSurface->width)
                       : std::max(16, ui.timelinePreview.sourceWidth);
    const int sourceHeight =
        previewSurface ? static_cast<int>(previewSurface->height)
                       : std::max(9, ui.timelinePreview.sourceHeight);
    const auto previewLayout = playback_video_timeline_preview::layoutCells(
        overlayLayout.width, overlayLayout.height,
        overlayLayout.progressBarY, overlayLayout.progressBarX,
        overlayLayout.progressBarWidth, ui.timelinePreview.anchorRatio,
        sourceWidth, sourceHeight, cellPixelWidth, cellPixelHeight,
        playback_video_timeline_preview::formatTimestamp(
            ui.timelinePreview.targetUs));
    if (imageReady) {
      target.clearRect(previewLayout.outerX, previewLayout.outerY,
                       previewLayout.outerWidth, previewLayout.outerHeight);
      renderTimelinePreviewChromeToTarget(target, previewLayout, styles);
    } else {
      renderTimelinePreviewTimestampToTarget(target, previewLayout, styles);
    }
    rendered = rendered || previewLayout.drawable();
  }
  if (ui.transientMessage) {
    renderTransientMessageToTarget(target, *ui.transientMessage,
                                   styles.accentStyle);
    rendered = true;
  }
  if (ui.contextMenu.visible) {
    const ContextMenuCellLayout menuLayout = layoutContextMenuCells(
        ui.contextMenu, overlayLayout.width, overlayLayout.height);
    renderContextMenuToTarget(target, menuLayout, styles);
    rendered = rendered || menuLayout.drawable();
  }
  return rendered;
}

}  // namespace playback_overlay
