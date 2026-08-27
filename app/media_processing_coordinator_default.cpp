#include "app/media_processing_coordinator.h"

#include "audio/audioplayback.h"
#include "audio/loopsplit/loopsplit.h"

namespace media_processing {

Coordinator::Coordinator(AudioPlaybackRuntime& audioPlayback)
    : Coordinator([&audioPlayback]() {
        Operations operations;
        operations.analyzeMelody =
            [&audioPlayback](const std::filesystem::path& sourceFile,
                             int trackIndex,
                             const std::filesystem::path& outputFile,
                             const MelodyProgressReporter& reportProgress,
                             std::string* error) {
              return audioPlayback.analyzeFileToMelodyFile(
                  sourceFile, trackIndex, outputFile, reportProgress, error);
            };
        operations.splitLoop =
            [](const std::filesystem::path& sourceFile,
               const std::filesystem::path& stingerOutput,
               const std::filesystem::path& loopOutput,
               const LoopSplitConfig& config, LoopSplitResult* result,
               std::string* error) {
              return splitAudioIntoLoopFiles(sourceFile, stingerOutput,
                                             loopOutput, config, result,
                                             error);
            };
#if RADIOIFY_HAS_AUDIO_SEPARATION
        operations.audioSeparationAvailable = true;
#else
        operations.audioSeparationAvailable = false;
#endif
        return operations;
      }()) {}

}  // namespace media_processing
