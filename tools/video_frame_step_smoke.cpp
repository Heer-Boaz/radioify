#include "playback/video/player.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audioplayback.h"
#include "playback/video/gpu/gpu_shared.h"

namespace {

std::atomic<int64_t> gLastAudioDiscardUntilUs{0};
std::atomic<int> gLastAudioFlushSerial{0};
std::atomic<uint64_t> gAudioFlushCount{0};

}  // namespace

#if defined(_WIN32)
#include <cwchar>
#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>
#endif

int64_t nowUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(
             steady_clock::now().time_since_epoch())
      .count();
}

std::string toUtf8String(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::wstring wide = path.wstring();
  if (wide.empty()) {
    return {};
  }
  int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                 static_cast<int>(wide.size()), nullptr, 0,
                                 nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                      static_cast<int>(wide.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
#else
  return path.string();
#endif
}

std::recursive_mutex& getSharedGpuMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

ID3D11Device* getSharedGpuDevice() {
#if defined(_WIN32)
  static Microsoft::WRL::ComPtr<ID3D11Device> device;
  static std::once_flag once;
  std::call_once(once, [] {
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selectedLevel{};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device,
        &selectedLevel, &context);
    if (FAILED(hr)) {
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                        static_cast<UINT>(std::size(levels)),
                        D3D11_SDK_VERSION, &device, &selectedLevel, &context);
    }
  });
  return device.Get();
#else
  return nullptr;
#endif
}

std::FILE* openFileUtf8(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
  std::wstring wideMode;
  for (const char* p = mode; p && *p; ++p) {
    wideMode.push_back(static_cast<wchar_t>(*p));
  }
  std::FILE* file = nullptr;
  if (_wfopen_s(&file, path.wstring().c_str(), wideMode.c_str()) != 0) {
    return nullptr;
  }
  return file;
#else
  return std::fopen(path.string().c_str(), mode);
#endif
}

bool audioStartStream(uint64_t) { return false; }
void audioStopStream() {}
size_t audioStreamBufferedFrames() { return 0; }
int64_t audioStreamOldestPtsUs() { return 0; }
bool audioStreamWriteSamples(const float*, uint64_t, int64_t, int, bool,
                             uint64_t*) {
  return false;
}
void audioStreamPrimeClock(int, int64_t) {}
void audioStreamSetEnd(bool) {}
void audioStreamFlushSerial(int serial, int64_t discardUntilUs) {
  int64_t ptsUs = (std::max)(int64_t{0}, discardUntilUs);
  gLastAudioDiscardUntilUs.store(ptsUs, std::memory_order_relaxed);
  gLastAudioFlushSerial.store(serial, std::memory_order_relaxed);
  gAudioFlushCount.fetch_add(1, std::memory_order_relaxed);
}
int audioStreamSerial() { return 0; }
int64_t audioStreamClockUs(int64_t) { return 0; }
int64_t audioStreamClockLastUpdatedUs() { return 0; }
bool audioStreamStarved() { return false; }
bool audioStreamClockReady() { return false; }
uint64_t audioStreamWaitForUpdate(uint64_t lastCounter, int) {
  return lastCounter;
}
uint64_t audioStreamUpdateCounter() { return 0; }
bool audioIsPaused() { return true; }
bool audioIsFinished() { return true; }
void audioPlay() {}
void audioPause() {}
void audioSetHold(bool) {}
AudioPerfStats audioGetPerfStats() { return {}; }

namespace {

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
  std::cout << label << " state=" << stateName(info.state)
            << " serial=" << info.currentSerial
            << " pending_seek=" << info.pendingSeekSerial
            << " inflight_seek=" << info.seekInFlightSerial
            << " frame_counter=" << player.videoFrameCounter()
            << " pts_us=" << info.lastPresentedPtsUs
            << " dur_us=" << info.lastPresentedDurationUs
            << " display_index=" << info.lastPresentedDisplayIndex
            << " queue=" << info.videoQueueDepth
            << " has_frame=" << (info.hasVideoFrame ? 1 : 0) << '\n';
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
             Predicate predicate, PlayerDebugInfo* out = nullptr) {
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
  std::cerr << "video_frame_step_smoke: timeout waiting for " << label << '\n';
  printDebug("last", player, last);
  return false;
}

