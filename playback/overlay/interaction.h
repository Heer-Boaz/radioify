#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "playback/video/edit/view.h"

namespace playback_video_chapters {
struct OverviewPanelLayout;
}

namespace playback_overlay {

struct OverlayCellLayout;

enum class OverlayControlId {
  Previous,
  PlayPause,
  Next,
  Radio,
  Hz50,
  AudioTrack,
  Subtitles,
  PictureInPicture,
  Chapters,
  ChapterOverviewClose,
  ChapterInstall,
  ChapterCancel,
  EditMarkIn,
  EditMarkOut,
  EditClearSelection,
  EditRippleDelete,
  EditTrim,
  EditSuggestions,
  EditSuggestionFilter,
  EditPreviousSuggestion,
  EditNextSuggestion,
  EditSelectSuggestion,
  EditHideSuggestion,
  EditUndoHideSuggestion,
  EditDone,
  EditStartExport,
  EditWaitForExport,
  EditCancelExport,
  EditConfirmPrompt,
  EditCancelPrompt,
  EditDiscardAndExit,
  EditCancelExit,
  MediaTaskCancel,
  MediaActionPrimary,
  MediaActionSecondary,
};

// Semantic presentation is independent from interaction state. In
// particular, an asynchronous failure must not be encoded as an "active"
// toggle merely to obtain a different color.
enum class OverlayControlTone : std::uint8_t {
  Normal,
  Warning,
  Error,
};

constexpr int overlayControlToken(OverlayControlId id) {
  return static_cast<int>(id);
}

struct InteractionRect {
  // Renderer-space bounds; right and bottom are exclusive.
  double left = 0.0;
  double top = 0.0;
  double right = 0.0;
  double bottom = 0.0;

  bool valid() const;
  bool contains(double x, double y) const;
};

struct ProgressBarRegion {
  InteractionRect bounds;
  int units = 0;
};

struct OverlayControlRegion {
  InteractionRect bounds;
  OverlayControlId id = OverlayControlId::Radio;
};

using ContextMenuItemToken = uint32_t;

struct ContextMenuItemRegion {
  InteractionRect bounds;
  ContextMenuItemToken token = 0;
};

struct EditBoundaryRegion {
  InteractionRect bounds;
  playback_video_edit::EditBoundary boundary =
      playback_video_edit::EditBoundary::In;
};

struct ChapterOverviewRegion {
  struct Item {
    InteractionRect bounds;
    std::int64_t startUs = 0;
  };

  InteractionRect bounds;
  std::optional<InteractionRect> closeButton;
  int scrollOffset = 0;
  int maximumScrollOffset = 0;
  std::vector<Item> items;
};

struct InteractionMap {
  // Immutable-by-convention snapshot of one rendered overlay.
  bool modal = false;
  std::optional<ProgressBarRegion> progressBar;
  std::vector<OverlayControlRegion> controls;
  std::vector<ContextMenuItemRegion> contextMenuItems;
  std::vector<EditBoundaryRegion> editBoundaries;
  std::optional<ChapterOverviewRegion> chapterOverview;

  bool contains(double x, double y) const;
};

struct ProgressBarHit {
  double ratio = 0.0;
  int units = 0;
};

struct InteractionHit {
  std::optional<ProgressBarHit> progressBar;
  std::optional<OverlayControlId> control;
  std::optional<ContextMenuItemToken> contextMenuItem;
  std::optional<playback_video_edit::EditBoundary> editBoundary;
  std::optional<ChapterOverviewRegion> chapterOverview;
  std::optional<std::int64_t> chapterStartUs;
};

std::optional<ProgressBarHit> progressBarHitAt(const ProgressBarRegion &region,
                                               double x, double y,
                                               bool captured = false);
std::optional<ProgressBarHit> progressBarHitAt(const InteractionMap &map,
                                               double x, double y,
                                               bool captured = false);
std::optional<OverlayControlId> overlayControlAt(const InteractionMap &map,
                                                 double x, double y);
std::optional<ContextMenuItemToken> contextMenuItemAt(const InteractionMap &map,
                                                      double x, double y);
std::optional<playback_video_edit::EditBoundary>
editBoundaryAt(const InteractionMap &map, double x, double y);
bool editBoundaryHandleAt(const InteractionMap &map, double x, double y);
InteractionHit interactionHitAt(const InteractionMap &map, double x, double y,
                                bool capturedProgress = false);
InteractionHit interactionHitAtTransformed(const InteractionMap &map,
                                           double offsetX, double offsetY,
                                           double scaleX, double scaleY,
                                           double x, double y,
                                           bool capturedProgress = false);

InteractionMap transformInteractionMap(const InteractionMap &map,
                                       double offsetX, double offsetY,
                                       double scaleX, double scaleY);
InteractionMap buildOverlayInteractionMap(
    const OverlayCellLayout &layout,
    const playback_video_edit::EditSnapshot *videoEdit = nullptr,
    playback_video_edit::Prompt videoEditPrompt =
        playback_video_edit::Prompt::None,
    bool mediaActionConfirmationPrompt = false,
    const ChapterOverviewRegion *chapterOverview = nullptr);

std::optional<ChapterOverviewRegion> chapterOverviewRegionForLayout(
    const playback_video_chapters::OverviewPanelLayout &layout);

} // namespace playback_overlay
