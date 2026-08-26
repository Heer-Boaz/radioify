#include "app/media_processing_coordinator.h"

#include <memory>
#include <utility>

#include "audio/audioplayback.h"
#include "audio/loopsplit/loopsplit.h"

namespace media_processing {

Coordinator::Coordinator()
    : Coordinator(Backends{
          [](const std::filesystem::path& sourceFile, int trackIndex,
             const std::filesystem::path& outputFile,
             const MelodyProgressReporter& reportProgress,
             std::string* error) {
            return audioAnalyzeFileToMelodyFile(
                sourceFile, trackIndex, outputFile, reportProgress, error);
          },
          [](const std::filesystem::path& sourceFile,
             const std::filesystem::path& stingerOutput,
             const std::filesystem::path& loopOutput,
             const LoopSplitConfig& config, LoopSplitResult* result,
             std::string* error) {
            return splitAudioIntoLoopFiles(sourceFile, stingerOutput,
                                           loopOutput, config, result, error);
          },
          std::make_unique<playback_video_transcript::GenerationJob>(),
          std::make_unique<audio_separation::Job>(),
#if RADIOIFY_HAS_AUDIO_SEPARATION
          true
#else
          false
#endif
      }) {}

}  // namespace media_processing
