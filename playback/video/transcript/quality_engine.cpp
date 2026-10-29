#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "playback/video/transcript/quality_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <windows.h>

#include "ffmpegaudio.h"
#include "core/published_json.h"
#include "playback/video/transcript/artifact.h"
#include "playback/video/transcript/source_identity.h"
#include "playback/video/transcript/runtime.h"
#include "runtime_helpers.h"

namespace playback_video_transcript {
namespace {
using Json = nlohmann::json;
constexpr int kSampleRate = 16000;

struct Handle {
  HANDLE value = nullptr;
  explicit Handle(HANDLE handle) : value(handle) {}
  ~Handle() { if (value) CloseHandle(value); }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};

struct Workspace {
  std::filesystem::path path;
  ~Workspace() {
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  }
};

// Same quoting rules as CommandLineToArgvW, including trailing backslashes.
std::wstring quote(const std::filesystem::path &path) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (wchar_t ch : path.wstring()) {
    if (ch == L'\\') { ++slashes; continue; }
    result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
    slashes = 0;
    if (ch == L'"') result += L'\\';
    result += ch;
  }
  result.append(slashes * 2, L'\\');
  return result + L'"';
}

} // namespace

bool createQualityTranscript(
    const std::filesystem::path &videoPath,
    const std::filesystem::path &outputPath, TranscriptPublishMode publishMode,
    const ProgressCallback &onProgress,
    const std::atomic<bool> *cancelRequested, std::string *error,
    const TranscriptCommitStarted &outputCommitStarted,
    std::filesystem::path *publishedPathResult) try {
  if (error) error->clear();
  if (publishedPathResult) publishedPathResult->clear();
  const auto fail = [&](std::string detail) {
    if (error) *error = std::move(detail);
    return false;
  };
  const auto cancelled = [&]() {
    return cancelRequested && cancelRequested->load(std::memory_order_relaxed);
  };
  float lastFraction = 0;
  const auto report = [&](float fraction, const std::string &phase) {
    lastFraction = std::max(lastFraction, std::clamp(fraction, 0.f, 1.f));
    if (onProgress) onProgress({lastFraction, phase});
  };
  if (cancelled()) return fail("Transcript cancelled.");
  if (publishMode == TranscriptPublishMode::ReplaceOwned)
    return fail("Manual transcription cannot replace background evidence.");

  auto identity = captureTranscriptSourceIdentity(videoPath, error);
  if (!identity) return false;
  TranscriptRuntime speechRuntime;
  if (!ensureTranscriptRuntime(RuntimeComponent::Quality, onProgress, cancelled, &speechRuntime, error))
    return false;
  const auto &runtime = speechRuntime.directory;

  // Each invocation owns its scratch directory; no media sidecars are used for
  // IPC, and cancelling never leaves a partly published transcript behind.
  Workspace workspace;
  const auto tempRoot = std::filesystem::temp_directory_path();
  for (unsigned attempt = 0; attempt != 100; ++attempt) {
    auto candidate = tempRoot / ("radioify-transcript-" +
        std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetTickCount64()) + "-" + std::to_string(attempt));
    std::error_code ec;
    if (std::filesystem::create_directory(candidate, ec)) {
      workspace.path = std::move(candidate);
      break;
    }
  }
  if (workspace.path.empty()) return fail("Could not create transcript workspace.");
  report(0.01f, "Decoding audio for Qwen3-ASR 1.7B");
  FfmpegAudioDecoder decoder;
  if (!decoder.init(videoPath, 1, kSampleRate, error)) return false;
  uint64_t totalFrames = 0, decodedFrames = 0;
  int64_t offsetFrames = 0;
  decoder.getTotalFrames(&totalFrames);
  decoder.getStartOffsetFrames(&offsetFrames);
  std::ofstream pcm(workspace.path / "audio.f32", std::ios::binary);
  std::array<float, 16384> block;
  for (;;) {
    if (cancelled()) return fail("Transcript cancelled.");
    uint64_t count = 0;
    if (!decoder.readFrames(block.data(), block.size(), &count))
      return fail("Could not decode the video's audio.");
    if (!count) break;
    pcm.write(reinterpret_cast<const char *>(block.data()), count * sizeof(float));
    if (!pcm) return fail("Could not write temporary transcript audio.");
    decodedFrames += count;
    if (totalFrames)
      report(0.01f + 0.04f * std::min(1., double(decodedFrames) / totalFrames),
             "Decoding audio for Qwen3-ASR 1.7B");
  }
  pcm.close();
  if (!pcm || !decodedFrames) return fail("The video has no decodable audio samples.");
  decoder.uninit();
  std::string language;
  if (auto configured = getEnvString("RADIOIFY_TRANSCRIPT_LANGUAGE"))
    language = *configured;
  else if (auto legacy = getEnvString("RADIOIFY_WHISPER_LANGUAGE"))
    language = *legacy;
  {
    std::ofstream request(workspace.path / "request.json", std::ios::binary);
    request << Json{{"version", 1}, {"language", language}}.dump();
    request.close();
    if (!request) return fail("Could not write transcript request.");
  }

  // Share admission with the existing analysis worker, so two large models
  // from separate operations do not compete for the same GPU allocation.
  Handle gpu(CreateMutexW(nullptr, FALSE, L"Local\\Radioify.ModelInference.GPU"));
  if (!gpu.value) return fail("Could not acquire transcript GPU ownership.");
  report(0.05f, "Waiting for transcript GPU");
  for (;;) {
    if (cancelled()) return fail("Transcript cancelled.");
    DWORD wait = WaitForSingleObject(gpu.value, 50);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) break;
    if (wait != WAIT_TIMEOUT) return fail("Could not acquire transcript GPU ownership.");
  }
  struct ReleaseGpu { HANDLE value; ~ReleaseGpu() { ReleaseMutex(value); } } release{gpu.value};
  Handle job(CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                            &limits, sizeof(limits)))
    return fail("Could not isolate transcript worker.");
  const auto executable = runtime / "python.exe";
  auto command = quote(executable) + L" -I -B " + quote(speechRuntime.worker) +
                 L" " + quote(workspace.path) + L" " + quote(runtime);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                      runtime.c_str(), &startup, &process))
    return fail("Could not start Qwen3-ASR transcript worker.");
  Handle processHandle(process.hProcess), threadHandle(process.hThread);
  if (!AssignProcessToJobObject(job.value, processHandle.value) ||
      ResumeThread(threadHandle.value) == static_cast<DWORD>(-1)) {
    TerminateProcess(processHandle.value, 1);
    WaitForSingleObject(processHandle.value, 5000);
    return fail("Could not attach transcript worker to its job.");
  }
  uint64_t sequence = 0;
  bool deviceReported = false;
  for (;;) {
    DWORD wait = WaitForSingleObject(processHandle.value, 100);
    auto progress = readPublishedJson(workspace.path / "progress.json", 65536);
    if (progress.is_object() && progress.value("sequence", uint64_t{0}) > sequence) {
      sequence = progress.value("sequence", uint64_t{0});
      const auto device = progress.value("device", std::string());
      if (!deviceReported && !device.empty()) {
        report(0.05f, "Qwen3-ASR 1.7B ready on CUDA: " + device);
        deviceReported = true;
      }
      report(0.05f + 0.92f * progress.value("fraction", 0.f),
             progress.value("phase", std::string("Transcribing audio")));
    }
    if (cancelled() || (wait != WAIT_TIMEOUT && wait != WAIT_OBJECT_0)) {
      TerminateJobObject(job.value, 1);
      WaitForSingleObject(processHandle.value, INFINITE);
      return fail(cancelled() ? "Transcript cancelled." : "Transcript worker wait failed.");
    }
    if (wait == WAIT_OBJECT_0) break;
  }
  DWORD exitCode = 1;
  GetExitCodeProcess(processHandle.value, &exitCode);
  auto result = readPublishedJson(workspace.path / "result.json", 64 * 1024 * 1024);
  if (!result.is_object() || result.value("version", 0) != 1)
    return fail("Qwen3-ASR worker exited without a valid result (code " +
                std::to_string(exitCode) + ").");
  if (exitCode || result.value("status", "") != "ok")
    return fail(result.value("error", "Qwen3-ASR transcript failed."));
  const auto &cues = result.at("cues");
  if (!cues.is_array() || cues.size() > 200000)
    return fail("Transcript worker returned an invalid cue list.");
  std::vector<Segment> segments;
  const int64_t offsetUs = offsetFrames * 1000000 / kSampleRate;
  const int64_t durationUs = static_cast<int64_t>(decodedFrames * 1000000 / kSampleRate);
  for (const auto &cue : cues) {
    int64_t start = cue.at("start_us").get<int64_t>();
    int64_t end = cue.at("end_us").get<int64_t>();
    auto text = cue.at("text").get<std::string>();
    if (start < 0 || end <= start || end > durationUs + 1000 || text.empty() ||
        text.size() > 16384 || text.find("-->") != std::string::npos)
      return fail("Transcript worker returned an invalid subtitle.");
    start = std::max(int64_t{0}, start + offsetUs);
    end = std::max(start + 1000, end + offsetUs);
    segments.push_back({start, end, std::move(text)});
  }
  if (segments.empty()) return fail("Speech transcription found no usable dialogue.");
  // Sorting does not clamp one speaker to another: overlapping barks are valid.
  std::stable_sort(segments.begin(), segments.end(), [](const auto &a, const auto &b) {
    return a.startUs < b.startUs;
  });
  auto destination = outputPath;
  if (outputPath.lexically_normal() == transcriptPathForVideo(videoPath).lexically_normal()) {
    auto tagged = languageTaggedTranscriptPathForVideo(videoPath, result.value("language", ""));
    if (!tagged.empty()) destination = std::move(tagged);
  }
  report(0.98f, "Saving transcript");
  const auto commit = [&]() {
    if (cancelled() || !transcriptSourceMatches(*identity, videoPath)) return false;
    if (outputCommitStarted && !outputCommitStarted()) return false;
    return transcriptSourceMatches(*identity, videoPath);
  };
  if (!writeIndexedTranscript(destination, segments, publishMode, error, commit)) return false;
  if (publishedPathResult) *publishedPathResult = destination;
  report(1.f, "Transcript complete (Qwen3-ASR 1.7B + Whisper large-v3)");
  return true;
} catch (const std::exception &exception) {
  if (error) *error = std::string("Transcript failed: ") + exception.what();
  return false;
}
} // namespace playback_video_transcript
