#include "audio/separation/artifact.h"
#include "audio/flac_writer.h"
#include "audio/separation/job.h"
#include "audio/separation/inference_backend.h"
#include "audio/separation/operation.h"
#include "audio/separation/spectral_transform.h"
#include "audio/separation/windows_ml_backend.h"
#include "core/file_output.h"
#include "audio/ffmpegaudio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "audio_separation_tests: " << message << '\n';
  return false;
}

std::filesystem::path uniqueTestDirectory() {
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("radioify-audio-separation-tests-" + std::to_string(ticks));
}

bool writeText(const std::filesystem::path& path, const char* text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << text;
  return static_cast<bool>(stream);
}

std::string readText(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

bool testArtifactContract(const std::filesystem::path& directory) {
  namespace separation = audio_separation;
  bool ok = true;
  const std::filesystem::path media = directory / L"film épisode.mp4";
  const separation::ArtifactPaths finalPaths =
      separation::artifactPathsFor(media);
  ok &= expect(finalPaths[0].filename() == L"film épisode.dialogue.flac" &&
                   finalPaths[1].filename() == L"film épisode.music.flac" &&
                   finalPaths[2].filename() == L"film épisode.effects.flac",
               "managed stems must retain the Unicode media identity");
  ok &= expect(separation::isManagedArtifactPath(finalPaths[0]) &&
                   separation::isManagedArtifactPath(finalPaths[1]) &&
                   separation::isManagedArtifactPath(finalPaths[2]) &&
                   !separation::isManagedArtifactPath(media),
               "managed output recognition must prevent recursive separation");

  std::vector<file_output::TransactionDestination> destinations;
  for (const std::filesystem::path& path : finalPaths) {
    destinations.push_back(
        {path, file_output::PublishMode::ReplaceExisting});
    ok &= expect(writeText(path, "old"),
                 "artifact fixture files must be writable");
  }
  std::string error;
  auto outputs =
      file_output::TransactionGroup::begin(std::move(destinations), &error);
  separation::ArtifactPaths temporaryPaths{};
  if (outputs) {
    for (std::size_t index = 0; index < separation::kStemCount; ++index) {
      temporaryPaths[index] = outputs->temporaryPath(index);
    }
  }
  ok &= expect(
      outputs && std::all_of(temporaryPaths.begin(), temporaryPaths.end(),
                  [](const std::filesystem::path& path) {
                    return path.extension() == L".tmp" &&
                           !separation::isManagedArtifactPath(path);
                  }),
      "staging stems must not have a playable media extension");
  for (std::size_t index = 0; index < separation::kStemCount; ++index) {
    ok &= expect(writeText(temporaryPaths[index], "new"),
                 "artifact fixture files must be writable");
  }
  ok &= expect(outputs && outputs->publish(&error),
               "a complete stem set must publish transactionally");
  for (const std::filesystem::path& path : finalPaths) {
    ok &= expect(readText(path) == "new",
                 "successful grouped publication must replace the complete "
                 "managed set");
  }
  ok &= expect(separation::artifactsExistFor(media),
               "only a complete published set must count as existing stems");

  std::vector<file_output::TransactionDestination> retryDestinations;
  for (const std::filesystem::path& path : finalPaths) {
    retryDestinations.push_back(
        {path, file_output::PublishMode::ReplaceExisting});
  }
  auto incomplete = file_output::TransactionGroup::begin(
      std::move(retryDestinations), &error);
  if (incomplete) {
    writeText(incomplete->temporaryPath(0), "partial");
    std::error_code injectedFailure;
    std::filesystem::remove(incomplete->temporaryPath(1), injectedFailure);
  }
  ok &= expect(incomplete && !incomplete->publish(&error),
               "an incomplete set must be rejected before replacement");
  for (const std::filesystem::path& path : finalPaths) {
    ok &= expect(readText(path) == "new",
                 "a failed publish must preserve every existing stem");
  }
  return ok;
}

bool testSpectralContract() {
  namespace separation = audio_separation;
  bool ok = true;
  separation::BanditSpectralTransform transform;
  std::string error;
  ok &= expect(transform.initialize(&error),
               "BandIt spectral transforms must initialize");
  if (!ok) {
    std::cerr << error << '\n';
    return false;
  }

  std::vector<float> input(
      separation::BanditSpectralTransform::kChunkFrames, 1.0f);
  std::vector<float> spectrogram;
  ok &= expect(transform.forward(input.data(), input.size(), &spectrogram,
                                 &error),
               "STFT must accept one exact model chunk");
  const float expectedDc =
      std::sqrt(static_cast<float>(
          separation::BanditSpectralTransform::kFftSize)) /
      2.0f;
  const std::size_t interiorTime = 2;
  const std::size_t dcIndex = 2 * interiorTime;
  ok &= expect(std::abs(spectrogram[dcIndex] - expectedDc) < 2.0e-3f &&
                   std::abs(spectrogram[dcIndex + 1]) < 2.0e-4f,
               "STFT must match torchaudio normalized=True scaling");

  std::vector<float> identityMask(spectrogram.size(), 0.0f);
  for (std::size_t index = 0; index < identityMask.size(); index += 2) {
    identityMask[index] = 1.0f;
  }
  std::vector<float> reconstructed;
  ok &= expect(transform.inverseMasked(spectrogram, identityMask.data(),
                                       &reconstructed, &error),
               "ISTFT must accept an identity complex mask");
  float maximumError = 0.0f;
  for (std::size_t index = 0; index < input.size(); ++index) {
    maximumError =
        std::max(maximumError, std::abs(input[index] - reconstructed[index]));
  }
  ok &= expect(maximumError < 2.0e-4f,
               "STFT and ISTFT must round-trip an identity mask");
  return ok;
}

bool testInferenceBackendContracts() {
  namespace separation = audio_separation;
  bool ok = true;
  separation::InferenceBackend directMl =
      separation::directMlInferenceBackend();
  ok &= expect(directMl.valid() &&
                   directMl.kind == separation::InferenceBackendKind::DirectMl &&
                   directMl.providerName == "DmlExecutionProvider" &&
                   directMl.providerLibrary.empty(),
               "the built-in DirectML adapter must be an explicit provider "
               "description");
  directMl.setProviderOption("test-option", "first");
  directMl.setProviderOption("test-option", "replacement");
  const auto testOptions = std::count_if(
      directMl.providerOptions.begin(), directMl.providerOptions.end(),
      [](const separation::InferenceProviderOption& option) {
        return option.key == "test-option" && option.value == "replacement";
      });
  ok &= expect(testOptions == 1,
               "provider options must replace an existing key instead of "
               "silently creating conflicting duplicates");

  const separation::WindowsMlBackendResolution nvidia =
      separation::resolveNvidiaWindowsMlBackend(
          separation::ProviderProvisioningPolicy::ObserveOnly);
  if (nvidia.ready()) {
    ok &= expect(
        nvidia.backend.valid() &&
            nvidia.backend.kind ==
                separation::InferenceBackendKind::WindowsMlNvidiaTensorRtRtx &&
            !nvidia.backend.providerLibrary.empty() &&
            !nvidia.backend.providerVersion.empty(),
        "a ready Windows ML backend must own a complete provider descriptor");
  } else {
    ok &= expect(!nvidia.detail.empty(),
                 "an unusable Windows ML backend must explain its status");
  }

  const separation::Job::Operation testOperation =
      [](const std::filesystem::path&,
         const separation::ArtifactPaths&,
         const separation::Job::ProgressReporter&,
         const separation::Job::DiagnosticReporter&,
         const separation::ExecutionControl&,
         const separation::Job::CommitStarted&, std::string*) {
        return false;
      };
  const separation::OperationBinding ready =
      separation::OperationBinding::ready(testOperation, "Test GPU");
  const separation::OperationBinding setupRequired =
      separation::OperationBinding::unavailable(
          separation::OperationAvailability::SetupRequired,
          "Install the certified provider.");
  ok &= expect(ready.ready() && ready.operation() &&
                   ready.backendName() == "Test GPU" &&
                   ready.detail().empty() && !setupRequired.ready() &&
                   !setupRequired.operation() &&
                   setupRequired.availability() ==
                       separation::OperationAvailability::SetupRequired &&
                   setupRequired.detail() ==
                       "Install the certified provider.",
               "backend bindings must make executable work and unavailable "
               "diagnostics mutually exclusive");

  const separation::OperationBinding production =
      separation::resolveProductionOperation();
  ok &= expect(production.ready()
                   ? !production.backendName().empty() &&
                         static_cast<bool>(production.operation())
                   : !production.detail().empty() &&
                         !static_cast<bool>(production.operation()),
               "production discovery must return either one usable native "
               "backend or one actionable unavailability reason");
  return ok;
}

bool testFlacWriter(const std::filesystem::path& directory) {
  constexpr std::uint32_t kSampleRate = 48000;
  constexpr std::uint32_t kChannels = 2;
  constexpr std::size_t kFrames = kSampleRate / 2;
  std::vector<float> samples(kFrames * kChannels);
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    const float value = 0.25f * static_cast<float>(std::sin(
                                    2.0 * std::numbers::pi * 440.0 *
                                    static_cast<double>(frame) / kSampleRate));
    samples[frame * kChannels] = value;
    samples[frame * kChannels + 1] = -value;
  }

  bool ok = true;
  std::string error;
  const std::filesystem::path path = directory / "writer.tmp";
  audio_file::FlacWriter writer;
  ok &= expect(writer.open(path, kSampleRate, kChannels, &error) &&
                   writer.writeFrames(samples.data(), 123, &error) &&
                   writer.writeFrames(samples.data() + 123 * kChannels,
                                      kFrames - 123, &error) &&
                   writer.finish(&error),
               "FLAC writer must finalize arbitrary frame blocks without "
               "depending on a playable filename extension");
  if (!ok) {
    std::cerr << error << '\n';
    return false;
  }

  FfmpegAudioDecoder decoder;
  ok &= expect(decoder.init(path, kChannels, kSampleRate, &error),
               "Radioify must decode its generated FLAC stem");
  std::vector<float> decoded(samples.size());
  std::uint64_t totalRead = 0;
  while (totalRead < kFrames) {
    std::uint64_t framesRead = 0;
    if (!decoder.readFrames(decoded.data() + totalRead * kChannels,
                            static_cast<std::uint32_t>(kFrames - totalRead),
                            &framesRead)) {
      ok = false;
      break;
    }
    if (framesRead == 0) break;
    totalRead += framesRead;
  }
  ok &= expect(totalRead == kFrames,
               "generated FLAC must retain its exact frame count");
  double squaredError = 0.0;
  double squaredSignal = 0.0;
  for (std::size_t index = 0; index < totalRead * kChannels; ++index) {
    const double difference =
        static_cast<double>(samples[index] - decoded[index]);
    squaredError += difference * difference;
    squaredSignal += static_cast<double>(samples[index]) * samples[index];
  }
  ok &= expect(squaredSignal > 0.0 && squaredError / squaredSignal < 1.0e-10,
               "24-bit FLAC output must remain effectively lossless");
  return ok;
}

