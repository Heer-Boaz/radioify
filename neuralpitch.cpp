#include "neuralpitch.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "ui_helpers.h"

#ifndef RADIOIFY_HAS_ONNXRUNTIME
#define RADIOIFY_HAS_ONNXRUNTIME 0
#endif

#if RADIOIFY_HAS_ONNXRUNTIME
#if __has_include(<onnxruntime/core/session/onnxruntime_c_api.h>)
#include <onnxruntime/core/session/onnxruntime_c_api.h>
#elif __has_include(<onnxruntime_c_api.h>)
#include <onnxruntime_c_api.h>
#else
#error "ONNX Runtime headers not found: expected onnxruntime/core/session/onnxruntime_c_api.h"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#endif

namespace {
constexpr uint32_t kModelSampleRate = 16000u;
constexpr int kModelFrameSize = 1024;
constexpr int kModelHopSize = 256;
constexpr int kCrepeBins = 360;
constexpr float kCrepeCentsPerBin = 20.0f;
constexpr float kCrepeCentsOffset = 1997.3794084376191f;
constexpr float kProbFloor = 1.0e-9f;
constexpr int kRingMax = 4096;
constexpr int kProducedFrameQueueMax = 8192;

float clampf(float value, float low, float high) {
  return std::max(low, std::min(high, value));
}

float midiToFrequency(float midi) {
  return 440.0f * std::exp2((midi - 69.0f) / 12.0f);
}

float frequencyToMidi(float hz) {
  if (!std::isfinite(hz) || hz <= 0.0f) {
    return -1.0f;
  }
  return 69.0f + 12.0f * std::log2(hz / 440.0f);
}

float crepeBinToMidi(int bin) {
  if (bin < 0 || bin >= kCrepeBins) return -1.0f;
  float cents =
      kCrepeCentsOffset + static_cast<float>(bin) * kCrepeCentsPerBin;
  float hz = 10.0f * std::exp2(cents / 1200.0f);
  return frequencyToMidi(hz);
}

int midiToState(float midi) {
  int rounded = static_cast<int>(std::lround(midi));
  if (rounded < kMelodyMidiMin || rounded > kMelodyMidiMax) {
    return kMelodyUnvoicedState;
  }
  return rounded - kMelodyMidiMin + 1;
}

float stableSigmoid(float x) {
  if (!std::isfinite(x)) return 0.0f;
  if (x >= 0.0f) {
    float z = std::exp(-x);
    return 1.0f / (1.0f + z);
  }
  float z = std::exp(x);
  return z / (1.0f + z);
}

void appendSchemaRegistrationHint(std::string* error) {
  if (!error || error->empty()) return;
  if (error->find("Trying to register schema") == std::string::npos ||
      error->find("already registered") == std::string::npos ||
      error->find("ONNX_DISABLE_STATIC_REGISTRATION") != std::string::npos) {
    return;
  }
  *error +=
      " Fix: rebuild ONNX/ONNX Runtime with "
      "ONNX_DISABLE_STATIC_REGISTRATION=ON "
      "(build.ps1 now sets this via VCPKG_CMAKE_CONFIGURE_OPTIONS).";
}

void normalizePositiveInplace(std::vector<float>* values) {
  if (!values || values->empty()) return;
  float sum = 0.0f;
  for (float& v : *values) {
    if (!std::isfinite(v) || v < 0.0f) {
      v = 0.0f;
    }
    sum += v;
  }
  if (!std::isfinite(sum) || sum <= 0.0f) {
    float uniform = 1.0f / static_cast<float>(values->size());
    for (float& v : *values) v = uniform;
    return;
  }
  for (float& v : *values) {
    v /= sum;
  }
}

bool convertPitchOutputToCrepeBins(const std::vector<float>& raw,
                                   std::vector<float>* pitchBins) {
  if (!pitchBins) return false;
  pitchBins->clear();
  if (raw.empty()) return false;

  if (raw.size() == static_cast<size_t>(kCrepeBins)) {
    *pitchBins = raw;
    return true;
  }

  if (raw.size() > static_cast<size_t>(kCrepeBins)) {
    if (raw.size() % static_cast<size_t>(kCrepeBins) == 0) {
      size_t chunks = raw.size() / static_cast<size_t>(kCrepeBins);
      size_t bestChunk = 0;
      float bestPeak = -std::numeric_limits<float>::infinity();
      for (size_t chunk = 0; chunk < chunks; ++chunk) {
        size_t base = chunk * static_cast<size_t>(kCrepeBins);
        float peak = -std::numeric_limits<float>::infinity();
        for (int i = 0; i < kCrepeBins; ++i) {
          float v = raw[base + static_cast<size_t>(i)];
          if (std::isfinite(v) && v > peak) peak = v;
        }
        if (peak > bestPeak) {
          bestPeak = peak;
          bestChunk = chunk;
        }
      }
      size_t base = bestChunk * static_cast<size_t>(kCrepeBins);
      pitchBins->assign(raw.begin() + static_cast<std::ptrdiff_t>(base),
                        raw.begin() + static_cast<std::ptrdiff_t>(base + kCrepeBins));
      return true;
    }

    size_t start = (raw.size() - static_cast<size_t>(kCrepeBins)) / 2u;
    pitchBins->assign(raw.begin() + static_cast<std::ptrdiff_t>(start),
                      raw.begin() + static_cast<std::ptrdiff_t>(start + kCrepeBins));
    return true;
  }

  if (raw.size() < 24) {
    return false;
  }
  pitchBins->assign(static_cast<size_t>(kCrepeBins), 0.0f);
  const size_t srcLast = raw.size() - 1;
  const size_t dstLast = static_cast<size_t>(kCrepeBins - 1);
  for (int i = 0; i < kCrepeBins; ++i) {
    double pos = (static_cast<double>(i) * static_cast<double>(srcLast)) /
                 static_cast<double>(dstLast);
    size_t i0 = static_cast<size_t>(std::floor(pos));
    size_t i1 = std::min(srcLast, i0 + 1);
    float frac = static_cast<float>(pos - static_cast<double>(i0));
    float v0 = raw[i0];
    float v1 = raw[i1];
    (*pitchBins)[static_cast<size_t>(i)] = v0 + (v1 - v0) * frac;
  }
  return true;
}

void normalizePosterior(std::array<float, kMelodyAnalyzerStates>* posterior) {
  if (!posterior) return;
  float sum = 0.0f;
  for (float p : *posterior) {
    if (std::isfinite(p) && p > 0.0f) {
      sum += p;
    }
  }
  if (sum <= 0.0f) {
    posterior->fill(0.0f);
    (*posterior)[kMelodyUnvoicedState] = 1.0f;
    return;
  }
  for (float& p : *posterior) {
    if (!std::isfinite(p) || p < 0.0f) {
      p = 0.0f;
    } else {
      p /= sum;
    }
  }
}

void buildPosteriorFromCrepe(const std::vector<float>& pitchBins,
                             float voicingProb,
                             std::array<float, kMelodyAnalyzerStates>* out,
                             float* outMidi,
                             int* outMidiNote) {
  if (!out) return;
  out->fill(0.0f);

  if (pitchBins.empty()) {
    (*out)[kMelodyUnvoicedState] = 1.0f;
    if (outMidi) *outMidi = -1.0f;
    if (outMidiNote) *outMidiNote = -1;
    return;
  }

  float voicedMass = clampf(voicingProb, 0.0f, 1.0f);
  float unvoicedMass = 1.0f - voicedMass;
  (*out)[kMelodyUnvoicedState] = unvoicedMass;

  int bestIdx = 0;
  float bestProb = pitchBins[0];
  for (int i = 1; i < static_cast<int>(pitchBins.size()); ++i) {
    if (pitchBins[static_cast<size_t>(i)] > bestProb) {
      bestProb = pitchBins[static_cast<size_t>(i)];
      bestIdx = i;
    }
  }

  float weightedMidi = 0.0f;
  float weightSum = 0.0f;
  int left = std::max(0, bestIdx - 5);
  int right = std::min(static_cast<int>(pitchBins.size()) - 1, bestIdx + 5);
  for (int i = left; i <= right; ++i) {
    float p = pitchBins[static_cast<size_t>(i)];
    float midi = crepeBinToMidi(i);
    if (!std::isfinite(midi) || midi <= 0.0f) continue;
    weightedMidi += p * midi;
    weightSum += p;
  }
  if (weightSum <= 0.0f) {
    weightedMidi = crepeBinToMidi(bestIdx);
    weightSum = 1.0f;
  } else {
    weightedMidi /= weightSum;
  }

  float localMass = 0.0f;
  int localLeft = std::max(0, bestIdx - 6);
  int localRight = std::min(static_cast<int>(pitchBins.size()) - 1, bestIdx + 6);
  for (int i = localLeft; i <= localRight; ++i) {
    float p = pitchBins[static_cast<size_t>(i)];
    if (p > 0.0f) localMass += p;
  }
  if (localMass <= 0.0f) {
    localMass = std::max(kProbFloor, bestProb);
    localLeft = bestIdx;
    localRight = bestIdx;
  }

  for (int i = localLeft; i <= localRight; ++i) {
    float p = pitchBins[static_cast<size_t>(i)];
    if (p <= 0.0f) continue;
    float midi = crepeBinToMidi(i);
    if (!std::isfinite(midi) || midi <= 0.0f) continue;
    int state = midiToState(midi);
    if (state == kMelodyUnvoicedState) continue;
    (*out)[state] += voicedMass * (p / localMass);
  }

  normalizePosterior(out);

  if (outMidi) {
    *outMidi = weightedMidi;
  }
  if (outMidiNote) {
    int rounded = static_cast<int>(std::lround(weightedMidi));
    if (rounded < kMelodyMidiMin || rounded > kMelodyMidiMax || voicedMass < 0.05f) {
      *outMidiNote = -1;
    } else {
      *outMidiNote = rounded;
    }
  }
}

#if RADIOIFY_HAS_ONNXRUNTIME
struct OrtReleaser {
  const OrtApi* api = nullptr;
  void operator()(OrtStatus* status) const {
    if (api && status) api->ReleaseStatus(status);
  }
};

using OrtStatusPtr = std::unique_ptr<OrtStatus, OrtReleaser>;

OrtStatusPtr wrapStatus(const OrtApi* api, OrtStatus* status) {
  return OrtStatusPtr(status, OrtReleaser{api});
}

bool checkStatus(const OrtApi* api, OrtStatus* status, std::string* error,
                 const char* context) {
  auto s = wrapStatus(api, status);
  if (!s) return true;
  if (error) {
    const char* msg = api->GetErrorMessage(s.get());
    if (msg && msg[0] != '\0') {
      *error = std::string(context) + ": " + msg;
    } else {
      *error = std::string(context) + ": unknown ONNX error";
    }
  }
  return false;
}

struct TensorShapeSummary {
  bool hasElementCount = false;
  size_t elementCount = 0;
  bool hasKnownProduct = false;
  size_t knownProduct = 0;
  int64_t maxKnownDim = 0;
  int64_t lastKnownDim = 0;
};

std::string asciiLower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

void summarizeTensorShape(const OrtApi* api,
                          const OrtTensorTypeAndShapeInfo* tensorInfo,
                          TensorShapeSummary* out) {
  if (!api || !tensorInfo || !out) return;
  out->hasElementCount = false;
  out->elementCount = 0;
  out->hasKnownProduct = false;
  out->knownProduct = 0;
  out->maxKnownDim = 0;
  out->lastKnownDim = 0;

  size_t elemCount = 0;
  OrtStatus* elemStatus = api->GetTensorShapeElementCount(tensorInfo, &elemCount);
  if (!elemStatus) {
    out->hasElementCount = true;
    out->elementCount = elemCount;
  } else {
    api->ReleaseStatus(elemStatus);
  }

  size_t dimCount = 0;
  OrtStatus* dimCountStatus = api->GetDimensionsCount(tensorInfo, &dimCount);
  if (dimCountStatus) {
    api->ReleaseStatus(dimCountStatus);
    return;
  }
  if (dimCount == 0) return;

  std::vector<int64_t> dims(dimCount, 0);
  OrtStatus* dimsStatus = api->GetDimensions(tensorInfo, dims.data(), dimCount);
  if (dimsStatus) {
    api->ReleaseStatus(dimsStatus);
    return;
  }

  bool allKnownPositive = true;
  size_t product = 1;
  for (int64_t dim : dims) {
    if (dim > 0) {
      if (dim > out->maxKnownDim) out->maxKnownDim = dim;
      out->lastKnownDim = dim;
      product *= static_cast<size_t>(dim);
    } else {
      allKnownPositive = false;
    }
  }
  if (allKnownPositive) {
    out->hasKnownProduct = true;
    out->knownProduct = product;
  }
}

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& text) {
  if (text.empty()) return {};
  int wideLen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (wideLen <= 0) return {};
  std::wstring out(static_cast<size_t>(wideLen - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), wideLen);
  return out;
}
#endif
#endif

