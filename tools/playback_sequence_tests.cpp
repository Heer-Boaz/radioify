#include <iostream>
#include <string>
#include <vector>

#include "playback_sequence.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "playback_sequence_tests: " << message << '\n';
    return false;
  }
  return true;
}

bool isTarget(const std::optional<PlaybackTarget>& actual,
              const std::filesystem::path& file, int trackIndex) {
  return actual && samePath(actual->file, file) &&
         actual->trackIndex == trackIndex;
}

}  // namespace

int main() {
  bool ok = true;
  const std::filesystem::path songA = "C:/Media/A.flac";
  const std::filesystem::path songB = "C:/Media/B.flac";
  const std::filesystem::path songC = "C:/Media/C.flac";

  PlaybackSequence sequence;
  std::vector<PlaybackTarget> source{{songA, -1}, {songB, -1}, {songC, -1}};
  sequence.replace(source, {songB, 0});
  source.clear();
  ok &= expect(sequence.size() == 3,
               "the sequence must own an immutable source snapshot");
  ok &= expect(isTarget(sequence.adjacent(-1), songA, -1) &&
                   isTarget(sequence.adjacent(1), songC, -1),
               "transport must resolve neighbours around the selected item");
  ok &= expect(!sequence.adjacent(1, 2),
               "transport must stop at a sequence boundary");

  ok &= expect(sequence.select({songA, 7}),
               "a directory item must represent any track in its container");
  ok &= expect(!sequence.adjacent(-1) &&
                   isTarget(sequence.adjacent(1, 2), songC, -1),
               "distance lookup must preserve stable sequence order");

  sequence.replace({{songA, 0}, {songA, 3}, {songA, 8}}, {songA, 3});
  ok &= expect(isTarget(sequence.adjacent(-1), songA, 0) &&
                   isTarget(sequence.adjacent(1), songA, 8),
               "a track-browser sequence must keep exact track order");
  ok &= expect(!sequence.select({songA, 6}) &&
                   isTarget(sequence.adjacent(-1), songA, 0),
               "selecting an absent target must not disturb the current item");

  sequence.replace({{songA, -1}, {songB, -1}}, {songC, -1});
  ok &= expect(sequence.size() == 1 && !sequence.adjacent(-1) &&
                   !sequence.adjacent(1),
               "an unrelated playback must not inherit an old sequence");

  sequence.clear();
  ok &= expect(sequence.empty() && !sequence.adjacent(1),
               "a cleared playback sequence must have no current item");

#ifdef _WIN32
  sequence.replace(
      {{std::filesystem::path("C:/Media/A.flac"), -1}, {songB, -1}},
      {std::filesystem::path("c:\\media\\a.FLAC"), 0});
  ok &= expect(isTarget(sequence.adjacent(1), songB, -1),
               "Windows sequence selection must use ordinal path identity");
#endif

  return ok ? 0 : 1;
}