bool expectAudioFrameStepTransition(int64_t expectedPtsUs,
                                    int previousSerial, int expectedSerial,
                                    uint64_t previousFlushCount,
                                    const char* label) {
  const int64_t actual =
      gLastAudioDiscardUntilUs.load(std::memory_order_relaxed);
  const int actualSerial = gLastAudioFlushSerial.load(std::memory_order_relaxed);
  const uint64_t actualFlushCount =
      gAudioFlushCount.load(std::memory_order_relaxed);
  const bool seekBackedStep = expectedSerial != previousSerial;
  const uint64_t expectedFlushCount =
      previousFlushCount + (seekBackedStep ? 1 : 0);
  const bool serialTransitionValid =
      !seekBackedStep || expectedSerial == previousSerial + 1;
  const bool anchorValid = !seekBackedStep || actualSerial == expectedSerial;
  if (serialTransitionValid && anchorValid &&
      actualFlushCount == expectedFlushCount) {
    return true;
  }
  std::cerr << "video_frame_step_smoke: " << label
            << " audio frame-step transition mismatch expected_pts_us="
            << expectedPtsUs << " actual_discard_until_us=" << actual
            << " previous_serial=" << previousSerial
            << " expected_serial=" << expectedSerial
            << " actual_flush_serial=" << actualSerial
            << " previous_flush_count=" << previousFlushCount
            << " expected_flush_count=" << expectedFlushCount
            << " actual_flush_count=" << actualFlushCount
            << '\n';
  return false;
}

bool expectAudioResumeSeekAnchor(int64_t expectedPtsUs, int previousSerial,
                                 int expectedSerial,
                                 uint64_t previousFlushCount,
                                 const char* label) {
  const int64_t actual =
      gLastAudioDiscardUntilUs.load(std::memory_order_relaxed);
  const int actualSerial = gLastAudioFlushSerial.load(std::memory_order_relaxed);
  const uint64_t actualFlushCount =
      gAudioFlushCount.load(std::memory_order_relaxed);
  const uint64_t expectedFlushCount = previousFlushCount + 1;
  if (expectedSerial == previousSerial + 1 && actual == expectedPtsUs &&
      actualSerial == expectedSerial &&
      actualFlushCount == expectedFlushCount) {
    return true;
  }
  std::cerr << "video_frame_step_smoke: " << label
            << " audio resume seek anchor mismatch expected_pts_us="
            << expectedPtsUs << " actual_discard_until_us=" << actual
            << " previous_serial=" << previousSerial
            << " expected_serial=" << expectedSerial
            << " actual_flush_serial=" << actualSerial
            << " previous_flush_count=" << previousFlushCount
            << " expected_flush_count=" << expectedFlushCount
            << " actual_flush_count=" << actualFlushCount << '\n';
  return false;
}