std::filesystem::path resolveModelPath() {
  if (const auto envPath = getEnvString("RADIOIFY_NEURAL_PITCH_MODEL")) {
    std::filesystem::path p(*envPath);
    if (std::filesystem::exists(p)) return p;
  }

  const std::array<std::filesystem::path, 4> candidates = {
      std::filesystem::current_path() / "models" / "melody_pitch.onnx",
      std::filesystem::current_path() / "models" / "crepe_small.onnx",
      std::filesystem::current_path() / "melody_pitch.onnx",
      std::filesystem::current_path() / "crepe_small.onnx",
  };
  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate)) return candidate;
  }
  return {};
}

void trimRings(NeuralPitchState* state) {
  if (!state) return;
  while (static_cast<int>(state->ring16k.size()) > kRingMax) {
    state->ring16k.pop_front();
  }
}

size_t resampleOne(NeuralPitchState* state, float monoSample) {
  if (!state || state->sourceStep <= 0.0) return 0;
  state->sourceBuffer.push_back(monoSample);
  size_t emitted = 0;

  while (true) {
    if (state->sourcePos + 1.0 >=
        static_cast<double>(state->sourceBuffer.size())) {
      break;
    }
    size_t i0 = static_cast<size_t>(state->sourcePos);
    float frac = static_cast<float>(state->sourcePos - static_cast<double>(i0));
    float s0 = state->sourceBuffer[i0];
    float s1 = state->sourceBuffer[i0 + 1];
    state->ring16k.push_back(s0 + (s1 - s0) * frac);
    ++emitted;
    state->sourcePos += state->sourceStep;
  }

  if (state->sourceBuffer.size() > 1) {
    size_t drop = static_cast<size_t>(std::floor(state->sourcePos));
    if (drop >= state->sourceBuffer.size()) {
      drop = state->sourceBuffer.size() - 1;
    }
    if (drop > 0) {
      state->sourceBuffer.erase(state->sourceBuffer.begin(),
                                state->sourceBuffer.begin() + drop);
      state->sourcePos -= static_cast<double>(drop);
      if (state->sourcePos < 0.0) state->sourcePos = 0.0;
    }
  }
  return emitted;
}

}  // namespace

