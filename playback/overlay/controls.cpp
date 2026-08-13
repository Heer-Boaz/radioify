#include "playback/overlay/overlay.h"

#include <algorithm>
#include <string>
#include <utility>

#include "unicode_display_width.h"

namespace playback_overlay {
namespace {

int visibleWidth(const std::string& text) {
  std::string filtered;
  filtered.reserve(text.size());
  for (char c : text) {
    if (c != '\r' && c != '\n') filtered.push_back(c);
  }
  return utf8DisplayWidth(filtered);
}

void finishControlSpecs(std::vector<OverlayControlSpec>* specs,
                        int hoverControlToken) {
  if (!specs) return;
  for (OverlayControlSpec& spec : *specs) {
    const bool hovered =
        spec.enabled && overlayControlToken(spec.id) == hoverControlToken;
    spec.renderText = hovered ? spec.hoverText : spec.normalText;
    const int textWidth = visibleWidth(spec.renderText);
    if (textWidth < spec.width) {
      spec.renderText.append(static_cast<size_t>(spec.width - textWidth), ' ');
    } else if (textWidth > spec.width) {
      spec.renderText = utf8TakeDisplayWidth(spec.renderText, spec.width);
    }
  }
}

OverlayControlSpec makePlayPauseSpec(const PlaybackOverlayState& state) {
  OverlayControlSpec spec = makeOverlayTextControlSpec(
      OverlayControlId::PlayPause, state.paused ? "Play" : "Pause",
      state.paused, state.playPauseAvailable);
  const OverlayControlSpec widest = makeOverlayTextControlSpec(
      OverlayControlId::PlayPause, "Pause", false);
  spec.width = std::max(spec.width, widest.width);
  return spec;
}

}  // namespace

OverlayControlSpec makeOverlayTextControlSpec(OverlayControlId id,
                                              const std::string& label,
                                              bool active, bool enabled) {
  OverlayControlSpec spec;
  spec.id = id;
  spec.normalText = " [" + label + "] ";
  spec.hoverText = "[ " + label + " ]";
  spec.width =
      std::max(utf8DisplayWidth(spec.normalText),
               utf8DisplayWidth(spec.hoverText));
  spec.active = active;
  spec.enabled = enabled;
  return spec;
}

std::vector<OverlayCellControlInput> buildOverlayCellControlInputs(
    const std::vector<OverlayControlSpec>& specs, int hoverControlToken) {
  std::vector<OverlayCellControlInput> controls;
  controls.reserve(specs.size());
  for (const OverlayControlSpec& spec : specs) {
    const bool hovered =
        spec.enabled && overlayControlToken(spec.id) == hoverControlToken;
    OverlayCellControlInput control;
    control.id = spec.id;
    control.text = hovered ? spec.hoverText : spec.normalText;
    control.width = spec.width;
    control.active = spec.active;
    control.hovered = hovered;
    control.enabled = spec.enabled;
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
  const auto add = [&](OverlayControlId id, const std::string& label,
                       bool active, bool enabled = true) {
    out.push_back(makeOverlayTextControlSpec(id, label, active, enabled));
  };
  const auto finish = [&]() {
    finishControlSpecs(&out, hoverControlToken);
  };

  if (state.videoEditPrompt == playback_video_edit::Prompt::LeaveEditMode) {
    add(OverlayControlId::EditConfirmPrompt, "Leave", true);
    add(OverlayControlId::EditCancelPrompt, "Cancel", false);
    finish();
    return out;
  }

  if (state.videoEditPrompt == playback_video_edit::Prompt::DiscardEdits) {
    add(OverlayControlId::EditConfirmPrompt, "Discard", false);
    add(OverlayControlId::EditCancelPrompt, "Cancel", true);
    finish();
    return out;
  }

  if (state.videoEditPrompt == playback_video_edit::Prompt::LeavePlayback) {
    add(OverlayControlId::EditExport,
        state.videoEditExport.running() ? "Wait" : "Export",
        state.videoEditExport.running());
    add(OverlayControlId::EditDiscardAndExit, "Discard", false);
    add(OverlayControlId::EditCancelExit, "Stay", true);
    finish();
    return out;
  }

  if (state.videoEdit.active) {
    // Keep command identity and placement stable. Availability is UI state;
    // it must not add or remove toolbar slots as the selection/history changes.
    out.push_back(makePlayPauseSpec(state));
    add(OverlayControlId::EditMarkIn, "In", false);
    add(OverlayControlId::EditMarkOut, "Out", false);
    add(OverlayControlId::EditRippleDelete, "Delete", false,
        state.videoEdit.canRippleDelete);
    add(OverlayControlId::EditTrim, "Trim", false,
        state.videoEdit.canTrim);
    add(OverlayControlId::EditUndo, "Undo", false, state.videoEdit.canUndo);
    add(OverlayControlId::EditRedo, "Redo", false, state.videoEdit.canRedo);
    add(OverlayControlId::EditReset, "Reset", false,
        state.videoEdit.hasEdits);
    add(OverlayControlId::EditExport,
        state.videoEditExport.running() ? "Cancel" : "Export",
        state.videoEditExport.running(),
        state.videoEdit.hasEdits || state.videoEditExport.running());
    add(OverlayControlId::EditLeave, "Leave", false);
    if (options.includePictureInPicture && state.pictureInPictureAvailable) {
      add(OverlayControlId::PictureInPicture, "PiP",
          state.pictureInPictureActive);
    }
    finish();
    return out;
  }

  if (state.canPlayPrevious) {
    add(OverlayControlId::Previous, "<<", false);
  }
  if (state.playPauseAvailable) {
    add(OverlayControlId::PlayPause, "pause", state.paused);
  }
  if (state.canPlayNext) {
    add(OverlayControlId::Next, ">>", false);
  }

  if (options.includeRadio) {
    add(OverlayControlId::Radio, state.radioLabel, state.radioEnabled);
  }

  if (state.audioOk && state.audioSupports50HzToggle) {
    add(OverlayControlId::Hz50, "50Hz", state.hz50Enabled);
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
    add(OverlayControlId::AudioTrack, audioLabel,
        state.audioOk && state.canCycleAudioTracks);
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
    add(OverlayControlId::Subtitles, subtitleLabel, subtitlesActive);
  }

  if (options.includePictureInPicture && state.pictureInPictureAvailable) {
    add(OverlayControlId::PictureInPicture, "PiP",
        state.pictureInPictureActive);
  }

  finish();
  return out;
}

std::vector<OverlayControlSpec> buildOverlayControlSpecs(
    const PlaybackOverlayState& state, int hoverControlToken) {
  return buildOverlayControlSpecs(state, hoverControlToken,
                                  OverlayControlSpecOptions{});
}

}  // namespace playback_overlay
