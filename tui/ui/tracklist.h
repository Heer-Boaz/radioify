#ifndef TRACKLIST_H
#define TRACKLIST_H

#include <cstddef>
#include <string>

struct TrackEntry {
  int index = 0;
  std::string title;
  int lengthMs = -1;
};

int trackLabelDigits(size_t count);
std::string formatTrackLabel(const TrackEntry& track, int digits);

#endif
