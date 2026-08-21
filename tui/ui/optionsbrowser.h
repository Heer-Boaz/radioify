#pragma once

#include <optional>
#include <string>

#include "browser_model.h"

enum class OptionsBrowserResult {
  NotHandled,
  Handled,
  Changed,
};

struct OptionsBrowserSubject {
  std::filesystem::path file;
  std::optional<int> trackIndex;
};

bool optionsBrowserIsActive(const BrowserState& browser);
std::optional<OptionsBrowserSubject> optionsBrowserSubjectForEntry(
    const BrowserEntry& entry);
BrowserLocation optionsBrowserOpenLocation(const OptionsBrowserSubject& subject);
bool optionsBrowserSupportsLocation(const BrowserLocation& location);
bool prepareOptionsBrowserContent(BrowserState& browser);
OptionsBrowserResult optionsBrowserActivateEntry(const BrowserState& browser,
                                                 const BrowserEntry& entry);
std::string optionsBrowserSelectionMeta(const BrowserState& browser);
std::string optionsBrowserShowingLabel(const BrowserState& browser);
