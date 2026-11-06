#include "radio_reception_profile.h"

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
      return config;
    case RadioReceptionProfile::NightSkywave:
      // Fringe of the groundwave service area after dark: both paths arrive
      // at comparable strength. A single skywave mode varies about 3 dB
      // within the hour (ITU-R P.1321); 0.35 RMS on the linear gain is close
      // to that. The groundwave stays steady.
      config.groundwaveGain = 0.60f;
      config.skywaveGain = 0.60f;
      config.skywaveAmplitudeVariationRms = 0.35f;
      return config;
  }
  throw std::invalid_argument("Unknown radio reception profile");
}
