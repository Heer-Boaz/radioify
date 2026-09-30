#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "playback/media_processing_presentation.h"
#include "playback/video/edit/overlay_model.h"
#include "subtitle_effects.h"

namespace playback_overlay {

PlaybackOverlayState
buildPlaybackOverlayState(const PlaybackOverlayInputs &inputs) {
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
  state.activeSubtitleLabel = inputs.subtitle.activeTrackLabel;
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
  state.subtitleText = inputs.subtitle.text;
  state.subtitleAssScript = inputs.subtitle.assScript;
  state.subtitleAssFonts = inputs.subtitle.assFonts;
  state.subtitleCues = inputs.subtitle.cues;
  state.subtitleRenderError = inputs.subtitleRenderError;
  state.debugLines = inputs.debugLines;
  state.contextMenu = inputs.contextMenu;
  state.videoEdit = inputs.videoEdit;
  state.videoEditExport = inputs.videoEditExport;
  state.videoEditPrompt = inputs.videoEditPrompt;
  state.mediaActionConfirmationPrompt = inputs.mediaActionConfirmationPrompt;
  state.mediaTaskActivity = inputs.mediaTaskActivity;
  state.chromeVisible =
      state.overlayVisible || !state.debugLines.empty() ||
      state.mediaActionConfirmationPrompt.has_value() ||
      state.mediaTaskActivity.has_value() ||
      playback_video_edit::needsOverlayPresentation(
          state.videoEdit, state.videoEditExport, state.videoEditPrompt);

  return state;
}

SubtitlePresentation
projectSubtitlePresentation(const SubtitleTrack *activeTrack,
                            bool subtitlesEnabled, bool seekingOverlay,
                            int64_t clockUs, bool hasSubtitles) {
  SubtitlePresentation presentation;
  presentation.activeTrackLabel =
      hasSubtitles && activeTrack && !activeTrack->label.empty()
          ? activeTrack->label : "N/A";
  presentation.text = buildSubtitleText(activeTrack, subtitlesEnabled,
                                        seekingOverlay, clockUs, hasSubtitles);
  presentation.cues = collectSubtitleCues(
      activeTrack, subtitlesEnabled, seekingOverlay, clockUs, hasSubtitles);
  if (subtitlesEnabled && !seekingOverlay && clockUs >= 0 && hasSubtitles) {
    if (activeTrack) {
      presentation.assScript = activeTrack->assScript;
      presentation.assFonts = activeTrack->assFonts;
    }
  }
  return presentation;
}

std::vector<WindowUiState::SubtitleCue>
collectSubtitleCues(const SubtitleTrack *activeTrack,
                    bool subtitlesEnabled, bool seekingOverlay, int64_t clockUs,
                    bool hasSubtitles) {
  std::vector<WindowUiState::SubtitleCue> out;
  if (!subtitlesEnabled || seekingOverlay || clockUs < 0 || !hasSubtitles) {
    return out;
  }
  if (!activeTrack) {
    return out;
  }

  std::vector<const SubtitleCue *> active;
  activeTrack->cuesAt(clockUs, &active);
  if (active.empty())
    return out;
  out.reserve(active.size());
  for (const SubtitleCue *cue : active) {
    if (!cue)
      continue;
    const float fadeOpacity = subtitleFadeOpacity(*cue, clockUs);
    if (fadeOpacity <= 0.001f)
      continue;
    const bool hasRenderableAss = cue->assStyled && !cue->rawText.empty();
    if (cue->text.empty() && !hasRenderableAss)
      continue;
    WindowUiState::SubtitleCue item;
    item.text = cue->text;
    item.rawText = cue->rawText;
    item.textRuns.reserve(cue->textRuns.size());
    for (const SubtitleTextRun &run : cue->textRuns) {
      if (run.text.empty())
        continue;
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
      item.posX = static_cast<float>(
          cue->moveStartXNorm + (cue->moveEndXNorm - cue->moveStartXNorm) * t);
      item.posY = static_cast<float>(
          cue->moveStartYNorm + (cue->moveEndYNorm - cue->moveStartYNorm) * t);
    }
    item.marginVNorm = cue->marginVNorm;
    item.marginLNorm = cue->marginLNorm;
    item.marginRNorm = cue->marginRNorm;
    applySubtitleTimelineEffects(&item, *cue, clockUs, fadeOpacity);
    out.push_back(std::move(item));
  }

  std::stable_sort(out.begin(), out.end(),
                   [](const WindowUiState::SubtitleCue &a,
                      const WindowUiState::SubtitleCue &b) {
                     if (a.layer != b.layer)
                       return a.layer < b.layer;
                     if (a.sizeScale != b.sizeScale)
                       return a.sizeScale > b.sizeScale;
                     return a.text < b.text;
                   });
  return out;
}

std::string buildSubtitleText(const SubtitleTrack *activeTrack,
                              bool subtitlesEnabled, bool seekingOverlay,
                              int64_t clockUs, bool hasSubtitles) {
  if (!subtitlesEnabled || seekingOverlay || clockUs < 0 || !hasSubtitles) {
    return {};
  }
  if (!activeTrack) {
    return {};
  }
  std::vector<const SubtitleCue *> active;
  activeTrack->cuesAt(clockUs, &active);
  if (active.empty())
    return {};

  std::stable_sort(active.begin(), active.end(),
                   [](const SubtitleCue *a, const SubtitleCue *b) {
                     if (!a || !b)
                       return a < b;
                     if (a->layer != b->layer)
                       return a->layer < b->layer;
                     if (a->sizeScale != b->sizeScale)
                       return a->sizeScale > b->sizeScale;
                     if (a->startUs != b->startUs)
                       return a->startUs < b->startUs;
                     return a->text < b->text;
                   });

  std::string merged;
  for (const SubtitleCue *cue : active) {
    if (!cue || cue->text.empty())
      continue;
    if (!merged.empty())
      merged.push_back('\n');
    merged += cue->text;
  }
  return merged;
}

std::string buildWindowOverlayTopLine(const PlaybackOverlayState &state) {
  const std::string badge =
      playback_video_edit::retainedProgramBadge(state.videoEdit);
  const std::string playbackTitle =
      badge.empty() ? state.windowTitle : badge + " " + state.windowTitle;
  if (!state.mediaTaskActivity)
    return playbackTitle;
  const std::string task =
      playback_media_processing::activityStatusLine(*state.mediaTaskActivity);
  return task.empty() ? playbackTitle : task + "\n" + playbackTitle;
}

WindowUiState buildWindowUiState(const PlaybackOverlayState &state,
                                 int hoverControlToken) {
  WindowUiState ui;
  ui.progress = (state.totalSec > 0.0 && std::isfinite(state.totalSec))
                    ? static_cast<float>(std::clamp(
                          state.displaySec / state.totalSec, 0.0, 1.0))
                    : 0.0f;
  ui.overlayAlpha = state.overlayVisible ? 1.0f : 0.0f;
  ui.chromeVisible = state.chromeVisible;
  ui.isPaused = state.paused;
  ui.title = buildWindowOverlayTopLine(state);
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
    btn.enabled = controlSpecs[i].enabled;
    btn.tone = controlSpecs[i].tone;
    btn.hovered = controlSpecs[i].enabled &&
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
  ui.mediaActionConfirmationPrompt = state.mediaActionConfirmationPrompt;
  return ui;
}

} // namespace playback_overlay
