#include "melodyanalysiscache.h"

#include "ffmpegaudio.h"
#include "gmeaudio.h"
#include "gsfaudio.h"
#include "kssaudio.h"
#include "melodyanalyzer.h"
#include "neuralpitch.h"
#include "psfaudio.h"
#include "vgmaudio.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t kAnalysisChunkFrames = 2048;
constexpr uint64_t kCaptureStrideFrames = 256;
constexpr int kOfflineStateCount = kMelodyAnalyzerStates;
constexpr int kOfflineUnvoicedState = 0;
constexpr float kOfflineProbFloor = 1.0e-9f;
constexpr float kOfflineLogFloor = -90.0f;
constexpr int kOfflineMinVoicedRunFrames = 5;
constexpr int kOfflineGapFillFrames = 2;
constexpr float kOfflineGapFillConfidence = 0.22f;
// Matches torchcrepe.threshold.Hysteresis defaults.
constexpr float kOfflineVoicingLower = 0.19f;
constexpr float kOfflineVoicingUpper = 0.31f;
constexpr float kOfflineLargeJumpPenalty = 8.0f;

struct MelodyAnalysisPoint {
  uint64_t frame = 0;
  MelodyOfflineFrame frameData{};
};

struct MelodyRawAnalysisPoint {
  uint64_t frame = 0;
  MelodyOfflineFrame frameData{};
  std::array<float, kOfflineStateCount> emissionLog{};
};

int stateToMidi(int state) {
  if (state <= kOfflineUnvoicedState) return -1;
  int midi = kMelodyMidiMin + state - 1;
  if (midi < kMelodyMidiMin || midi > kMelodyMidiMax) return -1;
  return midi;
}

float midiToFrequency(int midi) {
  if (midi < 0 || midi > 127) return 0.0f;
  return 440.0f * std::exp2((static_cast<float>(midi) - 69.0f) / 12.0f);
}

float frequencyToMidi(float frequencyHz) {
  if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0f) {
    return -1.0f;
  }
  return 69.0f + 12.0f * std::log2(frequencyHz / 440.0f);
}

float sanitizeConfidence(float confidence) {
  if (!std::isfinite(confidence)) return 0.0f;
  return std::clamp(confidence, 0.0f, 1.0f);
}

float estimateRawMidi(const MelodyOfflineFrame& frame) {
  if (frame.midiNote >= 0 && frame.midiNote <= 127) {
    return static_cast<float>(frame.midiNote);
  }
  return frequencyToMidi(frame.frequencyHz);
}

void makeUnvoiced(MelodyOfflineFrame* frame, float confidenceCap = 0.12f) {
  if (!frame) return;
  frame->frequencyHz = 0.0f;
  frame->midiNote = -1;
  float baseConfidence = sanitizeConfidence(frame->confidence);
  frame->confidence = std::clamp(baseConfidence, 0.0f, confidenceCap);
}

std::vector<float> buildVoicedTransitionPenalties() {
  constexpr int kVoicedStateCount = kOfflineStateCount - 1;
  std::vector<float> penalties(
      static_cast<size_t>(kVoicedStateCount * kVoicedStateCount), 0.0f);

  for (int from = 0; from < kVoicedStateCount; ++from) {
    for (int to = 0; to < kVoicedStateCount; ++to) {
      int semitone = std::abs(to - from);
      float penalty = 0.0f;
      if (semitone >= 12) {
        penalty = kOfflineLargeJumpPenalty;
      } else {
        // Matches the reference CREPE Viterbi topology:
        // transition weight = max(12 - abs(i-j), 0), row-normalized.
        float weight = static_cast<float>(12 - semitone);
        penalty = -std::log(std::max(kOfflineProbFloor, weight / 12.0f));
      }
      penalties[static_cast<size_t>(from) * kVoicedStateCount +
                static_cast<size_t>(to)] = penalty;
    }
  }
  return penalties;
}

void normalizeLogScores(std::vector<float>* scores) {
  if (!scores || scores->empty()) return;
  float best = -std::numeric_limits<float>::infinity();
  for (float v : *scores) {
    if (std::isfinite(v) && v > best) best = v;
  }
  if (!std::isfinite(best)) {
    std::fill(scores->begin(), scores->end(), kOfflineLogFloor);
    return;
  }
  for (float& v : *scores) {
    if (!std::isfinite(v)) {
      v = kOfflineLogFloor;
    } else {
      v -= best;
    }
  }
}

std::array<float, kOfflineStateCount> makeSilenceEmissionLog();

