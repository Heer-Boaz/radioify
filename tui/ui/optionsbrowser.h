#pragma once

#include <optional>
#include <string>

#include "browser_model.h"

enum class OptionsBrowserResult {
  NotHandled,
  Handled,
  Changed,
};

bool optionsBrowserIsActive(const BrowserState& browser);
bool optionsBrowserCanToggle(const BrowserState& browser);
std::optional<BrowserLocation> optionsBrowserOpenLocation(
    const BrowserState& browser);
bool optionsBrowserSupportsLocation(const BrowserLocation& location);
bool prepareOptionsBrowserContent(BrowserState& browser);
OptionsBrowserResult optionsBrowserActivateSelection(BrowserState& browser);
std::string optionsBrowserSelectionMeta(const BrowserState& browser);
std::string optionsBrowserShowingLabel(const BrowserState& browser);
