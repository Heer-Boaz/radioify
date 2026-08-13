#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "playback/video/edit/view.h"

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
  EditMarkIn,
  EditMarkOut,
  EditRippleDelete,
  EditTrim,
  EditUndo,
  EditRedo,
  EditReset,
  EditExport,
  EditLeave,
  EditConfirmClose,
  EditCancelClose,
  EditDiscardAndExit,
  EditCancelExit,
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

struct InteractionMap {
  // Immutable-by-convention snapshot of one rendered overlay.
  bool modal = false;
  std::optional<ProgressBarRegion> progressBar;
  std::vector<OverlayControlRegion> controls;
  std::vector<ContextMenuItemRegion> contextMenuItems;
  std::vector<EditBoundaryRegion> editBoundaries;

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
};

std::optional<ProgressBarHit> progressBarHitAt(
    const ProgressBarRegion& region, double x, double y,
    bool captured = false);
std::optional<ProgressBarHit> progressBarHitAt(
    const InteractionMap& map, double x, double y, bool captured = false);
std::optional<OverlayControlId> overlayControlAt(
    const InteractionMap& map, double x, double y);
std::optional<ContextMenuItemToken> contextMenuItemAt(
    const InteractionMap& map, double x, double y);
std::optional<playback_video_edit::EditBoundary> editBoundaryAt(
    const InteractionMap& map, double x, double y);
bool editBoundaryHandleAt(const InteractionMap& map, double x, double y);
InteractionHit interactionHitAt(const InteractionMap& map, double x, double y,
                                bool capturedProgress = false);
InteractionHit interactionHitAtTransformed(
    const InteractionMap& map, double offsetX, double offsetY, double scaleX,
    double scaleY, double x, double y, bool capturedProgress = false);

InteractionMap transformInteractionMap(const InteractionMap& map,
                                       double offsetX, double offsetY,
                                       double scaleX, double scaleY);

}  // namespace playback_overlay
