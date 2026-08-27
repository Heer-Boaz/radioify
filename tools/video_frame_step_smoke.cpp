#include "playback/video/player.h"
#include "playback/video/gpu/gpu_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "audio/audioplayback.h"

namespace {

static_assert(std::is_same_v<decltype(&Player::copyCurrentVideoFrame),
                             bool (Player::*)(VideoFrame*) const>);

constexpr int64_t kDefaultSeekUs = 60000000;
constexpr int kDefaultTimeoutMs = 120000;

const char* stateName(PlayerState state) {
  switch (state) {
    case PlayerState::Idle:
      return "Idle";
    case PlayerState::Opening:
      return "Opening";
    case PlayerState::Prefill:
      return "Prefill";
    case PlayerState::Priming:
      return "Priming";
    case PlayerState::Playing:
      return "Playing";
    case PlayerState::Paused:
      return "Paused";
    case PlayerState::FrameStep:
      return "FrameStep";
    case PlayerState::Seeking:
      return "Seeking";
    case PlayerState::Draining:
      return "Draining";
    case PlayerState::Ended:
      return "Ended";
    case PlayerState::Error:
      return "Error";
    case PlayerState::Closing:
      return "Closing";
  }
  return "Unknown";
}

void printDebug(const char* label, const Player& player,
                const PlayerDebugInfo& info) {
  const PlayerTimelineSnapshot timeline = player.timelineSnapshot();
  std::cout << label << " state=" << stateName(info.state)
            << " serial=" << info.currentSerial
            << " pending_seek=" << info.pendingSeekSerial
            << " inflight_seek=" << info.seekInFlightSerial
            << " frame_counter=" << player.videoFrameCounter()
            << " pts_us=" << info.lastPresentedPtsUs
            << " dur_us=" << info.lastPresentedDurationUs
            << " display_index=" << info.lastPresentedDisplayIndex
            << " queue=" << info.videoQueueDepth
            << " has_frame=" << (info.hasVideoFrame ? 1 : 0)
            << " audio_buffered=" << info.audioBufferedFrames
            << " audio_rate=" << info.audioSampleRate
            << " audio_finished=" << (player.audioFinished() ? 1 : 0)
            << " audio_clock_ready=" << (info.audioClockReady ? 1 : 0)
            << " presentation_us=" << timeline.positionUs
            << " source_us=" << timeline.sourcePositionUs
            << " presentation_duration_us=" << player.durationUs()
            << " source_duration_us=" << player.sourceDurationUs()
            << " seek_pending=" << (timeline.seekPending() ? 1 : 0) << '\n';
}

bool parseInt64(const char* value, int64_t* out) {
  if (!value || !out) {
    return false;
  }
  char* end = nullptr;
  long long parsed = std::strtoll(value, &end, 10);
  if (!end || *end != '\0') {
    return false;
  }
  *out = static_cast<int64_t>(parsed);
  return true;
}

bool parsePositiveSize(const char* value, size_t* out) {
  if (!value || !out) {
    return false;
  }
  char* end = nullptr;
  unsigned long long parsed = std::strtoull(value, &end, 10);
  if (!end || *end != '\0' || parsed == 0) {
    return false;
  }
  *out = static_cast<size_t>(parsed);
  return true;
}

template <typename Predicate>
bool waitFor(Player& player, int timeoutMs, const char* label,
             Predicate predicate, PlayerDebugInfo* out = nullptr,
             const char* expected = nullptr) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  PlayerDebugInfo last{};
  while (std::chrono::steady_clock::now() < deadline) {
    last = player.debugInfo();
    if (predicate(last)) {
      if (out) {
        *out = last;
      }
      printDebug(label, player, last);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::cerr << "video_frame_step_smoke: timeout waiting for " << label;
  if (expected) {
    std::cerr << "; expected " << expected;
  }
  std::cerr << '\n';
  printDebug("last", player, last);
  return false;
}

bool waitForAppliedAudioReset(uint64_t previousGeneration, int expectedSerial,
                              AudioStreamReset* out) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kDefaultTimeoutMs);
  AudioStreamReset observed{};
  while (std::chrono::steady_clock::now() < deadline) {
    observed = audioStreamLastAppliedReset();
    if (observed.generation > previousGeneration &&
        observed.serial == expectedSerial) {
      if (out) {
        *out = observed;
      }
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (out) {
    *out = observed;
  }
  return false;
}

bool expectFrameStepTransition(bool audioEnabled, int64_t expectedPtsUs,
                               int previousSerial, int expectedSerial,
                               const AudioStreamReset& previousReset,
                               const char* label) {
  const bool seekBackedStep = expectedSerial != previousSerial;
  if (seekBackedStep && expectedSerial != previousSerial + 1) {
    std::cerr << "video_frame_step_smoke: " << label
              << " invalid frame-step serial transition previous_serial="
              << previousSerial << " expected_serial=" << expectedSerial
              << '\n';
    return false;
  }
  if (!audioEnabled) {
    return true;
  }

  AudioStreamReset applied = audioStreamLastAppliedReset();
  if (seekBackedStep) {
    if (!waitForAppliedAudioReset(previousReset.generation, expectedSerial,
                                  &applied)) {
      std::cerr << "video_frame_step_smoke: " << label
                << " timed out waiting for applied audio reset"
                << " previous_generation=" << previousReset.generation
                << " expected_serial=" << expectedSerial
                << " actual_generation=" << applied.generation
                << " actual_serial=" << applied.serial << '\n';
      return false;
    }
    if (applied.generation != previousReset.generation + 1 ||
        applied.discardUntilUs != expectedPtsUs ||
        applied.resetPlaybackPosition) {
      std::cerr << "video_frame_step_smoke: " << label
                << " applied the wrong audio frame-step reset"
                << " expected_generation=" << previousReset.generation + 1
                << " actual_generation=" << applied.generation
                << " expected_discard_until_us=" << expectedPtsUs
                << " actual_discard_until_us=" << applied.discardUntilUs
                << " reset_playback_position="
                << (applied.resetPlaybackPosition ? 1 : 0) << '\n';
      return false;
    }
  } else if (applied.generation != previousReset.generation) {
    std::cerr << "video_frame_step_smoke: " << label
              << " reset audio during a cached frame step"
              << " previous_generation=" << previousReset.generation
              << " actual_generation=" << applied.generation << '\n';
    return false;
  }

  if (audioStreamSerial() != expectedSerial) {
    std::cerr << "video_frame_step_smoke: " << label
              << " audio stream serial mismatch previous_serial="
              << previousSerial << " expected_serial=" << expectedSerial
              << " actual_serial=" << audioStreamSerial() << '\n';
    return false;
  }
  return true;
}

bool expectSerialTransition(bool audioEnabled, int64_t expectedPtsUs,
                            int previousSerial, int expectedSerial,
                            const AudioStreamReset& previousReset,
                            const char* label) {
  if (expectedSerial != previousSerial + 1) {
    std::cerr << "video_frame_step_smoke: " << label
              << " invalid serial transition previous_serial="
              << previousSerial << " expected_serial=" << expectedSerial
              << '\n';
    return false;
  }
  if (!audioEnabled) {
    return true;
  }

  AudioStreamReset applied{};
  if (!waitForAppliedAudioReset(previousReset.generation, expectedSerial,
                                &applied) ||
      applied.generation != previousReset.generation + 1 ||
      applied.discardUntilUs != expectedPtsUs ||
      applied.resetPlaybackPosition ||
      audioStreamSerial() != expectedSerial) {
    std::cerr << "video_frame_step_smoke: " << label
              << " audio serial reset mismatch"
              << " expected_generation=" << previousReset.generation + 1
              << " actual_generation=" << applied.generation
              << " expected_discard_until_us=" << expectedPtsUs
              << " actual_discard_until_us=" << applied.discardUntilUs
              << " expected_serial=" << expectedSerial
              << " actual_serial=" << applied.serial
              << " stream_serial=" << audioStreamSerial() << '\n';
    return false;
  }
  return true;
}

bool expectSeekStepOverlapTransition(
    bool audioEnabled, int64_t requestedSeekUs, int64_t presentedPtsUs,
    int previousSerial, int expectedSerial,
    const AudioStreamReset& previousReset, const char* label) {
  if (expectedSerial <= previousSerial) {
    std::cerr << "video_frame_step_smoke: " << label
              << " did not advance the transport serial"
              << " previous_serial=" << previousSerial
              << " expected_serial=" << expectedSerial << '\n';
    return false;
  }
  if (!audioEnabled) {
    return true;
  }

  AudioStreamReset applied{};
  const uint64_t expectedGeneration =
      previousReset.generation +
      static_cast<uint64_t>(expectedSerial - previousSerial);
  const bool resetObserved = waitForAppliedAudioReset(
      previousReset.generation, expectedSerial, &applied);
  const bool discardMatchesTransportAnchor =
      applied.discardUntilUs == requestedSeekUs ||
      applied.discardUntilUs == presentedPtsUs;
  if (!resetObserved ||
      applied.generation != expectedGeneration ||
      !discardMatchesTransportAnchor ||
      applied.framePosition < previousReset.framePosition ||
      applied.resetPlaybackPosition ||
      audioStreamSerial() != expectedSerial) {
    std::cerr << "video_frame_step_smoke: " << label
              << " combined seek/frame-step audio reset mismatch"
              << " expected_generation=" << expectedGeneration
              << " actual_generation=" << applied.generation
              << " requested_seek_us=" << requestedSeekUs
              << " presented_pts_us=" << presentedPtsUs
              << " actual_discard_until_us=" << applied.discardUntilUs
              << " minimum_frame_position=" << previousReset.framePosition
              << " actual_frame_position=" << applied.framePosition
              << " reset_playback_position="
              << (applied.resetPlaybackPosition ? 1 : 0)
              << " expected_serial=" << expectedSerial
              << " actual_serial=" << applied.serial
              << " stream_serial=" << audioStreamSerial() << '\n';
    return false;
  }
  return true;
}

bool expectConcurrentResumeTransition(
    bool audioEnabled, int64_t expectedPtsUs, int expectedSerial,
    const AudioStreamReset& previousReset, const char* label) {
  if (!audioEnabled) {
    return true;
  }

  AudioStreamReset applied{};
  if (!waitForAppliedAudioReset(previousReset.generation, expectedSerial,
                                &applied) ||
      applied.discardUntilUs != expectedPtsUs ||
      applied.resetPlaybackPosition ||
      audioStreamSerial() != expectedSerial) {
    std::cerr << "video_frame_step_smoke: " << label
              << " concurrent audio resume reset mismatch"
              << " previous_generation=" << previousReset.generation
              << " actual_generation=" << applied.generation
              << " expected_discard_until_us=" << expectedPtsUs
              << " actual_discard_until_us=" << applied.discardUntilUs
              << " expected_serial=" << expectedSerial
              << " actual_serial=" << applied.serial
              << " stream_serial=" << audioStreamSerial() << '\n';
    return false;
  }
  return true;
}

int64_t chooseSeekUs(const Player& player, int64_t requestedSeekUs) {
  int64_t durationUs = player.durationUs();
  if (durationUs <= 0) {
    return requestedSeekUs;
  }
  if (durationUs <= 3000000) {
    return durationUs / 2;
  }
  int64_t latestSafeSeekUs = durationUs - 2000000;
  if (requestedSeekUs <= latestSafeSeekUs) {
    return requestedSeekUs;
  }
  return latestSafeSeekUs > 1000000 ? latestSafeSeekUs : durationUs / 2;
}

struct ObservedFrameStep {
  int64_t ptsUs = 0;
  int serial = 0;
  uint64_t frameCounter = 0;
};

bool requestAndObserveFrameStep(
    Player& player, bool audioEnabled,
    playback_video_frame_step::Direction direction, const std::string& label,
    const int64_t* expectedPtsUs, ObservedFrameStep* current,
    bool previousDiscoveryUsesBoundaryAnchor = false) {
  if (!current) {
    return false;
  }

  const int64_t beforePtsUs = current->ptsUs;
  const int beforeSerial = current->serial;
  const uint64_t beforeCounter = current->frameCounter;
  const AudioStreamReset beforeAudioReset =
      audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};

  if (!player.requestFrameStep(direction)) {
    std::cerr << "video_frame_step_smoke: " << label
              << " frame-step request rejected\n";
    return false;
  }

  PlayerDebugInfo observed{};
  if (!waitFor(player, kDefaultTimeoutMs, label.c_str(),
               [&](const PlayerDebugInfo& info) {
                 return info.hasVideoFrame &&
                        player.videoFrameCounter() > beforeCounter &&
                        info.seekInFlightSerial == 0 &&
                        info.pendingSeekSerial == 0 &&
                        info.state == PlayerState::FrameStep;
               },
               &observed)) {
    return false;
  }

  const bool movedInDirection =
      direction == playback_video_frame_step::Direction::Previous
          ? observed.lastPresentedPtsUs < beforePtsUs
          : observed.lastPresentedPtsUs > beforePtsUs;
  if (!movedInDirection) {
    std::cerr << "video_frame_step_smoke: " << label
              << " moved in the wrong direction; before_pts_us="
              << beforePtsUs
              << " actual_pts_us=" << observed.lastPresentedPtsUs
              << " before_serial=" << beforeSerial
              << " actual_serial=" << observed.currentSerial << '\n';
    return false;
  }
  if (expectedPtsUs && observed.lastPresentedPtsUs != *expectedPtsUs) {
    std::cerr << "video_frame_step_smoke: " << label
              << " did not return to the inverse-step frame; expected_pts_us="
              << *expectedPtsUs
              << " actual_pts_us=" << observed.lastPresentedPtsUs << '\n';
    return false;
  }
  const bool seekBackedPreviousDiscovery =
      previousDiscoveryUsesBoundaryAnchor &&
      direction == playback_video_frame_step::Direction::Previous &&
      observed.currentSerial != beforeSerial;
  const int64_t audioTargetUs = seekBackedPreviousDiscovery
                                    ? beforePtsUs
                                    : observed.lastPresentedPtsUs;
  if (!expectFrameStepTransition(audioEnabled, audioTargetUs,
                                 beforeSerial, observed.currentSerial,
                                 beforeAudioReset, label.c_str())) {
    return false;
  }
  const int64_t currentUs = player.currentUs();
  if (currentUs != observed.lastPresentedPtsUs) {
    std::cerr << "video_frame_step_smoke: " << label
              << " control position does not match the presented frame; "
                 "current_us="
              << currentUs
              << " presented_pts_us=" << observed.lastPresentedPtsUs << '\n';
    return false;
  }

  current->ptsUs = observed.lastPresentedPtsUs;
  current->serial = observed.currentSerial;
  current->frameCounter = player.videoFrameCounter();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: video_frame_step_smoke <video> [seek_us] "
                 "[step_count] [mode] [--audio]\n"
                 "Modes: startup, resume, forward-resume, mixed-resume, "
                 "rapid-resume, resume-during-step, burst-previous, "
                 "burst-forward, alternating, seek-alternating, "
                 "seek-step-overlap, "
                 "ended-replay, sequence, composition\n";
    return 2;
  }

