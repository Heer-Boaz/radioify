#include "../../../radio.h"
#include "radio_calibration_runtime.h"

#include <algorithm>
#include <cmath>
#include <complex>

// Analysis-only calibration sinks for observing pass behavior. This code is
// not part of the runtime signal path and does not behave like a node.

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
    Radio1938::CalibrationPassMetrics& pass,
    float sampleRate,
    bool flushPartial) {
  if (pass.fftFill == 0) return;
  if (!flushPartial && pass.fftFill < kRadioCalibrationFftSize) return;

  std::array<std::complex<float>, kRadioCalibrationFftSize> bins{};
  const auto& window = radioCalibrationWindow();
  for (size_t i = 0; i < kRadioCalibrationFftSize; ++i) {
    float sample = (i < pass.fftFill) ? pass.fftTimeBuffer[i] : 0.0f;
    bins[i] = std::complex<float>(sample * window[i], 0.0f);
  }
  fftInPlace(bins);

  const auto& edges = radioCalibrationBandEdgesHz();
  float binHz = sampleRate / static_cast<float>(kRadioCalibrationFftSize);
  for (size_t i = 1; i < kRadioCalibrationFftBinCount; ++i) {
    float hz = static_cast<float>(i) * binHz;
    double energy = std::norm(bins[i]);
    pass.fftBinEnergy[i] += energy;
    for (size_t band = 0; band < kRadioCalibrationBandCount; ++band) {
      if (hz >= edges[band] && hz < edges[band + 1]) {
        pass.bandEnergy[band] += energy;
        break;
      }
    }
  }

  pass.fftBlockCount++;
  pass.fftTimeBuffer.fill(0.0f);
  pass.fftFill = 0;
}

void Radio1938::CalibrationPassMetrics::clearAccumulators() {
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

void Radio1938::CalibrationPassMetrics::resetMeasurementState() {
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
  speakerReferenceRmsRatio = 0.0f;
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
  for (auto& pass : passes) {
    pass.clearAccumulators();
  }
}

void Radio1938::CalibrationState::resetMeasurementState() {
  for (auto& pass : passes) {
    pass.resetMeasurementState();
  }
}

void Radio1938::CalibrationPassMetrics::updateSnapshot(float sampleRate) {
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

void updatePassCalibration(Radio1938& radio,
                           PassId id,
                           float in,
                           float out) {
  if (!radio.calibration.enabled) return;
  auto& pass = radio.calibration.passes[static_cast<size_t>(id)];
  float clipThreshold = 1.0f;
  if (id == PassId::Power) {
    clipThreshold =
        std::max(radio.output.digitalReferenceSpeakerVoltsPeak, 1e-3f);
  }
  pass.sampleCount++;
  pass.inSum += static_cast<double>(in);
  pass.outSum += static_cast<double>(out);
  pass.inSumSq += static_cast<double>(in) * static_cast<double>(in);
  pass.outSumSq += static_cast<double>(out) * static_cast<double>(out);
  pass.peakIn = std::max(pass.peakIn, std::fabs(in));
  pass.peakOut = std::max(pass.peakOut, std::fabs(out));
  if (std::fabs(in) > clipThreshold) pass.clipCountIn++;
  if (std::fabs(out) > clipThreshold) pass.clipCountOut++;
  if (pass.fftFill < pass.fftTimeBuffer.size()) {
    pass.fftTimeBuffer[pass.fftFill++] = out;
  }
  if (pass.fftFill == pass.fftTimeBuffer.size()) {
    accumulateCalibrationSpectrum(pass, radio.sampleRate, false);
  }
}

void updateCalibrationSnapshot(Radio1938& radio) {
  if (!radio.calibration.enabled) return;
  for (auto& pass : radio.calibration.passes) {
    pass.updateSnapshot(radio.sampleRate);
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
  float referencePeak =
      std::max(radio.output.digitalReferenceSpeakerVoltsPeak, 1e-3f);
  float referenceRms = referencePeak * std::sqrt(0.5f);
  radio.calibration.speakerReferenceRmsRatio =
      radio.calibration.speakerSecondaryVolts.rms /
      std::max(referenceRms, 1e-6f);

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
        static_cast<float>(std::sqrt(radio.calibration.interstageSecondarySumSq *
                                     invValidationCount));
  }

  const auto& receiverPass =
      radio.calibration.passes[static_cast<size_t>(PassId::ReceiverCircuit)];
  const auto& powerPass =
      radio.calibration.passes[static_cast<size_t>(PassId::Power)];
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
      radio.calibration.speakerReferenceRmsRatio > 1.10f;
  radio.calibration.validationInterstageSecondary =
      radio.calibration.interstageSecondaryPeakVolts >
      4.0f * std::max(std::fabs(radio.power.outputTubeBiasVolts), 1.0f);
  radio.calibration.validationDcShift =
      (std::fabs(receiverPass.meanOut) >
           std::max(0.02, 0.10 * receiverPass.rmsOut)) ||
      (std::fabs(powerPass.meanOut) >
           std::max(0.10, 0.05 * powerPass.peakOut));
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
