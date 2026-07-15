#include "radio_filter_mode.h"

#include <cstdlib>

bool radioFilterModeEnabled(RadioFilterMode mode) {
  switch (mode) {
    case RadioFilterMode::Off:
      return false;
    case RadioFilterMode::Typical1930s:
    case RadioFilterMode::Philco37116:
      return true;
  }
  std::abort();
}

RadioReceiverProfile radioFilterModeReceiverProfile(RadioFilterMode mode) {
  switch (mode) {
    case RadioFilterMode::Typical1930s:
      return RadioReceiverProfile::Typical1930s;
    case RadioFilterMode::Philco37116:
      return RadioReceiverProfile::Philco37116;
    case RadioFilterMode::Off:
      std::abort();
  }
  std::abort();
}

RadioFilterMode radioFilterModeForReceiverProfile(
    RadioReceiverProfile profile) {
  switch (profile) {
    case RadioReceiverProfile::Philco37116:
      return RadioFilterMode::Philco37116;
    case RadioReceiverProfile::Typical1930s:
      return RadioFilterMode::Typical1930s;
  }
  std::abort();
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
  std::abort();
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
  std::abort();
}