  int64_t requestedSeekUs = kDefaultSeekUs;
  if (argc >= 3 && !parseInt64(argv[2], &requestedSeekUs)) {
    std::cerr << "video_frame_step_smoke: invalid seek_us: " << argv[2]
              << '\n';
    return 2;
  }
  if (requestedSeekUs < 1000000) {
    requestedSeekUs = 1000000;
  }
  size_t stepCount = 1;
  if (argc >= 4 && !parsePositiveSize(argv[3], &stepCount)) {
    std::cerr << "video_frame_step_smoke: invalid step_count: " << argv[3]
              << '\n';
    return 2;
  }
  bool resumeAfterPrevious = false;
  bool resumeAfterForward = false;
  bool resumeAfterMixed = false;
  bool rapidResume = false;
  bool resumeDuringStep = false;
  bool burstSteps = false;
  bool alternatingSteps = false;
  bool seekAlternatingSteps = false;
  bool seekStepOverlap = false;
  playback_video_frame_step::Direction burstDirection =
      playback_video_frame_step::Direction::Previous;
  bool replayAfterEnd = false;
  bool verifyStartup = false;
  bool verifySequence = false;
  bool verifyComposition = false;
  if (argc >= 5) {
    const std::string mode = argv[4];
    verifyStartup = mode == "startup";
    verifySequence = mode == "sequence";
    verifyComposition = mode == "composition";
    resumeAfterPrevious = mode == "resume";
    resumeAfterForward = mode == "forward-resume";
    resumeAfterMixed = mode == "mixed-resume";
    rapidResume = mode == "rapid-resume";
    resumeDuringStep = mode == "resume-during-step";
    burstSteps = mode == "burst-previous" || mode == "burst-forward";
    alternatingSteps = mode == "alternating";
    seekAlternatingSteps = mode == "seek-alternating";
    seekStepOverlap = mode == "seek-step-overlap";
    if (mode == "burst-forward") {
      burstDirection = playback_video_frame_step::Direction::Next;
    }
    replayAfterEnd = mode == "ended-replay";
    if (!verifyStartup && !verifySequence && !verifyComposition &&
        !resumeAfterPrevious &&
        !resumeAfterForward &&
        !resumeAfterMixed && !rapidResume && !resumeDuringStep && !burstSteps &&
        !alternatingSteps && !seekAlternatingSteps && !seekStepOverlap &&
        !replayAfterEnd) {
      std::cerr << "video_frame_step_smoke: invalid mode: " << argv[4]
                << " (expected 'startup', 'resume', 'forward-resume', "
                << "'mixed-resume', 'rapid-resume', or "
                << "'resume-during-step', 'burst-previous', "
                << "'burst-forward', 'alternating', 'seek-alternating', "
                << "'seek-step-overlap', 'ended-replay', 'sequence', or "
                   "'composition')\n";
      return 2;
    }
  }
  bool audioEnabled = false;
  if (argc >= 6) {
    if (std::string(argv[5]) != "--audio") {
      std::cerr << "video_frame_step_smoke: invalid option: " << argv[5]
                << " (expected '--audio')\n";
      return 2;
    }
    audioEnabled = true;
  }
  if (argc > 6) {
    std::cerr << "video_frame_step_smoke: too many arguments\n";
    return 2;
  }

