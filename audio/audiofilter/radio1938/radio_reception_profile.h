#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_RECEPTION_PROFILE_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_RECEPTION_PROFILE_H

#include <cstdint>
#include <string_view>

enum class RadioReceptionProfile : uint8_t {
  Everyday1938,
  StrongLocal,
};

inline constexpr RadioReceptionProfile kDefaultRadioReceptionProfile =
    RadioReceptionProfile::Everyday1938;

struct RadioReceptionConfig {
  RadioReceptionProfile profile = kDefaultRadioReceptionProfile;
  bool enabled = true;

  // Stable groundwave plus a weak, slowly rotating skywave phasor. The
  // Doppler range gives 10.8-28.8 composite fades per hour.
  float groundwaveGain = 0.92f;
  float skywaveGain = 0.10f;
  float skywaveAmplitudeVariationRms = 0.18f;
  float skywaveAmplitudeVariationSeconds = 55.0f;
  float skywaveDopplerMinHz = 0.003f;
  float skywaveDopplerMaxHz = 0.008f;
  float skywaveDopplerWanderRmsHz = 0.001f;
  float skywaveDopplerWanderSeconds = 75.0f;

  // A separate real RF carrier, never a post-detector audio oscillator.
  bool intermittentCarrierEnabled = true;
  float intermittentWaitMinSeconds = 60.0f;
  float intermittentWaitMaxSeconds = 180.0f;
  float intermittentDurationMinSeconds = 4.0f;
  float intermittentDurationMaxSeconds = 10.0f;
  float intermittentLevelMinDb = -50.0f;
  float intermittentLevelMaxDb = -42.0f;
  float intermittentOffsetMinHz = 320.0f;
  float intermittentOffsetMaxHz = 900.0f;
  float intermittentAttackMs = 750.0f;
  float intermittentReleaseMs = 1800.0f;

  uint32_t seed = 0x19380716u;
};

std::string_view radioReceptionProfileName(RadioReceptionProfile profile);
bool parseRadioReceptionProfile(std::string_view name,
                                RadioReceptionProfile& profile);
RadioReceptionConfig radioReceptionConfigForProfile(
    RadioReceptionProfile profile);

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_RECEPTION_PROFILE_H
