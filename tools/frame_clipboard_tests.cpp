#include "playback/video/framebuffer/frame_clipboard.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "frame_clipboard_tests: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;

  VideoFrameSnapshot snapshot;
  snapshot.width = 2;
  snapshot.height = 2;
  snapshot.strideBytes = 12;
  snapshot.rgba = {
      255, 0,   0,   255, 0,   255, 0,   128, 9, 9, 9, 9,
      0,   0,   255, 64,  255, 255, 255, 255, 8, 8, 8, 8,
  };

  std::vector<uint8_t> dib;
  std::string error;
  ok &= expect(playback_video_frame_clipboard::buildDibV5Payload(
                   snapshot, &dib, &error),
               "a valid strided RGBA snapshot must produce a DIBV5 payload");
  ok &= expect(error.empty(), "a valid snapshot must not report an error");
  ok &= expect(dib.size() == sizeof(BITMAPV5HEADER) + 16u,
               "the DIBV5 payload must contain tightly packed pixels");

  BITMAPV5HEADER header{};
  if (dib.size() >= sizeof(header)) {
    std::memcpy(&header, dib.data(), sizeof(header));
  }
  ok &= expect(header.bV5Size == sizeof(BITMAPV5HEADER),
               "the DIBV5 header size must be declared");
  ok &= expect(header.bV5Width == 2 && header.bV5Height == -2,
               "the DIBV5 must preserve dimensions in top-down order");
  ok &= expect(header.bV5BitCount == 32 &&
                   header.bV5Compression == BI_BITFIELDS,
               "the DIBV5 must declare a 32-bit bitfield image");
  ok &= expect(header.bV5RedMask == 0x00ff0000u &&
                   header.bV5GreenMask == 0x0000ff00u &&
                   header.bV5BlueMask == 0x000000ffu &&
                   header.bV5AlphaMask == 0xff000000u,
               "the DIBV5 channel masks must describe BGRA pixels");

  if (dib.size() >= sizeof(BITMAPV5HEADER) + 16u) {
    const uint8_t* pixels = dib.data() + sizeof(BITMAPV5HEADER);
    const uint8_t expected[] = {
        0,   0,   255, 255, 0,   255, 0,   128,
        255, 0,   0,   64,  255, 255, 255, 255,
    };
    ok &= expect(std::memcmp(pixels, expected, sizeof(expected)) == 0,
                 "RGBA rows must be swizzled to tight top-down BGRA");
  }

  VideoFrameSnapshot invalid = snapshot;
  invalid.strideBytes = 7;
  dib.assign(3, 42);
  error = "stale";
  ok &= expect(!playback_video_frame_clipboard::buildDibV5Payload(
                   invalid, &dib, &error),
               "a stride smaller than one RGBA row must be rejected");
  ok &= expect(!error.empty(), "an invalid snapshot must report its boundary error");
  ok &= expect(dib.empty(), "a failed conversion must not retain a stale payload");

  error = "stale";
  ok &= expect(playback_video_frame_clipboard::buildDibV5Payload(
                   snapshot, &dib, &error),
               "a valid conversion must remain reusable after an error");
  ok &= expect(error.empty(), "a successful conversion must clear stale errors");

  return ok ? 0 : 1;
}
