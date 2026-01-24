#include "tracklist.h"

#include <algorithm>
#include <cctype>

#include "ui_helpers.h"

namespace {
std::string trimAscii(const std::string& value) {
  size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start]))) {
    ++start;
  }
  size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(start, end - start);
}

bool isMeaningfulTitle(const std::string& value) {
  if (value.empty()) return false;
  int letters = 0;
  int nonAscii = 0;
  for (unsigned char c : value) {
    if (c >= 0x80) {
      nonAscii++;
      continue;
    }
    if (std::isalpha(c)) {
      letters++;
    }
  }
  if (nonAscii > 0) return true;
  return letters >= 2;
}
}  // namespace

int trackLabelDigits(size_t count) {
  int digits = 1;
  size_t n = count;
  while (n >= 10) {
    n /= 10;
    ++digits;
  }
  return std::max(3, digits);
}

std::string formatTrackLabel(const TrackEntry& track, int digits) {
  std::string idx = std::to_string(track.index + 1);
  if (static_cast<int>(idx.size()) < digits) {
    idx.insert(0, static_cast<size_t>(digits - idx.size()), '0');
  }
  std::string label = idx;
  std::string title = trimAscii(track.title);
  if (isMeaningfulTitle(title)) {
    label += " - " + title;
  }
  if (track.lengthMs > 0) {
    double seconds = static_cast<double>(track.lengthMs) / 1000.0;
    label += " (" + formatTime(seconds) + ")";
  }
  return label;
}
