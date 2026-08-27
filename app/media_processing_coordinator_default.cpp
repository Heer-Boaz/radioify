#include "app/media_processing_coordinator.h"

#include "audio/audioplayback.h"
#include "audio/audio_export.h"
#include "audio/loopsplit/loopsplit.h"
#include "playback/video/transcript/text_export.h"

namespace media_processing {

Coordinator::Coordinator(AudioPlaybackRuntime& audioPlayback)
    : Coordinator([&audioPlayback]() {
        Operations operations;
        operations.analyzeMelody =
            [&audioPlayback](const std::filesystem::path& sourceFile,
                             int trackIndex,
                             const std::filesystem::path& outputFile,
                             const ProgressReporter& reportProgress,
                             const CancellationRequested&
                                 cancellationRequested,
                             std::string* error) {
              return audioPlayback.analyzeFileToMelodyFile(
                  sourceFile, trackIndex, outputFile,
                  [&](float progress) {
                    reportProgress(progress, "Analyzing melody");
                  },
                  cancellationRequested, error);
            };
        operations.splitLoop =
            [](const std::filesystem::path& sourceFile,
               const std::filesystem::path& stingerOutput,
               const std::filesystem::path& loopOutput,
               const LoopSplitConfig& config, LoopSplitResult* result,
               const ProgressReporter& reportProgress,
               const CancellationRequested& cancellationRequested,
               std::string* error) {
              return splitAudioIntoLoopFiles(sourceFile, stingerOutput,
                                             loopOutput, config, result,
                                             reportProgress,
                                             cancellationRequested, error);
            };
        operations.exportAudio = audio_export::exportToFlac;
        operations.exportTranscriptText =
            playback_video_transcript::exportTranscriptText;
        operations.generateSubtitles =
            playback_video_transcript::GenerationJob::productionOperation();
#if RADIOIFY_HAS_AUDIO_SEPARATION
        operations.separateAudio =
            audio_separation::Job::productionOperation();
#endif
        return operations;
      }()) {}

}  // namespace media_processing
