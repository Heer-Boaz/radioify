#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "consoleinput.h"
#include "subtitle_manager.h"
#include "videowindow.h"

namespace playback_overlay {

enum class OverlayControlId {
  Previous,
  PlayPause,
  Next,
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
};

struct OverlayControlSpecOptions {
  bool includeRadio = true;
  bool includeAudioTrack = true;
  bool includeSubtitles = true;
  bool includePictureInPicture = true;
};

struct OverlayControlActions {
  std::function<bool()> previous;
  std::function<bool()> playPause;
  std::function<bool()> next;
  std::function<bool()> radio;
  std::function<bool()> hz50;
  std::function<bool()> audioTrack;
  std::function<bool()> subtitles;
  std::function<bool()> pictureInPicture;
};

struct OverlayCellControlInput {
  std::string text;
  int width = 0;
  bool active = false;
  bool hovered = false;
  int controlIndex = -1;
};

struct OverlayCellLayoutInput {
  int width = 0;
  int height = 0;
  std::string title;
  std::string suffix;
  int reservedRowsAboveProgress = 0;
  std::vector<OverlayCellControlInput> controls;
};

struct OverlayCellControlLayoutItem {
  std::string text;
  int controlIndex = -1;
  int x = 0;
  int y = 0;
  int width = 0;
  bool active = false;
  bool hovered = false;
};

struct OverlayCellLayout {
  int width = 0;
  int height = 0;
  int topY = -1;
  int titleX = 0;
  int titleY = -1;
  std::string titleText;
  int suffixX = 0;
  int suffixY = -1;
  std::string suffixText;
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
  std::vector<OverlayCellControlLayoutItem> controls;
};

struct PlaybackOverlayInputs {
  std::string windowTitle;
  bool audioOk = false;
  bool audioSupports50HzToggle = false;
  bool canPlayPrevious = false;
  bool canPlayNext = false;
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
  bool canPlayPrevious = false;
  bool canPlayNext = false;
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
std::vector<OverlayControlSpec> buildOverlayControlSpecs(
    const PlaybackOverlayState& state, int hoverIndex,
    const OverlayControlSpecOptions& options);

OverlayControlSpec makeOverlayTextControlSpec(OverlayControlId id,
                                              const std::string& label,
                                              bool active);
std::vector<OverlayCellControlInput> buildOverlayCellControlInputs(
    const std::vector<OverlayControlSpec>& specs, int hoverIndex);
bool dispatchOverlayControl(OverlayControlId id,
                            const OverlayControlActions& actions);

OverlayCellLayout layoutOverlayCells(const OverlayCellLayoutInput& input);

OverlayCellLayout layoutPlaybackOverlayCells(
    const PlaybackOverlayState& state, int width, int height, int hoverIndex);

int overlayCellControlAt(const OverlayCellLayout& layout, int cellX,
                         int cellY);

std::string buildWindowOverlayProgressSuffix(
    const PlaybackOverlayState& state);

std::string buildWindowOverlayTopLine(const PlaybackOverlayState& state);

int terminalOverlayControlAt(const PlaybackOverlayState& state,
                            const MouseEvent& mouse);

int windowOverlayControlAt(const PlaybackOverlayState& state,
                          const MouseEvent& mouse, int cellPixelWidth,
                          int cellPixelHeight);

bool isBackMousePressed(const MouseEvent& mouse);
bool overlayProgressRatioAt(const PlaybackOverlayState& state,
                            const MouseEvent& mouse, int cellPixelWidth,
                            int cellPixelHeight, double* outRatio);

WindowUiState buildWindowUiState(const PlaybackOverlayState& state,
                                int hoverIndex);

}  // namespace playback_overlay
