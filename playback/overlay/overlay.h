#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "consolescreen.h"
#include "gpu_text_grid.h"
#include "playback/overlay/context_menu.h"
#include "playback/overlay/interaction.h"
#include "playback/overlay/media_action_confirmation_presentation.h"
#include "playback/overlay/osd_state.h"
#include "playback/media_processing_service.h"
#include "playback/video/edit/command.h"
#include "playback/video/subtitle/manager.h"
#include "playback/video/framebuffer/window/window.h"
#include "playback/video/edit/view.h"

namespace playback_overlay {

struct OverlayControlSpec {
  OverlayControlId id = OverlayControlId::Radio;
  std::string normalText;
  std::string hoverText;
  std::string renderText;
  bool active = false;
  bool enabled = true;
  int width = 0;
};

struct OverlayControlSpecOptions {
  bool includeRadio = true;
  bool includeAudioTrack = true;
  bool includeSubtitles = true;
  bool includePictureInPicture = true;
};

enum class OverlayAction : std::uint8_t {
  Previous,
  TogglePlayPause,
  Next,
  ToggleRadio,
  Toggle50Hz,
  CycleAudioTrack,
  ToggleSubtitles,
  TogglePictureInPicture,
  WaitForVideoEditExport,
  ConfirmPendingExit,
  CancelPendingExit,
  CancelMediaTask,
  ConfirmMediaAction,
  DismissMediaAction,
};

using OverlayControlIntent =
    std::variant<OverlayAction, playback_video_edit::Command>;

struct OverlayCellControlInput {
  OverlayControlId id = OverlayControlId::Radio;
  std::string text;
  int width = 0;
  bool active = false;
  bool hovered = false;
  bool enabled = true;
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
  OverlayControlId id = OverlayControlId::Radio;
  std::string text;
  int x = 0;
  int y = 0;
  int width = 0;
  bool active = false;
  bool hovered = false;
  bool enabled = true;
};

struct OverlayCellTextLine {
  int x = 0;
  int y = -1;
  std::string text;
};

struct OverlayCellDialogLayout {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int titleX = 0;
  int titleY = -1;
  std::string title;
  std::vector<OverlayCellTextLine> contentLines;