struct NeuralPitchState::RuntimeOpaque {
#if RADIOIFY_HAS_ONNXRUNTIME
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  OrtSessionOptions* sessionOptions = nullptr;
  OrtSession* session = nullptr;
  OrtMemoryInfo* memoryInfo = nullptr;
  OrtAllocator* allocator = nullptr;
  std::string inputName;
  std::vector<std::string> outputNames;
  std::vector<const char*> outputNamePtrs;
  size_t pitchOutputIndex = static_cast<size_t>(-1);
  size_t pitchOutputCountHint = 0;
  size_t voicingOutputIndex = static_cast<size_t>(-1);
  bool hasVoicingOutput = false;

  ~RuntimeOpaque() {
    if (!api) return;
    if (session) {
      api->ReleaseSession(session);
      session = nullptr;
    }
    if (sessionOptions) {
      api->ReleaseSessionOptions(sessionOptions);
      sessionOptions = nullptr;
    }
    if (memoryInfo) {
      api->ReleaseMemoryInfo(memoryInfo);
      memoryInfo = nullptr;
    }
    if (env) {
      api->ReleaseEnv(env);
      env = nullptr;
    }
  }
#endif
};

void neuralPitchReset(NeuralPitchState* state) {
  if (!state) return;
  state->active = false;
  state->hasLatest = false;
  state->sourceSampleRate = 0;
  state->channels = 0;
  state->sourcePos = 0.0;
  state->sourceStep = 0.0;
  state->sourceFramesProcessed = 0;
  state->sourceBuffer.clear();
  state->ring16k.clear();
  state->samplesSinceHop = 0;
  state->producedFrames.clear();
  state->latest = {};
}

