#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "consoleinput.h"
#include "subtitle_manager.h"
#include "videowindow.h"

namespace playback_overlay {

enum class OverlayControlId {
  Radio,
  Hz50,
  AudioTrack,
  Subtitles,
  PictureInPicture,
};

struct OverlayControlSpec {
  OverlayControlId id = OverlayControlId::Radio;
  std::string normalText;
  std::string hoverText;
  std::string renderText;
  bool active = false;
  int width = 0;
  int charStart = 0;
};

struct TerminalOverlayControlLayoutItem {
  OverlayControlSpec spec;
  int controlIndex = -1;
  int x = 0;
  int y = 0;
};

struct PlaybackOverlayInputs {
  std::string windowTitle;
  bool audioOk = false;
  bool audioSupports50HzToggle = false;
  bool radioEnabled = false;
  bool hz50Enabled = false;
  bool canCycleAudioTracks = false;
  std::string activeAudioTrackLabel;
  const SubtitleManager* subtitleManager = nullptr;
  bool hasSubtitles = false;
  bool subtitlesEnabled = false;
  int64_t subtitleClockUs = 0;
  bool seekingOverlay = false;
  double displaySec = 0.0;
  double totalSec = -1.0;
  int volPct = 0;
  bool overlayVisible = false;
  bool paused = false;
  bool audioFinished = false;
  bool pictureInPictureAvailable = false;
  bool pictureInPictureActive = false;
  std::string subtitleRenderError;
  int screenWidth = 0;
  int screenHeight = 0;
  int windowWidth = 0;
  int windowHeight = 0;
  int artTop = 0;
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
};

struct PlaybackOverlayState {
  std::string windowTitle;
  bool audioOk = false;
  bool audioSupports50HzToggle = false;
  bool radioEnabled = false;
  bool hz50Enabled = false;
  bool canCycleAudioTracks = false;
  std::string activeAudioTrackLabel;
  bool hasSubtitles = false;
  bool subtitlesEnabled = false;
  std::string activeSubtitleLabel;
  int64_t subtitleClockUs = 0;
  bool seekingOverlay = false;
  double displaySec = 0.0;
  double totalSec = -1.0;
  int volPct = 0;
  bool overlayVisible = false;
  bool paused = false;
  bool audioFinished = false;
  bool pictureInPictureAvailable = false;
  bool pictureInPictureActive = false;
  std::string subtitleText;
  std::string subtitleRenderError;
  std::shared_ptr<const std::string> subtitleAssScript;
  std::shared_ptr<const SubtitleFontAttachmentList> subtitleAssFonts;
  std::vector<WindowUiState::SubtitleCue> subtitleCues;
  int screenWidth = 0;
  int screenHeight = 0;
  int windowWidth = 0;
  int windowHeight = 0;
  int artTop = 0;
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
};

PlaybackOverlayState buildPlaybackOverlayState(
    const PlaybackOverlayInputs& inputs);

std::vector<WindowUiState::SubtitleCue> collectSubtitleCues(
    const SubtitleManager& subtitleManager, bool subtitlesEnabled,
    bool seekingOverlay, int64_t clockUs, bool hasSubtitles);

std::string buildSubtitleText(const SubtitleManager& subtitleManager,
                             bool subtitlesEnabled, bool seekingOverlay,
                             int64_t clockUs, bool hasSubtitles);

std::vector<OverlayControlSpec> buildOverlayControlSpecs(
    const PlaybackOverlayState& state, int hoverIndex);

std::vector<TerminalOverlayControlLayoutItem> layoutTerminalOverlayControls(
    const PlaybackOverlayState& state, int hoverIndex);

std::string buildOverlayControlsText(const PlaybackOverlayState& state,
                                     int hoverIndex);

std::string buildWindowOverlayProgressSuffix(
    const PlaybackOverlayState& state);

std::string buildWindowOverlayTopLine(const PlaybackOverlayState& state);

int terminalOverlayControlAt(const PlaybackOverlayState& state,
                            const MouseEvent& mouse);

int windowOverlayControlAt(const PlaybackOverlayState& state,
                          const MouseEvent& mouse);

bool isBackMousePressed(const MouseEvent& mouse);
bool isProgressHit(const PlaybackOverlayState& state, const MouseEvent& mouse);

WindowUiState buildWindowUiState(const PlaybackOverlayState& state,
                                int hoverIndex);

}  // namespace playback_overlay