bool testJobLifecycle(const std::filesystem::path& directory) {
  namespace separation = audio_separation;
  static_assert(!std::is_default_constructible_v<separation::Job>,
                "background jobs must receive their backend explicitly");
  separation::Job unconfigured(separation::Job::Operation{});
  separation::Job job(
      [](const std::filesystem::path&, const separation::ArtifactPaths&,
         const separation::Job::ProgressReporter& progress,
         const separation::Job::DiagnosticReporter& diagnostics,
         const separation::ExecutionControl&,
         const separation::Job::CommitStarted& beginCommit,
         std::string*) {
        diagnostics(DiagnosticLevel::Info, "test", "operation invoked");
        progress(0.4f, "Separating test audio");
        return beginCommit();
      });
  bool ok = true;
  ok &= expect(!unconfigured.configured() && job.configured(),
               "job availability must derive from an actual operation");
  ok &= expect(job.tryStart("clip.mp4"),
               "a valid media path must start a separation job");
  std::optional<separation::JobSnapshot> completion;
  for (int attempt = 0; attempt < 100 && !completion; ++attempt) {
    completion = job.takeCompletion();
    if (!completion) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ok &= expect(completion && completion->succeeded() &&
                   completion->outputFiles[0] == "clip.dialogue.flac" &&
                   completion->outputFiles[1] == "clip.music.flac" &&
                   completion->outputFiles[2] == "clip.effects.flac" &&
                   !completion->diagnosticLog.empty(),
               "job completion must own and expose the managed stem set");
  if (completion && !completion->diagnosticLog.empty()) {
    const std::string diagnostics = readText(completion->diagnosticLog);
    ok &= expect(diagnostics.find("[test] operation invoked") !=
                         std::string::npos &&
                     diagnostics.find("completed successfully") !=
                         std::string::npos,
                 "a job log must retain backend diagnostics and outcome");
    std::error_code ignored;
    std::filesystem::remove(completion->diagnosticLog, ignored);
  }
  ok &= expect(!job.takeCompletion(),
               "a separation completion must be delivered exactly once");

  const std::filesystem::path missingModel =
      directory / "missing-explicit-model.onnx";
  const separation::Job::Operation explicitModelOperation =
      separation::makeModelOperation(missingModel);
  std::atomic<bool> notCancelled{false};
  const separation::ExecutionControl explicitModelControl(&notCancelled);
  std::string explicitModelError;
  ok &= expect(
      explicitModelOperation &&
          !explicitModelOperation(
              "clip.mp4", separation::artifactPathsFor("clip.mp4"),
              [](float, std::string) {},
              [](DiagnosticLevel, std::string_view, std::string_view) {},
              explicitModelControl, []() { return true; },
              &explicitModelError) &&
          explicitModelError.find("missing-explicit-model.onnx") !=
              std::string::npos,
      "diagnostic model selection must be explicit and identify a bad path");

  std::atomic<bool> operationStarted{false};
  separation::Job cancellable(
      [&](const std::filesystem::path&, const separation::ArtifactPaths&,
          const separation::Job::ProgressReporter& progress,
          const separation::Job::DiagnosticReporter&,
          const separation::ExecutionControl& control,
          const separation::Job::CommitStarted&, std::string* error) {
        progress(0.2f, "Separating test audio");
        operationStarted.store(true, std::memory_order_release);
        while (!control.cancellationRequested()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (error) *error = "Controlled cancellation.";
        return false;
      });
  ok &= expect(cancellable.tryStart("cancel.mp4"),
               "a cancellable operation must start");
  for (int attempt = 0;
       attempt < 100 && !operationStarted.load(std::memory_order_acquire);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ok &= expect(operationStarted.load(std::memory_order_acquire) &&
                   cancellable.requestCancel() &&
                   !cancellable.requestCancel(),
               "cancellation must be accepted exactly once");
  std::optional<separation::JobSnapshot> cancelled;
  for (int attempt = 0; attempt < 100 && !cancelled; ++attempt) {
    cancelled = cancellable.takeCompletion();
    if (!cancelled) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ok &= expect(cancelled && cancelled->state == separation::JobState::Cancelled &&
                   cancelled->error.empty() &&
                   !cancelled->diagnosticLog.empty(),
               "cancelled work must publish typed state without backend noise");
  if (cancelled && !cancelled->diagnosticLog.empty()) {
    std::error_code ignored;
    std::filesystem::remove(cancelled->diagnosticLog, ignored);
  }

  std::atomic<bool> initiallyPausedOperationEntered{false};
  separation::Job initiallyPaused(
      [&](const std::filesystem::path&, const separation::ArtifactPaths&,
          const separation::Job::ProgressReporter&,
          const separation::Job::DiagnosticReporter&,
          const separation::ExecutionControl&,
          const separation::Job::CommitStarted& beginCommit,
          std::string*) {
        initiallyPausedOperationEntered.store(true, std::memory_order_release);
        return beginCommit();
      });
  ok &= expect(initiallyPaused.tryStart(
                   "initially-paused.mp4",
                   separation::JobStartOptions{true}),
               "foreground priority must be part of the atomic job start");
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const separation::JobSnapshot initiallyPausedSnapshot =
      initiallyPaused.snapshot();
  const bool backendBlockedBeforeResume =
      !initiallyPausedOperationEntered.load(std::memory_order_acquire);
  const bool initiallyPausedResumeAccepted = initiallyPaused.setPaused(false);
  std::optional<separation::JobSnapshot> initiallyPausedCompletion;
  for (int attempt = 0; attempt < 100 && !initiallyPausedCompletion;
       ++attempt) {
    initiallyPausedCompletion = initiallyPaused.takeCompletion();
    if (!initiallyPausedCompletion) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ok &= expect(initiallyPausedSnapshot.scheduling ==
                       separation::JobSchedulingState::Suspended &&
                   backendBlockedBeforeResume &&
                   initiallyPausedResumeAccepted && initiallyPausedCompletion &&
                   initiallyPausedCompletion->succeeded() &&
                   initiallyPausedOperationEntered.load(
                       std::memory_order_acquire),
               "an initially paused job must not enter or allocate its backend");
  if (initiallyPausedCompletion &&
      !initiallyPausedCompletion->diagnosticLog.empty()) {
    const std::string diagnostics =
        readText(initiallyPausedCompletion->diagnosticLog);
    ok &= expect(diagnostics.find("Started paused while foreground video") !=
                     std::string::npos,
                 "the task log must record atomic foreground scheduling");
    std::error_code ignored;
    std::filesystem::remove(initiallyPausedCompletion->diagnosticLog, ignored);
  }

  std::atomic<bool> pauseOperationStarted{false};
  std::atomic<bool> pauseInterruptObserved{false};
  std::atomic<bool> enterPauseCheckpoint{false};
  std::atomic<bool> pauseResourcesYielded{false};
  std::atomic<bool> passedPauseCheckpoint{false};
  separation::Job pausable(
      [&](const std::filesystem::path&, const separation::ArtifactPaths&,
          const separation::Job::ProgressReporter&,
          const separation::Job::DiagnosticReporter&,
          const separation::ExecutionControl& control,
          const separation::Job::CommitStarted& beginCommit,
          std::string*) {
        pauseOperationStarted.store(true, std::memory_order_release);
        auto interruption = control.registerInterruption([&]() {
          pauseInterruptObserved.store(true, std::memory_order_release);
        });
        while (!pauseInterruptObserved.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        interruption.reset();
        while (!enterPauseCheckpoint.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        if (!control.checkpoint([&]() {
              pauseResourcesYielded.store(true, std::memory_order_release);
            })) {
          return false;
        }
        passedPauseCheckpoint.store(true, std::memory_order_release);
        return beginCommit();
      });
  ok &= expect(pausable.tryStart("pause.mp4"),
               "a pausable operation must start");
  for (int attempt = 0;
       attempt < 100 &&
       !pauseOperationStarted.load(std::memory_order_acquire);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const bool pauseAccepted = pausable.setPaused(true);
  for (int attempt = 0;
       attempt < 100 &&
       !pauseInterruptObserved.load(std::memory_order_acquire);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const separation::JobSnapshot suspendingSnapshot = pausable.snapshot();
  enterPauseCheckpoint.store(true, std::memory_order_release);
  for (int attempt = 0;
       attempt < 100 &&
       !pauseResourcesYielded.load(std::memory_order_acquire);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const separation::JobSnapshot pausedSnapshot = pausable.snapshot();
  const bool stayedPaused =
      !passedPauseCheckpoint.load(std::memory_order_acquire);
  const bool resumeAccepted = pausable.setPaused(false);
  std::optional<separation::JobSnapshot> resumedCompletion;
  for (int attempt = 0; attempt < 100 && !resumedCompletion; ++attempt) {
    resumedCompletion = pausable.takeCompletion();
    if (!resumedCompletion) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ok &= expect(pauseAccepted &&
                   pauseInterruptObserved.load(std::memory_order_acquire) &&
                   suspendingSnapshot.scheduling ==
                       separation::JobSchedulingState::Suspending &&
                   pausedSnapshot.scheduling ==
                       separation::JobSchedulingState::Suspended &&
                   stayedPaused &&
                   pauseResourcesYielded.load(std::memory_order_acquire) &&
                   resumeAccepted && resumedCompletion &&
                   resumedCompletion->succeeded() &&
                   passedPauseCheckpoint.load(std::memory_order_acquire),
               "pause must yield recreatable resources, block at a checkpoint, "
               "and resume without losing the job");
  if (resumedCompletion && !resumedCompletion->diagnosticLog.empty()) {
    const std::string pauseDiagnostics =
        readText(resumedCompletion->diagnosticLog);
    ok &= expect(pauseDiagnostics.find("Suspending for foreground video") !=
                         std::string::npos &&
                     pauseDiagnostics.find(
                         "Resumed after foreground video") !=
                         std::string::npos,
                 "the task log must explain scheduler-driven pauses");
    std::error_code ignored;
    std::filesystem::remove(resumedCompletion->diagnosticLog, ignored);
  }

  std::atomic<bool> pausedCancelOperationStarted{false};
  std::atomic<bool> enterPausedCancelCheckpoint{false};
  separation::Job cancelWhilePaused(
      [&](const std::filesystem::path&, const separation::ArtifactPaths&,
          const separation::Job::ProgressReporter&,
          const separation::Job::DiagnosticReporter&,
          const separation::ExecutionControl& control,
          const separation::Job::CommitStarted& beginCommit,
          std::string* error) {
        pausedCancelOperationStarted.store(true, std::memory_order_release);
        while (!enterPausedCancelCheckpoint.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        if (control.checkpoint()) return beginCommit();
        if (error) *error = "Controlled cancellation while paused.";
        return false;
      });
  ok &= expect(cancelWhilePaused.tryStart("paused-cancel.mp4"),
               "a paused cancellation operation must start");
  for (int attempt = 0;
       attempt < 100 &&
       !pausedCancelOperationStarted.load(std::memory_order_acquire);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const bool pausedBeforeCancellation = cancelWhilePaused.setPaused(true);
  enterPausedCancelCheckpoint.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const bool pausedCancellationAccepted = cancelWhilePaused.requestCancel();
  std::optional<separation::JobSnapshot> pausedCancellation;
  for (int attempt = 0; attempt < 100 && !pausedCancellation; ++attempt) {
    pausedCancellation = cancelWhilePaused.takeCompletion();
    if (!pausedCancellation) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ok &= expect(pausedBeforeCancellation && pausedCancellationAccepted &&
                   pausedCancellation &&
                   pausedCancellation->state ==
                       separation::JobState::Cancelled,
               "cancelling a paused job must wake and join its worker");
  if (pausedCancellation && !pausedCancellation->diagnosticLog.empty()) {
    std::error_code ignored;
    std::filesystem::remove(pausedCancellation->diagnosticLog, ignored);
  }

  std::atomic<bool> commitEntered{false};
  std::atomic<bool> releaseCommit{false};
  separation::Job committing(
      [&](const std::filesystem::path&, const separation::ArtifactPaths&,
          const separation::Job::ProgressReporter&,
          const separation::Job::DiagnosticReporter&,
          const separation::ExecutionControl& control,
          const separation::Job::CommitStarted& beginCommit,
          std::string*) {
        if (!beginCommit()) return false;
        commitEntered.store(true, std::memory_order_release);
        while (!releaseCommit.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return !control.cancellationRequested();
      });
  ok &= expect(committing.tryStart("publishing.mp4"),
               "a commit-barrier operation must start");
  for (int attempt = 0;
       attempt < 100 && !commitEntered.load(std::memory_order_acquire);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const separation::JobSnapshot publishing = committing.snapshot();
  const bool lateCancellationRejected = !committing.requestCancel();
  releaseCommit.store(true, std::memory_order_release);
  std::optional<separation::JobSnapshot> published;
  for (int attempt = 0; attempt < 100 && !published; ++attempt) {
    published = committing.takeCompletion();
    if (!published) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ok &= expect(commitEntered.load(std::memory_order_acquire) &&
                   publishing.state == separation::JobState::Publishing &&
                   publishing.running() && !publishing.cancellable() &&
                   publishing.phase == "Publishing audio stems" &&
                   lateCancellationRejected && published &&
                   published->succeeded(),
               "audio separation must remove Cancel at its linearized "
               "publication boundary");
  if (published && !published->diagnosticLog.empty()) {
    std::error_code ignored;
    std::filesystem::remove(published->diagnosticLog, ignored);
  }

  separation::Job missingBarrier(
      [](const std::filesystem::path&, const separation::ArtifactPaths&,
         const separation::Job::ProgressReporter&,
         const separation::Job::DiagnosticReporter&,
         const separation::ExecutionControl&,
         const separation::Job::CommitStarted&, std::string*) {
        return true;
      });
  ok &= expect(missingBarrier.tryStart("missing-barrier.mp4"),
               "a controlled contract-violation fixture must start");
  std::optional<separation::JobSnapshot> invalidCompletion;
  for (int attempt = 0; attempt < 100 && !invalidCompletion; ++attempt) {
    invalidCompletion = missingBarrier.takeCompletion();
    if (!invalidCompletion) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ok &= expect(invalidCompletion &&
                   invalidCompletion->state == separation::JobState::Failed &&
                   invalidCompletion->error.find("commit barrier") !=
                       std::string::npos,
               "a backend may not report success without claiming the "
               "publication boundary");
  if (invalidCompletion && !invalidCompletion->diagnosticLog.empty()) {
    std::error_code ignored;
    std::filesystem::remove(invalidCompletion->diagnosticLog, ignored);
  }
  return ok;
}

}  // namespace

int main() {
  const std::filesystem::path directory = uniqueTestDirectory();
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    std::cerr << "Could not create test directory: " << ec.message() << '\n';
    return EXIT_FAILURE;
  }

  bool ok = true;
  ok &= testArtifactContract(directory);
  ok &= testSpectralContract();
  ok &= testInferenceBackendContracts();
  ok &= testFlacWriter(directory);
  ok &= testJobLifecycle(directory);

  std::filesystem::remove_all(directory, ec);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
