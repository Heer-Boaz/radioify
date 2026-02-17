#include "melodyanalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kLogFloor = -120.0f;
constexpr float kEnergyEps = 1.0e-12f;
constexpr uint32_t kMelodyPitchSampleRate = 16000u;
constexpr uint32_t kMelodyHpcpSampleRate = 22050u;
constexpr int kMelodyPitchFrameSize = 1024;
constexpr int kMelodyHpcpFrameSize = 2048;
constexpr int kMelodyPitchHop = 256;
constexpr int kMelodyHpcpHop = 256;
constexpr int kMelodyPitchHarmonics = 5;
constexpr int kMelodyHpcpHarmonics = 4;
constexpr int kMelodyPitchRingMax = 4096;
constexpr int kMelodyHpcpRingMax = 8192;
constexpr int kMelodyOnsetWindow = 768;
constexpr float kMelodyDcAlphaBase = 0.999f;
constexpr float kMelodyVoicedPosteriorFloor = 0.08f;
constexpr float kMelodyUnvoicedPenalty = 1.15f;
constexpr float kMelodyUnvoicedToVoicedPenalty = 1.35f;
constexpr float kMelodyTempoSearchExponent = 0.45f;
constexpr float kMelodyTempoHopMinBpm = 48.0f;
constexpr float kMelodyTempoHopMaxBpm = 220.0f;
constexpr float kMelodyTuningLerp = 0.15f;
constexpr float kMelodyTuningClampCents = 50.0f;
float clampf(float value, float low, float high) {
  return std::max(low, std::min(high, value));
}

float hanningWindow(int idx, int len) {
  if (len <= 1) return 1.0f;
  return 0.5f *
         (1.0f - std::cos(2.0f * kPi * static_cast<float>(idx) /
                          static_cast<float>(len - 1)));
}

float midiToFrequency(int midi) {
  return 440.0f * std::exp2((static_cast<float>(midi) - 69.0f) / 12.0f);
}

float frequencyToMidiFloat(float hz) {
  if (!std::isfinite(hz) || hz <= 0.0f) return -1.0f;
  return 69.0f + 12.0f * std::log2(hz / 440.0f);
}

int stateToMidi(int state) {
  return state + kMelodyMidiMin - 1;
}

int midiToState(int midi) {
  return midi - kMelodyMidiMin + 1;
}

float computeLogSumExp(const float* values, size_t count) {
  if (count == 0) return -std::numeric_limits<float>::infinity();
  float maxValue = values[0];
  for (size_t i = 1; i < count; ++i) {
    maxValue = std::max(maxValue, values[i]);
  }
  float sum = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    sum += std::exp(values[i] - maxValue);
  }
  return maxValue + std::log(std::max(kEnergyEps, sum));
}

float computeGoertzelMagnitude(const float* samples, int sampleCount,
                              float frequency, uint32_t sampleRate) {
  if (!samples || sampleCount <= 1 || frequency <= 0.0f ||
      frequency >= 0.5f * static_cast<float>(sampleRate)) {
    return 0.0f;
  }
  float omega = 2.0f * kPi * frequency /
                std::max(1.0f, static_cast<float>(sampleRate));
  float coeff = 2.0f * std::cos(omega);
  float s1 = 0.0f;
  float s2 = 0.0f;
  for (int i = 0; i < sampleCount; ++i) {
    float s0 = samples[i] + (coeff * s1) - s2;
    s2 = s1;
    s1 = s0;
  }
  float mag2 = s1 * s1 + s2 * s2 - coeff * s1 * s2;
  if (!std::isfinite(mag2) || mag2 < 0.0f) return 0.0f;
  return std::sqrt(mag2);
}