void buildEmissionLogProbsFromPosterior(
    const std::array<float, kOfflineStateCount>& posterior,
    std::array<float, kOfflineStateCount>* out) {
  if (!out) return;
  out->fill(kOfflineLogFloor);

  float sum = 0.0f;
  for (float p : posterior) {
    if (std::isfinite(p) && p > 0.0f) {
      sum += p;
    }
  }
  if (sum <= kOfflineProbFloor) {
    *out = makeSilenceEmissionLog();
    return;
  }
  for (int state = 0; state < kOfflineStateCount; ++state) {
    float p = posterior[static_cast<size_t>(state)];
    if (!std::isfinite(p) || p <= 0.0f) {
      (*out)[state] = std::log(kOfflineProbFloor);
    } else {
      (*out)[state] = std::log(std::max(kOfflineProbFloor, p / sum));
    }
  }
}

std::array<float, kOfflineStateCount> makeSilenceEmissionLog() {
  std::array<float, kOfflineStateCount> out{};
  out.fill(std::log(kOfflineProbFloor));
  out[kOfflineUnvoicedState] =
      std::log(std::max(kOfflineProbFloor, 1.0f - 0.01f));
  float voicedEach =
      0.01f / static_cast<float>(std::max(1, kOfflineStateCount - 1));
  float voicedLog = std::log(std::max(kOfflineProbFloor, voicedEach));
  for (int state = 1; state < kOfflineStateCount; ++state) {
    out[static_cast<size_t>(state)] = voicedLog;
  }
  return out;
}

std::vector<int> decodeGlobalVoicedPath(
    const std::vector<MelodyRawAnalysisPoint>& rawFrames) {
  std::vector<int> path(rawFrames.size(), 1);
  if (rawFrames.empty()) return path;

  constexpr int kVoicedStateCount = kOfflineStateCount - 1;
  const std::vector<float> transition = buildVoicedTransitionPenalties();

  std::vector<uint8_t> backPointers(
      rawFrames.size() * static_cast<size_t>(kVoicedStateCount), 0u);
  std::vector<float> prev(static_cast<size_t>(kVoicedStateCount), kOfflineLogFloor);
  std::vector<float> cur(static_cast<size_t>(kVoicedStateCount), kOfflineLogFloor);

  const std::array<float, kOfflineStateCount>& emission0 = rawFrames[0].emissionLog;
  float voicedPrior = 1.0f / static_cast<float>(kVoicedStateCount);
  for (int state = 0; state < kVoicedStateCount; ++state) {
    int fullState = state + 1;
    prev[static_cast<size_t>(state)] =
        std::log(std::max(kOfflineProbFloor, voicedPrior)) + emission0[fullState];
  }
  normalizeLogScores(&prev);

  for (size_t t = 1; t < rawFrames.size(); ++t) {
    const std::array<float, kOfflineStateCount>& emission = rawFrames[t].emissionLog;
    for (int next = 0; next < kVoicedStateCount; ++next) {
      float best = -std::numeric_limits<float>::infinity();
      int bestPrev = 0;
      for (int prevState = 0; prevState < kVoicedStateCount; ++prevState) {
        float penalty = transition[static_cast<size_t>(prevState) *
                                   static_cast<size_t>(kVoicedStateCount) +
                                   static_cast<size_t>(next)];
        float candidate = prev[static_cast<size_t>(prevState)] - penalty;
        if (candidate > best) {
          best = candidate;
          bestPrev = prevState;
        }
      }
      cur[static_cast<size_t>(next)] = best + emission[next + 1];
      backPointers[t * static_cast<size_t>(kVoicedStateCount) +
                   static_cast<size_t>(next)] =
          static_cast<uint8_t>(bestPrev);
    }
    normalizeLogScores(&cur);
    prev.swap(cur);
  }

  int bestFinal = 0;
  float bestFinalScore = prev[0];
  for (int state = 1; state < kVoicedStateCount; ++state) {
    float score = prev[static_cast<size_t>(state)];
    if (score > bestFinalScore) {
      bestFinal = state;
      bestFinalScore = score;
    }
  }

  path.back() = bestFinal + 1;
  for (size_t t = rawFrames.size() - 1; t > 0; --t) {
    size_t idx = t * static_cast<size_t>(kVoicedStateCount) +
                 static_cast<size_t>(path[t] - 1);
    path[t - 1] = static_cast<int>(backPointers[idx]) + 1;
  }
  return path;
}

std::vector<bool> buildVoicingMaskHysteresis(
    const std::vector<MelodyRawAnalysisPoint>& rawFrames) {
  std::vector<bool> voiced(rawFrames.size(), false);
  bool active = false;
  for (size_t i = 0; i < rawFrames.size(); ++i) {
    float conf = sanitizeConfidence(rawFrames[i].frameData.confidence);
    if (active) {
      if (conf < kOfflineVoicingLower) active = false;
    } else {
      if (conf >= kOfflineVoicingUpper) active = true;
    }
    voiced[i] = active;
  }
  return voiced;
}