  AudioPlaybackConfig audioConfig;
  audioConfig.enableAudio = audioEnabled;
  audioConfig.enableRadio = false;
  audioConfig.dry = true;
  AudioPlaybackRuntime audioRuntime(audioConfig);
  if (audioEnabled) {
    audioRuntime.adjustVolume(-audioRuntime.snapshot().volume);
  }
  if (audioEnabled && audioRuntime.snapshot().volume != 0.0f) {
    std::cerr << "video_frame_step_smoke: failed to mute audio output\n";
    return 1;
  }
  GpuRuntime gpu;
  Player player(audioRuntime, gpu);
  PlayerConfig config;
  config.file = std::filesystem::path(argv[1]);
  config.logPath =
      std::filesystem::current_path() / "video_frame_step_smoke.timing.log";
  config.enableAudio = audioEnabled;

  std::string error;
  if (!player.open(config, &error)) {
    std::cerr << "video_frame_step_smoke: open failed: " << error << '\n';
    return 1;
  }

  if (!waitFor(player, kDefaultTimeoutMs, "init", [](const PlayerDebugInfo&) {
        return true;
      })) {
    player.close();
    return 1;
  }

  if (!waitFor(player, kDefaultTimeoutMs, "init_done",
               [&](const PlayerDebugInfo&) { return player.initDone(); })) {
    player.close();
    return 1;
  }
  if (!player.initOk()) {
    std::cerr << "video_frame_step_smoke: init failed: "
              << player.initError() << '\n';
    player.close();
    return 1;
  }

  if (verifyStartup) {
    PlayerDebugInfo startup{};
    if (!waitFor(player, kDefaultTimeoutMs, "startup_presented",
                 [&](const PlayerDebugInfo& info) {
                   return info.hasVideoFrame &&
                          player.videoFrameCounter() > 0;
                 },
                 &startup)) {
      player.close();
      return 1;
    }
    if (startup.lastPresentedPtsUs != 0) {
      std::cerr << "video_frame_step_smoke: startup skipped timeline origin; "
                   "first_pts_us="
                << startup.lastPresentedPtsUs << '\n';
      player.close();
      return 1;
    }
    std::cout << "video_frame_step_smoke: PASS startup_pts_us=0\n";
    player.close();
    return 0;
  }