const std::array<float, kMelodyAnalyzerStates * kMelodyAnalyzerStates>&
viterbiTransitionPenalty() {
  static const std::array<float, kMelodyAnalyzerStates * kMelodyAnalyzerStates>
      penalties = []() {
        std::array<float, kMelodyAnalyzerStates * kMelodyAnalyzerStates> penalties{};
        for (int from = 0; from < kMelodyAnalyzerStates; ++from) {
          for (int to = 0; to < kMelodyAnalyzerStates; ++to) {
            if (from == to) {
              penalties[from * kMelodyAnalyzerStates + to] = 0.0f;
              continue;
            }

            bool fromUnvoiced = (from == kMelodyUnvoicedState);
            bool toUnvoiced = (to == kMelodyUnvoicedState);
            if (fromUnvoiced && toUnvoiced) {
              penalties[from * kMelodyAnalyzerStates + to] = 0.4f;
              continue;
            }
            if (fromUnvoiced || toUnvoiced) {
              penalties[from * kMelodyAnalyzerStates + to] =
                  fromUnvoiced ? kMelodyUnvoicedToVoicedPenalty
                               : kMelodyUnvoicedPenalty;
              continue;
            }

            int fromMidi = stateToMidi(from);
            int toMidi = stateToMidi(to);
            float semitone = static_cast<float>(std::abs(toMidi - fromMidi));
            float penalty = 0.11f * semitone * semitone;
            if (semitone > 12.0f) penalty *= 2.2f;
            penalties[from * kMelodyAnalyzerStates + to] = penalty;
          }
        }
        return penalties;
      }();
  return penalties;
}

void resetResampler(MelodyResamplerState* state) {
  if (!state) return;
  state->sourcePos = 0.0;
  state->inputSamples.clear();
}

void setupResampler(MelodyResamplerState* state, uint32_t sourceRate,
                    uint32_t targetRate) {
  if (!state) return;
  if (state->srcRate == sourceRate && state->dstRate == targetRate) {
    if (state->sourceStep == 0.0) {
      state->sourceStep = static_cast<double>(sourceRate) /
                          static_cast<double>(std::max(1u, targetRate));
    }
    return;
  }
  state->srcRate = sourceRate;
  state->dstRate = targetRate;
  state->sourceStep = static_cast<double>(sourceRate) /
                      static_cast<double>(std::max(1u, targetRate));
  resetResampler(state);
  state->sourceStep = static_cast<double>(sourceRate) /
                      static_cast<double>(std::max(1u, targetRate));
}

size_t resampleSample(MelodyResamplerState* state, float inputSample,
                      std::deque<float>* outRing) {
  if (!state || !outRing || state->sourceStep <= 0.0) return 0;

  state->inputSamples.push_back(inputSample);
  size_t emitted = 0;

  while (true) {
    if (state->sourcePos + 1.0 >=
        static_cast<double>(state->inputSamples.size())) {
      break;
    }
    size_t i0 = static_cast<size_t>(state->sourcePos);
    float frac =
        static_cast<float>(state->sourcePos - static_cast<double>(i0));
    float s0 = state->inputSamples[i0];
    float s1 = state->inputSamples[i0 + 1];
    outRing->push_back(s0 + (s1 - s0) * frac);
    ++emitted;
    state->sourcePos += state->sourceStep;
  }

  if (state->inputSamples.size() > 1) {
    size_t drop = static_cast<size_t>(std::floor(state->sourcePos));
    if (drop >= state->inputSamples.size()) {
      drop = state->inputSamples.size() - 1;
    }
    if (drop > 0) {
      state->inputSamples.erase(state->inputSamples.begin(),
                               state->inputSamples.begin() + drop);
      state->sourcePos = std::max(0.0, state->sourcePos - static_cast<double>(drop));
    }
  }
  return emitted;
}

bool copyFrame(std::array<float, kMelodyPitchFrameSize>* pitchFrame,
              std::array<float, kMelodyHpcpFrameSize>* hpcpFrame,
              const std::deque<float>& ring, int frameSize) {
  if (frameSize <= 0) return false;
  if (pitchFrame && static_cast<size_t>(frameSize) <= pitchFrame->size()) {
    if (static_cast<size_t>(frameSize) > ring.size()) return false;
    for (int i = 0; i < frameSize; ++i) {
      (*pitchFrame)[static_cast<size_t>(i)] =
          ring[ring.size() - static_cast<size_t>(frameSize) + static_cast<size_t>(i)];
    }
    return true;
  }
  if (hpcpFrame && static_cast<size_t>(frameSize) <= hpcpFrame->size()) {
    if (static_cast<size_t>(frameSize) > ring.size()) return false;
    for (int i = 0; i < frameSize; ++i) {
      (*hpcpFrame)[static_cast<size_t>(i)] =
          ring[ring.size() - static_cast<size_t>(frameSize) + static_cast<size_t>(i)];
    }
    return true;
  }
  return false;
}

