#include "overlay.h"

#include "playback/video/edit/overlay_model.h"
#include "playback/video/edit/suggestion_panel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace playback_overlay {
namespace {

InteractionRect transformRect(const InteractionRect &rect, double offsetX,
                              double offsetY, double scaleX, double scaleY) {
  return {offsetX + rect.left * scaleX, offsetY + rect.top * scaleY,
          offsetX + rect.right * scaleX, offsetY + rect.bottom * scaleY};
}

} // namespace

InteractionMap
buildOverlayInteractionMap(const OverlayCellLayout &layout,
                           const playback_video_edit::EditSnapshot *videoEdit,
                           playback_video_edit::Prompt videoEditPrompt,
                           bool mediaActionConfirmationPrompt) {
  InteractionMap map;
  map.modal = mediaActionConfirmationPrompt ||
              videoEditPrompt != playback_video_edit::Prompt::None;
  for (const OverlayCellControlLayoutItem &item : layout.controls) {
    if (!item.enabled || item.width <= 0 || item.y < 0)
      continue;
    map.controls.push_back(
        {{static_cast<double>(item.x), static_cast<double>(item.y),
          static_cast<double>(item.x + item.width),
          static_cast<double>(item.y + 1)},
         item.id});
  }

  if (!map.modal && videoEdit) {
    const auto panel = playback_video_edit::layoutSuggestionPanel(
        *videoEdit, layout.width, layout.height, layout.topY);
    if (panel.drawable()) {
      map.editSuggestions = InteractionMap::SuggestionPanel{
          {double(panel.x), double(panel.y), double(panel.x + panel.width),
           double(panel.y + panel.height)},
          panel.scrollOffset, panel.maximumScrollOffset, {}};
      map.controls.push_back({
          {double(panel.x + panel.closeColumn), double(panel.y + 1),
           double(panel.x + panel.closeColumn + 7), double(panel.y + 2)},
          OverlayControlId::EditSuggestionsClose});
      for (size_t index = 0; index < panel.lines.size(); ++index) {
        const auto& line = panel.lines[index];
        if (!line.suggestionId) continue;
        map.editSuggestions->items.push_back({
            {double(panel.x + 1), double(panel.y + 1 + index),
             double(panel.x + panel.width - 1), double(panel.y + 2 + index)},
            *line.suggestionId});
      }
    }
  }

  if (layout.progressBarX < 0 || layout.progressBarY < 0 ||
      layout.progressBarWidth <= 0 || mediaActionConfirmationPrompt ||
      videoEditPrompt != playback_video_edit::Prompt::None) {
    return map;
  }

  map.progressBar = ProgressBarRegion{
      {static_cast<double>(layout.progressBarX),
       static_cast<double>(layout.progressBarY),
       static_cast<double>(layout.progressBarX + layout.progressBarWidth),
       static_cast<double>(layout.progressBarY + 1)},
      layout.progressBarWidth};

  if (!videoEdit || !videoEdit->active)
    return map;
  const playback_video_edit::OverlayModel model =
      playback_video_edit::buildOverlayModel(*videoEdit, nullptr,
                                             playback_video_edit::Prompt::None,
                                             layout.progressBarWidth, 0.0);
  const auto addBoundary = [&](std::optional<int> cell,
                               playback_video_edit::EditBoundary boundary) {
    if (!cell)
      return;
    const int center = layout.progressBarX + *cell;
    const int left = std::max(layout.progressBarX, center - 1);
    const int right =
        std::min(layout.progressBarX + layout.progressBarWidth, center + 2);
    map.editBoundaries.push_back(
        {{static_cast<double>(left), static_cast<double>(layout.progressBarY),
          static_cast<double>(right),
          static_cast<double>(layout.progressBarY + 1)},
         boundary});
  };
  addBoundary(model.inCell, playback_video_edit::EditBoundary::In);
  addBoundary(model.outCell, playback_video_edit::EditBoundary::Out);
  return map;
}

bool InteractionRect::valid() const {
  return std::isfinite(left) && std::isfinite(top) && std::isfinite(right) &&
         std::isfinite(bottom) && right > left && bottom > top;
}

bool InteractionRect::contains(double x, double y) const {
  return valid() && std::isfinite(x) && std::isfinite(y) && x >= left &&
         x < right && y >= top && y < bottom;
}

bool InteractionMap::contains(double x, double y) const {
  if (modal)
    return true;
  if (editSuggestions && editSuggestions->bounds.contains(x, y))
    return true;
  if (progressBar && progressBar->bounds.contains(x, y))
    return true;
  if (overlayControlAt(*this, x, y))
    return true;
  if (contextMenuItemAt(*this, x, y))
    return true;
  return editBoundaryHandleAt(*this, x, y);
}

std::optional<ProgressBarHit> progressBarHitAt(const ProgressBarRegion &region,
                                               double x, double y,
                                               bool captured) {
  if (!region.bounds.valid() || region.units <= 0 || !std::isfinite(x) ||
      !std::isfinite(y)) {
    return std::nullopt;
  }

  const InteractionRect &bounds = region.bounds;
  if (!captured && !bounds.contains(x, y))
    return std::nullopt;

  const double lastSample = std::max(bounds.left, bounds.right - 1.0);
  const double sampledX = captured ? std::clamp(x, bounds.left, lastSample) : x;
  const double denominator = std::max(1.0, lastSample - bounds.left);
  return ProgressBarHit{
      std::clamp((sampledX - bounds.left) / denominator, 0.0, 1.0),
      region.units};
}

