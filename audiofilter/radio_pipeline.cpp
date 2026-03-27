#include "../radio.h"
#include "math/radio_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <vector>

static void updateStageCalibration(Radio1938& radio,
                                   StageId id,
                                   float in,
                                   float out);
static void updateCalibrationSnapshot(Radio1938& radio);

namespace {

constexpr size_t kInvalidOrderIndex = std::numeric_limits<size_t>::max();

struct StepDependencies {
  std::array<StageId, 4> ids{};
  size_t count = 0;
};

static StepDependencies makeDependencies(std::initializer_list<StageId> ids) {
  StepDependencies deps;
  size_t index = 0;
  for (StageId id : ids) {
    if (index >= deps.ids.size()) break;
    deps.ids[index++] = id;
  }
  deps.count = index;
  return deps;
}

static StepDependencies blockDependencies(StageId) {
  return StepDependencies{};
}

static StepDependencies sampleControlDependencies(StageId id) {
  switch (id) {
    case StageId::AVC:
      return StepDependencies{};
    case StageId::AFC:
      return makeDependencies({StageId::AVC});
    case StageId::ControlBus:
      return makeDependencies({StageId::AFC});
    case StageId::InterferenceDerived:
      return makeDependencies({StageId::ControlBus});
    default:
      return StepDependencies{};
  }
}

static StepDependencies programPathDependencies(StageId id) {
  switch (id) {
    case StageId::Input:
      return StepDependencies{};
    case StageId::FrontEnd:
      return makeDependencies({StageId::Input});
    case StageId::Mixer:
      return makeDependencies({StageId::FrontEnd});
    case StageId::IFStrip:
      return makeDependencies({StageId::Mixer});
    case StageId::Demod:
      return makeDependencies({StageId::IFStrip});
    case StageId::ReceiverInputNetwork:
      return makeDependencies({StageId::Demod});
    case StageId::ReceiverCircuit:
      return makeDependencies({StageId::ReceiverInputNetwork});
    case StageId::Tone:
      return makeDependencies({StageId::ReceiverCircuit});
    case StageId::Power:
      return makeDependencies({StageId::Tone});
    case StageId::Noise:
      return makeDependencies({StageId::Power});
    case StageId::Speaker:
      return makeDependencies({StageId::Noise});
    case StageId::Cabinet:
      return makeDependencies({StageId::Speaker});
    case StageId::FinalLimiter:
      return makeDependencies({StageId::Cabinet});
    case StageId::OutputClip:
      return makeDependencies({StageId::FinalLimiter});
    default:
      return StepDependencies{};
  }
}

template <typename Step, size_t StepCount, typename DependencyFn>
static std::vector<size_t> compileOrder(
    const std::array<Step, StepCount>& steps,
    DependencyFn&& dependencyFn) {
  std::vector<size_t> order;
  order.reserve(StepCount);

  std::vector<bool> enabled(StepCount, false);
  for (size_t i = 0; i < StepCount; ++i) {
    enabled[i] = steps[i].enabled;
  }

  std::vector<size_t> enabledIndices;
  enabledIndices.reserve(StepCount);
  for (size_t i = 0; i < StepCount; ++i) {
    if (enabled[i]) enabledIndices.push_back(i);
  }

  std::vector<int> indegree(StepCount, 0);
  std::vector<std::vector<size_t>> adj(StepCount);

  for (size_t i = 0; i < StepCount; ++i) {
    if (!enabled[i]) continue;
    StepDependencies deps = dependencyFn(steps[i].id);
    for (size_t di = 0; di < deps.count; ++di) {
      StageId depId = deps.ids[di];
      for (size_t j = 0; j < StepCount; ++j) {
        if (!enabled[j]) continue;
        if (steps[j].id == depId) {
          adj[j].push_back(i);
          indegree[i]++;
          break;
        }
      }
    }
  }

  std::vector<size_t> queue;
  queue.reserve(enabledIndices.size());
  for (size_t i = 0; i < StepCount; ++i) {
    if (enabled[i] && indegree[i] == 0) {
      queue.push_back(i);
    }
  }

  for (size_t head = 0; head < queue.size(); ++head) {
    size_t n = queue[head];
    order.push_back(n);
    for (size_t to : adj[n]) {
      if (--indegree[to] == 0) {
        queue.push_back(to);
      }
    }
  }

  if (order.size() != enabledIndices.size()) {
    throw std::runtime_error("Radio pipeline cycle detected");
  }

  return order;
}

template <typename StepArray>
static size_t findOrderIndexAtOrAfter(const StepArray& steps,
                                      const std::vector<size_t>& order,
                                      StageId stageId) {
  const size_t target = static_cast<size_t>(stageId);
  for (size_t oi = 0; oi < order.size(); ++oi) {
    const auto& step = steps[order[oi]];
    if (static_cast<size_t>(step.id) >= target) return oi;
  }
  return order.size();
}

template <size_t StepCount>
static RadioBlockControl runBlockPrepare(
    Radio1938& radio,
    const std::array<Radio1938::BlockStep, StepCount>& steps,
    const std::vector<size_t>& order,
    uint32_t frames) {
  RadioBlockControl block{};
  for (size_t oi = 0; oi < order.size(); ++oi) {
    const auto& step = steps[order[oi]];
    if (!step.enabled || !step.prepare) continue;
    step.prepare(radio, block, frames);
  }
  return block;
}

template <size_t StepCount>
static void runSampleControl(
    Radio1938& radio,
    RadioSampleContext& ctx,
    const std::array<Radio1938::SampleControlStep, StepCount>& steps,
    const std::vector<size_t>& order) {
  for (size_t oi = 0; oi < order.size(); ++oi) {
    const auto& step = steps[order[oi]];
    if (!step.enabled || !step.update) continue;
    step.update(radio, ctx);
  }
}

template <size_t StepCount>
static float runProgramPath(
    Radio1938& radio,
    float y,
    const RadioSampleContext& ctx,
    const std::array<Radio1938::ProgramPathStep, StepCount>& steps,
    const std::vector<size_t>& order,
    size_t startIndex,
    size_t endIndex) {
  size_t begin = std::min(startIndex, order.size());
  size_t end = std::min(endIndex, order.size());
  for (size_t oi = begin; oi < end; ++oi) {
    const auto& step = steps[order[oi]];
    if (!step.enabled || !step.process) continue;
    float in = y;
    y = step.process(radio, y, ctx);
    if (!step.stateOnly) {
      updateStageCalibration(radio, step.id, in, y);
    }
  }
  return y;
}

static float scalePhysicalSpeakerVoltsToDigital(const Radio1938& radio,
                                                float speakerVolts) {
  if (!radio.graph.isEnabled(StageId::Power)) return speakerVolts;
  constexpr float kDigitalProgramPeakHeadroom = 1.12f;
  return speakerVolts /
         (requirePositiveFinite(radio.output.digitalReferenceSpeakerVoltsPeak) *
          kDigitalProgramPeakHeadroom);
}

static float applyDigitalMakeupGain(const Radio1938& radio, float yDigital) {
  if (!radio.graph.isEnabled(StageId::Power)) return yDigital;
  return yDigital * std::max(radio.output.digitalMakeupGain, 0.0f);
}

} // namespace