void evaluatePitchFrame(MelodyAnalyzerState* state,
                       const std::array<float, kMelodyPitchFrameSize>& frame,
                       uint32_t sampleRate) {
  std::array<float, kMelodyAnalyzerStates> emission{};
  emission.fill(kLogFloor);

  float frameEnergy = 0.0f;
  for (int i = 0; i < kMelodyPitchFrameSize; ++i) {
    float x = frame[static_cast<size_t>(i)] * hanningWindow(i, kMelodyPitchFrameSize);
    frameEnergy += x * x;
  }
  if (!std::isfinite(frameEnergy) || frameEnergy < kEnergyEps) {
    state->frequencyHz.store(0.0f, std::memory_order_relaxed);
    state->midiNote.store(-1, std::memory_order_relaxed);
    state->lastPitchHz = 0.0f;
    state->frameEnergy = 0.0f;
    state->voicingStrength = 0.0f;
    state->viterbiCur = {};
    return;
  }

  float nyquist = std::max(1.0f, 0.5f * static_cast<float>(sampleRate));
  float voicedEnergy = 0.0f;
  for (int midi = kMelodyMidiMin; midi <= kMelodyMidiMax; ++midi) {
    int stateIdx = midiToState(midi);
    float f0 = midiToFrequency(midi);
    float harmonicSum = 0.0f;
    float firstHarmonic = 0.0f;
    for (int harmonic = 1; harmonic <= kMelodyPitchHarmonics; ++harmonic) {
      float hf = f0 * static_cast<float>(harmonic);
      if (hf >= nyquist) break;
      float mag = computeGoertzelMagnitude(frame.data(), kMelodyPitchFrameSize, hf,
                                          sampleRate);
      if (!std::isfinite(mag) || mag <= 0.0f) continue;
      float weighted = mag / static_cast<float>(harmonic);
      harmonicSum += weighted;
      if (harmonic == 1) {
        firstHarmonic += weighted;
      }
    }
    if (harmonicSum > kEnergyEps) {
      float templatePrior = 0.33f + 0.67f *
                                      (firstHarmonic /
                                       std::max(kEnergyEps, harmonicSum));
      float scored = harmonicSum * templatePrior;
      emission[stateIdx] = std::log(std::max(kEnergyEps, scored));
      voicedEnergy += scored;
    }
  }

  float voicedEvidence = std::min(1.0f, voicedEnergy /
                                           std::max(kEnergyEps, frameEnergy));
  state->voicingStrength = clampf(voicedEvidence, 0.0f, 1.0f);

  float totalVoiced = 0.0f;
  for (int i = 1; i < kMelodyAnalyzerStates; ++i) {
    totalVoiced += std::exp(emission[i]);
  }

  float unvoiced = frameEnergy * (1.0f - voicedEvidence);
  unvoiced = std::max(unvoiced, 0.0f);
  emission[kMelodyUnvoicedState] =
      std::log(std::max(kEnergyEps, unvoiced));

  if (!std::isfinite(totalVoiced) || totalVoiced <= kEnergyEps) {
    emission[kMelodyUnvoicedState] =
        std::log(std::max(kEnergyEps, frameEnergy));
    for (int i = 1; i < kMelodyAnalyzerStates; ++i) {
      emission[i] = kLogFloor;
    }
  }

  float norm = computeLogSumExp(emission.data(), emission.size());
  for (auto& v : emission) {
    v -= norm;
  }

  const auto& transition = viterbiTransitionPenalty();
  if (!state->viterbiInited) {
    float uniform = -std::log(static_cast<float>(kMelodyAnalyzerStates));
    state->viterbiPrev.fill(uniform);
    state->viterbiCur.fill(uniform);
    state->viterbiInited = true;
  }

  for (int next = 0; next < kMelodyAnalyzerStates; ++next) {
    float best = -std::numeric_limits<float>::infinity();
    int bestPrev = 0;
    for (int prev = 0; prev < kMelodyAnalyzerStates; ++prev) {
      float candidate = state->viterbiPrev[prev] -
                        transition[static_cast<size_t>(prev) *
                                   kMelodyAnalyzerStates + next];
      if (candidate > best) {
        best = candidate;
        bestPrev = prev;
      }
    }
    state->viterbiCur[next] = best + emission[next];
    state->viterbiBack[next] = bestPrev;
  }

  float posteriorNorm = computeLogSumExp(state->viterbiCur.data(),
                                        state->viterbiCur.size());
  for (int i = 0; i < kMelodyAnalyzerStates; ++i) {
    state->viterbiCur[i] = std::exp(state->viterbiCur[i] - posteriorNorm);
  }

  float voicedPosterior = 0.0f;
  float weightedMidi = 0.0f;
  for (int i = 1; i < kMelodyAnalyzerStates; ++i) {
    voicedPosterior += state->viterbiCur[i];
    weightedMidi += state->viterbiCur[i] *
                    static_cast<float>(stateToMidi(i));
    state->lastPosterior[i] = state->viterbiCur[i];
  }
  state->lastPosterior[0] = state->viterbiCur[0];

  for (int i = 0; i < kMelodyAnalyzerStates; ++i) {
    state->viterbiPrev[i] = std::log(std::max(kEnergyEps, state->viterbiCur[i]));
  }

  state->frameEnergy = frameEnergy;
  if (voicedPosterior < kMelodyVoicedPosteriorFloor) {
    state->frequencyHz.store(0.0f, std::memory_order_relaxed);
    state->confidence.store(0.0f, std::memory_order_relaxed);
    state->midiNote.store(-1, std::memory_order_relaxed);
    state->lastPitchHz = 0.0f;
    return;
  }

  float meanMidi = weightedMidi / std::max(kEnergyEps, voicedPosterior);
  int midi = static_cast<int>(std::lround(meanMidi));
  midi = std::clamp(midi, kMelodyMidiMin, kMelodyMidiMax);
  state->midiNote.store(midi, std::memory_order_relaxed);
  float freq = midiToFrequency(midi);
  state->lastPitchHz = freq;
  state->frequencyHz.store(freq, std::memory_order_relaxed);
  state->confidence.store(clampf(voicedPosterior, 0.0f, 1.0f),
                         std::memory_order_relaxed);
}