MelodyOfflineFrame refineFrameForState(const MelodyOfflineFrame& raw, int state) {
  MelodyOfflineFrame out = raw;
  float rawConfidence = sanitizeConfidence(raw.confidence);
  if (state == kOfflineUnvoicedState) {
    makeUnvoiced(&out, std::min(0.14f, 0.4f * rawConfidence));
    return out;
  }

  int midi = stateToMidi(state);
  if (midi < 0) {
    makeUnvoiced(&out);
    return out;
  }
  float targetHz = midiToFrequency(midi);
  float rawMidi = estimateRawMidi(raw);
  float alignment = 0.7f;
  if (std::isfinite(rawMidi) && rawMidi > 0.0f) {
    float semitoneDistance = std::fabs(rawMidi - static_cast<float>(midi));
    float norm = semitoneDistance / 1.85f;
    alignment = std::exp(-0.5f * norm * norm);
  }
  float conf = 0.22f + 0.78f * rawConfidence * alignment;
  conf = std::clamp(conf, 0.02f, 1.0f);

  float refinedHz = targetHz;
  if (std::isfinite(raw.frequencyHz) && raw.frequencyHz > 0.0f) {
    float cents = std::fabs(1200.0f * std::log2(raw.frequencyHz / targetHz));
    if (std::isfinite(cents) && cents <= 90.0f) {
      float blend = std::clamp(0.15f + 0.7f * rawConfidence, 0.15f, 0.9f);
      refinedHz = targetHz * (1.0f - blend) + raw.frequencyHz * blend;
    }
  }

  out.frequencyHz = refinedHz;
  out.midiNote = midi;
  out.confidence = conf;
  return out;
}

void fillShortUnvoicedGaps(std::vector<MelodyAnalysisPoint>* frames) {
  if (!frames || frames->size() < 3) return;
  size_t i = 1;
  while (i + 1 < frames->size()) {
    if ((*frames)[i].frameData.midiNote >= 0) {
      ++i;
      continue;
    }
    size_t gapStart = i;
    while (i < frames->size() && (*frames)[i].frameData.midiNote < 0) {
      ++i;
    }
    if (i >= frames->size()) break;
    size_t gapEnd = i;
    size_t gapLength = gapEnd - gapStart;
    if (gapStart == 0 || gapLength > static_cast<size_t>(kOfflineGapFillFrames)) {
      continue;
    }
    int leftMidi = (*frames)[gapStart - 1].frameData.midiNote;
    int rightMidi = (*frames)[gapEnd].frameData.midiNote;
    if (leftMidi < 0 || rightMidi < 0 || std::abs(leftMidi - rightMidi) > 4) {
      continue;
    }
    int fillMidi = static_cast<int>(std::lround(
        0.5f * static_cast<float>(leftMidi + rightMidi)));
    float fillHz = midiToFrequency(fillMidi);
    for (size_t k = gapStart; k < gapEnd; ++k) {
      (*frames)[k].frameData.midiNote = fillMidi;
      (*frames)[k].frameData.frequencyHz = fillHz;
      (*frames)[k].frameData.confidence = std::max(
          (*frames)[k].frameData.confidence, kOfflineGapFillConfidence);
    }
  }
}

void suppressShortVoicedRuns(std::vector<MelodyAnalysisPoint>* frames) {
  if (!frames || frames->empty()) return;
  size_t i = 0;
  while (i < frames->size()) {
    bool voiced = (*frames)[i].frameData.midiNote >= 0;
    size_t runStart = i;
    while (i < frames->size() &&
           ((*frames)[i].frameData.midiNote >= 0) == voiced) {
      ++i;
    }
    size_t runLength = i - runStart;
    if (voiced && runLength < static_cast<size_t>(kOfflineMinVoicedRunFrames)) {
      for (size_t k = runStart; k < i; ++k) {
        makeUnvoiced(&(*frames)[k].frameData, 0.10f);
      }
    }
  }
}

void medianSmoothVoicedMidi(std::vector<MelodyAnalysisPoint>* frames) {
  if (!frames || frames->size() < 5) return;
  std::vector<int> smoothedMidi(frames->size(), -1);
  for (size_t i = 0; i < frames->size(); ++i) {
    if ((*frames)[i].frameData.midiNote < 0) continue;
    int window[5];
    int count = 0;
    size_t left = (i > 2) ? i - 2 : 0;
    size_t right = std::min(frames->size() - 1, i + 2);
    for (size_t j = left; j <= right; ++j) {
      int midi = (*frames)[j].frameData.midiNote;
      if (midi >= 0) {
        window[count++] = midi;
      }
    }
    if (count < 3) continue;
    std::sort(window, window + count);
    smoothedMidi[i] = window[count / 2];
  }
  for (size_t i = 0; i < frames->size(); ++i) {
    int midi = smoothedMidi[i];
    if (midi < 0) continue;
    (*frames)[i].frameData.midiNote = midi;
    (*frames)[i].frameData.frequencyHz = midiToFrequency(midi);
  }
}

