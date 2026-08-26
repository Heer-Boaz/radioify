#include "browser_action_strip.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "unicode_display_width.h"

namespace browser_action_strip {
namespace {

std::optional<ActionStripItem> actionForControl(
    playback_overlay::OverlayControlId id) {
  switch (id) {
    case playback_overlay::OverlayControlId::Previous:
      return ActionStripItem::Previous;
    case playback_overlay::OverlayControlId::PlayPause:
      return ActionStripItem::PlayPause;
    case playback_overlay::OverlayControlId::Next:
      return ActionStripItem::Next;
    case playback_overlay::OverlayControlId::Radio:
      return ActionStripItem::Radio;
    case playback_overlay::OverlayControlId::Hz50:
      return ActionStripItem::Hz50;
    case playback_overlay::OverlayControlId::PictureInPicture:
      return ActionStripItem::PictureInPicture;
    case playback_overlay::OverlayControlId::AudioTrack:
    case playback_overlay::OverlayControlId::Subtitles:
    case playback_overlay::OverlayControlId::EditMarkIn:
    case playback_overlay::OverlayControlId::EditMarkOut:
    case playback_overlay::OverlayControlId::EditClearSelection:
    case playback_overlay::OverlayControlId::EditRippleDelete:
    case playback_overlay::OverlayControlId::EditTrim:
    case playback_overlay::OverlayControlId::EditSuggestions:
    case playback_overlay::OverlayControlId::EditSuggestionFilter:
    case playback_overlay::OverlayControlId::EditPreviousSuggestion:
    case playback_overlay::OverlayControlId::EditNextSuggestion:
    case playback_overlay::OverlayControlId::EditSelectSuggestion:
    case playback_overlay::OverlayControlId::EditHideSuggestion:
    case playback_overlay::OverlayControlId::EditUndoHideSuggestion:
    case playback_overlay::OverlayControlId::EditDone:
    case playback_overlay::OverlayControlId::EditStartExport:
    case playback_overlay::OverlayControlId::EditWaitForExport:
    case playback_overlay::OverlayControlId::EditCancelExport:
    case playback_overlay::OverlayControlId::EditConfirmPrompt:
    case playback_overlay::OverlayControlId::EditCancelPrompt:
    case playback_overlay::OverlayControlId::EditDiscardAndExit:
    case playback_overlay::OverlayControlId::EditCancelExit:
      return std::nullopt;
  }
  return std::nullopt;
}

Item textItem(ActionStripItem id, const std::string& text, bool active) {
  Item item;
  item.id = id;
  item.label = " [" + text + "] ";
  item.hoverLabel = "[ " + text + " ]";
  item.active = active;
  item.width =
      std::max(utf8DisplayWidth(item.label),
               utf8DisplayWidth(item.hoverLabel));
  return item;
}

std::string viewLabel(BrowserState::ViewMode viewMode) {
  switch (viewMode) {
    case BrowserState::ViewMode::Thumbnails:
      return "\xE2\x96\xA6 Grid";
    case BrowserState::ViewMode::ListOnly:
      return "\xE2\x89\xA1 List";
    case BrowserState::ViewMode::ListPreview:
      return "\xE2\x98\x90 Preview";
  }
  return "View";
}

}  // namespace

std::vector<Item> build(const Input& input) {
  std::vector<Item> items;
  playback_overlay::OverlayControlSpecOptions controlOptions;
  controlOptions.includeAudioTrack = false;
  controlOptions.includeSubtitles = false;
  const std::vector<playback_overlay::OverlayControlSpec> controls =
      playback_overlay::buildOverlayControlSpecs(input.playback, -1,
                                                 controlOptions);
  for (const playback_overlay::OverlayControlSpec& control : controls) {
    const std::optional<ActionStripItem> action =
        actionForControl(control.id);
    if (!action) {
      continue;
    }
    Item item;
    item.id = *action;
    item.label = control.normalText;
    item.hoverLabel = control.hoverText;
    item.active = control.active;
    item.width = control.width;
    items.push_back(std::move(item));
  }

  if (input.browserControlsAvailable) {
    items.push_back(
        textItem(ActionStripItem::View, viewLabel(input.viewMode), false));
    if (input.optionsAvailable) {
      items.push_back(textItem(ActionStripItem::Options, "Options",
                               input.optionsActive));
    }
  }
  return items;
}

int wrappedLineCount(const std::vector<Item>& items, int width) {
  if (items.empty() || width <= 0) {
    return 0;
  }
  constexpr int kGapWidth = 2;
  int lines = 1;
  int x = 0;
  for (const Item& item : items) {
    const int itemWidth = std::min(std::max(1, item.width), width);
    const int gap = x > 0 ? kGapWidth : 0;
    if (x > 0 && x + gap + itemWidth > width) {
      ++lines;
      x = itemWidth;
    } else {
      x += gap + itemWidth;
    }
  }
  return lines;
}

}  // namespace browser_action_strip
