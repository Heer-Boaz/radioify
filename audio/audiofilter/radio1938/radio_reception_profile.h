#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_RECEPTION_PROFILE_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_RECEPTION_PROFILE_H

#include <cstdint>
#include <string_view>

enum class RadioReceptionProfile : uint8_t {
  Everyday1938,
  StrongLocal,
  NightSkywave,
};

inline constexpr RadioReceptionProfile kDefaultRadioReceptionProfile =
    RadioReceptionProfile::Everyday1938;

struct RadioReceptionConfig {
  RadioReceptionProfile profile = kDefaultRadioReceptionProfile;
  bool enabled = true;

  // Stable groundwave plus a weak, slowly rotating skywave phasor. The
  // Doppler range gives 10.8-28.8 composite fades per hour (ITU: 10-30).
  float groundwaveGain = 0.92f;
  float skywaveGain = 0.10f;
  float skywaveAmplitudeVariationRms = 0.18f;
  float skywaveAmplitudeVariationSeconds = 55.0f;
  float skywaveDopplerMinHz = 0.003f;
  float skywaveDopplerMaxHz = 0.008f;
  float skywaveDopplerWanderRmsHz = 0.001f;
  float skywaveDopplerWanderSeconds = 75.0f;
  // Extra path length of a one-hop skywave: reflection near 100 km height at
  // 100-300 km ground range adds 60-125 km, 0.2-0.4 ms. The delayed
  // modulation makes the composite frequency selective: sidebands spaced
  // 1/delay apart fade in turn, independently of the carrier.
  float skywaveDelayMinMs = 0.20f;
  float skywaveDelayMaxMs = 0.40f;

  // US co-channel carrier-only scenario: two compliant +/-50 Hz carriers
  // differ by at most 100 Hz (FRC rule effective June 22, 1932). The uniform
  // scenario draw is not an observed distribution; foreign/off-spec stations
  // can be represented explicitly with different offsets. No invented programme.
  // A separate real RF carrier, never a post-detector audio oscillator.
  bool intermittentCarrierEnabled = true;
  float intermittentWaitMinSeconds = 60.0f;
  float intermittentWaitMaxSeconds = 180.0f;
  float intermittentDurationMinSeconds = 4.0f;
  float intermittentDurationMaxSeconds = 10.0f;
  float intermittentLevelMinDb = -50.0f;
  float intermittentLevelMaxDb = -42.0f;
  float intermittentOffsetMinHz = 0.0f;
  float intermittentOffsetMaxHz = 100.0f;
  float intermittentAttackMs = 750.0f;
  float intermittentReleaseMs = 1800.0f;

  uint32_t seed = 0x19380716u;

  // External radio noise at the antenna. At medium frequencies a receiver is
  // limited by man-made and atmospheric noise picked up with the signal, not
  // by its own tubes: ITU-R P.372 puts man-made noise at 1 MHz 54-77 dB above
  // kT0b. Both the carrier field and the noise field act on the same
  // antenna, so their ratio sets the carrier-to-noise density at the receiver
  // input whatever its absolute sensitivity. Independent of `enabled`, which
  // controls only the propagation effects above.
  bool externalNoiseEnabled = true;
  // RMS groundwave carrier field. Default: the lower limit of the FCC 1939
  // primary service field for city residential areas (2-10 mV/m), the field
  // at which the usual noise of such areas is no longer objectionable.
  float groundwaveFieldVoltsPerMeter = 2e-3f;
  // ITU-R P.372 Table 1 median man-made noise, Fam = c - d log10(f / 1 MHz)
  // dB above kT0b (valid 0.3-250 MHz, short vertical monopole). Default:
  // residential. The coefficients come from 1970s measurements.
  float manMadeNoiseC = 72.5f;
  float manMadeNoiseD = 27.7f;
  uint32_t externalNoiseSeed = 0x1938a7e5u;
};

// Carrier-to-noise density ratio (dB-Hz) of the carrier at unit path gain,
// whose field is groundwaveFieldVoltsPerMeter / groundwaveGain (the
// groundwave field itself when propagation effects are disabled).
double radioReceptionCarrierToNoiseDensityDbHz(const RadioReceptionConfig& config,
                                               double stationFrequencyHz);

std::string_view radioReceptionProfileName(RadioReceptionProfile profile);
bool parseRadioReceptionProfile(std::string_view name,
                                RadioReceptionProfile& profile);
RadioReceptionConfig radioReceptionConfigForProfile(
    RadioReceptionProfile profile);

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_RECEPTION_PROFILE_H
