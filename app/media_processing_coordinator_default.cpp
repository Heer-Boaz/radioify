#include "app/media_processing_coordinator.h"

#include "audio/audioplayback.h"
#include "audio/audio_export.h"
#include "audio/loopsplit/loopsplit.h"
#include "audio/separation/operation.h"
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
                             const CommitStarted& outputCommitStarted,
                             std::string* error) {
              return audioPlayback.analyzeFileToMelodyFile(
                  sourceFile, trackIndex, outputFile,
                  [&](float progress) {
                    reportProgress(progress, "Analyzing melody");
                  },
                  cancellationRequested, error, outputCommitStarted);
            };
        operations.splitLoop =
            [](const std::filesystem::path& sourceFile,
               const std::filesystem::path& stingerOutput,
               const std::filesystem::path& loopOutput,
               const LoopSplitConfig& config, LoopSplitResult* result,
               const ProgressReporter& reportProgress,
               const CancellationRequested& cancellationRequested,
               const CommitStarted& outputCommitStarted,
               std::string* error) {
              return splitAudioIntoLoopFiles(sourceFile, stingerOutput,
                                             loopOutput, config, result,
                                             reportProgress,
                                             cancellationRequested, error,
                                             outputCommitStarted);
            };
        operations.exportAudio =
            [](const std::filesystem::path& sourceFile,
               const std::filesystem::path& outputFile,
               const ProgressReporter& reportProgress,
               const CancellationRequested& cancellationRequested,
               const CommitStarted& outputCommitStarted,
               std::string* error) {
              return audio_export::exportToFlac(
                  sourceFile, outputFile, reportProgress,
                  cancellationRequested, error, outputCommitStarted);
            };
        operations.exportTranscriptText =
            [](const std::filesystem::path& sourceFile,
               const std::filesystem::path& outputFile,
               const ProgressReporter& reportProgress,
               const CancellationRequested& cancellationRequested,
               const CommitStarted& outputCommitStarted,
               std::string* error) {
              return playback_video_transcript::exportTranscriptText(
                  sourceFile, outputFile, reportProgress,
                  cancellationRequested, error, outputCommitStarted);
            };
        operations.generateSubtitles =
            playback_video_transcript::GenerationJob::productionOperation();
#if RADIOIFY_HAS_AUDIO_SEPARATION
        operations.separateAudio =
            audio_separation::resolveProductionOperation();
#endif
        return operations;
      }()) {}

}  // namespace media_processing