void estimateTuningFromPitch(MelodyAnalyzerState* state, int midi,
                            float frequencyHz) {
  if (!state || midi < kMelodyMidiMin || midi > kMelodyMidiMax ||
      !std::isfinite(frequencyHz) || frequencyHz <= 0.0f) {
    return;
  }
  float exactMidi = frequencyToMidiFloat(frequencyHz);
  if (!std::isfinite(exactMidi)) return;
  float refMidi = std::round(static_cast<float>(midi));
  float cents = (exactMidi - refMidi) * 100.0f;
  float corrected = clampf(cents, -kMelodyTuningClampCents,
                           kMelodyTuningClampCents);
  state->tuningCents =
      (1.0f - kMelodyTuningLerp) * state->tuningCents +
      kMelodyTuningLerp * corrected;
}

void evaluateHpcpFrame(MelodyAnalyzerState* state,
                      const std::array<float, kMelodyHpcpFrameSize>& frame,
                      uint32_t sampleRate) {
  std::array<float, 12> profile{};
  profile.fill(0.0f);

  float frameEnergy = 0.0f;
  for (int i = 0; i < kMelodyHpcpFrameSize; ++i) {
    float x = frame[static_cast<size_t>(i)] *
              hanningWindow(i, kMelodyHpcpFrameSize);
    frameEnergy += x * x;
  }
  if (!std::isfinite(frameEnergy) || frameEnergy < kEnergyEps) {
    state->hpcpProfile.fill(0.0f);
    state->hpcpEntropy = 0.0f;
    state->prevHpcpFrameEnergy = 0.0f;
    state->tempoBpm = 0.0f;
    state->beatConfidence = 0.0f;
    state->prevHpcpProfile.fill(0.0f);
    return;
  }

  const float nyquist = std::max(1.0f, 0.5f * static_cast<float>(sampleRate));
  float totalHpcpEnergy = 0.0f;
  for (int midi = kMelodyMidiMin; midi <= kMelodyMidiMax; ++midi) {
    float f0 = midiToFrequency(midi);
    float score = 0.0f;
    for (int harmonic = 1; harmonic <= kMelodyHpcpHarmonics; ++harmonic) {
      float hf = f0 * static_cast<float>(harmonic);
      if (hf >= nyquist) break;
      float mag = computeGoertzelMagnitude(frame.data(), kMelodyHpcpFrameSize, hf,
                                          sampleRate);
      if (!std::isfinite(mag) || mag <= 0.0f) continue;
      score += mag / static_cast<float>(harmonic);
    }
    if (score <= kEnergyEps) continue;
    float classPos = std::fmod(static_cast<float>(midi) +
                                   (state->tuningCents / 100.0f),
                               12.0f);
    if (classPos < 0.0f) classPos += 12.0f;
    int left = static_cast<int>(std::floor(classPos));
    int right = (left + 1) % 12;
    float frac = classPos - static_cast<float>(left);
    profile[static_cast<size_t>(left)] += score * (1.0f - frac);
    profile[static_cast<size_t>(right)] += score * frac;
    totalHpcpEnergy += score;
  }

  if (totalHpcpEnergy > kEnergyEps) {
    for (auto& v : profile) {
      v /= totalHpcpEnergy;
    }
  } else {
    float uniform = 1.0f / 12.0f;
    profile.fill(uniform);
  }
  state->hpcpProfile = profile;

  float entropy = 0.0f;
  for (float v : profile) {
    if (v > kEnergyEps) {
      entropy -= v * std::log(v);
    }
  }
  float maxEntropy = std::log(12.0f);
  if (maxEntropy > 0.0f) {
    state->hpcpEntropy = std::clamp(entropy / maxEntropy, 0.0f, 1.0f);
  } else {
    state->hpcpEntropy = 0.0f;
  }

  float flux = 0.0f;
  for (int i = 0; i < 12; ++i) {
    float d = profile[i] - state->prevHpcpProfile[i];
    flux += std::max(0.0f, d);
  }
  state->prevHpcpProfile = profile;

  float spectralEnergyOnset = std::max(0.0f, frameEnergy - state->prevHpcpFrameEnergy);
  state->prevHpcpFrameEnergy = frameEnergy;
  float onset = 0.85f * flux + 0.15f * spectralEnergyOnset;

  state->hpcpOnsetHistory.push_back(onset);
  while (static_cast<int>(state->hpcpOnsetHistory.size()) > kMelodyOnsetWindow) {
    state->hpcpOnsetHistory.pop_front();
  }

  state->tempoBpm = 0.0f;
  state->beatConfidence = 0.0f;
  const int historySize = static_cast<int>(state->hpcpOnsetHistory.size());
  const int minLag =
      static_cast<int>(std::round((60.0f / kMelodyTempoHopMaxBpm) *
                                 static_cast<float>(kMelodyHpcpSampleRate) /
                                 static_cast<float>(kMelodyHpcpHop)));
  const int maxLag =
      static_cast<int>(std::round((60.0f / kMelodyTempoHopMinBpm) *
                                 static_cast<float>(kMelodyHpcpSampleRate) /
                                 static_cast<float>(kMelodyHpcpHop)));
  if (historySize >= std::max(2 * maxLag + 4, kMelodyOnsetWindow / 4)) {
    float bestScore = -1.0f;
    int bestLag = -1;
    for (int lag = minLag; lag <= maxLag; ++lag) {
      if (lag <= 0 || lag >= historySize) continue;
      float score = 0.0f;
      float normA = 0.0f;
      float normB = 0.0f;
      for (int i = lag; i < historySize; ++i) {
        float a = state->hpcpOnsetHistory[static_cast<size_t>(i)];
        float b = state->hpcpOnsetHistory[static_cast<size_t>(i - lag)];
        score += a * b;
        normA += a * a;
        normB += b * b;
      }
      float denom = std::sqrt(normA * normB);
      if (denom > 0.0f) {
        score /= denom;
      } else {
        score = 0.0f;
      }
      if (score > bestScore) {
        bestScore = score;
        bestLag = lag;
      }
    }
    if (bestLag > 0) {
      float bpm = (60.0f * static_cast<float>(kMelodyHpcpSampleRate)) /
                  (static_cast<float>(kMelodyHpcpHop) * static_cast<float>(bestLag));
      state->tempoBpm =
          clampf(bpm, static_cast<float>(kMelodyTempoHopMinBpm),
                  static_cast<float>(kMelodyTempoHopMaxBpm));
      float onsetSum = 0.0f;
      for (float v : state->hpcpOnsetHistory) {
        onsetSum += v;
      }
      float normConfidence = std::pow(std::clamp(bestScore, 0.0f, 1.0f),
                                     kMelodyTempoSearchExponent);
      float onsetDensity = std::clamp(
          onsetSum / static_cast<float>(historySize), 0.0f, 1.0f);
      state->beatConfidence =
          std::clamp(normConfidence * (0.25f + 0.75f * onsetDensity), 0.0f, 1.0f);
    }
  }
}