bool expectAudioConcurrentResumeSeekAnchor(int64_t expectedPtsUs,
                                           int expectedSerial,
                                           uint64_t previousFlushCount,
                                           const char* label) {
  const int64_t actual =
      gLastAudioDiscardUntilUs.load(std::memory_order_relaxed);
  const int actualSerial = gLastAudioFlushSerial.load(std::memory_order_relaxed);
  const uint64_t actualFlushCount =
      gAudioFlushCount.load(std::memory_order_relaxed);
  if (actual == expectedPtsUs && actualSerial == expectedSerial &&
      actualFlushCount > previousFlushCount) {
    return true;
  }
  std::cerr << "video_frame_step_smoke: " << label
            << " concurrent audio resume seek anchor mismatch expected_pts_us="
            << expectedPtsUs << " actual_discard_until_us=" << actual
            << " expected_serial=" << expectedSerial
            << " actual_flush_serial=" << actualSerial
            << " previous_flush_count=" << previousFlushCount
            << " actual_flush_count=" << actualFlushCount << '\n';
  return false;
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: video_frame_step_smoke <video> [seek_us] "
                 "[step_count] [mode]\n"
                 "Modes: startup, resume, forward-resume, mixed-resume, "
                 "rapid-resume, resume-during-step, ended-replay\n";
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
  bool replayAfterEnd = false;
  bool verifyStartup = false;
  if (argc >= 5) {
    const std::string mode = argv[4];
    verifyStartup = mode == "startup";
    resumeAfterPrevious = mode == "resume";
    resumeAfterForward = mode == "forward-resume";
    resumeAfterMixed = mode == "mixed-resume";
    rapidResume = mode == "rapid-resume";
    resumeDuringStep = mode == "resume-during-step";
    replayAfterEnd = mode == "ended-replay";
    if (!verifyStartup && !resumeAfterPrevious && !resumeAfterForward &&
        !resumeAfterMixed && !rapidResume && !resumeDuringStep &&
        !replayAfterEnd) {
      std::cerr << "video_frame_step_smoke: invalid mode: " << argv[4]
                 << " (expected 'startup', 'resume', 'forward-resume', "
                    "'mixed-resume', 'rapid-resume', or "
                    "'resume-during-step', or 'ended-replay')\n";
      return 2;
    }
  }
  gLastAudioDiscardUntilUs.store(0, std::memory_order_relaxed);
  gLastAudioFlushSerial.store(0, std::memory_order_relaxed);
  gAudioFlushCount.store(0, std::memory_order_relaxed);

  Player player;
  PlayerConfig config;
  config.file = std::filesystem::path(argv[1]);
  config.logPath =
      std::filesystem::current_path() / "video_frame_step_smoke.timing.log";
  config.enableAudio = false;

  std::string error;
  if (!player.open(config, &error)) {
    std::cerr << "video_frame_step_smoke: open failed: " << error << '\n';
    return 1;
  }
  player.setVideoPaused(true);

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

  if (resumeAfterForward) {
    int64_t lastPtsUs = boundaryPtsUs;
    int lastSerial = boundary.currentSerial;
    uint64_t lastCounter = player.videoFrameCounter();
    for (size_t i = 0; i < stepCount; ++i) {
      const int beforeStepSerial = lastSerial;
      const uint64_t beforeStepAudioFlushCount =
          gAudioFlushCount.load(std::memory_order_relaxed);
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
      if (!expectAudioFrameStepTransition(
              lastPtsUs, beforeStepSerial, next.currentSerial,
              beforeStepAudioFlushCount, label.c_str())) {
        player.close();
        return 1;
      }
      lastSerial = next.currentSerial;
    }

    const uint64_t beforeResumeAudioFlushCount =
        gAudioFlushCount.load(std::memory_order_relaxed);
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
    if (!expectAudioResumeSeekAnchor(
            lastPtsUs, lastSerial, resumed.currentSerial,
            beforeResumeAudioFlushCount, "resume_after_forward")) {
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
    const uint64_t beforeStepAudioFlushCount =
        gAudioFlushCount.load(std::memory_order_relaxed);
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
    if (!expectAudioFrameStepTransition(
            lastPtsUs, beforeStepSerial, previous.currentSerial,
            beforeStepAudioFlushCount, label.c_str())) {
      player.close();
      return 1;
    }
    lastSerial = previous.currentSerial;
    steppedPts.push_back(lastPtsUs);
  }

  if (resumeDuringStep) {
    if (!player.requestFrameStep(
            playback_video_frame_step::Direction::Previous)) {
      std::cerr << "video_frame_step_smoke: resume-during-step previous-frame "
                   "request rejected\n";
      player.close();
      return 1;
    }
    const uint64_t beforeResumeAudioFlushCount =
        gAudioFlushCount.load(std::memory_order_relaxed);
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
    if (!expectAudioConcurrentResumeSeekAnchor(
            lastPtsUs, resumed.currentSerial, beforeResumeAudioFlushCount,
            "resume_during_step")) {
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
    const uint64_t beforeResumeAudioFlushCount =
        gAudioFlushCount.load(std::memory_order_relaxed);
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
    if (!expectAudioResumeSeekAnchor(
            lastPtsUs, lastSerial, resumed.currentSerial,
            beforeResumeAudioFlushCount, "resume_after_previous")) {
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
    const uint64_t beforeStepAudioFlushCount =
        gAudioFlushCount.load(std::memory_order_relaxed);
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
    if (!expectAudioFrameStepTransition(
            lastPtsUs, beforeStepSerial, next.currentSerial,
            beforeStepAudioFlushCount, label.c_str())) {
      player.close();
      return 1;
    }
    lastSerial = next.currentSerial;
  }

  if (resumeAfterMixed) {
    const uint64_t beforeResumeAudioFlushCount =
        gAudioFlushCount.load(std::memory_order_relaxed);
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
    if (!expectAudioResumeSeekAnchor(
            lastPtsUs, lastSerial, resumed.currentSerial,
            beforeResumeAudioFlushCount, "resume_after_mixed")) {
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