bool neuralPitchInit(NeuralPitchState* state, uint32_t sourceSampleRate,
                     uint32_t channels, std::string* error) {
  if (!state || sourceSampleRate == 0 || channels == 0) {
    if (error) *error = "Neural pitch init: invalid arguments.";
    return false;
  }

  neuralPitchReset(state);
  state->sourceSampleRate = sourceSampleRate;
  state->channels = channels;
  state->sourceStep =
      static_cast<double>(sourceSampleRate) / static_cast<double>(kModelSampleRate);

#if !RADIOIFY_HAS_ONNXRUNTIME
  if (error) {
    *error = "Neural pitch unavailable: ONNX Runtime not linked.";
  }
  return false;
#else
  auto runtime = std::make_unique<NeuralPitchState::RuntimeOpaque>();
  runtime->api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
  if (!runtime->api) {
    if (error) *error = "Neural pitch init: failed to get ONNX API.";
    return false;
  }

  std::filesystem::path modelPath = resolveModelPath();
  if (modelPath.empty()) {
    if (error) {
      *error = "Neural pitch model not found. Set RADIOIFY_NEURAL_PITCH_MODEL or place models/melody_pitch.onnx.";
    }
    return false;
  }

  if (!checkStatus(runtime->api,
                   runtime->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING,
                                           "radioify_neural_pitch",
                                           &runtime->env),
                   error,
                   "CreateEnv")) {
    return false;
  }
  if (!checkStatus(runtime->api,
                   runtime->api->CreateSessionOptions(&runtime->sessionOptions),
                   error,
                   "CreateSessionOptions")) {
    return false;
  }

  runtime->api->SetIntraOpNumThreads(runtime->sessionOptions, 1);
  runtime->api->SetInterOpNumThreads(runtime->sessionOptions, 1);
  runtime->api->SetSessionGraphOptimizationLevel(runtime->sessionOptions,
                                                  ORT_ENABLE_EXTENDED);

