#include <iostream>
#include <vector>

#include "image_viewer_sequence.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "image_viewer_sequence_tests: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  const std::filesystem::path first = "C:/Media/first.jpg";
  const std::filesystem::path second = "C:/Media/second.png";
  const std::filesystem::path third = "C:/Media/third.webp";

  std::vector<std::filesystem::path> files{first, {}, second, third};
  std::optional<image_viewer_sequence::Sequence> sequence =
      image_viewer_sequence::Sequence::create(std::move(files), second);
  ok &= expect(sequence.has_value(),
               "a source containing the current image must be accepted");
  if (!sequence) {
    return 1;
  }

  ok &= expect(
      samePath(sequence->current(), second) &&
          sequence->canMove(image_viewer_sequence::Direction::Previous) &&
          sequence->canMove(image_viewer_sequence::Direction::Next),
      "the sequence must preserve its captured current position");
  ok &= expect(sequence->move(image_viewer_sequence::Direction::Previous) &&
                   samePath(sequence->current(), first) &&
                   !sequence->move(image_viewer_sequence::Direction::Previous),
               "previous must stop at the captured source boundary");
  ok &= expect(sequence->move(image_viewer_sequence::Direction::Next) &&
                   samePath(sequence->current(), second) &&
                   sequence->move(image_viewer_sequence::Direction::Next) &&
                   samePath(sequence->current(), third) &&
                   !sequence->move(image_viewer_sequence::Direction::Next),
               "next must follow stable captured source order");

  ok &= expect(!image_viewer_sequence::Sequence::create({first}, third),
               "a source missing its current image must be rejected");

#ifdef _WIN32
  ok &=
      expect(image_viewer_sequence::Sequence::create(
                 {first, second}, std::filesystem::path("c:\\media\\FIRST.JPG"))
                 .has_value(),
             "Windows image matching must use ordinal path identity");
#endif

  return ok ? 0 : 1;
}
