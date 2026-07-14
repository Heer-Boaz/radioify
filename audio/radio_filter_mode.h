#pragma once

#include <cstdint>
#include <string_view>

#include "audiofilter/radio1938/radio_receiver_profile.h"

enum class RadioFilterMode : uint8_t {
  Off,
  Typical1930s,
  Philco37116,
};

inline constexpr RadioFilterMode kDefaultRadioFilterMode =
    RadioFilterMode::Off;

bool radioFilterModeEnabled(RadioFilterMode mode);
RadioReceiverProfile radioFilterModeReceiverProfile(RadioFilterMode mode);
RadioFilterMode radioFilterModeForReceiverProfile(
    RadioReceiverProfile profile);
RadioFilterMode nextRadioFilterMode(RadioFilterMode mode);
std::string_view radioFilterModeLabel(RadioFilterMode mode);
