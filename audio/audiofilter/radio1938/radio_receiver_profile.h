#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_RECEIVER_PROFILE_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_RECEIVER_PROFILE_H

#include <cstdint>
#include <string_view>

enum class RadioReceiverProfile : uint8_t {
  Philco37116,
  Typical1930s,
};

inline constexpr RadioReceiverProfile kDefaultRadioReceiverProfile =
    RadioReceiverProfile::Typical1930s;

std::string_view radioReceiverProfileName(RadioReceiverProfile profile);
bool parseRadioReceiverProfile(std::string_view name,
                               RadioReceiverProfile& profile);

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_RECEIVER_PROFILE_H
