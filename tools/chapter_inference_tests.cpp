#include "playback/video/chapter/inference.h"

#include <iostream>
#include <string_view>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "chapter_inference_tests: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace playback_video_chapters;
  const bool requireVulkan =
      argc == 2 && std::string_view(argv[1]) == "--require-vulkan";

  InferenceEngine engine;
  OperationControl cancelled;
  cancelled.cancelled = [] { return true; };
  cancelled.backgroundGpuAllowed = [] { return true; };
  bool ok = expect(engine.inspect(cancelled).state ==
                       CapabilityState::Cancelled,
                   "cancellation must win before backend initialization");

  OperationControl yielded;
  yielded.cancelled = [] { return false; };
  yielded.backgroundGpuAllowed = [] { return false; };
  ok &= expect(engine.inspect(yielded).state == CapabilityState::Yielded,
               "foreground playback must prevent backend initialization");

  OperationControl available;
  available.cancelled = [] { return false; };
  available.backgroundGpuAllowed = [] { return true; };
  const CapabilityResult capability = engine.inspect(available);
  ok &= expect(capability.state == CapabilityState::Ready ||
                   (capability.state == CapabilityState::Unsupported &&
                    !capability.detail.empty()),
               "device inspection must return a typed, explained result");
  if (requireVulkan) {
    ok &= expect(capability.state == CapabilityState::Ready,
                 "the requested Vulkan integration probe must initialize");
  }

  InferenceRequest invalid;
  const InferenceResult invalidResult = engine.run(invalid, available);
  if (capability.state == CapabilityState::Ready) {
    ok &= expect(invalidResult.status == OperationStatus::Failed &&
                     !invalidResult.detail.empty(),
                 "an incomplete in-memory request must fail before loading a "
                 "model");
  } else {
    ok &= expect(invalidResult.status == OperationStatus::Unsupported,
                 "unsupported hardware must remain an explicit result");
  }
  return ok ? 0 : 1;
}
