#include "radio_filter_mode.h"

bool radioFilterModeEnabled(RadioFilterMode mode) {
  return mode != RadioFilterMode::Off;
}

RadioReceiverProfile radioFilterModeReceiverProfile(RadioFilterMode mode) {
  switch (mode) {
    case RadioFilterMode::Off:
    case RadioFilterMode::Typical1930s:
      return RadioReceiverProfile::Typical1930s;
    case RadioFilterMode::Philco37116:
      return RadioReceiverProfile::Philco37116;
  }
  return RadioReceiverProfile::Typical1930s;
}

RadioFilterMode radioFilterModeForReceiverProfile(
    RadioReceiverProfile profile) {
  switch (profile) {
    case RadioReceiverProfile::Philco37116:
      return RadioFilterMode::Philco37116;
    case RadioReceiverProfile::Typical1930s:
      return RadioFilterMode::Typical1930s;
  }
  return RadioFilterMode::Typical1930s;
}

RadioFilterMode nextRadioFilterMode(RadioFilterMode mode) {
  switch (mode) {
    case RadioFilterMode::Off:
      return RadioFilterMode::Typical1930s;
    case RadioFilterMode::Typical1930s:
      return RadioFilterMode::Philco37116;
    case RadioFilterMode::Philco37116:
      return RadioFilterMode::Off;
  }
  return RadioFilterMode::Off;
}

std::string_view radioFilterModeLabel(RadioFilterMode mode) {
  switch (mode) {
    case RadioFilterMode::Off:
      return "Radio: Off";
    case RadioFilterMode::Typical1930s:
      return "Radio: Typical";
    case RadioFilterMode::Philco37116:
      return "Radio: Philco";
  }
  return "Radio: Off";
}
