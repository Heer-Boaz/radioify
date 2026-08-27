#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "browser_model.h"
#include "kssoptions.h"
#include "nsfoptions.h"
#include "vgmoptions.h"

class AudioPlaybackRuntime;

enum class OptionsBrowserResult {
  NotHandled,
  Handled,
  Changed,
};

struct OptionsBrowserSubject {
  std::filesystem::path file;
  std::optional<uint32_t> trackIndex;
};

struct OptionsBrowserRuntimeSnapshot {
  KssPlaybackOptions kssOptions;
  NsfPlaybackOptions nsfOptions;
  VgmPlaybackOptions vgmOptions;
  bool instrumentAuditionActive = false;
  KssInstrumentDevice auditionDevice = KssInstrumentDevice::None;
  uint32_t auditionHash = 0;
  std::optional<VgmDeviceOptions> selectedVgmDeviceOptions;
  uint32_t scanSampleRate = 48000;
  uint32_t scanChannels = 2;
};

bool optionsBrowserIsActive(const BrowserState& browser);
std::optional<OptionsBrowserSubject> optionsBrowserSubjectForEntry(
    const BrowserEntry& entry);
BrowserLocation optionsBrowserOpenLocation(const OptionsBrowserSubject& subject);
bool optionsBrowserSupportsLocation(const BrowserLocation& location);
OptionsBrowserRuntimeSnapshot captureOptionsBrowserRuntimeSnapshot(
    const BrowserLocation& location, const AudioPlaybackRuntime& audioPlayback,
    uint32_t sampleRate, uint32_t channels);
bool prepareOptionsBrowserContent(
    BrowserState& browser, const OptionsBrowserRuntimeSnapshot& runtime,
    const std::function<bool()>& cancellationRequested = {});
OptionsBrowserResult optionsBrowserActivateEntry(const BrowserState& browser,
                                                 const BrowserEntry& entry,
                                                 AudioPlaybackRuntime&
                                                     audioPlayback);
std::string optionsBrowserSelectionMeta(const BrowserState& browser);
std::string optionsBrowserShowingLabel(const BrowserState& browser);