#ifdef _WIN32
  std::wstring widePath = utf8ToWide(modelPath.u8string());
  if (widePath.empty()) {
    if (error) *error = "CreateSession: invalid model path encoding.";
    return false;
  }
  if (!checkStatus(runtime->api,
                   runtime->api->CreateSession(runtime->env,
                                               widePath.c_str(),
                                               runtime->sessionOptions,
                                               &runtime->session),
                   error,
                   "CreateSession")) {
    appendSchemaRegistrationHint(error);
    return false;
  }
#else
  std::string modelPathUtf8 = modelPath.u8string();
  if (!checkStatus(runtime->api,
                   runtime->api->CreateSession(runtime->env,
                                               modelPathUtf8.c_str(),
                                               runtime->sessionOptions,
                                               &runtime->session),
                   error,
                   "CreateSession")) {
    appendSchemaRegistrationHint(error);
    return false;
  }
#endif

  if (!checkStatus(runtime->api,
                   runtime->api->CreateCpuMemoryInfo(OrtArenaAllocator,
                                                     OrtMemTypeDefault,
                                                     &runtime->memoryInfo),
                   error,
                   "CreateCpuMemoryInfo")) {
    return false;
  }

  if (!checkStatus(runtime->api,
                   runtime->api->GetAllocatorWithDefaultOptions(&runtime->allocator),
                   error,
                   "GetAllocatorWithDefaultOptions")) {
    return false;
  }

  size_t inputCount = 0;
  if (!checkStatus(runtime->api,
                   runtime->api->SessionGetInputCount(runtime->session, &inputCount),
                   error,
                   "SessionGetInputCount") ||
      inputCount == 0) {
    if (error && error->empty()) {
      *error = "Model has no inputs.";
    }
    return false;
  }

  char* inputName = nullptr;
  if (!checkStatus(runtime->api,
                   runtime->api->SessionGetInputName(runtime->session, 0,
                                                     runtime->allocator,
                                                     &inputName),
                   error,
                   "SessionGetInputName") ||
      !inputName) {
    return false;
  }
  runtime->inputName = inputName;
  runtime->allocator->Free(runtime->allocator, inputName);

  size_t outputCount = 0;
  if (!checkStatus(runtime->api,
                   runtime->api->SessionGetOutputCount(runtime->session,
                                                       &outputCount),
                   error,
                   "SessionGetOutputCount") ||
      outputCount == 0) {
    if (error && error->empty()) {
      *error = "Model has no outputs.";
    }
    return false;
  }

  runtime->outputNames.reserve(outputCount);
  runtime->outputNamePtrs.reserve(outputCount);

  for (size_t i = 0; i < outputCount; ++i) {
    char* outputName = nullptr;
    if (!checkStatus(runtime->api,
                     runtime->api->SessionGetOutputName(runtime->session, i,
                                                        runtime->allocator,
                                                        &outputName),
                     error,
                     "SessionGetOutputName") ||
        !outputName) {
      return false;
    }
    runtime->outputNames.emplace_back(outputName);
    runtime->allocator->Free(runtime->allocator, outputName);
  }
  for (const std::string& name : runtime->outputNames) {
    runtime->outputNamePtrs.push_back(name.c_str());
  }

  int bestPitchScore = std::numeric_limits<int>::min();
  int bestVoicingScore = std::numeric_limits<int>::min();
  for (size_t i = 0; i < outputCount; ++i) {
    OrtTypeInfo* typeInfo = nullptr;
    if (!checkStatus(runtime->api,
                     runtime->api->SessionGetOutputTypeInfo(runtime->session, i,
                                                            &typeInfo),
                     error,
                     "SessionGetOutputTypeInfo")) {
      return false;
    }
    const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
    if (!checkStatus(runtime->api,
                     runtime->api->CastTypeInfoToTensorInfo(typeInfo,
                                                            &tensorInfo),
                     error,
                     "CastTypeInfoToTensorInfo")) {
      runtime->api->ReleaseTypeInfo(typeInfo);
      return false;
    }
    if (!tensorInfo) {
      runtime->api->ReleaseTypeInfo(typeInfo);
      if (error) {
        *error = "CastTypeInfoToTensorInfo: tensor info is null.";
      }
      return false;
    }

    TensorShapeSummary shapeSummary;
    summarizeTensorShape(runtime->api, tensorInfo, &shapeSummary);
    runtime->api->ReleaseTypeInfo(typeInfo);

    size_t countHint = 0;
    if (shapeSummary.hasElementCount) {
      countHint = shapeSummary.elementCount;
    } else if (shapeSummary.hasKnownProduct) {
      countHint = shapeSummary.knownProduct;
    } else if (shapeSummary.maxKnownDim > 0) {
      countHint = static_cast<size_t>(shapeSummary.maxKnownDim);
    }

    std::string outputNameLower = asciiLower(runtime->outputNames[i]);
    bool nameSuggestsPitch =
        outputNameLower.find("pitch") != std::string::npos ||
        outputNameLower.find("f0") != std::string::npos ||
        outputNameLower.find("note") != std::string::npos ||
        outputNameLower.find("prob") != std::string::npos;
    bool nameSuggestsVoicing =
        outputNameLower.find("voic") != std::string::npos ||
        outputNameLower.find("vuv") != std::string::npos;

    int pitchScore = std::numeric_limits<int>::min() / 4;
    if (countHint > 0) {
      int diff = static_cast<int>((countHint > static_cast<size_t>(kCrepeBins))
                                      ? (countHint - static_cast<size_t>(kCrepeBins))
                                      : (static_cast<size_t>(kCrepeBins) - countHint));
      pitchScore = 2000 - diff;
      if (countHint == static_cast<size_t>(kCrepeBins)) pitchScore += 2500;
      if (countHint >= 256 && countHint <= 1024) pitchScore += 600;
      if (countHint <= 16) pitchScore -= 3000;
    } else {
      pitchScore = -4000;
    }
    if (nameSuggestsPitch) pitchScore += 1000;
    if (nameSuggestsVoicing) pitchScore -= 800;
    if (pitchScore > bestPitchScore) {
      bestPitchScore = pitchScore;
      runtime->pitchOutputIndex = i;
      runtime->pitchOutputCountHint = countHint;
    }

    int voicingScore = std::numeric_limits<int>::min() / 4;
    if (countHint > 0 && countHint <= 8) {
      voicingScore = 1200 - static_cast<int>(countHint);
    } else if (shapeSummary.maxKnownDim > 0 && shapeSummary.maxKnownDim <= 8) {
      voicingScore = 1000 - static_cast<int>(shapeSummary.maxKnownDim);
    } else {
      voicingScore = -3000;
    }
    if (nameSuggestsVoicing) {
      voicingScore += 1500;
    }
    if (nameSuggestsPitch) {
      voicingScore -= 600;
    }
    if (voicingScore > bestVoicingScore) {
      bestVoicingScore = voicingScore;
      runtime->voicingOutputIndex = i;
    }
  }

  if (runtime->pitchOutputIndex == static_cast<size_t>(-1)) {
    if (error) {
      *error = "Neural pitch model output mismatch: no pitch-like output found.";
    }
    return false;
  }
  runtime->hasVoicingOutput =
      runtime->voicingOutputIndex != static_cast<size_t>(-1) &&
      runtime->voicingOutputIndex != runtime->pitchOutputIndex &&
      bestVoicingScore > 0;

  state->runtime = runtime.release();
  state->active = true;
  return true;
