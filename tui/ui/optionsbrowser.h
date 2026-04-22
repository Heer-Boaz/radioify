#pragma once

#include <string>

#include "browser_model.h"

enum class OptionsBrowserResult {
  NotHandled,
  Handled,
  Changed,
};

bool optionsBrowserIsActive(const BrowserState& browser);
bool optionsBrowserCanToggle(const BrowserState& browser);
bool optionsBrowserRefresh(BrowserState& browser);
OptionsBrowserResult optionsBrowserActivateSelection(BrowserState& browser);
void optionsBrowserToggle(BrowserState& browser);
bool optionsBrowserNavigateUp(BrowserState& browser);
std::string optionsBrowserSelectionMeta(const BrowserState& browser);
std::string optionsBrowserShowingLabel();