  if (verifyComposition) {
    const int64_t sourceDurationUs = player.sourceDurationUs();
    if (sourceDurationUs < 3'500'000) {
      std::cerr << "video_frame_step_smoke: composition mode requires at "
                   "least 3.5 seconds of media; source_duration_us="
                << sourceDurationUs << '\n';
      player.close();
      return 1;
    }
    if (!waitFor(player, kDefaultTimeoutMs, "composition_startup",
                 [&](const PlayerDebugInfo& info) {
                   return info.hasVideoFrame &&
                          player.videoFrameCounter() > 0;
                 })) {
      player.close();
      return 1;
    }
    player.setVideoPaused(true);
    if (!waitFor(player, kDefaultTimeoutMs, "composition_paused",
                 [](const PlayerDebugInfo& info) {
                   return info.state == PlayerState::Paused;
                 })) {
      player.close();
      return 1;
    }

    const std::vector<playback_video_sequence::SourceRange> ranges{
        {500'000, 1'500'000}, {2'500'000, 3'500'000}};
    const auto smooth =
        playback_video_sequence::Transition::motionSmooth();
    const int64_t frameDurationUs = std::max<int64_t>(
        1, player.timelineSnapshot().nominalFrameDurationUs);
    const int64_t transitionStartUs =
        1'000'000 - smooth.outgoingFrames * frameDurationUs;
    const int64_t targetUs = transitionStartUs + frameDurationUs / 2;
    const int beforeSerial = player.debugInfo().currentSerial;
    const uint64_t beforeCompositionCounter = player.videoFrameCounter();
    const AudioStreamReset beforeAudioReset =
        audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};
    if (player.motionCompositionSupport() !=
        MotionCompositionSupport::Available) {
      std::cerr << "video_frame_step_smoke: composition preview is not "
                   "supported for this source\n";
      player.close();
      return 1;
    }
    const int64_t shortClipUs = std::max<int64_t>(1, frameDurationUs * 2);
    if (player.setPlaybackComposition(
            {{500'000, 500'000 + shortClipUs}, {2'500'000, 3'500'000}},
            {smooth}, 0) ||
        player.debugInfo().currentSerial != beforeSerial) {
      std::cerr << "video_frame_step_smoke: an unrenderable composition was "
                   "accepted\n";
      player.close();
      return 1;
    }
    if (!player.setPlaybackComposition(ranges, {smooth}, targetUs)) {
      std::cerr << "video_frame_step_smoke: composition was rejected\n";
      player.close();
      return 1;
    }
    PlayerDebugInfo projected{};
    if (!waitFor(
            player, kDefaultTimeoutMs, "composition_projected",
            [&](const PlayerDebugInfo& info) {
              const PlayerTimelineSnapshot timeline =
                  player.timelineSnapshot();
              return info.hasVideoFrame && !timeline.seekPending() &&
                     info.currentSerial == beforeSerial + 1 &&
                     std::llabs(timeline.positionUs - targetUs) <=
                         std::max<int64_t>(frameDurationUs, 50'000);
            },
            &projected)) {
      player.close();
      return 1;
    }
    const int projectedSerial = projected.currentSerial;
    const int64_t projectedPositionUs = player.timelineSnapshot().positionUs;
    const AudioStreamReset projectedAudioReset =
        audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};

    VideoFrame composed;
    const auto previewDeadline = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(kDefaultTimeoutMs);
    uint64_t counter = player.videoFrameCounter();
    while (std::chrono::steady_clock::now() < previewDeadline) {
      if (player.videoFrameCounter() >= beforeCompositionCounter + 2 &&
          player.copyCurrentVideoFrame(&composed) &&
          (composed.format == VideoPixelFormat::NV12 ||
           composed.format == VideoPixelFormat::P010) &&
          !composed.yuv.empty()) {
        break;
      }
      player.waitForVideoFrame(counter, 100);
      counter = player.videoFrameCounter();
    }
    if ((composed.format != VideoPixelFormat::NV12 &&
         composed.format != VideoPixelFormat::P010) ||
        composed.yuv.empty()) {
      std::cerr << "video_frame_step_smoke: transition render cache did not "
                   "publish a composited frame\n";
      player.close();
      return 1;
    }
    const PlayerTimelineSnapshot afterRender = player.timelineSnapshot();
    if (afterRender.serial != projectedSerial || afterRender.seekPending() ||
        std::llabs(afterRender.positionUs - projectedPositionUs) >
            std::max<int64_t>(frameDurationUs, 50'000)) {
      std::cerr << "video_frame_step_smoke: cache completion mutated the "
                   "transport; before_serial="
                << projectedSerial << " after_serial=" << afterRender.serial
                << " before_position_us=" << projectedPositionUs
                << " after_position_us=" << afterRender.positionUs << '\n';
      player.close();
      return 1;
    }
    if (audioEnabled) {
      const AudioStreamReset afterRenderAudioReset =
          audioStreamLastAppliedReset();
      if (afterRenderAudioReset.generation !=
              projectedAudioReset.generation ||
          projectedAudioReset.generation <= beforeAudioReset.generation) {
        std::cerr << "video_frame_step_smoke: cache completion changed the "
                     "audio reset contract\n";
        player.close();
        return 1;
      }
    }

    const int64_t compositionSeekTargetUs = targetUs + frameDurationUs;
    const uint64_t beforeCompositionSeekCounter =
        player.videoFrameCounter();
    if (!player.requestSeek(compositionSeekTargetUs)) {
      std::cerr << "video_frame_step_smoke: composition seek was rejected\n";
      player.close();
      return 1;
    }
    PlayerDebugInfo afterCompositionSeek{};
    if (!waitFor(
            player, kDefaultTimeoutMs, "composition_seek",
            [&](const PlayerDebugInfo& info) {
              const PlayerTimelineSnapshot timeline =
                  player.timelineSnapshot();
              return info.currentSerial == projectedSerial + 1 &&
                     player.videoFrameCounter() >
                         beforeCompositionSeekCounter &&
                     !timeline.seekPending() &&
                     std::llabs(timeline.positionUs -
                                compositionSeekTargetUs) <=
                         std::max<int64_t>(frameDurationUs, 50'000);
            },
            &afterCompositionSeek)) {
      player.close();
      return 1;
    }
    VideoFrame composedAfterSeek;
    if (!player.copyCurrentVideoFrame(&composedAfterSeek) ||
        (composedAfterSeek.format != VideoPixelFormat::NV12 &&
         composedAfterSeek.format != VideoPixelFormat::P010) ||
        composedAfterSeek.yuv.empty()) {
      std::cerr << "video_frame_step_smoke: transport seek detached the "
                   "immutable composition cache\n";
      player.close();
      return 1;
    }
    const int activeCompositionSerial = afterCompositionSeek.currentSerial;
    const int64_t activeCompositionPositionUs =
        player.timelineSnapshot().positionUs;

    ObservedFrameStep compositionStep{
        activeCompositionPositionUs, activeCompositionSerial,
        player.videoFrameCounter()};
    for (size_t step = 0; step < 3; ++step) {
      if (!requestAndObserveFrameStep(
              player, audioEnabled,
              playback_video_frame_step::Direction::Next,
              "composition_next_" + std::to_string(step + 1), nullptr,
              &compositionStep, true)) {
        player.close();
        return 1;
      }
      VideoFrame stepped;
      const bool insideTransition = step < smooth.incomingFrames;
      const bool copied = player.copyCurrentVideoFrame(&stepped);
      const bool composedStep =
          copied && (stepped.format == VideoPixelFormat::NV12 ||
                     stepped.format == VideoPixelFormat::P010);
      if ((insideTransition && !composedStep) ||
          (!insideTransition &&
           (!copied || stepped.format != VideoPixelFormat::HWTexture))) {
        std::cerr << "video_frame_step_smoke: forward frame-step selected "
                     "the wrong side of the half-open transition window\n";
        player.close();
        return 1;
      }
    }
    for (size_t step = 0; step < 3; ++step) {
      if (!requestAndObserveFrameStep(
              player, audioEnabled,
              playback_video_frame_step::Direction::Previous,
              "composition_previous_" + std::to_string(step + 1), nullptr,
              &compositionStep, true)) {
        player.close();
        return 1;
      }
      VideoFrame stepped;
      if (!player.copyCurrentVideoFrame(&stepped) ||
          (stepped.format != VideoPixelFormat::NV12 &&
           stepped.format != VideoPixelFormat::P010)) {
        std::cerr << "video_frame_step_smoke: backward frame-step bypassed "
                     "the transition composition cache\n";
        player.close();
        return 1;
      }
    }
    if (compositionStep.serial != activeCompositionSerial ||
        std::llabs(compositionStep.ptsUs - activeCompositionPositionUs) >
            std::max<int64_t>(frameDurationUs, 50'000)) {
      std::cerr << "video_frame_step_smoke: bidirectional transition "
                   "frame-step changed serial or failed to return\n";
      player.close();
      return 1;
    }

    if (!player.updatePlaybackComposition(
            ranges, {playback_video_sequence::Transition::hard()})) {
      std::cerr << "video_frame_step_smoke: hard-cut composition update was "
                   "rejected\n";
      player.close();
      return 1;
    }
    VideoFrame hardCut;
    const auto hardCutDeadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < hardCutDeadline) {
      if (player.copyCurrentVideoFrame(&hardCut) &&
          hardCut.format == VideoPixelFormat::HWTexture) {
        break;
      }
      player.waitForVideoFrame(player.videoFrameCounter(), 50);
    }
    const PlayerTimelineSnapshot afterHardCut = player.timelineSnapshot();
    if (hardCut.format != VideoPixelFormat::HWTexture ||
        afterHardCut.serial != activeCompositionSerial ||
        afterHardCut.seekPending()) {
      std::cerr << "video_frame_step_smoke: transition-only update did not "
                   "restore the base frame without a serial transition\n";
      player.close();
      return 1;
    }
    std::cout << "video_frame_step_smoke: PASS composition format="
              << (composed.format == VideoPixelFormat::P010 ? "P010"
                                                             : "NV12")
              << " frames=" << static_cast<int>(smooth.durationFrames())
              << " serial=" << activeCompositionSerial << '\n';
    player.close();
    return 0;
  }

  if (verifySequence) {
    const int64_t sourceDurationUs = player.sourceDurationUs();
    if (sourceDurationUs < 5'000'000) {
      std::cerr << "video_frame_step_smoke: sequence mode requires at least "
                   "five seconds of media; source_duration_us="
                << sourceDurationUs << '\n';
      return 1;
    }
    const std::vector<playback_video_sequence::SourceRange> ranges{
        {1'000'000, 2'000'000}, {4'000'000, 5'000'000}};
    constexpr int64_t kSequenceStartUs = 250'000;
    if (!waitFor(player, kDefaultTimeoutMs, "sequence_startup",
                 [&](const PlayerDebugInfo& info) {
                   return info.hasVideoFrame &&
                          player.videoFrameCounter() > 0 &&
                          info.state == PlayerState::Playing;
                 }, nullptr,
                 "playing state with at least one presented source frame")) {
      return 1;
    }
    player.setVideoPaused(true);
    if (!waitFor(player, kDefaultTimeoutMs, "sequence_paused",
                 [](const PlayerDebugInfo& info) {
                   return info.state == PlayerState::Paused;
                 }, nullptr, "paused state before changing the sequence")) {
      return 1;
    }
    const uint64_t beforeSequenceCounter = player.videoFrameCounter();
    const int beforeSequenceSerial = player.debugInfo().currentSerial;
    const AudioStreamReset beforeSequenceAudioReset =
        audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};
    if (!player.setPlaybackSequence(ranges, kSequenceStartUs)) {
      std::cerr << "video_frame_step_smoke: player rejected the immutable "
                   "sequence; source_duration_us="
                << sourceDurationUs << " range_count=" << ranges.size()
                << '\n';
      return 1;
    }
    PlayerDebugInfo firstClip{};
    if (!waitFor(
            player, kDefaultTimeoutMs, "sequence_first_clip",
            [&](const PlayerDebugInfo& info) {
              const PlayerTimelineSnapshot timeline =
                  player.timelineSnapshot();
              return player.durationUs() == 2'000'000 &&
                     player.videoFrameCounter() > beforeSequenceCounter &&
                     !timeline.seekPending() &&
                     timeline.sourcePositionUs >= 1'000'000 &&
                     timeline.sourcePositionUs < 2'000'000 &&
                     info.hasVideoFrame;
            },
            &firstClip,
            "a two-second edited timeline presenting source [1s,2s) with no "
            "seek pending")) {
      return 1;
    }
    if (!expectSerialTransition(
            audioEnabled, kSequenceStartUs, beforeSequenceSerial,
            firstClip.currentSerial, beforeSequenceAudioReset,
            "sequence_apply")) {
      return 1;
    }
    PlayerTimelineSnapshot timeline = player.timelineSnapshot();
    const int64_t initialMappingErrorUs =
        std::llabs(timeline.sourcePositionUs -
                   (1'000'000 + timeline.positionUs));
    if (std::llabs(timeline.positionUs - kSequenceStartUs) >
            std::max<int64_t>(firstClip.lastPresentedDurationUs, 50'000) ||
        initialMappingErrorUs >
            std::max<int64_t>(firstClip.lastPresentedDurationUs, 50'000)) {
      std::cerr << "video_frame_step_smoke: explicit sequence start "
                   "diverged requested_us="
                << kSequenceStartUs
                << " presentation_us=" << timeline.positionUs
                << " source_us=" << timeline.sourcePositionUs
                << " mapping_error_us=" << initialMappingErrorUs << '\n';
      return 1;
    }
    const AudioStreamReset sequenceAudioReset =
        audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};
    constexpr size_t kStepsAcrossCut = 40;
    for (size_t step = 0; step < kStepsAcrossCut; ++step) {
      if (!player.requestFrameStep(
              playback_video_frame_step::Direction::Next)) {
        const PlayerTimelineSnapshot timeline = player.timelineSnapshot();
        std::cerr << "video_frame_step_smoke: forward sequence frame-step "
                     "rejected at request="
                  << (step + 1) << " presentation_us=" << timeline.positionUs
                  << " source_us=" << timeline.sourcePositionUs << '\n';
        return 1;
      }
    }

    PlayerDebugInfo afterCut{};
    if (!waitFor(
            player, kDefaultTimeoutMs, "sequence_second_clip",
            [&](const PlayerDebugInfo& info) {
              const PlayerTimelineSnapshot timeline =
                  player.timelineSnapshot();
              return player.durationUs() == 2'000'000 &&
                     player.videoFrameCounter() >=
                         beforeSequenceCounter + 1 + kStepsAcrossCut &&
                     !timeline.seekPending() &&
                     timeline.positionUs >= 1'000'000 &&
                     timeline.sourcePositionUs >= 4'000'000 &&
                     info.hasVideoFrame;
            },
            &afterCut,
            "40 accepted next-frame steps presenting source [4s,5s) on the "
            "same edited timeline")) {
      return 1;
    }
    timeline = player.timelineSnapshot();
    const int64_t mappedSourceUs =
        4'000'000 + timeline.positionUs - 1'000'000;
    const int64_t mappingErrorUs =
        std::llabs(timeline.sourcePositionUs - mappedSourceUs);
    if (mappingErrorUs >
        std::max<int64_t>(afterCut.lastPresentedDurationUs, 50'000)) {
      std::cerr << "video_frame_step_smoke: sequence source/presentation "
                   "mapping diverged presentation_us="
                << timeline.positionUs
                << " source_us=" << timeline.sourcePositionUs
                << " error_us=" << mappingErrorUs << '\n';
      return 1;
    }
    if (audioEnabled) {
      const AudioStreamReset afterCutReset = audioStreamLastAppliedReset();
      if (afterCutReset.generation != sequenceAudioReset.generation ||
          afterCutReset.serial != sequenceAudioReset.serial) {
        std::cerr << "video_frame_step_smoke: clip boundary incorrectly "
                     "reset the audio timeline; before_generation="
                  << sequenceAudioReset.generation
                  << " after_generation=" << afterCutReset.generation
                  << " before_serial=" << sequenceAudioReset.serial
                  << " after_serial=" << afterCutReset.serial << '\n';
        return 1;
      }
    }

    constexpr size_t kStepsBackAcrossCut = 20;
    ObservedFrameStep reverse{timeline.positionUs, afterCut.currentSerial,
                              player.videoFrameCounter()};
    for (size_t step = 0; step < kStepsBackAcrossCut; ++step) {
      const std::string label =
          "sequence_reverse_" + std::to_string(step + 1);
      if (!requestAndObserveFrameStep(
              player, audioEnabled,
              playback_video_frame_step::Direction::Previous, label,
              nullptr, &reverse, true)) {
        return 1;
      }
    }
    const PlayerDebugInfo beforeCut = player.debugInfo();
    timeline = player.timelineSnapshot();
    if (timeline.positionUs >= 1'000'000 ||
        timeline.sourcePositionUs >= 2'000'000) {
      std::cerr << "video_frame_step_smoke: reverse sequence steps did not "
                   "cross the edit; presentation_us="
                << timeline.positionUs
                << " source_us=" << timeline.sourcePositionUs
                << " step_count=" << kStepsBackAcrossCut << '\n';
      return 1;
    }
    const int64_t reverseMappingErrorUs =
        std::llabs(timeline.sourcePositionUs -
                   (1'000'000 + timeline.positionUs));
    if (reverseMappingErrorUs >
        std::max<int64_t>(beforeCut.lastPresentedDurationUs, 50'000)) {
      std::cerr << "video_frame_step_smoke: reverse sequence mapping "
                   "diverged presentation_us="
                << timeline.positionUs
                << " source_us=" << timeline.sourcePositionUs
                << " error_us=" << reverseMappingErrorUs << '\n';
      return 1;
    }

    const AudioStreamReset beforeResumeAudioReset =
        audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};
    const int resumeSerial = reverse.serial;
    const int64_t resumeTargetUs = reverse.ptsUs;
    const uint64_t beforeResumeCounter = reverse.frameCounter;
    player.setVideoPaused(false);
    PlayerDebugInfo resumed{};
    if (!waitFor(
            player, kDefaultTimeoutMs, "sequence_resume_across_cut",
            [&](const PlayerDebugInfo& info) {
              const PlayerTimelineSnapshot current =
                  player.timelineSnapshot();
              const bool resumedOrCompleted =
                  info.state == PlayerState::Playing ||
                  info.state == PlayerState::Draining ||
                  info.state == PlayerState::Ended;
              return resumedOrCompleted &&
                     player.videoFrameCounter() > beforeResumeCounter &&
                     !current.seekPending() &&
                     current.positionUs >= 1'000'000 &&
                     current.sourcePositionUs >= 4'000'000 &&
                     info.hasVideoFrame;
            },
            &resumed,
            "resumed or completed playback crossing into source [4s,5s) "
            "without a pending seek")) {
      return 1;
    }
    if (!expectSerialTransition(
            audioEnabled, resumeTargetUs, resumeSerial,
            resumed.currentSerial, beforeResumeAudioReset,
            "sequence_resume_across_cut")) {
      return 1;
    }
    PlayerDebugInfo completed{};
    if (!waitFor(
            player, kDefaultTimeoutMs, "sequence_completed",
            [&](const PlayerDebugInfo& info) {
              const PlayerTimelineSnapshot current =
                  player.timelineSnapshot();
              const int64_t endToleranceUs =
                  (std::max)(info.lastPresentedDurationUs, int64_t{50'000});
              return info.state == PlayerState::Ended &&
                     !current.seekPending() &&
                     current.positionUs >=
                         player.durationUs() - endToleranceUs;
            },
            &completed,
            "the edited timeline to drain video and muted audio and reach "
            "Ended")) {
      return 1;
    }
    timeline = player.timelineSnapshot();
    std::cout << "video_frame_step_smoke: PASS sequence presentation_us="
              << timeline.positionUs
              << " source_us=" << timeline.sourcePositionUs
              << " forward_steps=" << kStepsAcrossCut
              << " reverse_steps=" << kStepsBackAcrossCut << '\n';
    return 0;
  }

  const int64_t seekUs =
      replayAfterEnd && player.durationUs() > 500000
          ? player.durationUs() - 500000
          : chooseSeekUs(player, requestedSeekUs);
  std::cout << "video_frame_step_smoke: duration_us=" << player.durationUs()
            << " seek_us=" << seekUs << '\n';

  const uint64_t beforeSeekCounter = player.videoFrameCounter();
  player.requestSeek(seekUs);

  PlayerDebugInfo boundary{};
  if (!waitFor(player, kDefaultTimeoutMs, "seek_presented",
               [&](const PlayerDebugInfo& info) {
                 return info.hasVideoFrame && info.seekInFlightSerial == 0 &&
                         info.pendingSeekSerial == 0 &&
                         !player.seekPending() &&
                        player.videoFrameCounter() > beforeSeekCounter &&
                        info.lastPresentedPtsUs > 0;
               },
               &boundary)) {
    player.close();
    return 1;
  }

  if (audioEnabled &&
      !waitFor(player, kDefaultTimeoutMs, "audio_clock_ready",
               [](const PlayerDebugInfo& info) {
                 return info.state == PlayerState::Playing && info.audioOk &&
                        info.audioClockReady && info.audioClockFresh &&
                        info.masterSource == PlayerClockSource::Audio;
               },
               &boundary)) {
    player.close();
    return 1;
  }
  player.setVideoPaused(true);
  if (!waitFor(player, kDefaultTimeoutMs, "paused",
               [&](const PlayerDebugInfo& info) {
                 return info.state == PlayerState::Paused &&
                        (!audioEnabled ||
                         (info.audioClockReady && info.audioClockFresh));
               },
               &boundary)) {
    player.close();
    return 1;
  }
  const int64_t boundaryPtsUs = boundary.lastPresentedPtsUs;
  if (replayAfterEnd) {
    player.setVideoPaused(false);
    if (!waitFor(player, kDefaultTimeoutMs, "ended",
                 [&](const PlayerDebugInfo& info) {
                   return info.state == PlayerState::Ended &&
                          info.seekInFlightSerial == 0 &&
                          !player.seekPending();
                 })) {
      player.close();
      return 1;
    }

    const uint64_t beforeReplayCounter = player.videoFrameCounter();
    player.requestSeek(0);
    player.setVideoPaused(false);
    PlayerDebugInfo replayed{};
    if (!waitFor(player, kDefaultTimeoutMs, "ended_replay",
                 [&](const PlayerDebugInfo& info) {
                   return info.state == PlayerState::Playing &&
                          info.seekInFlightSerial == 0 &&
                          info.pendingSeekSerial == 0 &&
                          !player.seekPending() &&
                          player.videoFrameCounter() > beforeReplayCounter &&
                          info.lastPresentedPtsUs < 500000;
                 },
                 &replayed)) {
      player.close();
      return 1;
    }
    std::cout << "video_frame_step_smoke: PASS ended_pts_us="
              << boundaryPtsUs
              << " replayed_pts_us=" << replayed.lastPresentedPtsUs << '\n';
    player.close();
    return 0;
  }

  if (seekStepOverlap) {
    ObservedFrameStep current{boundaryPtsUs, boundary.currentSerial,
                              player.videoFrameCounter()};
    for (size_t cycle = 0; cycle < stepCount; ++cycle) {
      const int64_t deltaUs = cycle % 2 == 0 ? 5000000 : -5000000;
      int64_t expectedSeekTargetUs =
          (std::max)(int64_t{0}, current.ptsUs + deltaUs * 2);
      if (player.durationUs() > 0) {
        expectedSeekTargetUs =
            (std::min)(expectedSeekTargetUs, player.durationUs());
      }
      const playback_video_frame_step::Direction direction =
          cycle % 2 == 0 ? playback_video_frame_step::Direction::Previous
                         : playback_video_frame_step::Direction::Next;
      const uint64_t beforeCounter = player.videoFrameCounter();
      const int beforeSerial = current.serial;
      const PlayerTimelineSnapshot beforeTimeline = player.timelineSnapshot();
      const AudioStreamReset beforeAudioReset =
          audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};

      // Submit two simultaneous relative seeks before the frame step. This is
      // stricter than keyboard input: publication order and event-queue order
      // must remain one atomic transport sequence even across calling threads.
      std::atomic<int> readySubmitters{0};
      std::atomic<bool> releaseSubmitters{false};
      bool seekAccepted[2] = {false, false};
      auto submitSeek = [&](size_t index) {
        readySubmitters.fetch_add(1, std::memory_order_release);
        while (!releaseSubmitters.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        seekAccepted[index] = player.requestRelativeSeek(deltaUs);
      };
      std::thread firstSubmitter(submitSeek, size_t{0});
      std::thread secondSubmitter(submitSeek, size_t{1});
      while (readySubmitters.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
      }
      releaseSubmitters.store(true, std::memory_order_release);
      firstSubmitter.join();
      secondSubmitter.join();
      if (!seekAccepted[0] || !seekAccepted[1]) {
        std::cerr << "video_frame_step_smoke: concurrent relative seek "
                     "request rejected at cycle "
                  << (cycle + 1) << '\n';
        player.close();
        return 1;
      }
      const PlayerTimelineSnapshot requestedTimeline =
          player.timelineSnapshot();
      const int64_t seekTargetUs = requestedTimeline.positionUs;
      if (!requestedTimeline.seekPending() ||
          seekTargetUs != expectedSeekTargetUs ||
          requestedTimeline.latestSeekRequestGeneration !=
              beforeTimeline.latestSeekRequestGeneration + 2) {
        std::cerr << "video_frame_step_smoke: Player did not publish the "
                     "queued seek as its authoritative timeline position; "
                     "cycle="
                  << (cycle + 1)
                  << " expected_target_us=" << expectedSeekTargetUs
                  << " target_us=" << seekTargetUs
                  << " current_us=" << requestedTimeline.positionUs
                  << " seek_pending="
                  << (requestedTimeline.seekPending() ? 1 : 0)
                  << " previous_generation="
                  << beforeTimeline.latestSeekRequestGeneration
                  << " request_generation="
                  << requestedTimeline.latestSeekRequestGeneration
                  << '\n';
        player.close();
        return 1;
      }
      if (!player.requestFrameStep(direction)) {
        std::cerr << "video_frame_step_smoke: overlap frame-step request "
                     "rejected at cycle "
                  << (cycle + 1) << '\n';
        player.close();
        return 1;
      }

      PlayerDebugInfo observed{};
      const std::string label =
          "seek_step_overlap_" + std::to_string(cycle + 1);
      if (!waitFor(player, kDefaultTimeoutMs, label.c_str(),
                   [&](const PlayerDebugInfo& info) {
                     return info.hasVideoFrame &&
                            player.videoFrameCounter() > beforeCounter &&
                            info.seekInFlightSerial == 0 &&
                            info.pendingSeekSerial == 0 &&
                            !player.seekPending() &&
                            info.state == PlayerState::FrameStep;
                   },
                   &observed)) {
        player.close();
        return 1;
      }

      constexpr int64_t kMaximumTargetErrorUs = 500000;
      const int64_t targetErrorUs =
          std::llabs(observed.lastPresentedPtsUs - seekTargetUs);
      const PlayerTimelineSnapshot completedTimeline =
          player.timelineSnapshot();
      if (targetErrorUs > kMaximumTargetErrorUs ||
          completedTimeline.positionUs != observed.lastPresentedPtsUs ||
          completedTimeline.seekPending()) {
        std::cerr << "video_frame_step_smoke: overlap left the requested "
                     "timeline; cycle="
                  << (cycle + 1) << " target_us=" << seekTargetUs
                  << " actual_pts_us=" << observed.lastPresentedPtsUs
                  << " target_error_us=" << targetErrorUs
                  << " current_us=" << completedTimeline.positionUs
                  << " seek_pending="
                  << (completedTimeline.seekPending() ? 1 : 0) << '\n';
        player.close();
        return 1;
      }
      if (!expectSeekStepOverlapTransition(
              audioEnabled, seekTargetUs, observed.lastPresentedPtsUs,
              beforeSerial, observed.currentSerial, beforeAudioReset,
              label.c_str())) {
        player.close();
        return 1;
      }

      current.ptsUs = observed.lastPresentedPtsUs;
      current.serial = observed.currentSerial;
      current.frameCounter = player.videoFrameCounter();
    }

    std::cout << "video_frame_step_smoke: PASS mode=seek-step-overlap"
              << " final_pts_us=" << current.ptsUs
              << " final_serial=" << current.serial
              << " cycles=" << stepCount << '\n';
    player.close();
    return 0;
  }

  if (alternatingSteps || seekAlternatingSteps) {
    ObservedFrameStep current{boundaryPtsUs, boundary.currentSerial,
                              player.videoFrameCounter()};
    constexpr size_t kDirectionCycles = 4;
    const size_t reverseCount = (std::max)(size_t{1}, stepCount / 2);

    auto runReversibleLeg = [&](playback_video_frame_step::Direction direction,
                                size_t cycle) {
      std::vector<int64_t> visitedPts;
      visitedPts.reserve(stepCount + 1);
      visitedPts.push_back(current.ptsUs);
      const char* directionName =
          direction == playback_video_frame_step::Direction::Next ? "next"
                                                                   : "previous";
      for (size_t i = 0; i < stepCount; ++i) {
        const std::string label =
            "alternating_" + std::to_string(cycle + 1) + "_" +
            directionName + "_" + std::to_string(i + 1);
        if (!requestAndObserveFrameStep(player, audioEnabled, direction, label,
                                        nullptr, &current)) {
          return false;
        }
        visitedPts.push_back(current.ptsUs);
      }

      const playback_video_frame_step::Direction inverse =
          direction == playback_video_frame_step::Direction::Next
              ? playback_video_frame_step::Direction::Previous
              : playback_video_frame_step::Direction::Next;
      for (size_t i = 0; i < reverseCount; ++i) {
        const int64_t expectedPtsUs =
            visitedPts[stepCount - i - 1];
        const std::string label =
            "alternating_" + std::to_string(cycle + 1) + "_" +
            directionName + "_inverse_" + std::to_string(i + 1);
        if (!requestAndObserveFrameStep(player, audioEnabled, inverse, label,
                                        &expectedPtsUs, &current)) {
          return false;
        }
      }
      for (size_t i = 0; i < reverseCount; ++i) {
        const int64_t expectedPtsUs =
            visitedPts[stepCount - reverseCount + i + 1];
        const std::string label =
            "alternating_" + std::to_string(cycle + 1) + "_" +
            directionName + "_return_" + std::to_string(i + 1);
        if (!requestAndObserveFrameStep(player, audioEnabled, direction, label,
                                        &expectedPtsUs, &current)) {
          return false;
        }
      }
      return true;
    };

    for (size_t cycle = 0; cycle < kDirectionCycles; ++cycle) {
      if (seekAlternatingSteps) {
        const int64_t seekDeltaUs = cycle % 2 == 0 ? 5000000 : -5000000;
        const int64_t controlPositionUs = player.currentUs();
        if (controlPositionUs != current.ptsUs) {
          std::cerr << "video_frame_step_smoke: alternating seek base does not "
                       "match the presented frame; current_us="
                    << controlPositionUs
                    << " presented_pts_us=" << current.ptsUs << '\n';
          player.close();
          return 1;
        }
        const int64_t seekTargetUs = chooseSeekUs(
            player,
            (std::max)(int64_t{1000000}, controlPositionUs + seekDeltaUs));
        const uint64_t beforeSeekCounter = current.frameCounter;
        const int beforeSeekSerial = current.serial;
        const AudioStreamReset beforeSeekAudioReset =
            audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};
        player.requestSeek(seekTargetUs);
        PlayerDebugInfo seeked{};
        const std::string label =
            "alternating_seek_" + std::to_string(cycle + 1);
        if (!waitFor(player, kDefaultTimeoutMs, label.c_str(),
                     [&](const PlayerDebugInfo& info) {
                       return info.hasVideoFrame &&
                              player.videoFrameCounter() > beforeSeekCounter &&
                              info.seekInFlightSerial == 0 &&
                              info.pendingSeekSerial == 0 &&
                              !player.seekPending() &&
                              info.currentSerial > current.serial;
                     },
                     &seeked)) {
          player.close();
          return 1;
        }
        if (!expectSerialTransition(
                audioEnabled, seekTargetUs, beforeSeekSerial,
                seeked.currentSerial, beforeSeekAudioReset, label.c_str())) {
          player.close();
          return 1;
        }
        current.ptsUs = seeked.lastPresentedPtsUs;
        current.serial = seeked.currentSerial;
        current.frameCounter = player.videoFrameCounter();
      }

      if (!runReversibleLeg(playback_video_frame_step::Direction::Next,
                            cycle) ||
          !runReversibleLeg(playback_video_frame_step::Direction::Previous,
                            cycle)) {
        player.close();
        return 1;
      }
    }

    std::cout << "video_frame_step_smoke: PASS mode="
              << (seekAlternatingSteps ? "seek-alternating" : "alternating")
              << " final_pts_us=" << current.ptsUs
              << " final_serial=" << current.serial
              << " steps_per_leg=" << stepCount
              << " cycles=" << kDirectionCycles << '\n';
    player.close();
    return 0;
  }

  if (rapidResume) {
    const uint64_t beforeRapidCounter = player.videoFrameCounter();
    for (size_t i = 0; i < stepCount; ++i) {
      if (!player.requestFrameStep(
              playback_video_frame_step::Direction::Previous)) {
        std::cerr
            << "video_frame_step_smoke: rapid previous-frame request rejected "
            << "at step " << (i + 1) << '\n';
        player.close();
        return 1;
      }
    }
    player.setVideoPaused(false);
    PlayerDebugInfo resumed{};
    if (!waitFor(player, kDefaultTimeoutMs, "rapid_resume",
                 [&](const PlayerDebugInfo& info) {
                   return info.hasVideoFrame &&
                          player.videoFrameCounter() > beforeRapidCounter &&
                          info.seekInFlightSerial == 0 &&
                          info.pendingSeekSerial == 0 &&
                          info.state == PlayerState::Playing;
                 },
                 &resumed)) {
      std::cerr << "video_frame_step_smoke: rapid resume did not return to "
                   "playing after "
                << stepCount << " queued previous-frame requests\n";
      player.close();
      return 1;
    }
    std::cout << "video_frame_step_smoke: PASS boundary_pts_us="
              << boundaryPtsUs << " resumed_pts_us="
              << resumed.lastPresentedPtsUs << " step_count=" << stepCount
              << '\n';
    player.close();
    return 0;
  }

  if (burstSteps) {
    const uint64_t beforeBurstCounter = player.videoFrameCounter();
    const auto burstStart = std::chrono::steady_clock::now();
    for (size_t i = 0; i < stepCount; ++i) {
      if (!player.requestFrameStep(burstDirection)) {
        std::cerr << "video_frame_step_smoke: burst frame-step request "
                     "rejected at step "
                  << (i + 1) << '\n';
        player.close();
        return 1;
      }
    }

    PlayerDebugInfo burst{};
    if (!waitFor(player, kDefaultTimeoutMs, "burst_steps",
                 [&](const PlayerDebugInfo& info) {
                   return info.hasVideoFrame &&
                          player.videoFrameCounter() >=
                              beforeBurstCounter + stepCount &&
                          info.seekInFlightSerial == 0 &&
                          info.pendingSeekSerial == 0 &&
                          info.state == PlayerState::FrameStep;
                 },
                 &burst)) {
      std::cerr << "video_frame_step_smoke: burst did not present all "
                << stepCount << " requested frames\n";
      player.close();
      return 1;
    }
    const auto burstElapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - burstStart)
            .count();
    const bool movedInDirection =
        burstDirection == playback_video_frame_step::Direction::Previous
            ? burst.lastPresentedPtsUs < boundaryPtsUs
            : burst.lastPresentedPtsUs > boundaryPtsUs;
    if (!movedInDirection || burst.currentSerial != boundary.currentSerial) {
      std::cerr << "video_frame_step_smoke: burst left the cached frame-step "
                   "timeline; boundary_pts_us="
                << boundaryPtsUs
                << " actual_pts_us=" << burst.lastPresentedPtsUs
                << " boundary_serial=" << boundary.currentSerial
                << " actual_serial=" << burst.currentSerial << '\n';
      player.close();
      return 1;
    }
    std::cout << "video_frame_step_smoke: PASS burst_direction="
              << (burstDirection ==
                          playback_video_frame_step::Direction::Previous
                      ? "previous"
                      : "forward")
              << " boundary_pts_us=" << boundaryPtsUs
              << " final_pts_us=" << burst.lastPresentedPtsUs
              << " step_count=" << stepCount
              << " elapsed_ms=" << burstElapsedMs << '\n';
    player.close();
    return 0;
  }

  if (resumeAfterForward) {
    int64_t lastPtsUs = boundaryPtsUs;
    int lastSerial = boundary.currentSerial;
    uint64_t lastCounter = player.videoFrameCounter();
    for (size_t i = 0; i < stepCount; ++i) {
      const int beforeStepSerial = lastSerial;
      const AudioStreamReset beforeStepAudioReset =
          audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};
      if (!player.requestFrameStep(playback_video_frame_step::Direction::Next)) {
        std::cerr << "video_frame_step_smoke: next-frame request rejected at "
                  << "step " << (i + 1) << '\n';
        player.close();
        return 1;
      }

      PlayerDebugInfo next{};
      std::string label = "forward_" + std::to_string(i + 1);
      if (!waitFor(player, kDefaultTimeoutMs, label.c_str(),
                   [&](const PlayerDebugInfo& info) {
                     return info.hasVideoFrame &&
                            player.videoFrameCounter() > lastCounter &&
                            info.seekInFlightSerial == 0 &&
                            info.pendingSeekSerial == 0 &&
                            info.lastPresentedPtsUs > lastPtsUs;
                   },
                   &next)) {
        std::cerr << "video_frame_step_smoke: next-frame step " << (i + 1)
                  << " did not move after pts_us=" << lastPtsUs << '\n';
        player.close();
        return 1;
      }
      lastPtsUs = next.lastPresentedPtsUs;
      lastCounter = player.videoFrameCounter();
      if (!expectFrameStepTransition(
              audioEnabled, next.lastPresentedPtsUs, beforeStepSerial,
              next.currentSerial, beforeStepAudioReset, label.c_str())) {
        player.close();
        return 1;
      }
      lastSerial = next.currentSerial;
    }

    const AudioStreamReset beforeResumeAudioReset =
        audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};
    player.setVideoPaused(false);
    PlayerDebugInfo resumed{};
    if (!waitFor(player, kDefaultTimeoutMs, "resume_after_forward",
                 [&](const PlayerDebugInfo& info) {
                   return info.hasVideoFrame &&
                          player.videoFrameCounter() > lastCounter &&
                          info.seekInFlightSerial == 0 &&
                          info.pendingSeekSerial == 0 &&
                          info.state == PlayerState::Playing &&
                          info.lastPresentedPtsUs > lastPtsUs;
                 },
                 &resumed)) {
      std::cerr << "video_frame_step_smoke: resume did not advance after "
                << stepCount << " next-frame steps from pts_us=" << lastPtsUs
                << '\n';
      player.close();
      return 1;
    }
    if (!expectSerialTransition(audioEnabled, lastPtsUs, lastSerial,
                                resumed.currentSerial, beforeResumeAudioReset,
                                "resume_after_forward")) {
      player.close();
      return 1;
    }
    std::cout << "video_frame_step_smoke: PASS boundary_pts_us="
              << boundaryPtsUs << " resumed_from_pts_us=" << lastPtsUs
              << " resumed_pts_us=" << resumed.lastPresentedPtsUs
              << " step_count=" << stepCount << '\n';
    player.close();
    return 0;
  }

  std::vector<int64_t> steppedPts;
  steppedPts.reserve(stepCount + 1);
  steppedPts.push_back(boundaryPtsUs);

  int64_t lastPtsUs = boundaryPtsUs;
  int lastSerial = boundary.currentSerial;
  uint64_t lastCounter = player.videoFrameCounter();
  for (size_t i = 0; i < stepCount; ++i) {
    const int beforeStepSerial = lastSerial;
    const AudioStreamReset beforeStepAudioReset =
        audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};
    if (!player.requestFrameStep(
            playback_video_frame_step::Direction::Previous)) {
      std::cerr << "video_frame_step_smoke: previous-frame request rejected at "
                << "step " << (i + 1) << '\n';
      player.close();
      return 1;
    }

    PlayerDebugInfo previous{};
    std::string label = "previous_" + std::to_string(i + 1);
    if (!waitFor(player, kDefaultTimeoutMs, label.c_str(),
                 [&](const PlayerDebugInfo& info) {
                   return info.hasVideoFrame &&
                          player.videoFrameCounter() > lastCounter &&
                          info.seekInFlightSerial == 0 &&
                          info.pendingSeekSerial == 0 &&
                          info.lastPresentedPtsUs > 0 &&
                          info.lastPresentedPtsUs < lastPtsUs;
                 },
                 &previous)) {
      std::cerr << "video_frame_step_smoke: previous-frame step " << (i + 1)
                << " did not move before pts_us=" << lastPtsUs << '\n';
      player.close();
      return 1;
    }
    lastPtsUs = previous.lastPresentedPtsUs;
    lastCounter = player.videoFrameCounter();
    if (!expectFrameStepTransition(
            audioEnabled, previous.lastPresentedPtsUs, beforeStepSerial,
            previous.currentSerial, beforeStepAudioReset, label.c_str())) {
      player.close();
      return 1;
    }
    lastSerial = previous.currentSerial;
    steppedPts.push_back(lastPtsUs);
  }

  if (resumeDuringStep) {
    const AudioStreamReset beforeResumeAudioReset =
        audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};
    if (!player.requestFrameStep(
            playback_video_frame_step::Direction::Previous)) {
      std::cerr << "video_frame_step_smoke: resume-during-step previous-frame "
                   "request rejected\n";
      player.close();
      return 1;
    }
    player.setVideoPaused(false);
    PlayerDebugInfo resumed{};
    if (!waitFor(player, kDefaultTimeoutMs, "resume_during_step",
                 [&](const PlayerDebugInfo& info) {
                   return info.hasVideoFrame &&
                          player.videoFrameCounter() > lastCounter &&
                          info.seekInFlightSerial == 0 &&
                          info.pendingSeekSerial == 0 &&
                          info.state == PlayerState::Playing;
                 },
                 &resumed)) {
      std::cerr << "video_frame_step_smoke: resume did not recover while a "
                   "frame-step request was pending after pts_us="
                << lastPtsUs << '\n';
      player.close();
      return 1;
    }
    if (!expectConcurrentResumeTransition(
            audioEnabled, lastPtsUs, resumed.currentSerial,
            beforeResumeAudioReset, "resume_during_step")) {
      player.close();
      return 1;
    }
    std::cout << "video_frame_step_smoke: PASS boundary_pts_us="
              << boundaryPtsUs << " resumed_from_pts_us=" << lastPtsUs
              << " resumed_pts_us=" << resumed.lastPresentedPtsUs
              << " step_count=" << stepCount << '\n';
    player.close();
    return 0;
  }