  bool valid() const { return width >= 4 && height >= 4; }
};

struct OverlayDialogButtonInput {
  OverlayControlId id = OverlayControlId::Radio;
  std::string label;
  std::string compactLabel;
  bool selected = false;
  bool hovered = false;
  bool enabled = true;
};

struct OverlayDialogLayoutInput {
  int width = 0;
  int height = 0;
  std::string title;
  std::vector<std::string> text;
  std::vector<OverlayDialogButtonInput> buttons;
};

struct OverlayCellLayout {
  int width = 0;
  int height = 0;
  int topY = -1;
  int titleX = 0;
  int titleY = -1;
  std::string titleText;
  std::vector<OverlayCellTextLine> titleLines;
  int suffixX = 0;
  int suffixY = -1;
  std::string suffixText;
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
  std::vector<OverlayCellControlLayoutItem> controls;
  std::optional<OverlayCellDialogLayout> dialog;
};

struct SubtitlePresentation {
  std::string activeTrackLabel = "N/A";
  std::string text;
  std::shared_ptr<const std::string> assScript;
  std::shared_ptr<const SubtitleFontAttachmentList> assFonts;
  std::vector<WindowUiState::SubtitleCue> cues;
};

inline int overlayCellCountForPixels(int pixelExtent, int cellExtent) {
  const int safeCellExtent = std::max(1, cellExtent);
  const int safePixelExtent = std::max(0, pixelExtent);
  return std::max(1, (safePixelExtent + safeCellExtent - 1) / safeCellExtent);
}

struct PlaybackOverlayInputs {
  std::string windowTitle;
  bool audioOk = false;
  bool playPauseAvailable = false;
  bool audioSupports50HzToggle = false;
  bool canPlayPrevious = false;
  bool canPlayNext = false;
  bool radioEnabled = false;
  std::string radioLabel = "Radio: Off";
  bool hz50Enabled = false;
  bool canCycleAudioTracks = false;
  std::string activeAudioTrackLabel;
  SubtitlePresentation subtitle;
  bool hasSubtitles = false;
  bool subtitlesEnabled = false;
  int64_t subtitleClockUs = 0;
  bool seekingOverlay = false;
  double displaySec = 0.0;
  double totalSec = -1.0;
  int volPct = 0;
  PlaybackOsdSnapshot osd;
  bool paused = false;
  bool pictureInPictureAvailable = false;
  bool pictureInPictureActive = false;
  std::string subtitleRenderError;
  std::vector<std::string> debugLines;
  ContextMenuSnapshot contextMenu;
  playback_video_edit::EditSnapshot videoEdit;
  playback_video_edit::ExportProgress videoEditExport;
  playback_video_edit::Prompt videoEditPrompt =
      playback_video_edit::Prompt::None;
  std::optional<MediaActionConfirmationDialog>
      mediaActionConfirmationPrompt;
  std::optional<playback_media_processing::Activity> mediaTaskActivity;
};

struct PlaybackOverlayState {
  std::string windowTitle;
  bool audioOk = false;
  bool playPauseAvailable = false;
  bool audioSupports50HzToggle = false;
  bool canPlayPrevious = false;
  bool canPlayNext = false;
  bool radioEnabled = false;
  std::string radioLabel = "Radio: Off";
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
  // Resolved presentation policy. Render backends consume this value instead
  // of inferring chrome visibility from edit/export document state.
  bool chromeVisible = false;
  std::shared_ptr<const std::string> transientMessage;
  bool paused = false;
  bool pictureInPictureAvailable = false;
  bool pictureInPictureActive = false;
  std::string subtitleText;
  std::string subtitleRenderError;
  std::shared_ptr<const std::string> subtitleAssScript;
  std::shared_ptr<const SubtitleFontAttachmentList> subtitleAssFonts;
  std::vector<WindowUiState::SubtitleCue> subtitleCues;
  std::vector<std::string> debugLines;
  ContextMenuSnapshot contextMenu;
  playback_video_edit::EditSnapshot videoEdit;
  playback_video_edit::ExportProgress videoEditExport;
  playback_video_edit::Prompt videoEditPrompt =
      playback_video_edit::Prompt::None;
  std::optional<MediaActionConfirmationDialog>
      mediaActionConfirmationPrompt;
  std::optional<playback_media_processing::Activity> mediaTaskActivity;
};

PlaybackOverlayState buildPlaybackOverlayState(
    const PlaybackOverlayInputs& inputs);

SubtitlePresentation projectSubtitlePresentation(
    const SubtitleManager& subtitleManager, bool subtitlesEnabled,
    bool seekingOverlay, int64_t clockUs, bool hasSubtitles);

std::vector<WindowUiState::SubtitleCue> collectSubtitleCues(
    const SubtitleManager& subtitleManager, bool subtitlesEnabled,
    bool seekingOverlay, int64_t clockUs, bool hasSubtitles);

std::string buildSubtitleText(const SubtitleManager& subtitleManager,
                             bool subtitlesEnabled, bool seekingOverlay,
                             int64_t clockUs, bool hasSubtitles);

std::vector<OverlayControlSpec> buildOverlayControlSpecs(
    const PlaybackOverlayState& state, int hoverControlToken);
std::vector<OverlayControlSpec> buildOverlayControlSpecs(
    const PlaybackOverlayState& state, int hoverControlToken,
    const OverlayControlSpecOptions& options);

OverlayControlSpec makeOverlayTextControlSpec(OverlayControlId id,
                                              const std::string& label,
                                              bool active,
                                              bool enabled = true);
std::vector<OverlayCellControlInput> buildOverlayCellControlInputs(
    const std::vector<OverlayControlSpec>& specs, int hoverControlToken);
std::vector<OverlayDialogButtonInput>
buildMediaActionConfirmationDialogButtons(
    const MediaActionConfirmationDialog& prompt,
    int hoverControlToken);
OverlayControlIntent intentForOverlayControl(OverlayControlId id);

OverlayCellLayout layoutOverlayCells(const OverlayCellLayoutInput& input);
OverlayCellLayout layoutOverlayControlCells(
    const std::vector<OverlayCellControlInput>& controls, int width);
OverlayCellLayout layoutOverlayDialogCells(
    const OverlayDialogLayoutInput& input);
OverlayCellLayout layoutMediaActionConfirmationDialogCells(
    const MediaActionConfirmationDialog& prompt, int width,
    int height, int hoverControlToken = -1);
OverlayCellLayout layoutPlaybackOverlayCells(
    const PlaybackOverlayState& state, int width, int height,
    int hoverControlToken);
OverlayCellLayout layoutWindowOverlayCells(const WindowUiState& ui, int width,
                                           int height);

std::string buildWindowOverlayProgressSuffix(
    const PlaybackOverlayState& state);

std::string buildWindowOverlayTopLine(const PlaybackOverlayState& state);

WindowUiState buildWindowUiState(const PlaybackOverlayState& state,
                                int hoverControlToken);

struct OverlayRenderStyles {
  Style baseStyle{{219, 224, 230}, {5, 6, 7}};
  Style accentStyle{{250, 176, 51}, baseStyle.bg};
  Style progressEmptyStyle{{32, 38, 46}, {32, 38, 46}};
  Style progressFrameStyle{{160, 170, 182}, baseStyle.bg};
  Color progressStart{110, 231, 183};
  Color progressEnd{255, 214, 110};
};

enum class TimelinePreviewPresentation {
  TimestampOnly,
  ImageAndTimestamp,
};

void renderOverlayToScreen(ConsoleScreen& screen,
                           const OverlayCellLayout& layout,
                           const OverlayRenderStyles& styles,
                           double progress,
                           const playback_video_edit::EditSnapshot* videoEdit,
                           const playback_video_edit::ExportProgress*
                               videoEditExport,
                           playback_video_edit::Prompt videoEditPrompt,
                           const std::optional<MediaActionConfirmationDialog>&
                               mediaActionConfirmationPrompt,
                           int minY,
                           int maxY);

void renderTransientMessageToScreen(ConsoleScreen& screen,
                                    const std::string& message,
                                    const Style& style);

void renderContextMenuToScreen(ConsoleScreen& screen,
                               const ContextMenuCellLayout& layout,
                               const OverlayRenderStyles& styles);

void renderTimelinePreviewChromeToScreen(
    ConsoleScreen& screen,
    const playback_video_timeline_preview::CellLayout& layout,
    const OverlayRenderStyles& styles);

void renderTimelinePreviewTimestampToScreen(
    ConsoleScreen& screen,
    const playback_video_timeline_preview::CellLayout& layout,
    const OverlayRenderStyles& styles);

bool renderWindowUiToGpuTextGrid(const WindowUiState& ui,
                                 const OverlayCellLayout& overlayLayout,
                                 int cellPixelWidth, int cellPixelHeight,
                                 TimelinePreviewPresentation previewPresentation,
                                 const OverlayRenderStyles& styles,
                                 GpuTextGridFrame& outFrame);

}  // namespace playback_overlay