#endif
}

void neuralPitchUninit(NeuralPitchState* state) {
  if (!state) return;
  if (state->runtime) {
    delete state->runtime;
    state->runtime = nullptr;
  }
#if !RADIOIFY_HAS_ONNXRUNTIME
  state->runtime = nullptr;
#endif
  neuralPitchReset(state);
}

bool neuralPitchIsActive(const NeuralPitchState* state) {
  return state && state->active;
}

bool neuralPitchHasFrame(const NeuralPitchState* state) {
  return state && state->active && state->hasLatest;
}

NeuralPitchFrame neuralPitchGetLatest(const NeuralPitchState* state) {
  if (!state) return {};
  return state->latest;
}

bool neuralPitchPopFrame(NeuralPitchState* state, NeuralPitchFrame* out) {
  if (!state || !out || !state->active || state->producedFrames.empty()) {
    return false;
  }
  *out = state->producedFrames.front();
  state->producedFrames.pop_front();
  return true;
}

void neuralPitchUpdate(NeuralPitchState* state, const float* samples,
                       uint32_t frameCount, uint32_t channels) {
  if (!state || !state->active || !samples || frameCount == 0 || channels == 0) {
    return;
  }

#if !RADIOIFY_HAS_ONNXRUNTIME
  (void)state;
  (void)samples;
  (void)frameCount;
  (void)channels;
#else
  auto* runtime = state->runtime;
  if (!runtime || !runtime->api || !runtime->session || !runtime->memoryInfo) {
    return;
  }

  for (uint32_t frame = 0; frame < frameCount; ++frame) {
    float mono = samples[static_cast<size_t>(frame) * channels];
    if (channels > 1) {
      for (uint32_t ch = 1; ch < channels; ++ch) {
        mono += samples[static_cast<size_t>(frame) * channels + ch];
      }
      mono /= static_cast<float>(channels);
    }
    state->sourceFramesProcessed++;
    size_t emitted = resampleOne(state, mono);
    state->samplesSinceHop += static_cast<int>(emitted);

    trimRings(state);

    while (state->samplesSinceHop >= kModelHopSize &&
           static_cast<int>(state->ring16k.size()) >= kModelFrameSize) {
      std::vector<float> input(static_cast<size_t>(kModelFrameSize), 0.0f);
      for (int i = 0; i < kModelFrameSize; ++i) {
        input[static_cast<size_t>(i)] =
            state->ring16k[state->ring16k.size() -
                           static_cast<size_t>(kModelFrameSize) +
                           static_cast<size_t>(i)];
      }

      int64_t inputShape[2] = {1, kModelFrameSize};
      OrtValue* inputTensor = nullptr;
      OrtStatus* inStatus = runtime->api->CreateTensorWithDataAsOrtValue(
          runtime->memoryInfo,
          input.data(),
          input.size() * sizeof(float),
          inputShape,
          2,
          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
          &inputTensor);
      if (inStatus) {
        runtime->api->ReleaseStatus(inStatus);
        for (int i = 0; i < kModelHopSize && !state->ring16k.empty(); ++i) {
          state->ring16k.pop_front();
        }
        state->samplesSinceHop -= kModelHopSize;
        continue;
      }

      const char* inputNames[] = {runtime->inputName.c_str()};
      OrtValue* inputValues[] = {inputTensor};
      std::vector<OrtValue*> outputValues(runtime->outputNamePtrs.size(), nullptr);

      OrtStatus* runStatus = runtime->api->Run(
          runtime->session,
          nullptr,
          inputNames,
          inputValues,
          1,
          runtime->outputNamePtrs.data(),
          runtime->outputNamePtrs.size(),
          outputValues.data());

      runtime->api->ReleaseValue(inputTensor);

      if (!runStatus) {
        std::vector<float> rawPitch;
        std::vector<float> pitchBins;
        float voicingProb = 0.0f;
        float inferredVoicingProb = 0.0f;
        bool hasExplicitVoicing = false;

        if (runtime->pitchOutputIndex < outputValues.size()) {
          OrtValue* pitchValue = outputValues[runtime->pitchOutputIndex];
          if (pitchValue) {
            OrtTensorTypeAndShapeInfo* shapeInfo = nullptr;
            if (!runtime->api->GetTensorTypeAndShape(pitchValue, &shapeInfo)) {
              TensorShapeSummary shapeSummary;
              summarizeTensorShape(runtime->api, shapeInfo, &shapeSummary);
              size_t elemCount = 0;
              if (shapeSummary.hasElementCount) {
                elemCount = shapeSummary.elementCount;
              } else if (shapeSummary.hasKnownProduct) {
                elemCount = shapeSummary.knownProduct;
              } else if (shapeSummary.maxKnownDim > 0) {
                elemCount = static_cast<size_t>(shapeSummary.maxKnownDim);
              }
              if (elemCount > 0) {
                float* data = nullptr;
                if (!runtime->api->GetTensorMutableData(pitchValue,
                                                        reinterpret_cast<void**>(&data)) &&
                    data) {
                  rawPitch.assign(data, data + elemCount);
                }
              }
              runtime->api->ReleaseTensorTypeAndShapeInfo(shapeInfo);
            }
          }
        }

        if (rawPitch.empty() && runtime->pitchOutputCountHint > 0 &&
            runtime->pitchOutputCountHint <= 4096 &&
            runtime->pitchOutputIndex < outputValues.size()) {
          OrtValue* pitchValue = outputValues[runtime->pitchOutputIndex];
          if (pitchValue) {
            float* data = nullptr;
            if (!runtime->api->GetTensorMutableData(
                    pitchValue, reinterpret_cast<void**>(&data)) &&
                data) {
              rawPitch.assign(data, data + runtime->pitchOutputCountHint);
            }
          }
        }

        convertPitchOutputToCrepeBins(rawPitch, &pitchBins);

        if (!pitchBins.empty()) {
          float transformedMax = -std::numeric_limits<float>::infinity();
          bool inUnitRange = true;
          for (float& v : pitchBins) {
            if (!std::isfinite(v)) {
              inUnitRange = false;
              continue;
            }
            if (v < 0.0f || v > 1.0f) {
              inUnitRange = false;
            }
          }

          if (inUnitRange) {
            for (float& v : pitchBins) {
              v = std::isfinite(v) ? clampf(v, 0.0f, 1.0f) : 0.0f;
              if (v > transformedMax) transformedMax = v;
            }
          } else {
            for (float& v : pitchBins) {
              v = std::isfinite(v) ? stableSigmoid(v) : 0.0f;
              if (v > transformedMax) transformedMax = v;
            }
          }

          if (std::isfinite(transformedMax)) {
            inferredVoicingProb = clampf(transformedMax, 0.0f, 1.0f);
          }
          normalizePositiveInplace(&pitchBins);
        }

        if (runtime->hasVoicingOutput &&
            runtime->voicingOutputIndex < outputValues.size()) {
          OrtValue* voiceValue = outputValues[runtime->voicingOutputIndex];
          if (voiceValue) {
            float* data = nullptr;
            if (!runtime->api->GetTensorMutableData(voiceValue,
                                                    reinterpret_cast<void**>(&data)) &&
                data) {
              float raw = data[0];
              if (raw >= 0.0f && raw <= 1.0f) {
                voicingProb = raw;
              } else {
                voicingProb = stableSigmoid(raw);
              }
              hasExplicitVoicing = true;
            }
          }
        }
        if (!hasExplicitVoicing) {
          voicingProb = inferredVoicingProb;
        }

        NeuralPitchFrame frameOut;
        std::array<float, kMelodyAnalyzerStates> posterior{};
        posterior.fill(0.0f);
        float estimatedMidi = -1.0f;
        int midiNote = -1;
        buildPosteriorFromCrepe(pitchBins, voicingProb, &posterior, &estimatedMidi,
                                &midiNote);

        frameOut.posterior = posterior;
        frameOut.voicingProb = clampf(voicingProb, 0.0f, 1.0f);
        frameOut.midiNote = midiNote;
        if (midiNote >= 0) {
          frameOut.frequencyHz = midiToFrequency(static_cast<float>(midiNote));
        } else if (estimatedMidi > 0.0f) {
          frameOut.frequencyHz = midiToFrequency(estimatedMidi);
        } else {
          frameOut.frequencyHz = 0.0f;
        }

        uint64_t sourceWindow =
            static_cast<uint64_t>(std::llround(
                (static_cast<double>(kModelFrameSize) * state->sourceSampleRate) /
                static_cast<double>(kModelSampleRate)));
        uint64_t centerLag = sourceWindow / 2;
        if (state->sourceFramesProcessed > centerLag) {
          frameOut.sourceFrame = state->sourceFramesProcessed - centerLag;
        } else {
          frameOut.sourceFrame = state->sourceFramesProcessed;
        }

        state->latest = frameOut;
        state->hasLatest = true;
        state->producedFrames.push_back(frameOut);
        while (static_cast<int>(state->producedFrames.size()) >
               kProducedFrameQueueMax) {
          state->producedFrames.pop_front();
        }
      } else {
        runtime->api->ReleaseStatus(runStatus);
      }

      for (OrtValue* outputValue : outputValues) {
        if (outputValue) {
          runtime->api->ReleaseValue(outputValue);
        }
      }

      for (int i = 0; i < kModelHopSize && !state->ring16k.empty(); ++i) {
        state->ring16k.pop_front();
      }
      state->samplesSinceHop -= kModelHopSize;
    }
  }
#endif
}
