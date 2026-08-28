#include "audio/separation/separator.h"

#include <utility>

namespace audio_separation {

bool separateMediaAudio(const std::filesystem::path&,
                        const ArtifactPaths&, const ProgressCallback&,
                        const DiagnosticReporter&,
                        const ExecutionControl&, std::string* error) {
  if (error) {
    *error =
        "Audio separation is only available in the Windows x64 build.";
  }
  return false;
}

bool separateMediaAudioWithModel(
    const std::filesystem::path&, const std::filesystem::path&,
    const ArtifactPaths&, const ProgressCallback&,
    const DiagnosticReporter&,
    const ExecutionControl&, std::string* error) {
  if (error) {
    *error =
        "Audio separation is only available in the Windows x64 build.";
  }
  return false;
}

}  // namespace audio_separation