Radio1938::BlockStep* Radio1938::RadioExecutionGraph::findBlock(StageId id) {
  for (auto& step : blockSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

const Radio1938::BlockStep* Radio1938::RadioExecutionGraph::findBlock(
    StageId id) const {
  for (const auto& step : blockSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

Radio1938::SampleControlStep* Radio1938::RadioExecutionGraph::findSampleControl(
    StageId id) {
  for (auto& step : sampleControlSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

const Radio1938::SampleControlStep*
Radio1938::RadioExecutionGraph::findSampleControl(StageId id) const {
  for (const auto& step : sampleControlSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

Radio1938::ProgramPathStep* Radio1938::RadioExecutionGraph::findProgramPath(
    StageId id) {
  for (auto& step : programPathSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

const Radio1938::ProgramPathStep* Radio1938::RadioExecutionGraph::findProgramPath(
    StageId id) const {
  for (const auto& step : programPathSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

bool Radio1938::RadioExecutionGraph::isEnabled(StageId id) const {
  if (const auto* step = findBlock(id)) return step->enabled;
  if (const auto* step = findSampleControl(id)) return step->enabled;
  if (const auto* step = findProgramPath(id)) return step->enabled;
  return false;
}

void Radio1938::RadioExecutionGraph::setEnabled(StageId id, bool value) {
  bool changed = false;
  if (auto* step = findBlock(id)) {
    changed = step->enabled != value;
    step->enabled = value;
    if (changed) invalidate();
    return;
  }
  if (auto* step = findSampleControl(id)) {
    changed = step->enabled != value;
    step->enabled = value;
    if (changed) invalidate();
    return;
  }
  if (auto* step = findProgramPath(id)) {
    changed = step->enabled != value;
    step->enabled = value;
    if (changed) invalidate();
    return;
  }
}

void Radio1938::RadioExecutionGraph::invalidate() {
  compiled = false;
  blockExecutionOrder.clear();
  sampleControlExecutionOrder.clear();
  programPathExecutionOrder.clear();
}

void Radio1938::RadioExecutionGraph::compile() {
  if (compiled) return;

  blockExecutionOrder = compileOrder(blockSteps, blockDependencies);
  sampleControlExecutionOrder =
      compileOrder(sampleControlSteps, sampleControlDependencies);
  programPathExecutionOrder =
      compileOrder(programPathSteps, programPathDependencies);
  compiled = true;
}

size_t Radio1938::RadioExecutionGraph::findBlockOrderIndex(StageId id) const {
  for (size_t oi = 0; oi < blockExecutionOrder.size(); ++oi) {
    if (blockSteps[blockExecutionOrder[oi]].id == id) return oi;
  }
  return kInvalidOrderIndex;
}

size_t Radio1938::RadioExecutionGraph::findSampleControlOrderIndex(
    StageId id) const {
  for (size_t oi = 0; oi < sampleControlExecutionOrder.size(); ++oi) {
    if (sampleControlSteps[sampleControlExecutionOrder[oi]].id == id) {
      return oi;
    }
  }
  return kInvalidOrderIndex;
}

size_t Radio1938::RadioExecutionGraph::findProgramPathOrderIndex(
    StageId id) const {
  for (size_t oi = 0; oi < programPathExecutionOrder.size(); ++oi) {
    if (programPathSteps[programPathExecutionOrder[oi]].id == id) {
      return oi;
    }
  }
  return kInvalidOrderIndex;
}

void Radio1938::RadioLifecycle::configure(Radio1938& radio,
                                          RadioInitContext& initCtx) const {
  for (const auto& step : configureSteps) {
    if (!step.init) continue;
    step.init(radio, initCtx);
  }
}

void Radio1938::RadioLifecycle::allocate(Radio1938& radio,
                                         RadioInitContext& initCtx) const {
  for (const auto& step : allocateSteps) {
    if (!step.init) continue;
    step.init(radio, initCtx);
  }
}

void Radio1938::RadioLifecycle::initializeDependentState(
    Radio1938& radio,
    RadioInitContext& initCtx) const {
  for (const auto& step : initializeDependentStateSteps) {
    if (!step.init) continue;
    step.init(radio, initCtx);
  }
}

void Radio1938::RadioLifecycle::resetRuntime(Radio1938& radio) const {
  for (const auto& step : resetSteps) {
    if (!step.reset) continue;
    step.reset(radio);
  }
}

// Calibration bookkeeping
void Radio1938::CalibrationRmsMetric::accumulate(float value) {
  if (!std::isfinite(value)) return;
  sampleCount++;
  sumSq += static_cast<double>(value) * static_cast<double>(value);
  peak = std::max(peak, std::fabs(value));
}

void Radio1938::CalibrationRmsMetric::updateSnapshot() {
  if (sampleCount == 0) {
    rms = 0.0f;
    return;
  }
  rms = static_cast<float>(
      std::sqrt(sumSq / static_cast<double>(sampleCount)));
}

static void fftInPlace(
    std::array<std::complex<float>, kRadioCalibrationFftSize>& bins) {
  const size_t n = bins.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(bins[i], bins[j]);
    }
  }

  for (size_t len = 2; len <= n; len <<= 1) {
    float angle = -kRadioTwoPi / static_cast<float>(len);
    std::complex<float> wLen(std::cos(angle), std::sin(angle));
    for (size_t i = 0; i < n; i += len) {
      std::complex<float> w(1.0f, 0.0f);
      size_t half = len >> 1;
      for (size_t j = 0; j < half; ++j) {
        std::complex<float> u = bins[i + j];
        std::complex<float> v = bins[i + j + half] * w;
        bins[i + j] = u + v;
        bins[i + j + half] = u - v;
        w *= wLen;
      }
    }
  }
}

static void accumulateCalibrationSpectrum(
    Radio1938::CalibrationStageMetrics& stage,
    float sampleRate,
    bool flushPartial) {
  if (stage.fftFill == 0) return;
  if (!flushPartial && stage.fftFill < kRadioCalibrationFftSize) return;

  std::array<std::complex<float>, kRadioCalibrationFftSize> bins{};
  const auto& window = radioCalibrationWindow();
  for (size_t i = 0; i < kRadioCalibrationFftSize; ++i) {
    float sample = (i < stage.fftFill) ? stage.fftTimeBuffer[i] : 0.0f;
    bins[i] = std::complex<float>(sample * window[i], 0.0f);
  }
  fftInPlace(bins);

  const auto& edges = radioCalibrationBandEdgesHz();
  float binHz = sampleRate / static_cast<float>(kRadioCalibrationFftSize);
  for (size_t i = 1; i < kRadioCalibrationFftBinCount; ++i) {
    float hz = static_cast<float>(i) * binHz;
    double energy = std::norm(bins[i]);
    stage.fftBinEnergy[i] += energy;
    for (size_t band = 0; band < kRadioCalibrationBandCount; ++band) {
      if (hz >= edges[band] && hz < edges[band + 1]) {
        stage.bandEnergy[band] += energy;
        break;
      }
    }
  }

  stage.fftBlockCount++;
  stage.fftTimeBuffer.fill(0.0f);
  stage.fftFill = 0;
}

void Radio1938::CalibrationStageMetrics::clearAccumulators() {
  sampleCount = 0;
  rmsIn = 0.0;
  rmsOut = 0.0;
  meanIn = 0.0;
  meanOut = 0.0;
  peakIn = 0.0f;
  peakOut = 0.0f;
  crestIn = 0.0f;
  crestOut = 0.0f;
  spectralCentroidHz = 0.0f;
  bandwidth3dBHz = 0.0f;
  bandwidth6dBHz = 0.0f;
  clipCountIn = 0;
  clipCountOut = 0;
  inSum = 0.0;
  outSum = 0.0;
  inSumSq = 0.0;
  outSumSq = 0.0;
  bandEnergy.fill(0.0);
  fftBinEnergy.fill(0.0);
  fftTimeBuffer.fill(0.0f);
  fftFill = 0;
  fftBlockCount = 0;
}

void Radio1938::CalibrationStageMetrics::resetMeasurementState() {
  fftTimeBuffer.fill(0.0f);
  fftFill = 0;
}

void Radio1938::CalibrationRmsMetric::reset() {
  sampleCount = 0;
  sumSq = 0.0;
  rms = 0.0f;
  peak = 0.0f;
}

void Radio1938::CalibrationState::reset() {
  totalSamples = 0;
  preLimiterClipCount = 0;
  postLimiterClipCount = 0;
  limiterActiveSamples = 0;
  limiterDutyCycle = 0.0f;
  limiterAverageGainReduction = 0.0f;
  limiterMaxGainReduction = 0.0f;
  limiterAverageGainReductionDb = 0.0f;
  limiterMaxGainReductionDb = 0.0f;
  limiterGainReductionSum = 0.0;
  limiterGainReductionDbSum = 0.0;
  validationSampleCount = 0;
  driverGridPositiveSamples = 0;
  outputGridPositiveSamples = 0;
  outputGridAPositiveSamples = 0;
  outputGridBPositiveSamples = 0;
  driverGridPositiveFraction = 0.0f;
  outputGridPositiveFraction = 0.0f;
  outputGridAPositiveFraction = 0.0f;
  outputGridBPositiveFraction = 0.0f;
  maxMixerPlateCurrentAmps = 0.0f;
  maxReceiverPlateCurrentAmps = 0.0f;
  maxDriverPlateCurrentAmps = 0.0f;
  maxOutputPlateCurrentAAmps = 0.0f;
  maxOutputPlateCurrentBAmps = 0.0f;
  interstageSecondarySumSq = 0.0;
  interstageSecondaryRmsVolts = 0.0f;
  interstageSecondaryPeakVolts = 0.0f;
  maxSpeakerSecondaryVolts = 0.0f;
  maxSpeakerReferenceRatio = 0.0f;
  maxDigitalOutput = 0.0f;
  detectorIfCrackleEventCount = 0;
  detectorIfCrackleMaxBurstAmp = 0.0f;
  detectorIfCrackleMaxEnv = 0.0f;
  detectorNodeVolts.reset();
  receiverGridVolts.reset();
  receiverPlateSwingVolts.reset();
  driverGridVolts.reset();
  driverPlateSwingVolts.reset();
  outputGridAVolts.reset();
  outputGridBVolts.reset();
  outputPrimaryVolts.reset();
  speakerSecondaryVolts.reset();
  validationDriverGridPositive = false;
  validationFailed = false;
  validationOutputGridPositive = false;
  validationOutputGridAPositive = false;
  validationOutputGridBPositive = false;
  validationSpeakerOverReference = false;
  validationInterstageSecondary = false;
  validationDcShift = false;
  validationDigitalClip = false;
  for (auto& stage : stages) {
    stage.clearAccumulators();
  }
}

void Radio1938::CalibrationState::resetMeasurementState() {
  for (auto& stage : stages) {
    stage.resetMeasurementState();
  }
}

void Radio1938::CalibrationStageMetrics::updateSnapshot(float sampleRate) {
  accumulateCalibrationSpectrum(*this, sampleRate, true);
  if (sampleCount == 0) return;
  double invCount = 1.0 / static_cast<double>(sampleCount);
  meanIn = inSum * invCount;
  meanOut = outSum * invCount;
  rmsIn = std::sqrt(inSumSq * invCount);
  rmsOut = std::sqrt(outSumSq * invCount);
  crestIn =
      (rmsIn > 1e-12) ? peakIn / static_cast<float>(rmsIn) : 0.0f;
  crestOut =
      (rmsOut > 1e-12) ? peakOut / static_cast<float>(rmsOut) : 0.0f;

  double totalEnergy = 0.0;
  double weightedHz = 0.0;
  double maxEnergy = 0.0;
  bandwidth3dBHz = 0.0f;
  bandwidth6dBHz = 0.0f;
  float binHz = sampleRate / static_cast<float>(kRadioCalibrationFftSize);
  for (size_t i = 1; i < fftBinEnergy.size(); ++i) {
    double energy = fftBinEnergy[i];
    float hz = static_cast<float>(i) * binHz;
    totalEnergy += energy;
    weightedHz += energy * hz;
    maxEnergy = std::max(maxEnergy, energy);
  }
  spectralCentroidHz =
      (totalEnergy > 1e-18) ? static_cast<float>(weightedHz / totalEnergy) : 0.0f;
  if (maxEnergy <= 0.0) return;

  double threshold3dB = maxEnergy * std::pow(10.0, -3.0 / 10.0);
  double threshold6dB = maxEnergy * std::pow(10.0, -6.0 / 10.0);
  for (size_t i = 1; i < fftBinEnergy.size(); ++i) {
    float hz = static_cast<float>(i) * binHz;
    if (fftBinEnergy[i] >= threshold3dB) {
      bandwidth3dBHz = hz;
    }
    if (fftBinEnergy[i] >= threshold6dB) {
      bandwidth6dBHz = hz;
    }
  }
}

static void updateStageCalibration(Radio1938& radio,
                                   StageId id,
                                   float in,
                                   float out) {
  if (!radio.calibration.enabled) return;
  auto& stage = radio.calibration.stages[static_cast<size_t>(id)];
  float clipThreshold = 1.0f;
  if (id == StageId::Power) {
    clipThreshold =
        std::max(radio.output.digitalReferenceSpeakerVoltsPeak, 1e-3f);
  }
  stage.sampleCount++;
  stage.inSum += static_cast<double>(in);
  stage.outSum += static_cast<double>(out);
  stage.inSumSq += static_cast<double>(in) * static_cast<double>(in);
  stage.outSumSq += static_cast<double>(out) * static_cast<double>(out);
  stage.peakIn = std::max(stage.peakIn, std::fabs(in));
  stage.peakOut = std::max(stage.peakOut, std::fabs(out));
  if (std::fabs(in) > clipThreshold) stage.clipCountIn++;
  if (std::fabs(out) > clipThreshold) stage.clipCountOut++;
  if (stage.fftFill < stage.fftTimeBuffer.size()) {
    stage.fftTimeBuffer[stage.fftFill++] = out;
  }
  if (stage.fftFill == stage.fftTimeBuffer.size()) {
    accumulateCalibrationSpectrum(stage, radio.sampleRate, false);
  }
}

static void updateCalibrationSnapshot(Radio1938& radio) {
  if (!radio.calibration.enabled) return;
  for (auto& stage : radio.calibration.stages) {
    stage.updateSnapshot(radio.sampleRate);
  }
  radio.calibration.detectorNodeVolts.updateSnapshot();
  radio.calibration.receiverGridVolts.updateSnapshot();
  radio.calibration.receiverPlateSwingVolts.updateSnapshot();
  radio.calibration.driverGridVolts.updateSnapshot();
  radio.calibration.driverPlateSwingVolts.updateSnapshot();
  radio.calibration.outputGridAVolts.updateSnapshot();
  radio.calibration.outputGridBVolts.updateSnapshot();
  radio.calibration.outputPrimaryVolts.updateSnapshot();
  radio.calibration.speakerSecondaryVolts.updateSnapshot();

  if (radio.calibration.totalSamples > 0) {
    float invCount = 1.0f / static_cast<float>(radio.calibration.totalSamples);
    radio.calibration.limiterDutyCycle =
        radio.calibration.limiterActiveSamples * invCount;
    radio.calibration.limiterAverageGainReduction =
        static_cast<float>(radio.calibration.limiterGainReductionSum * invCount);
    radio.calibration.limiterAverageGainReductionDb =
        static_cast<float>(radio.calibration.limiterGainReductionDbSum * invCount);
  }

  if (radio.calibration.validationSampleCount > 0) {
    float invValidationCount =
        1.0f / static_cast<float>(radio.calibration.validationSampleCount);
    radio.calibration.driverGridPositiveFraction =
        radio.calibration.driverGridPositiveSamples * invValidationCount;
    radio.calibration.outputGridPositiveFraction =
        radio.calibration.outputGridPositiveSamples * invValidationCount;
    radio.calibration.outputGridAPositiveFraction =
        radio.calibration.outputGridAPositiveSamples * invValidationCount;
    radio.calibration.outputGridBPositiveFraction =
        radio.calibration.outputGridBPositiveSamples * invValidationCount;
    radio.calibration.interstageSecondaryRmsVolts =
        std::sqrt(radio.calibration.interstageSecondarySumSq *
                  invValidationCount);
  }

  const auto& receiverStage =
      radio.calibration.stages[static_cast<size_t>(StageId::ReceiverCircuit)];
  const auto& powerStage =
      radio.calibration.stages[static_cast<size_t>(StageId::Power)];
  radio.calibration.detectorIfCrackleEventCount =
      radio.demod.am.ifCrackleEventCount;
  radio.calibration.detectorIfCrackleMaxBurstAmp =
      radio.demod.am.ifCrackleMaxBurstAmp;
  radio.calibration.detectorIfCrackleMaxEnv = radio.demod.am.ifCrackleMaxEnv;
  radio.calibration.validationDriverGridPositive =
      radio.calibration.driverGridPositiveFraction > 0.01f;
  radio.calibration.validationOutputGridAPositive =
      radio.calibration.outputGridAPositiveFraction > 0.002f;
  radio.calibration.validationOutputGridBPositive =
      radio.calibration.outputGridBPositiveFraction > 0.002f;
  radio.calibration.validationOutputGridPositive =
      radio.calibration.validationOutputGridAPositive ||
      radio.calibration.validationOutputGridBPositive;
  radio.calibration.validationSpeakerOverReference =
      radio.calibration.maxSpeakerReferenceRatio > 1.10f;
  radio.calibration.validationInterstageSecondary =
      radio.calibration.interstageSecondaryPeakVolts >
      4.0f * std::max(std::fabs(radio.power.outputTubeBiasVolts), 1.0f);
  radio.calibration.validationDcShift =
      (std::fabs(receiverStage.meanOut) >
           std::max(0.02, 0.10 * receiverStage.rmsOut)) ||
      (std::fabs(powerStage.meanOut) >
           std::max(0.10, 0.05 * powerStage.peakOut));
  radio.calibration.validationDigitalClip =
      radio.calibration.maxDigitalOutput > 1.0f ||
      radio.diagnostics.outputClip || radio.diagnostics.finalLimiterActive;
  radio.calibration.validationFailed =
      radio.calibration.validationDriverGridPositive ||
      radio.calibration.validationOutputGridPositive ||
      radio.calibration.validationSpeakerOverReference ||
      radio.calibration.validationInterstageSecondary ||
      radio.calibration.validationDcShift ||
      radio.calibration.validationDigitalClip;
}

template <typename InputSampleFn, typename OutputSampleFn>
static void processRadioFrames(Radio1938& radio,
                               uint32_t frames,
                               StageId programStartStage,
                               InputSampleFn&& inputSample,
                               OutputSampleFn&& outputSample) {
  if (frames == 0 || radio.graph.bypass) return;

  radio.graph.compile();
  radio.diagnostics.reset();
  const auto& blockOrder = radio.graph.blockOrder();
  const auto& sampleOrder = radio.graph.sampleControlOrder();
  const auto& programOrder = radio.graph.programPathOrder();
  RadioBlockControl block =
      runBlockPrepare(radio, radio.graph.blockSteps, blockOrder, frames);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    RadioSampleContext ctx{};
    ctx.block = &block;
    runSampleControl(radio, ctx, radio.graph.sampleControlSteps, sampleOrder);
    float x = inputSample(frame);
    size_t programStartIndex = findOrderIndexAtOrAfter(
        radio.graph.programPathSteps, programOrder, programStartStage);
    size_t digitalStartIndex = findOrderIndexAtOrAfter(
        radio.graph.programPathSteps, programOrder, StageId::FinalLimiter);
    // Physical-domain chain: RF/input through speaker/cabinet.
    float yPhysical = runProgramPath(
        radio, x, ctx, radio.graph.programPathSteps, programOrder,
        programStartIndex, digitalStartIndex);
    float yDigital = scalePhysicalSpeakerVoltsToDigital(radio, yPhysical);
    yDigital = applyDigitalMakeupGain(radio, yDigital);
    // Digital tail: limiter and output clip.
    yDigital = runProgramPath(
        radio, yDigital, ctx, radio.graph.programPathSteps, programOrder,
        digitalStartIndex, programOrder.size());
    if (radio.calibration.enabled) {
      radio.calibration.maxDigitalOutput =
          std::max(radio.calibration.maxDigitalOutput, std::fabs(yDigital));
    }
    outputSample(frame, yDigital);
  }

  if (radio.calibration.enabled) {
    updateCalibrationSnapshot(radio);
  }
}

static float sampleInterleavedToMono(const float* samples,
                                     uint32_t frame,
                                     int channels) {
  if (!samples) return 0.0f;
  if (channels <= 1) return samples[frame];
  float sum = 0.0f;
  size_t base = static_cast<size_t>(frame) * static_cast<size_t>(channels);
  for (int channel = 0; channel < channels; ++channel) {
    sum += samples[base + static_cast<size_t>(channel)];
  }
  return sum / static_cast<float>(channels);
}

static void writeMonoToInterleaved(float* samples,
                                   uint32_t frame,
                                   int channels,
                                   float y) {
  if (!samples) return;
  if (channels <= 1) {
    samples[frame] = y;
    return;
  }
  size_t base = static_cast<size_t>(frame) * static_cast<size_t>(channels);
  for (int channel = 0; channel < channels; ++channel) {
    samples[base + static_cast<size_t>(channel)] = y;
  }
}

// Preset and identity wiring
static void applyPhilco37116Preset(Radio1938& radio) {
  radio.identity.driftDepth = 0.06f;
  radio.globals.ifNoiseMix = 0.22f;
  radio.globals.inputPad = 1.0f;
  radio.globals.enableAutoLevel = false;
  radio.globals.autoTargetDb = -21.0f;
  radio.globals.autoMaxBoostDb = 2.5f;

  radio.tuning.safeBwMinHz = 2400.0f;
  radio.tuning.safeBwMaxHz = 9000.0f;
  radio.tuning.preBwScale = 1.00f;
  radio.tuning.postBwScale = 1.00f;
  radio.tuning.smoothTau = 0.05f;
  radio.tuning.updateEps = 0.25f;
  radio.tuning.magneticTuningEnabled = true;
  radio.tuning.afcCaptureHz = 420.0f;
  radio.tuning.afcMaxCorrectionHz = 110.0f;
  radio.tuning.afcDeadband = 0.015f;
  radio.tuning.afcResponseMs = 240.0f;

  radio.frontEnd.inputHpHz = 115.0f;
  radio.frontEnd.rfGain = 1.0f;
  radio.frontEnd.avcGainDepth = 0.18f;
  radio.frontEnd.selectivityPeakHz = 0.0f;
  radio.frontEnd.selectivityPeakQ = 0.707f;
  radio.frontEnd.selectivityPeakGainDb = 0.0f;
  radio.frontEnd.antennaInductanceHenries = 0.011f;
  radio.frontEnd.antennaLoadResistanceOhms = 2200.0f;
  radio.frontEnd.rfInductanceHenries = 0.016f;
  radio.frontEnd.rfLoadResistanceOhms = 3300.0f;

  // Philco 37-116 uses a 6L7G mixer. RCA's 1935 release data gives about
  // 250 V plate supply, 160 V screen, grid-1 bias near -6 V, grid-3 bias near
  // -20 V, 25 V peak oscillator injection, about 3.5 mA plate current, and at
  // least 325 umho conversion conductance. The reduced-order mixer needs a
  // finite DC plate drop for its operating-point fit, so keep the quiescent
  // plate near the same 10 k load order used for the AC conversion model
  // instead of the impossible 250 V-at-3.5 mA no-drop placeholder.
  radio.mixer.rfGridDriveVolts = 1.0f;
  radio.mixer.loGridDriveVolts = 18.0f;
  radio.mixer.loGridBiasVolts = -15.0f;
  radio.mixer.avcGridDriveVolts = 24.0f;
  radio.mixer.plateSupplyVolts = 250.0f;
  radio.mixer.plateDcVolts = 215.0f;
  radio.mixer.screenVolts = 160.0f;
  radio.mixer.biasVolts = -6.0f;
  radio.mixer.cutoffVolts = -45.0f;
  radio.mixer.plateCurrentAmps = 0.0035f;
  radio.mixer.mutualConductanceSiemens = 0.0011f;
  radio.mixer.acLoadResistanceOhms = 10000.0f;
  radio.mixer.plateKneeVolts = 22.0f;
  radio.mixer.gridSoftnessVolts = 2.0f;

  radio.ifStrip.enabled = true;
  radio.ifStrip.ifMinBwHz = 2400.0f;
  radio.ifStrip.primaryInductanceHenries = 0.00022f;
  radio.ifStrip.secondaryInductanceHenries = 0.00025f;
  radio.ifStrip.secondaryLoadResistanceOhms = 680.0f;
  // Reduced-order IF gain stands in for the missing plate-voltage swing of the
  // 470 kHz strip. Calibrate it to land the detector audio in the few-hundred
  // millivolt range before the 6J5/6F6/6B4 chain, rather than recovering that
  // level digitally after the speaker model.
  radio.ifStrip.stageGain = 2.0f;
  radio.ifStrip.avcGainDepth = 0.18f;
  radio.ifStrip.ifCenterHz = 470000.0f;
  radio.ifStrip.interstageCouplingCoeff = 0.15f;
  radio.ifStrip.outputCouplingCoeff = 0.11f;

  radio.demod.am.audioDiodeDrop = 0.0100f;
  radio.demod.am.avcDiodeDrop = 0.0080f;
  radio.demod.am.audioJunctionSlopeVolts = 0.0045f;
  radio.demod.am.avcJunctionSlopeVolts = 0.0040f;
  // Reduced-order 6H6 detector network: detector storage and delayed AVC live
  // here, while the explicit volume/loudness/grid-coupling load is solved in
  // the first-audio receiver stage and applied directly back onto the detector.
  radio.demod.am.detectorStorageCapFarads = 350e-12f;
  radio.demod.am.audioChargeResistanceOhms = 5100.0f;
  radio.demod.am.audioDischargeResistanceOhms = 160000.0f;
  radio.demod.am.avcChargeResistanceOhms = 1000000.0f;
  radio.demod.am.avcDischargeResistanceOhms =
      parallelResistance(1000000.0f, 1000000.0f);
  radio.demod.am.avcFilterCapFarads = 0.15e-6f;
  // Let delayed AVC ride on a multi-volt detector reference so the detector
  // can build a realistic carrier/audio voltage before the RF/IF gain is
  // pinched back.
  radio.demod.am.controlVoltageRef = 3.0f;
  radio.demod.am.senseLowHz = 0.0f;
  radio.demod.am.senseHighHz = 0.0f;
  radio.demod.am.afcSenseLpHz = 34.0f;

  radio.receiverCircuit.enabled = true;
  // Philco 37-116 control 33-5158 is 2 Mohm total with a 1 Mohm tap. The
  // surrounding RC here stays reduced-order, but the control geometry and
  // first-audio tube are now anchored to the 37-116 chassis rather than 116X.
  radio.receiverCircuit.volumeControlResistanceOhms = 2000000.0f;
  radio.receiverCircuit.volumeControlTapResistanceOhms = 1000000.0f;
  radio.receiverCircuit.volumeControlPosition = 1.0f;
  radio.receiverCircuit.volumeControlLoudnessResistanceOhms = 490000.0f;
  radio.receiverCircuit.volumeControlLoudnessCapFarads = 0.015e-6f;
  radio.receiverCircuit.couplingCapFarads = 0.01e-6f;
  radio.receiverCircuit.gridLeakResistanceOhms = 1000000.0f;
  // The 37-116 first audio stage is a 6J5G triode. A common 250 V / -8 V
  // operating point with about 9 mA plate current, rp about 7.7k, gm about
  // 2600 umho, and mu about 20 puts the plate near the roughly 90 V socket
  // reading shown in the service data with a ~15k plate load.
  radio.receiverCircuit.tubePlateSupplyVolts = 250.0f;
  radio.receiverCircuit.tubePlateDcVolts = 90.0f;
  radio.receiverCircuit.tubeScreenVolts = 0.0f;
  radio.receiverCircuit.tubeBiasVolts = -8.0f;
  radio.receiverCircuit.tubePlateCurrentAmps = 0.009f;
  radio.receiverCircuit.tubeMutualConductanceSiemens = 0.0026f;
  radio.receiverCircuit.tubeMu = 20.0f;
  radio.receiverCircuit.tubeTriodeConnected = true;
  radio.receiverCircuit.tubeLoadResistanceOhms = 15000.0f;
  radio.receiverCircuit.tubePlateKneeVolts = 16.0f;
  radio.receiverCircuit.tubeGridSoftnessVolts = 0.6f;

  radio.tone.presenceHz = 0.0f;
  radio.tone.presenceQ = 0.78f;
  radio.tone.presenceGainDb = 0.0f;
  radio.tone.tiltSplitHz = 0.0f;

  radio.power.sagStart = 0.06f;
  radio.power.sagEnd = 0.22f;
  radio.power.rippleDepth = 0.01f;
  radio.power.sagAttackMs = 60.0f;
  radio.power.sagReleaseMs = 900.0f;
  radio.power.rectifierMinHz = 80.0f;
  radio.power.rippleSecondHarmonicMix = 0.0f;
  radio.power.gainSagPerPower = 0.015f;
  radio.power.rippleGainBase = 0.20f;
  radio.power.rippleGainDepth = 0.30f;
  radio.power.gainMin = 0.92f;
  radio.power.gainMax = 1.02f;
  radio.power.supplyDriveDepth = 0.01f;
  radio.power.supplyBiasDepth = 0.0f;
  radio.power.postLpHz = 0.0f;
  // The 37-116 driver is a triode-connected 6F6G feeding the 6B4G pair through
  // an interstage transformer. Tung-Sol RC-13 gives 250 V plate, 250 V screen,
  // -20 V grid, about 21 mA plate current, mu about 6.8, gm about 2600 umho,
  // and 4000 ohm load for single-ended class-A triode operation.
  radio.power.gridCouplingCapFarads = 0.05e-6f;
  radio.power.gridLeakResistanceOhms = 100000.0f;
  radio.power.tubePlateSupplyVolts = 250.0f;
  radio.power.tubeScreenVolts = 250.0f;
  radio.power.tubeBiasVolts = -20.0f;
  radio.power.tubePlateCurrentAmps = 0.021f;
  radio.power.tubeMutualConductanceSiemens = 0.0026f;
  radio.power.tubeMu = 6.8f;
  radio.power.tubeTriodeConnected = true;
  radio.power.tubeAcLoadResistanceOhms = 4000.0f;
  radio.power.tubePlateKneeVolts = 24.0f;
  radio.power.tubeGridSoftnessVolts = 0.8f;
  radio.power.tubeGridCurrentResistanceOhms = 1000.0f;
  radio.power.outputGridLeakResistanceOhms = 250000.0f;
  radio.power.outputGridCurrentResistanceOhms = 1200.0f;
  radio.power.interstagePrimaryLeakageInductanceHenries = 0.45f;
  radio.power.interstageMagnetizingInductanceHenries = 15.0f;
  radio.power.interstagePrimaryResistanceOhms = 430.0f;
  radio.power.tubePlateDcVolts =
      radio.power.tubePlateSupplyVolts -
      radio.power.tubePlateCurrentAmps *
          radio.power.interstagePrimaryResistanceOhms;
  // The 6F6 driver must voltage-step up into the fixed-bias 6B4G pair; the
  // earlier 1:1.4 and 1:3 equivalents still left each 6B4 grid far below the
  // AB1 drive window. In this center-tapped model the ratio is primary to full
  // secondary, so a 1:6 transformer means about 1:3 step-up to each grid half.
  // Keep enough voltage gain for the 6B4 pair, but avoid using the interstage
  // transformer itself as a hidden loudness boost.
  radio.power.interstageTurnsRatioPrimaryToSecondary = 1.0f / 6.0f;
  radio.power.interstagePrimaryCoreLossResistanceOhms = 220000.0f;
  // The pF stray capacitances are ultrasonic details in the real chassis.
  // In this 48 kHz reduced-order audio model they destabilize the transformer
  // solve and collapse the audible output, so the preset leaves them out.
  radio.power.interstagePrimaryShuntCapFarads = 0.0f;
  radio.power.interstageSecondaryLeakageInductanceHenries = 0.040f;
  radio.power.interstageSecondaryResistanceOhms = 296.0f;
  radio.power.interstageSecondaryShuntCapFarads = 0.0f;
  radio.power.interstageIntegrationSubsteps = 8;
  // The 37-116 uses push-pull 6B4G output tubes. Tung-Sol gives the fixed-bias
  // AB1 pair at 325 V plate, -68 V grid, 40 mA zero-signal plate current per
  // tube, and 3000 ohm plate-to-plate load for 15 W output. The 6B4G is the
  // octal 2A3 family member, so keep the same mu/gm class but bias it at the
  // published Philco-era AB1 point instead of the earlier too-hot -39 V guess.
  radio.power.outputTubePlateSupplyVolts = 325.0f;
  radio.power.outputTubeBiasVolts = -68.0f;
  radio.power.outputTubePlateCurrentAmps = 0.040f;
  radio.power.outputTubeMutualConductanceSiemens = 0.00525f;
  radio.power.outputTubeMu = 4.2f;
  radio.power.outputTubePlateToPlateLoadOhms = 3000.0f;
  radio.power.outputTubePlateKneeVolts = 18.0f;
  radio.power.outputTubeGridSoftnessVolts = 2.0f;
  radio.power.outputTransformerPrimaryLeakageInductanceHenries = 35e-3f;
  radio.power.outputTransformerMagnetizingInductanceHenries = 20.0f;
  radio.power.outputTransformerTurnsRatioPrimaryToSecondary =
      std::sqrt(3000.0f / 3.9f);
  radio.power.outputTransformerPrimaryResistanceOhms = 235.0f;
  radio.power.outputTubePlateDcVolts =
      radio.power.outputTubePlateSupplyVolts -
      radio.power.outputTubePlateCurrentAmps *
          (0.5f * radio.power.outputTransformerPrimaryResistanceOhms);
  radio.power.outputTransformerPrimaryCoreLossResistanceOhms = 90000.0f;
  radio.power.outputTransformerPrimaryShuntCapFarads = 0.0f;
  radio.power.outputTransformerSecondaryLeakageInductanceHenries = 60e-6f;
  radio.power.outputTransformerSecondaryResistanceOhms = 0.32f;
  radio.power.outputTransformerSecondaryShuntCapFarads = 0.0f;
  radio.power.outputTransformerIntegrationSubsteps = 8;
  radio.power.outputLoadResistanceOhms = 3.9f;
  // The clean output power reference is derived from the modeled 6B4/output
  // transformer combination during RadioPowerNode::init, not hard-coded here.
  radio.power.nominalOutputPowerWatts = 0.0f;
  assert(std::fabs(radio.power.tubePlateSupplyVolts -
                   radio.power.tubePlateCurrentAmps *
                       radio.power.interstagePrimaryResistanceOhms -
                   radio.power.tubePlateDcVolts) < 1.0f);
  assert(std::fabs(radio.power.outputTubePlateSupplyVolts -
                   radio.power.outputTubePlateCurrentAmps *
                       (0.5f * radio.power.outputTransformerPrimaryResistanceOhms) -
                   radio.power.outputTubePlateDcVolts) < 1.0f);
  radio.output.digitalMakeupGain = 1.0f;

  // --- Global oversampling and output clip settings ---
  // The speaker, limiter and output-clip stages use processOversampled2x, so
  // the oversample factor for biquad anti-alias filters is 2.
  radio.globals.oversampleFactor = 2.0f;
  radio.globals.oversampleCutoffFraction = 0.45f;
  radio.globals.outputClipThreshold = 1.0f;
  radio.globals.postNoiseMix = 0.35f;
  radio.globals.noiseFloorAmp = 0.0f;

  // --- Noise configuration (mains frequency feeds the physical ripple model;
  // hiss/crackle remain explicit stochastic stages) ---
  radio.noiseConfig.enableHumTone = false;
  radio.noiseConfig.humHzDefault = 60.0f;
  radio.noiseConfig.noiseWeightRef = 0.15f;
  radio.noiseConfig.noiseWeightScaleMax = 2.0f;
  radio.noiseConfig.humAmpScale = 0.0f;
  radio.noiseConfig.crackleAmpScale = 0.025f;
  radio.noiseConfig.crackleRateScale = 1.2f;

  // --- Noise shaping (tube hiss spectral character for 1930s receiver) ---
  radio.noiseRuntime.hum.noiseHpHz = 500.0f;
  radio.noiseRuntime.hum.noiseLpHz = 5500.0f;
  radio.noiseRuntime.hum.filterQ = kRadioBiquadQ;
  radio.noiseRuntime.hum.scAttackMs = 2.0f;
  radio.noiseRuntime.hum.scReleaseMs = 80.0f;
  radio.noiseRuntime.hum.crackleDecayMs = 8.0f;
  radio.noiseRuntime.hum.sidechainMaskRef = 0.15f;
  radio.noiseRuntime.hum.hissMaskDepth = 0.5f;
  radio.noiseRuntime.hum.burstMaskDepth = 0.3f;
  radio.noiseRuntime.hum.pinkFastPole = 0.85f;
  radio.noiseRuntime.hum.pinkSlowPole = 0.97f;
  radio.noiseRuntime.hum.brownStep = 0.02f;
  radio.noiseRuntime.hum.hissDriftPole = 0.9992f;
  radio.noiseRuntime.hum.hissDriftNoise = 0.002f;
  radio.noiseRuntime.hum.hissDriftSlowPole = 0.9998f;
  radio.noiseRuntime.hum.hissDriftSlowNoise = 0.001f;
  radio.noiseRuntime.hum.whiteMix = 0.35f;
  radio.noiseRuntime.hum.pinkFastMix = 0.45f;
  radio.noiseRuntime.hum.pinkDifferenceMix = 0.12f;
  radio.noiseRuntime.hum.pinkFastSubtract = 0.6f;
  radio.noiseRuntime.hum.brownMix = 0.08f;
  radio.noiseRuntime.hum.hissBase = 0.7f;
  radio.noiseRuntime.hum.hissDriftDepth = 0.3f;
  radio.noiseRuntime.hum.hissDriftSlowMix = 0.04f;
  radio.noiseRuntime.hum.humSecondHarmonicMix = 0.42f;

  // --- Speaker: 12" electrodynamic field-coil driver (Philco 37-116) ---
  radio.speakerStage.drive = 1.0f;
  radio.speakerStage.speaker.suspensionHz = 65.0f;
  radio.speakerStage.speaker.suspensionQ = 0.90f;
  radio.speakerStage.speaker.suspensionGainDb = 2.2f;
  radio.speakerStage.speaker.coneBodyHz = 1200.0f;
  radio.speakerStage.speaker.coneBodyQ = 0.50f;
  radio.speakerStage.speaker.coneBodyGainDb = 0.25f;
  radio.speakerStage.speaker.topLpHz = 3800.0f;
  radio.speakerStage.speaker.filterQ = kRadioBiquadQ;
  radio.speakerStage.speaker.drive = 1.0f;
  radio.speakerStage.speaker.limit = 0.0f;
  radio.speakerStage.speaker.excursionRef = 8.0f;
  radio.speakerStage.speaker.complianceLossDepth = 0.05f;

  // --- Cabinet: large floor-model console (Philco 37-116) ---
  radio.cabinet.enabled = true;
  radio.cabinet.panelHz = 180.0f;
  radio.cabinet.panelQ = 1.25f;
  radio.cabinet.panelGainDb = 1.0f;
  radio.cabinet.chassisHz = 650.0f;
  radio.cabinet.chassisQ = 0.80f;
  radio.cabinet.chassisGainDb = -0.8f;
  radio.cabinet.cavityDipHz = 900.0f;
  radio.cabinet.cavityDipQ = 1.6f;
  radio.cabinet.cavityDipGainDb = -1.6f;
  radio.cabinet.grilleLpHz = 5000.0f;
  radio.cabinet.rearDelayMs = 0.90f;
  radio.cabinet.rearMix = 0.08f;
  radio.cabinet.rearHpHz = 200.0f;
  radio.cabinet.rearLpHz = 2600.0f;

  // --- Final limiter (digital safety brick-wall) ---
  radio.finalLimiter.enabled = false;
  radio.finalLimiter.threshold = 1.0f;
  radio.finalLimiter.lookaheadMs = 2.0f;
  radio.finalLimiter.attackMs = 0.5f;
  radio.finalLimiter.releaseMs = 80.0f;
}

static void applySetIdentity(Radio1938& radio) {
  auto& identity = radio.identity;
  float drift = std::clamp(identity.driftDepth, 0.0f, 0.25f);
  identity.frontEndAntennaDrift =
      std::clamp(applySeededDrift(1.0f, 0.45f * drift, identity.seed, 0x4101u),
                 0.78f, 1.22f);
  identity.frontEndRfDrift =
      std::clamp(applySeededDrift(1.0f, 0.40f * drift, identity.seed, 0x4102u),
                 0.80f, 1.20f);
  identity.mixerDriveDrift =
      std::clamp(applySeededDrift(1.0f, 0.28f * drift, identity.seed, 0x4103u),
                 0.86f, 1.14f);
  identity.mixerBiasDrift =
      std::clamp(applySeededDrift(1.0f, 0.18f * drift, identity.seed, 0x4104u),
                 0.90f, 1.10f);
  identity.ifPrimaryDrift =
      std::clamp(applySeededDrift(1.0f, 0.35f * drift, identity.seed, 0x4105u),
                 0.84f, 1.16f);
  identity.ifSecondaryDrift =
      std::clamp(applySeededDrift(1.0f, 0.35f * drift, identity.seed, 0x4106u),
                 0.84f, 1.16f);
  identity.ifCouplingDrift =
      std::clamp(applySeededDrift(1.0f, 0.40f * drift, identity.seed, 0x4107u),
                 0.82f, 1.18f);
  identity.detectorLoadDrift =
      std::clamp(applySeededDrift(1.0f, 0.30f * drift, identity.seed, 0x4108u),
                 0.88f, 1.12f);
}

static void refreshIdentityDependentStages(Radio1938& radio) {
  applySetIdentity(radio);
  RadioInitContext initCtx{};
  RadioMixerNode::init(radio, initCtx);
  RadioMixerNode::reset(radio);
  RadioIFStripNode::init(radio, initCtx);
  RadioIFStripNode::reset(radio);
  RadioReceiverCircuitNode::init(radio, initCtx);
  RadioReceiverCircuitNode::reset(radio);
  RadioPowerNode::init(radio, initCtx);
  RadioPowerNode::reset(radio);
  if (radio.calibration.enabled) {
    radio.resetCalibration();
  }
}

// Public Radio1938 entry points
std::string_view Radio1938::presetName(Preset preset) {
  switch (preset) {
    case Preset::Philco37116:
      return "philco_37_116";
  }
  return "philco_37_116";
}

Radio1938::Radio1938() { applyPreset(preset); }

std::string_view Radio1938::stageName(StageId id) {
  switch (id) {
    case StageId::Tuning:
      return "MagneticTuning";
    case StageId::Input:
      return "Input";
    case StageId::AVC:
      return "AVC";
    case StageId::AFC:
      return "AFC";
    case StageId::ControlBus:
      return "ControlBus";
    case StageId::InterferenceDerived:
      return "InterferenceDerived";
    case StageId::FrontEnd:
      return "RFFrontEnd";
    case StageId::Mixer:
      return "Mixer";
    case StageId::IFStrip:
      return "IFStrip";
    case StageId::Demod:
      return "Detector";
    case StageId::ReceiverInputNetwork:
      return "ReceiverInputNetwork";
    case StageId::ReceiverCircuit:
      return "AudioStage";
    case StageId::Tone:
      return "Tone";
    case StageId::Power:
      return "Power";
    case StageId::Noise:
      return "Noise";
    case StageId::Speaker:
      return "Speaker";
    case StageId::Cabinet:
      return "Cabinet";
    case StageId::FinalLimiter:
      return "FinalLimiter";
    case StageId::OutputClip:
      return "OutputClip";
  }
  return "Unknown";
}

bool Radio1938::applyPreset(std::string_view presetNameValue) {
  if (presetNameValue == "philco_37_116") {
    applyPreset(Preset::Philco37116);
    return true;
  }
  if (presetNameValue == "philco_37_116x") {
    applyPreset(Preset::Philco37116);
    return true;
  }
  return false;
}

void Radio1938::applyPreset(Preset presetValue) {
  preset = presetValue;
  switch (presetValue) {
    case Preset::Philco37116:
      applyPhilco37116Preset(*this);
      break;
  }
  if (!initialized) return;
  init(channels, sampleRate, bwHz, noiseWeight);
}

void Radio1938::setIdentitySeed(uint32_t seed) {
  identity.seed = seed;
  if (!initialized) return;
  refreshIdentityDependentStages(*this);
}

void Radio1938::setCalibrationEnabled(bool enabled) {
  calibration.enabled = enabled;
  resetCalibration();
}

void Radio1938::resetCalibration() {
  calibration.reset();
}

void Radio1938::init(int ch, float sr, float bw, float noise) {
  channels = ch;
  sampleRate = sr;
  bwHz = bw;
  noiseWeight = noise;
  applySetIdentity(*this);
  RadioInitContext initCtx{};
  lifecycle.configure(*this, initCtx);
  lifecycle.allocate(*this, initCtx);
  lifecycle.initializeDependentState(*this, initCtx);
  graph.compile();
  initialized = true;
  if (calibration.enabled) {
    resetCalibration();
  }
  reset();
}

void Radio1938::reset() {
  diagnostics.reset();
  iqInput.resetRuntime();
  sourceFrame.resetRuntime();
  lifecycle.resetRuntime(*this);
  if (calibration.enabled) {
    resetCalibration();
  }
}

void Radio1938::processIfReal(float* samples, uint32_t frames) {
  if (!samples || frames == 0) return;
  processRadioFrames(
      *this, frames, StageId::Input,
      [&](uint32_t frame) {
        float x = sampleInterleavedToMono(samples, frame, channels);
        sourceFrame.setRealRf(x);
        return x;
      },
      [&](uint32_t frame, float y) {
        writeMonoToInterleaved(samples, frame, channels, y);
      });
}

void Radio1938::processAmAudio(const float* audioSamples,
                               float* outSamples,
                               uint32_t frames,
                               float receivedCarrierRmsVolts,
                               float modulationIndex) {
  if (!audioSamples || !outSamples || frames == 0) return;
  std::vector<float> rfScratch(frames);
  float safeSampleRate = std::max(sampleRate, 1.0f);
  float carrierHz =
      std::clamp(ifStrip.sourceCarrierHz, 1000.0f, safeSampleRate * 0.45f);
  float carrierStep = kRadioTwoPi * (carrierHz / safeSampleRate);
  float carrierPeak =
      std::sqrt(2.0f) * std::max(receivedCarrierRmsVolts, 0.0f);
  float phase = iqInput.iqPhase;
  for (uint32_t frame = 0; frame < frames; ++frame) {
    float envelopeFactor =
        std::max(0.0f, 1.0f + modulationIndex * audioSamples[frame]);
    float envelope = carrierPeak * envelopeFactor;
    rfScratch[frame] = envelope * std::cos(phase);
    phase += carrierStep;
    if (phase >= kRadioTwoPi) phase -= kRadioTwoPi;
  }
  iqInput.iqPhase = phase;
  processIfReal(rfScratch.data(), frames);
  std::copy(rfScratch.begin(), rfScratch.end(), outSamples);
}

void Radio1938::processIqBaseband(const float* iqInterleaved,
                                  float* outSamples,
                                  uint32_t frames) {
  if (!iqInterleaved || !outSamples || frames == 0) return;
  processRadioFrames(
      *this, frames, StageId::Mixer,
      [&](uint32_t frame) {
        size_t base = static_cast<size_t>(frame) * 2u;
        float i = iqInterleaved[base];
        float q = iqInterleaved[base + 1u];
        sourceFrame.setComplexEnvelope(i, q);
        return i;
      },
      [&](uint32_t frame, float y) {
        writeMonoToInterleaved(outSamples, frame, channels, y);
      });
}