void processResampledFrames(MelodyAnalyzerState* state) {
  while (state->pitchSamplesSinceAnalysis >= kMelodyPitchHop &&
         static_cast<int>(state->pitchRing.size()) >= kMelodyPitchFrameSize) {
    std::array<float, kMelodyPitchFrameSize> pitchFrame{};
    if (copyFrame(&pitchFrame, nullptr, state->pitchRing,
                  kMelodyPitchFrameSize)) {
      evaluatePitchFrame(state, pitchFrame, kMelodyPitchSampleRate);
      int midi = state->midiNote.load(std::memory_order_relaxed);
      float hz = state->frequencyHz.load(std::memory_order_relaxed);
      estimateTuningFromPitch(state, midi, hz);
    }
    for (int i = 0; i < kMelodyPitchHop; ++i) {
      if (!state->pitchRing.empty()) state->pitchRing.pop_front();
    }
    state->pitchSamplesSinceAnalysis -= kMelodyPitchHop;
  }

  while (state->hpcpSamplesSinceAnalysis >= kMelodyHpcpHop &&
         static_cast<int>(state->hpcpRing.size()) >= kMelodyHpcpFrameSize) {
    std::array<float, kMelodyHpcpFrameSize> hpcpFrame{};
    if (copyFrame(nullptr, &hpcpFrame, state->hpcpRing,
                  kMelodyHpcpFrameSize)) {
      evaluateHpcpFrame(state, hpcpFrame, kMelodyHpcpSampleRate);
    }
    for (int i = 0; i < kMelodyHpcpHop; ++i) {
      if (!state->hpcpRing.empty()) state->hpcpRing.pop_front();
    }
    state->hpcpSamplesSinceAnalysis -= kMelodyHpcpHop;
  }
}