std::vector<MelodyAnalysisPoint> refineOfflineFrames(
    const std::vector<MelodyRawAnalysisPoint>& rawFrames) {
  if (rawFrames.empty()) return {};
  std::vector<int> voicedStates = decodeGlobalVoicedPath(rawFrames);
  std::vector<bool> voicedMask = buildVoicingMaskHysteresis(rawFrames);
  std::vector<MelodyAnalysisPoint> refined;
  refined.reserve(rawFrames.size());
  for (size_t i = 0; i < rawFrames.size(); ++i) {
    int state = kOfflineUnvoicedState;
    if (i < voicedStates.size() && i < voicedMask.size() && voicedMask[i]) {
      state = voicedStates[i];
    }
    MelodyAnalysisPoint point;
    point.frame = rawFrames[i].frame;
    point.frameData = refineFrameForState(rawFrames[i].frameData, state);
    refined.push_back(point);
  }
  fillShortUnvoicedGaps(&refined);
  suppressShortVoicedRuns(&refined);
  medianSmoothVoicedMidi(&refined);
  return refined;
}

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool isMiniaudioExt(const std::filesystem::path& path) {
  const std::string ext = toLower(path.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

bool isM4aExt(const std::filesystem::path& path) {
  const std::string ext = toLower(path.extension().string());
  return ext == ".m4a" || ext == ".mp4" || ext == ".webm";
}

bool isGmeExt(const std::filesystem::path& path) {
  const std::string ext = toLower(path.extension().string());
  return ext == ".nsf";
}

bool isGsfExt(const std::filesystem::path& path) {
#if !RADIOIFY_DISABLE_GSF_GPL
  const std::string ext = toLower(path.extension().string());
  return ext == ".gsf" || ext == ".minigsf";
#else
  (void)path;
  return false;
#endif
}

bool isVgmExt(const std::filesystem::path& path) {
  const std::string ext = toLower(path.extension().string());
  return ext == ".vgm" || ext == ".vgz";
}

bool isPsfExt(const std::filesystem::path& path) {
  const std::string ext = toLower(path.extension().string());
  return ext == ".psf" || ext == ".minipsf" || ext == ".psf2" ||
         ext == ".minipsf2";
}

bool isKssExt(const std::filesystem::path& path) {
  const std::string ext = toLower(path.extension().string());
  return ext == ".kss";
}

struct MelodyOfflineJob {
  std::filesystem::path file;
  int trackIndex = 0;
  uint32_t sourceSampleRate = 0;
  uint32_t channels = 1;
  uint64_t leadInFrames = 0;
  KssPlaybackOptions kssOptions{};
  NsfPlaybackOptions nsfOptions{};
  VgmPlaybackOptions vgmOptions{};
  std::unordered_map<uint32_t, VgmDeviceOptions> vgmDeviceOverrides;
};

class DecoderContext {
 public:
  enum class Type {
    Unknown,
    Ffmpeg,
    Gme,
    Gsf,
    Vgm,
    Kss,
    Psf,
  };

  ~DecoderContext() { uninit(); }

  bool init(const MelodyOfflineJob& job, std::string* error) {
    if (!job.file.empty() && !std::filesystem::exists(job.file)) {
      if (error) *error = "Input file not found.";
      return false;
    }

    channels_ = std::max<uint32_t>(1u, job.channels);
    sampleRate_ = std::max<uint32_t>(1u, job.sourceSampleRate);
    type_ = Type::Unknown;

    if (isM4aExt(job.file) || isMiniaudioExt(job.file)) {
      type_ = Type::Ffmpeg;
      if (!ffmpeg_.init(job.file, channels_, sampleRate_, error)) {
        return false;
      }
      return true;
    }

    if (isGmeExt(job.file)) {
      type_ = Type::Gme;
      if (!gme_.init(job.file, channels_, sampleRate_, error, job.trackIndex)) {
        return false;
      }
      gme_.applyNsfOptions(job.nsfOptions);
      return true;
    }

    if (isGsfExt(job.file)) {
      type_ = Type::Gsf;
      if (!gsf_.init(job.file, channels_, sampleRate_, error, job.trackIndex)) {
        return false;
      }
      return true;
    }

    if (isVgmExt(job.file)) {
      type_ = Type::Vgm;
      if (!vgm_.init(job.file, channels_, sampleRate_, error)) {
        return false;
      }
      vgm_.applyOptions(job.vgmOptions);
      for (const auto& entry : job.vgmDeviceOverrides) {
        vgm_.setDeviceOptions(entry.first, entry.second);
      }
      return true;
    }

    if (isPsfExt(job.file)) {
      type_ = Type::Psf;
      if (!psf_.init(job.file, channels_, sampleRate_, error, job.trackIndex)) {
        return false;
      }
      return true;
    }

    if (isKssExt(job.file)) {
      type_ = Type::Kss;
      if (!kss_.init(job.file, channels_, sampleRate_, error, job.trackIndex,
                    job.kssOptions)) {
        return false;
      }
      return true;
    }

    if (error) {
      *error = "Unsupported format for melody offline analysis.";
    }
    return false;
  }

  void uninit() {
    if (type_ == Type::Ffmpeg) ffmpeg_.uninit();
    if (type_ == Type::Gme) gme_.uninit();
    if (type_ == Type::Gsf) gsf_.uninit();
    if (type_ == Type::Vgm) vgm_.uninit();
    if (type_ == Type::Kss) kss_.uninit();
    if (type_ == Type::Psf) psf_.uninit();
    type_ = Type::Unknown;
  }

  bool readFrames(float* out, uint32_t frameCount, uint64_t* outFrames) {
    if (outFrames) *outFrames = 0;
    if (out == nullptr || frameCount == 0 || type_ == Type::Unknown) {
      return false;
    }

    switch (type_) {
      case Type::Ffmpeg:
        return ffmpeg_.readFrames(out, frameCount, outFrames);
      case Type::Gme:
        return gme_.readFrames(out, frameCount, outFrames);
      case Type::Gsf:
        return gsf_.readFrames(out, frameCount, outFrames);
      case Type::Vgm:
        return vgm_.readFrames(out, frameCount, outFrames);
      case Type::Kss:
        return kss_.readFrames(out, frameCount, outFrames);
      case Type::Psf:
        return psf_.readFrames(out, frameCount, outFrames);
      default:
        return false;
    }
  }

  uint64_t getTotalFrames() const {
    uint64_t total = 0;
    switch (type_) {
      case Type::Ffmpeg:
        ffmpeg_.getTotalFrames(&total);
        return total;
      case Type::Gme:
        gme_.getTotalFrames(&total);
        return total;
      case Type::Gsf:
        gsf_.getTotalFrames(&total);
        return total;
      case Type::Vgm:
        vgm_.getTotalFrames(&total);
        return total;
      case Type::Kss:
        kss_.getTotalFrames(&total);
        return total;
      case Type::Psf:
        psf_.getTotalFrames(&total);
        return total;
      default:
        return 0;
    }
  }

 private:
  Type type_ = Type::Unknown;
  uint32_t sampleRate_ = 0;
  uint32_t channels_ = 0;
  FfmpegAudioDecoder ffmpeg_;
  GmeAudioDecoder gme_;
  GsfAudioDecoder gsf_;
  VgmAudioDecoder vgm_;
  KssAudioDecoder kss_;
  PsfAudioDecoder psf_;
};

struct MelodyOfflineCache {
  std::mutex mutex;
  std::vector<MelodyAnalysisPoint> frames;
  bool ready = false;
  bool running = false;
  float progress = 0.0f;
  size_t frameCount = 0;
  std::string error;
  uint32_t sourceSampleRate = 0;
  uint64_t leadInFrames = 0;
  std::thread worker;
  std::atomic<bool> stopRequested{false};
};

MelodyOfflineCache gCache;

void setCacheState(MelodyOfflineCache* cache, const MelodyOfflineJob& job) {
  if (!cache) return;
  std::lock_guard<std::mutex> lock(cache->mutex);
  cache->frames.clear();
  cache->stopRequested.store(false, std::memory_order_release);
  cache->ready = false;
  cache->running = false;
  cache->progress = 0.0f;
  cache->frameCount = 0;
  cache->error.clear();
  cache->sourceSampleRate = job.sourceSampleRate;
  cache->leadInFrames = job.leadInFrames;
}

void stopWorker() {
  gCache.stopRequested.store(true, std::memory_order_release);
  if (gCache.worker.joinable()) {
    gCache.worker.join();
  }
  std::lock_guard<std::mutex> lock(gCache.mutex);
  gCache.running = false;
  gCache.ready = false;
  gCache.progress = 0.0f;
  gCache.frameCount = 0;
  gCache.frames.clear();
  gCache.error.clear();
}

std::array<float, kOfflineStateCount> buildEmissionForPoint(
    const std::array<float, kOfflineStateCount>* posterior) {
  std::array<float, kOfflineStateCount> emission{};
  if (posterior) {
    buildEmissionLogProbsFromPosterior(*posterior, &emission);
  } else {
    emission = makeSilenceEmissionLog();
  }
  return emission;
}

void updateProgressLocked(MelodyOfflineCache* cache, size_t frameCount,
                         float progress) {
  if (!cache) return;
  cache->frameCount = frameCount;
  cache->progress = progress;
}

void storePoint(MelodyOfflineCache* cache, uint64_t frame,
                const MelodyOfflineFrame& data) {
  if (!cache) return;
  std::lock_guard<std::mutex> lock(cache->mutex);
  cache->frames.push_back({frame, data});
  cache->frameCount = cache->frames.size();
}

bool writeMelodyFramesToFile(
    const std::filesystem::path& outputFile, uint32_t sourceSampleRate,
    const std::vector<MelodyAnalysisPoint>& frames, std::string* error) {
  if (outputFile.empty()) {
    if (error) *error = "Output path is empty.";
    return false;
  }
  if (frames.empty()) {
    if (error) *error = "No melody frames available.";
    return false;
  }

  std::error_code ec;
  if (outputFile.has_parent_path()) {
    std::filesystem::create_directories(outputFile.parent_path(), ec);
    if (ec) {
      if (error) *error = "Unable to create output directory.";
      return false;
    }
  }

  std::ofstream out(outputFile, std::ios::trunc);
  if (!out.is_open()) {
    if (error) *error = "Unable to open output file for writing.";
    return false;
  }

  out << "RADIOIFY_MELODY 1\n";
  out << "sample_rate " << sourceSampleRate << "\n";
  out << "stride_frames " << kCaptureStrideFrames << "\n";
  out << "frame_count " << frames.size() << "\n";
  out << "columns frame frequency_hz confidence midi\n";
  out << std::setprecision(9);
  for (const auto& point : frames) {
    out << point.frame << " " << point.frameData.frequencyHz << " "
        << point.frameData.confidence << " " << point.frameData.midiNote
        << "\n";
  }

  if (!out.good()) {
    if (error) *error = "Failed while writing melody output.";
    return false;
  }
  return true;
}

void analyzeTrackForCache(MelodyOfflineCache* cache, MelodyOfflineJob job,
                          const std::function<void(float)>& progressCallback) {
  if (!cache) return;
  NeuralPitchState neural{};
  DecoderContext decoder;

  {
    std::lock_guard<std::mutex> lock(cache->mutex);
    cache->running = true;
    cache->ready = false;
    cache->progress = 0.0f;
    cache->frameCount = 0;
    cache->frames.clear();
    cache->error.clear();
    cache->leadInFrames = job.leadInFrames;
    cache->sourceSampleRate = job.sourceSampleRate;
    cache->stopRequested.store(false, std::memory_order_release);
  }
  if (progressCallback) progressCallback(0.0f);

  std::string error;
  if (!decoder.init(job, &error)) {
    std::lock_guard<std::mutex> lock(cache->mutex);
    cache->running = false;
    cache->ready = false;
    cache->progress = 0.0f;
    cache->frameCount = 0;
    cache->error = error.empty() ? "Offline melody analysis decoder init failed."
                                 : error;
    return;
  }

  std::string neuralError;
  if (!neuralPitchInit(&neural, job.sourceSampleRate, job.channels,
                       &neuralError)) {
    std::lock_guard<std::mutex> lock(cache->mutex);
    cache->running = false;
    cache->ready = false;
    cache->progress = 0.0f;
    cache->frameCount = 0;
    cache->frames.clear();
    cache->error = neuralError.empty() ? "Neural melody analyzer init failed."
                                       : neuralError;
    return;
  }

  const uint64_t totalFrames = decoder.getTotalFrames();
  const bool totalKnown = totalFrames > 0;
  std::vector<float> buffer(
      static_cast<size_t>(kAnalysisChunkFrames) * std::max<uint32_t>(1u, job.channels));
  std::vector<MelodyRawAnalysisPoint> rawFrames;
  if (totalKnown) {
    rawFrames.reserve(static_cast<size_t>(totalFrames / kCaptureStrideFrames) + 8);
  }

  uint64_t processedFrames = 0;
  uint64_t nextCaptureFrame = 0;
  bool seenNeuralFrame = false;
  bool hasAlignedNeuralFrame = false;
  NeuralPitchFrame alignedNeuralFrame{};
  std::deque<NeuralPitchFrame> pendingNeuralFrames;

  if (job.leadInFrames > 0) {
    MelodyOfflineFrame silence;
    silence.frequencyHz = 0.0f;
    silence.confidence = 0.0f;
    silence.midiNote = -1;
    rawFrames.push_back({0, silence, makeSilenceEmissionLog()});
    storePoint(cache, 0, silence);
    nextCaptureFrame = std::min(kCaptureStrideFrames, job.leadInFrames);
  }

  while (!cache->stopRequested.load(std::memory_order_acquire)) {
    uint64_t framesToRead = kAnalysisChunkFrames;
    if (totalKnown && totalFrames > processedFrames) {
      uint64_t remaining = totalFrames - processedFrames;
      if (remaining < framesToRead) framesToRead = remaining;
    }
    if (framesToRead == 0) {
      break;
    }

    uint64_t readFrames = 0;
    if (!decoder.readFrames(buffer.data(),
                           static_cast<uint32_t>(framesToRead),
                           &readFrames)) {
      break;
    }
    if (readFrames == 0) {
      break;
    }

    neuralPitchUpdate(&neural, buffer.data(), static_cast<uint32_t>(readFrames),
                      job.channels);
    {
      NeuralPitchFrame produced{};
      while (neuralPitchPopFrame(&neural, &produced)) {
        pendingNeuralFrames.push_back(produced);
      }
    }
    processedFrames += readFrames;

    const uint64_t decodedFramePos = job.leadInFrames + processedFrames;
    while (!cache->stopRequested.load(std::memory_order_acquire) &&
           nextCaptureFrame <= decodedFramePos) {
      while (!pendingNeuralFrames.empty() &&
             pendingNeuralFrames.front().sourceFrame <= nextCaptureFrame) {
        alignedNeuralFrame = pendingNeuralFrames.front();
        pendingNeuralFrames.pop_front();
        hasAlignedNeuralFrame = true;
      }

      MelodyOfflineFrame point;
      std::array<float, kOfflineStateCount> emission{};
      if (nextCaptureFrame < job.leadInFrames) {
        point.frequencyHz = 0.0f;
        point.confidence = 0.0f;
        point.midiNote = -1;
        emission = makeSilenceEmissionLog();
      } else {
        if (hasAlignedNeuralFrame) {
          emission = buildEmissionForPoint(&alignedNeuralFrame.posterior);
          float neuralVoicing =
              std::clamp(alignedNeuralFrame.voicingProb, 0.0f, 1.0f);
          seenNeuralFrame = true;
          if (alignedNeuralFrame.midiNote >= 0 &&
              alignedNeuralFrame.midiNote <= 127) {
            point.midiNote = alignedNeuralFrame.midiNote;
            point.frequencyHz = alignedNeuralFrame.frequencyHz;
            point.confidence = neuralVoicing;
          } else {
            point.frequencyHz = 0.0f;
            point.midiNote = -1;
            point.confidence = neuralVoicing;
          }
        } else {
          point.frequencyHz = 0.0f;
          point.confidence = 0.0f;
          point.midiNote = -1;
          emission = makeSilenceEmissionLog();
        }
      }
      rawFrames.push_back({nextCaptureFrame, point, emission});
      storePoint(cache, nextCaptureFrame, point);
      nextCaptureFrame += kCaptureStrideFrames;
    }

    float progress = 0.0f;
    if (totalKnown) {
      progress = totalFrames > 0
                     ? static_cast<float>(processedFrames) /
                           static_cast<float>(totalFrames)
                     : 0.0f;
      if (progress > 1.0f) progress = 1.0f;
    }
    {
      std::lock_guard<std::mutex> lock(cache->mutex);
      updateProgressLocked(cache, cache->frames.size(),
                          totalKnown ? progress : cache->progress);
    }
    if (progressCallback && totalKnown) {
      progressCallback(progress);
    }

    if (totalKnown && processedFrames >= totalFrames) {
      break;
    }
  }

  const bool stopped = cache->stopRequested.load(std::memory_order_acquire);
  std::vector<MelodyAnalysisPoint> refinedFrames;
  if (!stopped && !rawFrames.empty()) {
    refinedFrames = refineOfflineFrames(rawFrames);
  }

  neuralPitchUninit(&neural);

  {
    std::lock_guard<std::mutex> lock(cache->mutex);
    cache->running = false;
    cache->ready = !stopped && seenNeuralFrame;
    if (!stopped && seenNeuralFrame && !refinedFrames.empty()) {
      cache->frames = std::move(refinedFrames);
      cache->error.clear();
    } else if (!stopped && !seenNeuralFrame) {
      cache->frames.clear();
      cache->error = "Neural melody analyzer produced no frames.";
    } else if (stopped) {
      cache->error.clear();
    }
    cache->progress =
        stopped ? cache->progress : (seenNeuralFrame ? 1.0f : 0.0f);
    cache->frameCount = cache->frames.size();
  }
  if (progressCallback) {
    float done = 0.0f;
    {
      std::lock_guard<std::mutex> lock(cache->mutex);
      done = std::clamp(cache->progress, 0.0f, 1.0f);
    }
    progressCallback(done);
  }
}

void analyzeTrack(MelodyOfflineJob job) {
  analyzeTrackForCache(&gCache, std::move(job), {});
}
}  // namespace

void melodyOfflineStart(const std::filesystem::path& file, int trackIndex,
                       uint32_t sourceSampleRate, uint32_t channels,
                       uint64_t leadInFrames,
                       const KssPlaybackOptions& kssOptions,
                       const NsfPlaybackOptions& nsfOptions,
                       const VgmPlaybackOptions& vgmOptions,
                       const std::unordered_map<uint32_t, VgmDeviceOptions>&
                           vgmDeviceOverrides) {
  if (file.empty() || !std::filesystem::exists(file)) {
    return;
  }
  if (sourceSampleRate == 0 || channels == 0 || channels > 2) {
    return;
  }

  stopWorker();

  MelodyOfflineJob job;
  job.file = file;
  job.trackIndex = trackIndex;
  job.sourceSampleRate = sourceSampleRate;
  job.channels = channels;
  job.leadInFrames = leadInFrames;
  job.kssOptions = kssOptions;
  job.nsfOptions = nsfOptions;
  job.vgmOptions = vgmOptions;
  job.vgmDeviceOverrides = vgmDeviceOverrides;

  setCacheState(&gCache, job);

  gCache.worker = std::thread([job = std::move(job)]() { analyzeTrack(std::move(job)); });
}

void melodyOfflineStop() {
  stopWorker();
}

MelodyOfflineFrame melodyOfflineGetFrame(double timeSec) {
  if (!std::isfinite(timeSec) || timeSec < 0.0) {
    return {};
  }

  std::lock_guard<std::mutex> lock(gCache.mutex);
  if (gCache.frames.empty() || gCache.sourceSampleRate == 0) {
    return {};
  }

  double safeTime = std::max(0.0, timeSec);
  uint64_t targetFrame = static_cast<uint64_t>(
      std::llround(safeTime * static_cast<double>(gCache.sourceSampleRate)));

  auto it = std::lower_bound(
      gCache.frames.begin(), gCache.frames.end(), targetFrame,
      [](const MelodyAnalysisPoint& lhs, uint64_t rhs) {
        return lhs.frame < rhs;
      });

  if (it == gCache.frames.end()) {
    return gCache.frames.back().frameData;
  }
  if (it == gCache.frames.begin()) {
    return it->frameData;
  }
  if (it->frame != targetFrame) {
    --it;
  }
  return it->frameData;
}

MelodyOfflineAnalysisState melodyOfflineGetState() {
  std::lock_guard<std::mutex> lock(gCache.mutex);
  MelodyOfflineAnalysisState state;
  state.ready = gCache.ready;
  state.running = gCache.running;
  state.progress = gCache.progress;
  state.frameCount = gCache.frameCount;
  state.error = gCache.error;
  return state;
}

bool melodyOfflineAnalyzeToFile(
    const std::filesystem::path& file, int trackIndex, uint32_t sourceSampleRate,
    uint32_t channels, uint64_t leadInFrames,
    const KssPlaybackOptions& kssOptions,
    const NsfPlaybackOptions& nsfOptions,
    const VgmPlaybackOptions& vgmOptions,
    const std::unordered_map<uint32_t, VgmDeviceOptions>& vgmDeviceOverrides,
    const std::filesystem::path& outputFile,
    const std::function<void(float)>& progressCallback, std::string* error) {
  if (file.empty() || !std::filesystem::exists(file)) {
    if (error) *error = "Input file not found.";
    return false;
  }
  if (sourceSampleRate == 0 || channels == 0 || channels > 2) {
    if (error) *error = "Invalid analysis audio format.";
    return false;
  }
  if (outputFile.empty()) {
    if (error) *error = "Output path is empty.";
    return false;
  }

  MelodyOfflineJob job;
  job.file = file;
  job.trackIndex = trackIndex;
  job.sourceSampleRate = sourceSampleRate;
  job.channels = channels;
  job.leadInFrames = leadInFrames;
  job.kssOptions = kssOptions;
  job.nsfOptions = nsfOptions;
  job.vgmOptions = vgmOptions;
  job.vgmDeviceOverrides = vgmDeviceOverrides;

  MelodyOfflineCache localCache;
  localCache.stopRequested.store(false, std::memory_order_release);
  analyzeTrackForCache(&localCache, std::move(job), progressCallback);

  std::vector<MelodyAnalysisPoint> frames;
  uint32_t resultSampleRate = sourceSampleRate;
  std::string localError;
  {
    std::lock_guard<std::mutex> lock(localCache.mutex);
    resultSampleRate = localCache.sourceSampleRate;
    if (!localCache.ready || localCache.frames.empty()) {
      localError = localCache.error;
    } else {
      frames = localCache.frames;
    }
  }

  if (frames.empty()) {
    if (error) {
      *error = localError.empty() ? "Melody analysis produced no frames."
                                  : localError;
    }
    return false;
  }
  return writeMelodyFramesToFile(outputFile, resultSampleRate, frames, error);
}
