#ifndef TRACKLIST_H
#define TRACKLIST_H

#include <string>

struct TrackEntry {
  int index = 0;
  std::string title;
  int lengthMs = -1;
};

#endif