void trimRings(MelodyAnalyzerState* state) {
  while (static_cast<int>(state->pitchRing.size()) > kMelodyPitchRingMax) {
    state->pitchRing.pop_front();
  }
  while (static_cast<int>(state->hpcpRing.size()) > kMelodyHpcpRingMax) {
    state->hpcpRing.pop_front();
  }
}

void setupStateForSampleRate(MelodyAnalyzerState* state, uint32_t sourceRate) {
  setupResampler(&state->pitchResampler, sourceRate, kMelodyPitchSampleRate);
  setupResampler(&state->hpcpResampler, sourceRate, kMelodyHpcpSampleRate);

  float cutoff = 20.0f;
  float omega = 2.0f * kPi * cutoff /
                std::max(1.0f, static_cast<float>(sourceRate));
  state->dcAlpha = std::exp(-omega);
}
}  // namespace

void melodyAnalyzerReset(MelodyAnalyzerState* state) {
  if (!state) return;
  state->pitchResampler = {};
  state->hpcpResampler = {};
  state->pitchRing.clear();
  state->hpcpRing.clear();
  state->hpcpOnsetHistory.clear();
  state->pitchSamplesSinceAnalysis = 0;
  state->hpcpSamplesSinceAnalysis = 0;
  state->viterbiPrev.fill(-std::numeric_limits<float>::infinity());
  state->viterbiCur.fill(-std::numeric_limits<float>::infinity());
  state->viterbiBack.fill(0);
  state->lastPosterior.fill(0.0f);
  state->dcPrevInput = 0.0f;
  state->dcPrevOutput = 0.0f;
  state->dcAlpha = kMelodyDcAlphaBase;
  state->prevHpcpFrameEnergy = 0.0f;
  state->frameEnergy = 0.0f;
  state->hpcpEntropy = 0.0f;
  state->voicingStrength = 0.0f;
  state->tempoBpm = 0.0f;
  state->beatConfidence = 0.0f;
  state->tuningCents = 0.0f;
  state->viterbiInited = false;
  state->sampleRate = 0;
  state->hpcpProfile.fill(0.0f);
  state->prevHpcpProfile.fill(0.0f);
  state->frequencyHz.store(0.0f, std::memory_order_relaxed);
  state->confidence.store(0.0f, std::memory_order_relaxed);
  state->midiNote.store(-1, std::memory_order_relaxed);
  state->lastPitchHz = 0.0f;
}

