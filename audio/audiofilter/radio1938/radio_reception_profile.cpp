#include "radio_reception_profile.h"

#include <cmath>
#include <stdexcept>

std::string_view radioReceptionProfileName(RadioReceptionProfile profile) {
  switch (profile) {
    case RadioReceptionProfile::Everyday1938:
      return "everyday-1938";
    case RadioReceptionProfile::StrongLocal:
      return "strong-local";
    case RadioReceptionProfile::NightSkywave:
      return "night-skywave";
  }
  throw std::invalid_argument("Unknown radio reception profile");
}

bool parseRadioReceptionProfile(std::string_view name,
                                RadioReceptionProfile& profile) {
  if (name == "everyday-1938") {
    profile = RadioReceptionProfile::Everyday1938;
    return true;
  }
  if (name == "strong-local") {
    profile = RadioReceptionProfile::StrongLocal;
    return true;
  }
  if (name == "night-skywave") {
    profile = RadioReceptionProfile::NightSkywave;
    return true;
  }
  return false;
}

RadioReceptionConfig radioReceptionConfigForProfile(
    RadioReceptionProfile profile) {
  RadioReceptionConfig config;
  config.profile = profile;
  switch (profile) {
    case RadioReceptionProfile::Everyday1938:
      return config;
    case RadioReceptionProfile::StrongLocal:
      config.enabled = false;
      // The FCC 1939 site rule asks for at least 5-10 mV/m over the most
      // distant residential section of the station's own city.
      config.groundwaveFieldVoltsPerMeter = 10e-3f;
      return config;
    case RadioReceptionProfile::NightSkywave:
      // Fringe of the groundwave service area after dark: both paths arrive
      // at comparable strength. A single skywave mode varies about 3 dB
      // within the hour (ITU-R P.1321); 0.35 RMS on the linear gain is close
      // to that. The groundwave stays steady.
      config.groundwaveGain = 0.60f;
      config.skywaveGain = 0.60f;
      config.skywaveAmplitudeVariationRms = 0.35f;
      // Rural listener at 0.5 mV/m, the FCC 1939 primary field for towns of
      // up to 2,500 and the upper limit for rural areas in winter. P.372
      // rural man-made noise. The evening atmospheric noise that arrives by
      // skywave after dark (spring, north-eastern United States, about 13 dB
      // above it) is modelled but left off: its large crashes drive the
      // Philco's push-pull output and supply model into a state it does not
      // recover from, which must be fixed first. Until then this profile is
      // a lower bound on night noise.
      config.groundwaveFieldVoltsPerMeter = 0.5e-3f;
      config.manMadeNoiseC = 67.2f;
      config.manMadeNoiseD = 27.7f;
      return config;
  }
  throw std::invalid_argument("Unknown radio reception profile");
}

float radioReceptionAntennaCarrierRmsVolts(const RadioReceptionConfig& config) {
  if (!(config.groundwaveFieldVoltsPerMeter > 0.0f) || !(config.groundwaveGain > 0.0f) ||
      !(config.antennaEffectiveHeightMeters > 0.0f) ||
      !std::isfinite(config.groundwaveFieldVoltsPerMeter * config.antennaEffectiveHeightMeters))
    throw std::invalid_argument("invalid antenna field configuration");
  const float pathGain = config.enabled ? config.groundwaveGain : 1.0f;
  return config.groundwaveFieldVoltsPerMeter * config.antennaEffectiveHeightMeters / pathGain;
}

namespace {

// ITU-R P.372 eq. (7) per hertz: En = Fa + 20 log f(MHz) - 95.5 dB(uV/m),
// with Fa = c - d log10(f MHz); returned as carrier field minus En.
double carrierToNoiseDbHz(const RadioReceptionConfig& config, double stationFrequencyHz,
                          double c, double d) {
  const double megahertz = stationFrequencyHz * 1e-6;
  if (!(megahertz >= 0.3 && megahertz <= 250.0) ||
      !(config.groundwaveFieldVoltsPerMeter > 0.0f) ||
      !(config.groundwaveGain > 0.0f) || !std::isfinite(c) || !std::isfinite(d)) {
    throw std::invalid_argument("invalid external radio noise configuration");
  }
  const double faDb = c - d * std::log10(megahertz);
  const double noiseFieldDensityDbUv = faDb + 20.0 * std::log10(megahertz) - 95.5;
  // Without propagation effects the carrier arrives at unit path gain.
  const double pathGain = config.enabled ? config.groundwaveGain : 1.0;
  const double carrierFieldDbUv = 20.0 * std::log10(
      1e6 * config.groundwaveFieldVoltsPerMeter / pathGain);
  return carrierFieldDbUv - noiseFieldDensityDbUv;
}

}  // namespace

double radioReceptionCarrierToManMadeNoiseDbHz(const RadioReceptionConfig& config,
                                              double stationFrequencyHz) {
  return carrierToNoiseDbHz(config, stationFrequencyHz, config.manMadeNoiseC,
                            config.manMadeNoiseD);
}

double radioReceptionCarrierToAtmosphericNoiseDbHz(const RadioReceptionConfig& config,
                                                  double stationFrequencyHz) {
  return carrierToNoiseDbHz(config, stationFrequencyHz, config.atmosphericNoiseFamDb,
                            config.atmosphericNoiseD);
}