  if (resumeAfterPrevious) {
    const AudioStreamReset beforeResumeAudioReset =
        audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};
    player.setVideoPaused(false);
    PlayerDebugInfo resumed{};
    if (!waitFor(player, kDefaultTimeoutMs, "resume_after_previous",
                 [&](const PlayerDebugInfo& info) {
                   return info.hasVideoFrame &&
                          player.videoFrameCounter() > lastCounter &&
                          info.seekInFlightSerial == 0 &&
                          info.pendingSeekSerial == 0 &&
                          info.lastPresentedPtsUs > lastPtsUs;
                 },
                 &resumed)) {
      std::cerr << "video_frame_step_smoke: resume did not advance after "
                << stepCount << " previous-frame steps from pts_us="
                << lastPtsUs << '\n';
      player.close();
      return 1;
    }
    if (!expectSerialTransition(audioEnabled, lastPtsUs, lastSerial,
                                resumed.currentSerial, beforeResumeAudioReset,
                                "resume_after_previous")) {
      player.close();
      return 1;
    }
    std::cout << "video_frame_step_smoke: PASS boundary_pts_us="
              << boundaryPtsUs << " resumed_from_pts_us=" << lastPtsUs
              << " resumed_pts_us=" << resumed.lastPresentedPtsUs
              << " step_count=" << stepCount << '\n';
    player.close();
    return 0;
  }

  for (size_t i = 0; i < stepCount; ++i) {
    const size_t targetIndex = stepCount - i - 1;
    const int64_t expectedPtsUs = steppedPts[targetIndex];
    const int beforeStepSerial = lastSerial;
    const AudioStreamReset beforeStepAudioReset =
        audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};
    if (!player.requestFrameStep(playback_video_frame_step::Direction::Next)) {
      std::cerr << "video_frame_step_smoke: next-frame request rejected at step "
                << (i + 1) << '\n';
      player.close();
      return 1;
    }

    PlayerDebugInfo next{};
    std::string label = "next_" + std::to_string(i + 1);
    if (!waitFor(player, kDefaultTimeoutMs, label.c_str(),
                 [&](const PlayerDebugInfo& info) {
                   return info.hasVideoFrame &&
                          player.videoFrameCounter() > lastCounter &&
                          info.seekInFlightSerial == 0 &&
                          info.pendingSeekSerial == 0 &&
                          info.lastPresentedPtsUs == expectedPtsUs;
                 },
                 &next)) {
      std::cerr << "video_frame_step_smoke: next-frame step " << (i + 1)
                << " did not return to expected pts_us=" << expectedPtsUs
                << '\n';
      player.close();
      return 1;
    }
    lastPtsUs = next.lastPresentedPtsUs;
    lastCounter = player.videoFrameCounter();
    if (!expectFrameStepTransition(audioEnabled, next.lastPresentedPtsUs,
                                   beforeStepSerial, next.currentSerial,
                                   beforeStepAudioReset, label.c_str())) {
      player.close();
      return 1;
    }
    lastSerial = next.currentSerial;
  }

  if (resumeAfterMixed) {
    const AudioStreamReset beforeResumeAudioReset =
        audioEnabled ? audioStreamLastAppliedReset() : AudioStreamReset{};
    player.setVideoPaused(false);
    PlayerDebugInfo resumed{};
    if (!waitFor(player, kDefaultTimeoutMs, "resume_after_mixed",
                 [&](const PlayerDebugInfo& info) {
                   return info.hasVideoFrame &&
                          player.videoFrameCounter() > lastCounter &&
                          info.seekInFlightSerial == 0 &&
                          info.pendingSeekSerial == 0 &&
                          info.state == PlayerState::Playing &&
                          info.lastPresentedPtsUs > lastPtsUs;
                 },
                 &resumed)) {
      std::cerr << "video_frame_step_smoke: resume did not advance after "
                << stepCount << " previous-frame and next-frame steps from "
                << "pts_us=" << lastPtsUs << '\n';
      player.close();
      return 1;
    }
    if (!expectSerialTransition(audioEnabled, lastPtsUs, lastSerial,
                                resumed.currentSerial, beforeResumeAudioReset,
                                "resume_after_mixed")) {
      player.close();
      return 1;
    }
    std::cout << "video_frame_step_smoke: PASS boundary_pts_us="
              << boundaryPtsUs << " resumed_from_pts_us=" << lastPtsUs
              << " resumed_pts_us=" << resumed.lastPresentedPtsUs
              << " step_count=" << stepCount << '\n';
    player.close();
    return 0;
  }

  std::cout << "video_frame_step_smoke: PASS boundary_pts_us=" << boundaryPtsUs
            << " earliest_previous_pts_us=" << steppedPts.back()
            << " step_count=" << stepCount << '\n';
  player.close();
  return 0;
}