void melodyAnalyzerUpdate(MelodyAnalyzerState* state, const float* samples,
                         uint32_t frameCount, uint32_t channels,
                         uint32_t sampleRate) {
  if (!state || !samples || frameCount == 0 || channels == 0 || sampleRate == 0) {
    return;
  }

  if (state->sampleRate != sampleRate) {
    melodyAnalyzerReset(state);
    state->sampleRate = sampleRate;
    setupStateForSampleRate(state, sampleRate);
  }

  for (uint32_t frame = 0; frame < frameCount; ++frame) {
    float sample = samples[static_cast<size_t>(frame) * channels];
    if (channels > 1) {
      for (uint32_t ch = 1; ch < channels; ++ch) {
        sample += samples[static_cast<size_t>(frame) * channels + ch];
      }
      sample /= static_cast<float>(channels);
    }

    float filtered = sample - state->dcPrevInput + state->dcAlpha * state->dcPrevOutput;
    state->dcPrevInput = sample;
    state->dcPrevOutput = filtered;

    state->pitchSamplesSinceAnalysis +=
        static_cast<int>(resampleSample(&state->pitchResampler, filtered,
                                       &state->pitchRing));
    state->hpcpSamplesSinceAnalysis +=
        static_cast<int>(resampleSample(&state->hpcpResampler, filtered,
                                       &state->hpcpRing));

    trimRings(state);
    processResampledFrames(state);
  }
}

MelodyAnalyzerInfo melodyAnalyzerGetInfo(const MelodyAnalyzerState* state) {
  MelodyAnalyzerInfo info;
  if (!state) return info;
  info.frequencyHz = state->frequencyHz.load(std::memory_order_relaxed);
  info.confidence = state->confidence.load(std::memory_order_relaxed);
  info.midiNote = state->midiNote.load(std::memory_order_relaxed);
  info.harmonicity = 1.0f - std::clamp(state->hpcpEntropy, 0.0f, 1.0f);
  info.tempoBpm = state->tempoBpm;
  info.beatConfidence = state->beatConfidence;
  info.hpcpProfile = state->hpcpProfile;
  info.pitchPosterior = state->lastPosterior;

  float sum = 0.0f;
  for (float p : info.pitchPosterior) {
    if (std::isfinite(p) && p > 0.0f) {
      sum += p;
    }
  }
  if (sum <= kEnergyEps) {
    info.pitchPosterior.fill(0.0f);
    info.pitchPosterior[kMelodyUnvoicedState] = 1.0f;
  } else {
    for (float& p : info.pitchPosterior) {
      if (!std::isfinite(p) || p < 0.0f) {
        p = 0.0f;
      } else {
        p /= sum;
      }
    }
  }
  return info;
}
