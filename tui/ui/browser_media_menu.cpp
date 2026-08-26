#include "tui/ui/browser_media_menu.h"

#include <string>
#include <utility>

namespace tui_browser_media_menu {

bool Model::open(BrowserEntry entry,
                 std::vector<playback_media_actions::Item> items,
                 tui_popup_menu::Anchor anchor) {
  dismiss();
  if (items.empty()) return false;

  std::vector<std::string> labels;
  labels.reserve(items.size());
  actions_.reserve(items.size());
  for (playback_media_actions::Item& item : items) {
    labels.push_back(std::move(item.label));
    actions_.push_back(item.action);
  }
  entry_.emplace(std::move(entry));
  popup_.open(std::move(labels), anchor);
  return true;
}

bool Model::dismiss() {
  const bool changed = popup_.dismiss() || entry_.has_value() ||
                       !actions_.empty();
  entry_.reset();
  actions_.clear();
  return changed;
}

Interaction Model::handle(const InputEvent& event,
                          const tui_popup_menu::Bounds& bounds) {
  const tui_popup_menu::Interaction popupInteraction =
      popup_.handle(event, bounds);
  Interaction interaction;
  interaction.consumed = popupInteraction.consumed;
  interaction.changed = popupInteraction.changed;
  interaction.dismissed = popupInteraction.dismissed;
  if (popupInteraction.activatedItem && entry_ &&
      *popupInteraction.activatedItem < actions_.size()) {
    interaction.command.emplace(
        Command{*entry_, actions_[*popupInteraction.activatedItem]});
  }
  if (popupInteraction.dismissed) dismiss();
  return interaction;
}

}  // namespace tui_browser_media_menu
