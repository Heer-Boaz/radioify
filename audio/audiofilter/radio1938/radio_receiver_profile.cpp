#include "radio_receiver_profile.h"

#include <cstdlib>

std::string_view radioReceiverProfileName(RadioReceiverProfile profile) {
  switch (profile) {
    case RadioReceiverProfile::Philco37116:
      return "philco-37-116";
    case RadioReceiverProfile::Typical1930s:
      return "typical-1930s";
  }
  std::abort();
}

bool parseRadioReceiverProfile(std::string_view name,
                               RadioReceiverProfile& profile) {
  if (name == "philco-37-116") {
    profile = RadioReceiverProfile::Philco37116;
    return true;
  }
  if (name == "typical-1930s") {
    profile = RadioReceiverProfile::Typical1930s;
    return true;
  }
  return false;
}
