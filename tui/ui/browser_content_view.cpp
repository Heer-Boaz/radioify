#include "browser_content_view.h"

#include <algorithm>
#include <cctype>

namespace browser_content_view {
namespace {

bool cancellationWasRequested(
    const CancellationRequested& cancellationRequested) {
  return cancellationRequested && cancellationRequested();
}

std::string lowercaseAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

}  // namespace

Outcome apply(BrowserState& state, const Request& request,
              CancellationRequested cancellationRequested) {
  if (!state.filter.empty()) {
    const std::string lowercaseFilter = lowercaseAscii(state.filter);
    state.entries.erase(
        std::remove_if(
            state.entries.begin(), state.entries.end(),
            [&](const BrowserEntry& entry) {
              if (entry.isSectionHeader() || entry.isStatus() ||
                  entry.actionAs<browser_entry::NavigateUp>()) {
                return false;
              }
              return lowercaseAscii(entry.name).find(lowercaseFilter) ==
                     std::string::npos;
            }),
        state.entries.end());
  }

  if (cancellationWasRequested(cancellationRequested)) {
    return Outcome::Cancelled;
  }

  if (!state.entries.empty() &&
      request.entryOrder == EntryOrder::Sorted) {
    sortBrowserEntries(state);
  }

  if (cancellationWasRequested(cancellationRequested)) {
    return Outcome::Cancelled;
  }

  if (state.entries.empty()) {
    state.selected = 0;
    state.scrollRow = 0;
    return Outcome::Completed;
  }

  const auto isSelectable = [](const BrowserEntry& entry) {
    return entry.isSelectable();
  };
  if (state.selected < 0 ||
      state.selected >= static_cast<int>(state.entries.size()) ||
      !isSelectable(state.entries[static_cast<size_t>(state.selected)])) {
    state.selected = -1;
    for (size_t index = 0; index < state.entries.size(); ++index) {
      if (isSelectable(state.entries[index])) {
        state.selected = static_cast<int>(index);
        break;
      }
    }
    if (state.selected < 0) {
      state.selected = 0;
    }
  }
  state.scrollRow = 0;

  if (!request.initialName.empty()) {
    const std::string lowercaseInitialName =
        lowercaseAscii(request.initialName);
    for (size_t index = 0; index < state.entries.size(); ++index) {
      if (!state.entries[index].isSelectable()) {
        continue;
      }
      if (lowercaseAscii(state.entries[index].name) == lowercaseInitialName) {
        state.selected = static_cast<int>(index);
        state.viewportRestoreMode =
            BrowserState::ViewportRestoreMode::RevealSelection;
        break;
      }
    }
  }

  return Outcome::Completed;
}

}  // namespace browser_content_view
