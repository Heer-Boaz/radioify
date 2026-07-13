#include "radio_reception_profile.h"

#include <stdexcept>

std::string_view radioReceptionProfileName(RadioReceptionProfile profile) {
  switch (profile) {
    case RadioReceptionProfile::Everyday1938:
      return "everyday-1938";
    case RadioReceptionProfile::StrongLocal:
      return "strong-local";
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
  }
  throw std::invalid_argument("Unknown radio reception profile");
}