std::optional<ProgressBarHit>
progressBarHitAt(const InteractionMap &map, double x, double y, bool captured) {
  return map.progressBar ? progressBarHitAt(*map.progressBar, x, y, captured)
                         : std::nullopt;
}

std::optional<OverlayControlId> overlayControlAt(const InteractionMap &map,
                                                 double x, double y) {
  for (const OverlayControlRegion &control : map.controls) {
    if (control.bounds.contains(x, y))
      return control.id;
  }
  return std::nullopt;
}

std::optional<ContextMenuItemToken> contextMenuItemAt(const InteractionMap &map,
                                                      double x, double y) {
  for (const ContextMenuItemRegion &item : map.contextMenuItems) {
    if (item.bounds.contains(x, y))
      return item.token;
  }
  return std::nullopt;
}

std::optional<playback_video_edit::EditBoundary>
editBoundaryAt(const InteractionMap &map, double x, double y) {
  const EditBoundaryRegion *nearest = nullptr;
  double nearestDistance = std::numeric_limits<double>::max();
  for (const EditBoundaryRegion &handle : map.editBoundaries) {
    if (!handle.bounds.contains(x, y))
      continue;
    const double center = (handle.bounds.left + handle.bounds.right) * 0.5;
    const double distance = std::abs(x - center);
    if (distance < nearestDistance) {
      nearest = &handle;
      nearestDistance = distance;
    }
  }
  return nearest ? std::optional(nearest->boundary) : std::nullopt;
}

bool editBoundaryHandleAt(const InteractionMap &map, double x, double y) {
  return editBoundaryAt(map, x, y).has_value();
}

InteractionHit interactionHitAt(const InteractionMap &map, double x, double y,
                                bool capturedProgress) {
  InteractionHit hit;
  hit.progressBar = progressBarHitAt(map, x, y, capturedProgress);
  hit.control = overlayControlAt(map, x, y);
  hit.contextMenuItem = contextMenuItemAt(map, x, y);
  hit.editBoundary = editBoundaryAt(map, x, y);
  if (map.editSuggestions && map.editSuggestions->bounds.contains(x, y)) {
    hit.editSuggestions = map.editSuggestions;
    for (const auto& item : map.editSuggestions->items) {
      if (item.bounds.contains(x, y)) {
        hit.suggestionId = item.id;
        break;
      }
    }
  }
  return hit;
}

InteractionHit interactionHitAtTransformed(const InteractionMap &map,
                                           double offsetX, double offsetY,
                                           double scaleX, double scaleY,
                                           double x, double y,
                                           bool capturedProgress) {
  InteractionHit hit;
  if (!std::isfinite(offsetX) || !std::isfinite(offsetY) || !(scaleX > 0.0) ||
      !(scaleY > 0.0) || !std::isfinite(scaleX) || !std::isfinite(scaleY) ||
      !std::isfinite(x) || !std::isfinite(y)) {
    return hit;
  }

  const double localX = (x - offsetX) / scaleX;
  const double localY = (y - offsetY) / scaleY;
  hit = interactionHitAt(map, localX, localY);
  if (map.progressBar) {
    ProgressBarRegion transformed = *map.progressBar;
    transformed.bounds = transformRect(map.progressBar->bounds, offsetX,
                                       offsetY, scaleX, scaleY);
    hit.progressBar = progressBarHitAt(transformed, x, y, capturedProgress);
  }
  return hit;
}

InteractionMap transformInteractionMap(const InteractionMap &map,
                                       double offsetX, double offsetY,
                                       double scaleX, double scaleY) {
  InteractionMap out;
  if (!std::isfinite(offsetX) || !std::isfinite(offsetY) || !(scaleX > 0.0) ||
      !(scaleY > 0.0) || !std::isfinite(scaleX) || !std::isfinite(scaleY)) {
    return out;
  }
  out.modal = map.modal;
  if (map.editSuggestions) {
    out.editSuggestions = *map.editSuggestions;
    out.editSuggestions->bounds = transformRect(
        map.editSuggestions->bounds, offsetX, offsetY, scaleX, scaleY);
    for (auto& item : out.editSuggestions->items)
      item.bounds = transformRect(item.bounds, offsetX, offsetY, scaleX, scaleY);
  }
  if (map.progressBar) {
    out.progressBar = *map.progressBar;
    out.progressBar->bounds = transformRect(map.progressBar->bounds, offsetX,
                                            offsetY, scaleX, scaleY);
  }
  out.controls.reserve(map.controls.size());
  for (const OverlayControlRegion &control : map.controls) {
    out.controls.push_back(
        {transformRect(control.bounds, offsetX, offsetY, scaleX, scaleY),
         control.id});
  }
  out.contextMenuItems.reserve(map.contextMenuItems.size());
  for (const ContextMenuItemRegion &item : map.contextMenuItems) {
    out.contextMenuItems.push_back(
        {transformRect(item.bounds, offsetX, offsetY, scaleX, scaleY),
         item.token});
  }
  out.editBoundaries.reserve(map.editBoundaries.size());
  for (const EditBoundaryRegion &handle : map.editBoundaries) {
    out.editBoundaries.push_back(
        {transformRect(handle.bounds, offsetX, offsetY, scaleX, scaleY),
         handle.boundary});
  }
  return out;
}


} // namespace playback_overlay
