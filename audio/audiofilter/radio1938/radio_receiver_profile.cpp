#include "radio_receiver_profile.h"

std::string_view radioReceiverProfileName(RadioReceiverProfile profile) {
  switch (profile) {
    case RadioReceiverProfile::Philco37116:
      return "philco-37-116";
    case RadioReceiverProfile::Typical1930s:
      return "typical-1930s";
  }
  return "typical-1930s";
}

bool parseRadioReceiverProfile(std::string_view name,
                               RadioReceiverProfile& profile) {
  if (name == "philco-37-116" || name == "philco_37_116" ||
      name == "philco_37_116x") {
    profile = RadioReceiverProfile::Philco37116;
    return true;
  }
  if (name == "typical-1930s" || name == "philco-38-12" ||
      name == "philco_38_12") {
    profile = RadioReceiverProfile::Typical1930s;
    return true;
  }
  return false;
}
