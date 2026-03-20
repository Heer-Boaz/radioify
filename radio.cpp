#include "radio.h"

#include <algorithm>
#include <cmath>
#include <complex>

using BlockStep = Radio1938::BlockStep;
using AllocateStep = Radio1938::AllocateStep;
using ConfigureStep = Radio1938::ConfigureStep;
using InitializeDependentStateStep = Radio1938::InitializeDependentStateStep;
using PresentationPathStep = Radio1938::PresentationPathStep;
using ProgramPathStep = Radio1938::ProgramPathStep;
using ResetStep = Radio1938::ResetStep;
using SampleControlStep = Radio1938::SampleControlStep;

static inline float clampf(float x, float a, float b) {
  return std::min(std::max(x, a), b);
}

static inline float wrapPhase(float phase) {
  while (phase >= kRadioTwoPi) phase -= kRadioTwoPi;
  while (phase < 0.0f) phase += kRadioTwoPi;
  return phase;
}

static inline float db2lin(float db) { return std::pow(10.0f, db / 20.0f); }

static inline float lin2db(float x) {
  return 20.0f * std::log10(std::max(x, kRadioLinDbFloor));
}

static inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

static inline float seededSignedUnit(uint32_t seed, uint32_t salt) {
  uint32_t h = hash32(seed ^ salt);
  return 2.0f * (static_cast<float>(h) * kRadioHashUnitInv) - 1.0f;
}

static inline float applySeededDrift(float base,
                                     float relativeDepth,
                                     uint32_t seed,
                                     uint32_t salt) {
  return base * (1.0f + relativeDepth * seededSignedUnit(seed, salt));
}

static void applyNoiseHumDefaults(NoiseHum& hum) {
  hum.fs = 48000.0f;
  hum.noiseHpHz = 220.0f;
  hum.noiseLpHz = 5500.0f;
  hum.humHz = 50.0f;
  hum.filterQ = kRadioBiquadQ;
  hum.scAttackMs = 10.0f;
  hum.scReleaseMs = 320.0f;
  hum.crackleDecayMs = 12.0f;
  hum.sidechainMaskRef = 0.18f;
  hum.hissMaskDepth = 0.12f;
  hum.burstMaskDepth = 0.04f;
  hum.pinkFastPole = 0.985f;
  hum.pinkSlowPole = 0.9975f;
  hum.brownStep = 0.0015f;
  hum.hissDriftPole = 0.9995f;
  hum.hissDriftNoise = 0.0005f;
  hum.hissDriftSlowPole = 0.99985f;
  hum.hissDriftSlowNoise = 0.00015f;
  hum.whiteMix = 0.82f;
  hum.pinkFastMix = 0.22f;
  hum.pinkDifferenceMix = 0.12f;
  hum.pinkFastSubtract = 0.5f;
  hum.brownMix = 0.08f;
  hum.hissBase = 0.88f;
  hum.hissDriftDepth = 0.20f;
  hum.hissDriftSlowMix = 0.06f;
  hum.humSecondHarmonicMix = 0.35f;
}

static void applyRadioBaseDefaults(Radio1938& radio) {
  radio.sampleRate = 48000.0f;
  radio.channels = 1;
  radio.bwHz = 4800.0f;
  radio.noiseWeight = 0.0f;
  radio.makeupGain = 1.0f;
  radio.presentationGain = 1.0f;
  radio.preset = Radio1938::Preset::Philco37116X;
  radio.initialized = false;

  radio.graph = Radio1938::RadioExecutionGraph{};

  radio.controlSense = Radio1938::RadioControlSenseState{};
  radio.controlBus = Radio1938::RadioControlBusState{};
  radio.identity = Radio1938::IdentityState{};
  radio.iqInput = Radio1938::IqInputState{};
  radio.sourceFrame = Radio1938::SourceFrameState{};
  radio.identity.driftDepth = 1.0f;
  radio.frontEnd = Radio1938::FrontEndNodeState{};
  radio.mixer = Radio1938::MixerNodeState{};
  radio.ifStrip = Radio1938::IFStripNodeState{};
  radio.demod = Radio1938::DemodNodeState{};
  radio.receiverCircuit = Radio1938::ReceiverCircuitNodeState{};
  radio.tone = Radio1938::ToneNodeState{};
  radio.power = Radio1938::PowerNodeState{};
  radio.noiseDerived = Radio1938::NoiseDerivedState{};
  radio.speakerStage = Radio1938::SpeakerStageState{};
  radio.cabinet = Radio1938::CabinetNodeState{};
  radio.finalLimiter = Radio1938::FinalLimiterNodeState{};
  radio.output = Radio1938::OutputNodeState{};

  applyNoiseHumDefaults(radio.noiseRuntime.hum);

  radio.globals.oversampleFactor = 2.0f;
  radio.globals.ifNoiseMix = 0.0f;
  radio.globals.postNoiseMix = 0.0f;
  radio.globals.noiseFloorAmp = 0.0f;
  radio.globals.inputPad = 1.0f;
  radio.globals.enableAutoLevel = false;
  radio.globals.autoTargetDb = 0.0f;
  radio.globals.autoMaxBoostDb = 0.0f;
  radio.globals.satClipDelta = 0.03f;
  radio.globals.satClipMinLevel = 0.70f;
  radio.globals.outputClipThreshold = 0.98f;
  radio.globals.oversampleCutoffFraction = 0.45f;

  radio.tuning.safeBwMinHz = 2400.0f;
  radio.tuning.safeBwMaxHz = 9000.0f;
  radio.tuning.preBwScale = 1.0f;
  radio.tuning.postBwScale = 1.0f;
  radio.tuning.smoothTau = 0.05f;
  radio.tuning.updateEps = 0.25f;
  radio.tuning.magneticTuningEnabled = false;
  radio.tuning.afcCorrectionHz = 0.0f;
  radio.tuning.afcCaptureHz = 0.0f;
  radio.tuning.afcMaxCorrectionHz = 0.0f;
  radio.tuning.afcDeadband = 0.0f;
  radio.tuning.afcResponseMs = 0.0f;

  radio.input.autoEnvAttackMs = 8.0f;
  radio.input.autoEnvReleaseMs = 220.0f;
  radio.input.autoGainAttackMs = 140.0f;
  radio.input.autoGainReleaseMs = 1200.0f;

  radio.frontEnd.rfGain = 1.0f;
  radio.frontEnd.avcGainDepth = 0.0f;
  radio.frontEnd.selectivityPeakHz = 0.0f;
  radio.frontEnd.selectivityPeakQ = 0.707f;
  radio.frontEnd.selectivityPeakGainDb = 0.0f;

  radio.mixer.conversionGain = 1.0f;
  radio.mixer.tuneLossDepth = 0.0f;
  radio.mixer.loFeedthrough = 0.0f;
  radio.mixer.drive = 0.0f;
  radio.mixer.bias = 0.0f;

  radio.ifStrip.enabled = true;
  radio.ifStrip.ifMinBwHz = 2400.0f;
  radio.ifStrip.stageGain = 1.0f;
  radio.ifStrip.avcGainDepth = 0.0f;
  radio.ifStrip.ifCenterHz = 470000.0f;

  radio.cabinet.enabled = false;
  radio.speakerStage.drive = 0.0f;
  radio.speakerStage.speaker.suspensionHz = 0.0f;
  radio.speakerStage.speaker.coneBodyHz = 0.0f;
  radio.speakerStage.speaker.upperBreakupHz = 0.0f;
  radio.speakerStage.speaker.coneDipHz = 0.0f;
  radio.speakerStage.speaker.topLpHz = 0.0f;
  radio.speakerStage.speaker.hfLossLpHz = 0.0f;
  radio.speakerStage.speaker.complianceLossDepth = 0.0f;
  radio.speakerStage.speaker.hfLossDepth = 0.0f;

  radio.demod.am.audioDiodeDrop = 0.0f;
  radio.demod.am.avcDiodeDrop = 0.0f;
  radio.demod.am.detectorChargeResNorm = 1.0f;
  radio.demod.am.audioDischargeMs = 1.0f;
  radio.demod.am.avcChargeMs = 100.0f;
  radio.demod.am.avcReleaseMs = 1000.0f;
  radio.demod.am.controlVoltageRef = 1.0f;
  radio.demod.am.dcMs = 450.0f;
  radio.demod.am.senseLowHz = 120.0f;
  radio.demod.am.senseHighHz = 3600.0f;
  radio.demod.am.audioHpHz = 45.0f;
  radio.demod.am.detLpScale = 0.48f;
  radio.demod.am.detLpMinHz = 120.0f;
  radio.demod.am.audioEnvelopeLpHz = 0.0f;
  radio.demod.am.avcSenseLpHz = 0.0f;
  radio.demod.am.afcSenseLpHz = 0.0f;

  radio.receiverCircuit.postTransformerLpHz = 0.0f;
  radio.receiverCircuit.voltageGain = 1.0f;

  radio.cabinet.clarifier1Hz = 0.0f;
  radio.cabinet.clarifier1Q = 0.0f;
  radio.cabinet.clarifier1Coupling = 0.0f;
  radio.cabinet.clarifier2Hz = 0.0f;
  radio.cabinet.clarifier2Q = 0.0f;
  radio.cabinet.clarifier2Coupling = 0.0f;
  radio.cabinet.clarifier3Hz = 0.0f;
  radio.cabinet.clarifier3Q = 0.0f;
  radio.cabinet.clarifier3Coupling = 0.0f;

  radio.noiseConfig.enableHumTone = true;
  radio.noiseConfig.humHzDefault = 50.0f;
  radio.noiseConfig.noiseWeightRef = 0.015f;
  radio.noiseConfig.noiseWeightScaleMax = 2.0f;
  radio.noiseConfig.humAmpScale = 0.0018f;
  radio.noiseConfig.crackleAmpScale = 0.0f;
  radio.noiseConfig.crackleRateScale = 0.0f;

  radio.finalLimiter.enabled = true;
  radio.finalLimiter.threshold = 0.98f;
  radio.finalLimiter.lookaheadMs = 2.0f;
  radio.finalLimiter.attackMs = 0.20f;
  radio.finalLimiter.releaseMs = 160.0f;

  radio.power.postLpHz = 0.0f;
}

template <typename Nonlinear>
static inline float processOversampled2x(float x,
                                         float& prev,
                                         Biquad& lp1,
                                         Biquad& lp2,
                                         Nonlinear&& nonlinear) {
  float mid = 0.5f * (prev + x);
  float y0 = nonlinear(mid);
  float y1 = nonlinear(x);
  y0 = lp1.process(y0);
  y0 = lp2.process(y0);
  y1 = lp1.process(y1);
  y1 = lp2.process(y1);
  prev = x;
  return y1;
}

float Biquad::process(float x) {
  float y = b0 * x + z1;
  z1 = b1 * x - a1 * y + z2;
  z2 = b2 * x - a2 * y;
  return y;
}

void Biquad::reset() {
  z1 = 0.0f;
  z2 = 0.0f;
}

void Biquad::setLowpass(float sampleRate, float freq, float q) {
  float w0 = kRadioTwoPi * freq / sampleRate;
  float cosw = std::cos(w0);
  float sinw = std::sin(w0);
  float alpha = sinw / (2.0f * q);
  float a0 = 1.0f + alpha;
  b0 = (1.0f - cosw) / 2.0f / a0;
  b1 = (1.0f - cosw) / a0;
  b2 = (1.0f - cosw) / 2.0f / a0;
  a1 = -2.0f * cosw / a0;
  a2 = (1.0f - alpha) / a0;
}

void Biquad::setHighpass(float sampleRate, float freq, float q) {
  float w0 = kRadioTwoPi * freq / sampleRate;
  float cosw = std::cos(w0);
  float sinw = std::sin(w0);
  float alpha = sinw / (2.0f * q);
  float a0 = 1.0f + alpha;
  b0 = (1.0f + cosw) / 2.0f / a0;
  b1 = -(1.0f + cosw) / a0;
  b2 = (1.0f + cosw) / 2.0f / a0;
  a1 = -2.0f * cosw / a0;
  a2 = (1.0f - alpha) / a0;
}

void Biquad::setBandpass(float sampleRate, float freq, float q) {
  float w0 = kRadioTwoPi * freq / sampleRate;
  float cosw = std::cos(w0);
  float sinw = std::sin(w0);
  float alpha = sinw / (2.0f * q);
  float a0 = 1.0f + alpha;
  b0 = alpha / a0;
  b1 = 0.0f;
  b2 = -alpha / a0;
  a1 = -2.0f * cosw / a0;
  a2 = (1.0f - alpha) / a0;
}

void Biquad::setPeaking(float sampleRate, float freq, float q, float gainDb) {
  float a = std::pow(10.0f, gainDb / 40.0f);
  float w0 = kRadioTwoPi * freq / sampleRate;
  float cosw = std::cos(w0);
  float sinw = std::sin(w0);
  float alpha = sinw / (2.0f * q);
  float a0 = 1.0f + alpha / a;
  b0 = (1.0f + alpha * a) / a0;
  b1 = -2.0f * cosw / a0;
  b2 = (1.0f - alpha * a) / a0;
  a1 = -2.0f * cosw / a0;
  a2 = (1.0f - alpha / a) / a0;
}

void Compressor::setFs(float newFs) {
  fs = newFs;
  setTimes(attackMs, releaseMs);
}

void Compressor::setTimes(float aMs, float rMs) {
  attackMs = aMs;
  releaseMs = rMs;
  atkCoeff = std::exp(-1.0f / (fs * (attackMs / 1000.0f)));
  relCoeff = std::exp(-1.0f / (fs * (releaseMs / 1000.0f)));
  gainAtkCoeff = std::exp(-1.0f / (fs * (attackMs / 1000.0f)));
  gainRelCoeff = std::exp(-1.0f / (fs * (releaseMs / 1000.0f)));
}

void Compressor::reset() {
  env = 0.0f;
  gainDb = 0.0f;
}

float Compressor::process(float x) {
  float a = std::fabs(x);
  if (a > env) {
    env = atkCoeff * env + (1.0f - atkCoeff) * a;
  } else {
    env = relCoeff * env + (1.0f - relCoeff) * a;
  }

  float levelDb = lin2db(env);
  float targetGrDb = 0.0f;
  if (levelDb > thresholdDb) {
    float over = levelDb - thresholdDb;
    float compressedOver = over / ratio;
    float outDb = thresholdDb + compressedOver;
    targetGrDb = outDb - levelDb;
  }

  if (targetGrDb < gainDb) {
    gainDb = gainAtkCoeff * gainDb + (1.0f - gainAtkCoeff) * targetGrDb;
  } else {
    gainDb = gainRelCoeff * gainDb + (1.0f - gainRelCoeff) * targetGrDb;
  }

  return x * db2lin(gainDb);
}

float Saturator::process(float x) {
  float yd = std::tanh(drive * x) / std::tanh(drive);
  return (1.0f - mix) * x + mix * yd;
}

static inline float softClip(float x,
                             float t = kRadioSoftClipThresholdDefault) {
  float ax = std::fabs(x);
  if (ax <= t) return x;
  float s = (x < 0.0f) ? -1.0f : 1.0f;
  float u = (ax - t) / (1.0f - t);
  float y = t + (1.0f - std::exp(-u)) * (1.0f - t);
  return s * y;
}

static inline float asymmetricSaturate(float x,
                                       float drive,
                                       float bias,
                                       float asymT) {
  auto shapeHalf = [](float v, float d) {
    return std::tanh(d * v) / std::max(1e-6f, d);
  };
  float shifted = x + bias;
  float posDrive = drive * std::max(0.25f, 1.0f - 0.10f * asymT);
  float negDrive = drive * (1.0f + 0.18f * asymT);
  float y = (shifted >= 0.0f) ? shapeHalf(shifted, posDrive)
                              : shapeHalf(shifted, negDrive);
  float base = (bias >= 0.0f) ? shapeHalf(bias, posDrive)
                              : shapeHalf(bias, negDrive);
  return y - base;
}

void NoiseHum::setFs(float newFs, float noiseBwHz) {
  fs = newFs;
  noiseLpHz = (noiseBwHz > 0.0f) ? noiseBwHz : noiseLpHz;
  float safeLp = std::clamp(noiseLpHz, noiseHpHz + 200.0f, fs * 0.45f);
  hp.setHighpass(fs, noiseHpHz, filterQ);
  lp.setLowpass(fs, safeLp, filterQ);
  crackleHp.setHighpass(fs, noiseHpHz, filterQ);
  crackleLp.setLowpass(fs, safeLp, filterQ);
  hp.reset();
  lp.reset();
  crackleHp.reset();
  crackleLp.reset();
  scAtk = std::exp(-1.0f / (fs * (scAttackMs / 1000.0f)));
  scRel = std::exp(-1.0f / (fs * (scReleaseMs / 1000.0f)));
  crackleDecay = std::exp(-1.0f / (fs * (crackleDecayMs / 1000.0f)));
}

void NoiseHum::reset() {
  humPhase = 0.0f;
  scEnv = 0.0f;
  crackleEnv = 0.0f;
  pinkFast = 0.0f;
  pinkSlow = 0.0f;
  brown = 0.0f;
  hissDrift = 0.0f;
  hissDriftSlow = 0.0f;
  hp.reset();
  lp.reset();
  crackleHp.reset();
  crackleLp.reset();
}

float NoiseHum::process(const NoiseInput& in) {
  float programAbs = std::fabs(in.programSample);
  if (programAbs > scEnv) {
    scEnv = scAtk * scEnv + (1.0f - scAtk) * programAbs;
  } else {
    scEnv = scRel * scEnv + (1.0f - scRel) * programAbs;
  }
  float maskT = clampf(scEnv / sidechainMaskRef, 0.0f, 1.0f);
  float hissMask = 1.0f - hissMaskDepth * maskT;
  float burstMask = 1.0f - burstMaskDepth * maskT;

  float white = dist(rng);
  pinkFast = pinkFastPole * pinkFast + (1.0f - pinkFastPole) * white;
  pinkSlow = pinkSlowPole * pinkSlow + (1.0f - pinkSlowPole) * white;
  brown = clampf(brown + brownStep * white, -1.0f, 1.0f);
  hissDrift = hissDriftPole * hissDrift + hissDriftNoise * dist(rng);
  hissDriftSlow =
      hissDriftSlowPole * hissDriftSlow + hissDriftSlowNoise * dist(rng);
  float n = whiteMix * white + pinkFastMix * pinkFast +
            pinkDifferenceMix * (pinkSlow - pinkFastSubtract * pinkFast) +
            brownMix * brown;
  n *= hissBase + hissDriftDepth * hissDrift;
  n += hissDriftSlowMix * hissDriftSlow;
  n = hp.process(n);
  n = lp.process(n);
  n *= in.noiseAmp * hissMask;

  float c = 0.0f;
  if (in.crackleRate > 0.0f && in.crackleAmp > 0.0f && fs > 0.0f) {
    float chance = in.crackleRate / fs;
    if (dist01(rng) < chance) {
      crackleEnv = 1.0f;
    }
    float raw = dist(rng) * crackleEnv;
    crackleEnv *= crackleDecay;
    raw = crackleHp.process(raw);
    raw = crackleLp.process(raw);
    c = raw * in.crackleAmp * burstMask;
  }

  float h = 0.0f;
  if (in.humToneEnabled && in.humAmp > 0.0f && fs > 0.0f) {
    humPhase += kRadioTwoPi * (humHz / fs);
    if (humPhase > kRadioTwoPi) humPhase -= kRadioTwoPi;
    h = std::sin(humPhase) + humSecondHarmonicMix * std::sin(2.0f * humPhase);
    h *= in.humAmp * hissMask;
  }

  return n + c + h;
}

void AMDetector::init(float newFs, float newBw, float newTuneHz) {
  fs = std::max(newFs, 1.0f);
  bwHz = newBw;
  tuneOffsetHz = newTuneHz;
  float chargeSeconds = 0.00005f * std::max(detectorChargeResNorm, 1e-6f);
  audioChargeCoeff = std::exp(-1.0f / (fs * chargeSeconds));
  audioReleaseCoeff =
      std::exp(-1.0f / (fs * (audioDischargeMs / 1000.0f)));
  avcChargeCoeff = std::exp(-1.0f / (fs * (avcChargeMs / 1000.0f)));
  avcReleaseCoeff = std::exp(-1.0f / (fs * (avcReleaseMs / 1000.0f)));
  dcCoeff = std::exp(-1.0f / (fs * (dcMs / 1000.0f)));
  setBandwidth(newBw, newTuneHz);
  reset();
}

void AMDetector::setBandwidth(float newBw, float newTuneHz) {
  bwHz = newBw;
  tuneOffsetHz = newTuneHz;
  float safeBw = std::max(bwHz, 1.0f);
  float detLpHz =
      std::clamp(safeBw * std::max(detLpScale, 0.1f), 120.0f, fs * 0.10f);
  float envLpHz =
      (audioEnvelopeLpHz > 0.0f)
          ? std::clamp(audioEnvelopeLpHz, 120.0f, fs * 0.10f)
          : detLpHz;
  float avcLpHz =
      (avcSenseLpHz > 0.0f)
          ? std::clamp(avcSenseLpHz, 80.0f, fs * 0.12f)
          : std::clamp(0.08f * safeBw, 80.0f, 1200.0f);
  float lowSenseBound = std::max(40.0f, senseLowHz);
  float highSenseBound = std::max(lowSenseBound + 180.0f, senseHighHz);
  float ifCenter = 0.5f * (lowSenseBound + highSenseBound);
  float afcOffset =
      std::clamp(0.18f * (highSenseBound - lowSenseBound), 120.0f,
                 std::max(120.0f, 0.30f * (highSenseBound - lowSenseBound)));
  float lowSenseHz =
      std::clamp(ifCenter - afcOffset, lowSenseBound, highSenseBound - 180.0f);
  float highSenseHz =
      std::clamp(ifCenter + afcOffset, lowSenseHz + 120.0f, highSenseBound);
  float afcLpHz = (afcSenseLpHz > 0.0f)
                      ? std::clamp(afcSenseLpHz, 5.0f, 180.0f)
                      : 45.0f;
  audioHp.setHighpass(fs, audioHpHz, kRadioBiquadQ);
  audioLp1.setLowpass(fs, detLpHz, kRadioBiquadQ);
  afcLowSense.setBandpass(fs, lowSenseHz, 1.10f);
  afcHighSense.setBandpass(fs, highSenseHz, 1.10f);
  afcErrorLp.setLowpass(fs, afcLpHz, kRadioBiquadQ);
  audioEnvelopeLp.setLowpass(fs, envLpHz, kRadioBiquadQ);
  avcSenseLp.setLowpass(fs, avcLpHz, kRadioBiquadQ);
}

void AMDetector::setSenseWindow(float lowHz, float highHz) {
  senseLowHz = lowHz;
  senseHighHz = highHz;
  if (fs > 0.0f) {
    setBandwidth(bwHz, tuneOffsetHz);
  }
}

void AMDetector::reset() {
  audioRect = 0.0f;
  avcRect = 0.0f;
  audioEnv = 0.0f;
  avcEnv = 0.0f;
  dcEnv = 0.0f;
  afcError = 0.0f;
  afcLowSense.reset();
  afcHighSense.reset();
  afcErrorLp.reset();
  audioHp.reset();
  audioLp1.reset();
  audioEnvelopeLp.reset();
  avcSenseLp.reset();
}

void Radio1938::CalibrationStageMetrics::clearAccumulators() {
  sampleCount = 0;
  rmsIn = 0.0;
  rmsOut = 0.0;
  peakIn = 0.0f;
  peakOut = 0.0f;
  crestIn = 0.0f;
  crestOut = 0.0f;
  spectralCentroidHz = 0.0f;
  bandwidth3dBHz = 0.0f;
  bandwidth6dBHz = 0.0f;
  clipCountIn = 0;
  clipCountOut = 0;
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
  for (auto& stage : stages) {
    stage.clearAccumulators();
  }
}

void Radio1938::CalibrationState::resetMeasurementState() {
  for (auto& stage : stages) {
    stage.resetMeasurementState();
  }
}

static void updateStageCalibration(Radio1938& radio,
                                   StageId id,
                                   float in,
                                   float out) {
  if (!radio.calibration.enabled) return;
  auto& stage =
      radio.calibration.stages[static_cast<size_t>(id)];
  stage.sampleCount++;
  stage.inSumSq += static_cast<double>(in) * static_cast<double>(in);
  stage.outSumSq += static_cast<double>(out) * static_cast<double>(out);
  stage.peakIn = std::max(stage.peakIn, std::fabs(in));
  stage.peakOut = std::max(stage.peakOut, std::fabs(out));
  if (std::fabs(in) > 1.0f) stage.clipCountIn++;
  if (std::fabs(out) > 1.0f) stage.clipCountOut++;
  if (stage.fftFill < stage.fftTimeBuffer.size()) {
    stage.fftTimeBuffer[stage.fftFill++] = out;
  }
  if (stage.fftFill == stage.fftTimeBuffer.size()) {
    accumulateCalibrationSpectrum(stage, radio.sampleRate, false);
  }
}

void Radio1938::CalibrationStageMetrics::updateSnapshot(float sampleRate) {
  accumulateCalibrationSpectrum(*this, sampleRate, true);
  if (sampleCount == 0) return;
  double invCount = 1.0 / static_cast<double>(sampleCount);
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
  spectralCentroidHz = (totalEnergy > 1e-18) ? static_cast<float>(weightedHz / totalEnergy) : 0.0f;
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

static void updateCalibrationSnapshot(Radio1938& radio) {
  if (!radio.calibration.enabled) return;
  for (auto& stage : radio.calibration.stages) {
    stage.updateSnapshot(radio.sampleRate);
  }

  if (radio.calibration.totalSamples > 0) {
    float invCount = 1.0f / static_cast<float>(radio.calibration.totalSamples);
    radio.calibration.limiterDutyCycle =
        radio.calibration.limiterActiveSamples * invCount;
    radio.calibration.limiterAverageGainReduction =
        static_cast<float>(radio.calibration.limiterGainReductionSum * invCount);
    radio.calibration.limiterAverageGainReductionDb =
        static_cast<float>(radio.calibration.limiterGainReductionDbSum * invCount);
  }
}

float AMDetector::process(const AMDetectorSampleInput& in) {
  float ifSample = in.signal;
  if (in.ifNoiseAmp > 0.0f) {
    ifSample += dist(rng) * in.ifNoiseAmp;
  }

  float afcLow = std::fabs(afcLowSense.process(ifSample));
  float afcHigh = std::fabs(afcHighSense.process(ifSample));
  float afcDen = std::max(afcLow + afcHigh, 1e-6f);
  float rawAfcError = (afcHigh - afcLow) / afcDen;
  afcError = afcErrorLp.process(rawAfcError);

  audioRect = std::max(0.0f, ifSample - audioDiodeDrop);
  avcRect = std::max(0.0f, ifSample - avcDiodeDrop);

  if (audioRect > audioEnv) {
    audioEnv = audioChargeCoeff * audioEnv +
               (1.0f - audioChargeCoeff) * audioRect;
  } else {
    audioEnv = audioReleaseCoeff * audioEnv +
               (1.0f - audioReleaseCoeff) * audioRect;
  }

  float detectedAudio = audioEnvelopeLp.process(audioEnv);
  float avcSense = avcSenseLp.process(avcRect);
  if (avcSense > avcEnv) {
    avcEnv = avcChargeCoeff * avcEnv + (1.0f - avcChargeCoeff) * avcSense;
  } else {
    avcEnv =
        avcReleaseCoeff * avcEnv + (1.0f - avcReleaseCoeff) * avcSense;
  }

  dcEnv = dcCoeff * dcEnv + (1.0f - dcCoeff) * detectedAudio;
  float out = detectedAudio - dcEnv;
  out = audioHp.process(out);
  out *= detectorGain;
  return out;
}

void SpeakerSim::init(float fs) {
  float suspensionHzDerived =
      std::clamp(suspensionHz *
                     (1.0f + 0.45f * coneMassTolerance -
                      0.65f * suspensionComplianceTolerance),
                 80.0f, fs * 0.20f);
  float coneBodyHzDerived =
      std::clamp(coneBodyHz *
                     (1.0f + 0.22f * coneMassTolerance +
                      0.16f * voiceCoilTolerance),
                 suspensionHzDerived + 120.0f, fs * 0.30f);
  suspensionRes.setPeaking(fs, suspensionHzDerived, suspensionQ, suspensionGainDb);
  coneBody.setPeaking(fs, coneBodyHzDerived, coneBodyQ, coneBodyGainDb);
  upperBreakup = Biquad{};
  coneDip = Biquad{};
  if (topLpHz > 0.0f) {
    float topLpHzDerived =
        std::clamp(topLpHz /
                       std::max(0.35f, 1.0f + 0.40f * voiceCoilTolerance),
                   coneBodyHzDerived + 220.0f, fs * 0.45f);
    topLp.setLowpass(fs, topLpHzDerived, filterQ);
  } else {
    topLp = Biquad{};
  }
  hfLossLp = Biquad{};
  excursionAtk = std::exp(-1.0f / (fs * 0.010f));
  excursionRel = std::exp(-1.0f / (fs * 0.120f));
}

void SpeakerSim::reset() {
  suspensionRes.reset();
  coneBody.reset();
  upperBreakup.reset();
  coneDip.reset();
  topLp.reset();
  hfLossLp.reset();
  excursionEnv = 0.0f;
}

float SpeakerSim::process(float x, bool& clipped) {
  float y = x * std::max(drive, 0.0f);
  y = suspensionRes.process(y);
  y = coneBody.process(y);
  if (topLpHz > 0.0f) {
    y = topLp.process(y);
  }

  float a = std::fabs(y);
  if (a > excursionEnv) {
    excursionEnv = excursionAtk * excursionEnv + (1.0f - excursionAtk) * a;
  } else {
    excursionEnv = excursionRel * excursionEnv + (1.0f - excursionRel) * a;
  }

  float excursionT =
      clampf(excursionEnv / std::max(excursionRef, 1e-6f), 0.0f, 1.0f);
  float complianceGain = 1.0f - complianceLossDepth * excursionT;
  y *= std::max(0.70f, complianceGain);
  clipped = limit > 0.0f && std::fabs(y) > limit;
  if (limit > 0.0f && limit < 1.0f) {
    return softClip(y, limit);
  }
  return y;
}

float RadioTuningNode::applyFilters(Radio1938& radio, float tuneHz, float bwHz) {
  auto& tuning = radio.tuning;
  auto& frontEnd = radio.frontEnd;
  float safeBw = std::clamp(bwHz, tuning.safeBwMinHz, tuning.safeBwMaxHz);
  float preBw =
      std::clamp(safeBw * tuning.preBwScale, 1200.0f, radio.sampleRate * 0.45f);
  float rfBw =
      std::clamp(safeBw * tuning.postBwScale, preBw, radio.sampleRate * 0.45f);

  frontEnd.preLpfIn.setLowpass(radio.sampleRate, preBw, kRadioBiquadQ);
  frontEnd.preLpfOut.setLowpass(radio.sampleRate, rfBw, kRadioBiquadQ);
  RadioIFStripNode::setBandwidth(radio, safeBw, tuneHz);

  tuning.tunedBw = safeBw;
  return safeBw;
}

void RadioTuningNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& tuning = radio.tuning;
  tuning.afcCorrectionHz = 0.0f;
  initCtx.tunedBw = applyFilters(radio, tuning.tuneOffsetHz, radio.bwHz);
  tuning.tuneAppliedHz = tuning.tuneOffsetHz;
  tuning.bwAppliedHz = radio.bwHz;
  tuning.tuneSmoothedHz = tuning.tuneOffsetHz;
  tuning.bwSmoothedHz = radio.bwHz;
}

void RadioTuningNode::reset(Radio1938& radio) {
  auto& tuning = radio.tuning;
  tuning.afcCorrectionHz = 0.0f;
  tuning.tuneAppliedHz = tuning.tuneOffsetHz;
  tuning.bwAppliedHz = radio.bwHz;
  tuning.tuneSmoothedHz = tuning.tuneOffsetHz;
  tuning.bwSmoothedHz = radio.bwHz;
}

void RadioTuningNode::prepare(Radio1938& radio,
                              RadioBlockControl& block,
                              uint32_t frames) {
  auto& tuning = radio.tuning;
  auto& demod = radio.demod;
  auto& noiseRuntime = radio.noiseRuntime;
  float rate = std::max(1.0f, radio.sampleRate);
  float tick =
      1.0f - std::exp(-static_cast<float>(frames) / (rate * tuning.smoothTau));
  float effectiveTuneHz = tuning.tuneOffsetHz;
  if (tuning.magneticTuningEnabled) {
    effectiveTuneHz += tuning.afcCorrectionHz;
  }
  tuning.tuneSmoothedHz += tick * (effectiveTuneHz - tuning.tuneSmoothedHz);
  tuning.bwSmoothedHz += tick * (radio.bwHz - tuning.bwSmoothedHz);

  float safeBw =
      std::clamp(tuning.bwSmoothedHz, tuning.safeBwMinHz, tuning.safeBwMaxHz);
  float bwHalf = 0.5f * std::max(1.0f, safeBw);
  block.tuneNorm = clampf(tuning.tuneSmoothedHz / bwHalf, -1.0f, 1.0f);

  if (std::fabs(tuning.tuneSmoothedHz - tuning.tuneAppliedHz) > tuning.updateEps ||
      std::fabs(tuning.bwSmoothedHz - tuning.bwAppliedHz) > tuning.updateEps) {
    float tunedBw = applyFilters(radio, tuning.tuneSmoothedHz, tuning.bwSmoothedHz);
    tuning.tuneAppliedHz = tuning.tuneSmoothedHz;
    tuning.bwAppliedHz = tuning.bwSmoothedHz;
    demod.am.setBandwidth(tunedBw, tuning.tuneSmoothedHz);
    noiseRuntime.hum.setFs(radio.sampleRate, tunedBw);
  }
}

void RadioInputNode::init(Radio1938& radio, RadioInitContext&) {
  auto& input = radio.input;
  input.autoEnvAtk =
      std::exp(-1.0f / (radio.sampleRate * (input.autoEnvAttackMs / 1000.0f)));
  input.autoEnvRel =
      std::exp(-1.0f / (radio.sampleRate * (input.autoEnvReleaseMs / 1000.0f)));
  input.autoGainAtk =
      std::exp(-1.0f / (radio.sampleRate * (input.autoGainAttackMs / 1000.0f)));
  input.autoGainRel =
      std::exp(-1.0f / (radio.sampleRate * (input.autoGainReleaseMs / 1000.0f)));
}

void RadioInputNode::reset(Radio1938& radio) {
  radio.input.autoEnv = 0.0f;
  radio.input.autoGainDb = 0.0f;
}

float RadioInputNode::process(Radio1938& radio,
                              float x,
                              const RadioSampleContext&) {
  auto& input = radio.input;
  x *= radio.globals.inputPad;
  if (!radio.globals.enableAutoLevel) return x;

  float ax = std::fabs(x);
  if (ax > input.autoEnv) {
    input.autoEnv = input.autoEnvAtk * input.autoEnv + (1.0f - input.autoEnvAtk) * ax;
  } else {
    input.autoEnv = input.autoEnvRel * input.autoEnv + (1.0f - input.autoEnvRel) * ax;
  }
  float envDb = lin2db(input.autoEnv);
  float targetBoostDb =
      std::clamp(radio.globals.autoTargetDb - envDb, 0.0f,
                 radio.globals.autoMaxBoostDb);
  if (targetBoostDb < input.autoGainDb) {
    input.autoGainDb = input.autoGainAtk * input.autoGainDb +
                       (1.0f - input.autoGainAtk) * targetBoostDb;
  } else {
    input.autoGainDb = input.autoGainRel * input.autoGainDb +
                       (1.0f - input.autoGainRel) * targetBoostDb;
  }
  return x * db2lin(input.autoGainDb);
}

void RadioControlBusNode::init(Radio1938&, RadioInitContext&) {}

void RadioControlBusNode::reset(Radio1938& radio) {
  radio.controlSense.reset();
  radio.controlBus.reset();
}

void RadioAVCNode::update(Radio1938& radio, RadioSampleContext&) {
  auto& controlBus = radio.controlBus;
  const auto& controlSense = radio.controlSense;
  controlBus.controlVoltage =
      clampf(controlSense.controlVoltageSense, 0.0f, 1.25f);
}

void RadioAFCNode::update(Radio1938& radio, RadioSampleContext&) {
  auto& tuning = radio.tuning;
  const auto& controlSense = radio.controlSense;
  if (!tuning.magneticTuningEnabled || tuning.afcMaxCorrectionHz <= 0.0f ||
      tuning.afcResponseMs <= 0.0f) {
    tuning.afcCorrectionHz = 0.0f;
    return;
  }

  float rate = std::max(radio.sampleRate, 1.0f);
  float afcSeconds = tuning.afcResponseMs * 0.001f;
  float afcTick = 1.0f - std::exp(-1.0f / (rate * afcSeconds));
  float error = controlSense.tuningErrorSense;
  if (std::fabs(error) < tuning.afcDeadband) error = 0.0f;
  float captureT =
      1.0f - clampf(std::fabs(tuning.tuneOffsetHz) /
                        std::max(tuning.afcCaptureHz, 1e-6f),
                    0.0f, 1.0f);
  float signalT =
      clampf(controlSense.controlVoltageSense / 0.85f, 0.0f, 1.0f);
  float afcTarget =
      -error * tuning.afcMaxCorrectionHz * captureT * signalT;
  tuning.afcCorrectionHz += afcTick * (afcTarget - tuning.afcCorrectionHz);
  tuning.afcCorrectionHz =
      clampf(tuning.afcCorrectionHz, -tuning.afcMaxCorrectionHz,
             tuning.afcMaxCorrectionHz);
}

void RadioControlBusNode::update(Radio1938& radio, RadioSampleContext&) {
  auto& controlBus = radio.controlBus;
  const auto& controlSense = radio.controlSense;
  const auto& power = radio.power;
  float supplyTarget =
      clampf((controlSense.powerSagSense - power.sagStart) /
                 std::max(1e-6f, power.sagEnd - power.sagStart),
             0.0f, 1.0f);
  controlBus.supplySag = supplyTarget;
}

void RadioFrontEndNode::init(Radio1938& radio, RadioInitContext&) {
  radio.frontEnd.hpf.setHighpass(radio.sampleRate, radio.frontEnd.inputHpHz,
                                 kRadioBiquadQ);
  radio.frontEnd.selectivityPeak.setPeaking(radio.sampleRate,
                                            radio.frontEnd.selectivityPeakHz,
                                            radio.frontEnd.selectivityPeakQ,
                                            radio.frontEnd.selectivityPeakGainDb);
}

void RadioFrontEndNode::reset(Radio1938& radio) {
  auto& frontEnd = radio.frontEnd;
  frontEnd.hpf.reset();
  frontEnd.preLpfIn.reset();
  frontEnd.preLpfOut.reset();
  frontEnd.selectivityPeak.reset();
}

float RadioFrontEndNode::process(Radio1938& radio,
                                 float x,
                                 const RadioSampleContext&) {
  auto& frontEnd = radio.frontEnd;
  float rfHold = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float y = frontEnd.hpf.process(x);
  y *= frontEnd.rfGain * std::max(0.35f, 1.0f - frontEnd.avcGainDepth * rfHold);
  y = frontEnd.preLpfIn.process(y);
  y = frontEnd.preLpfOut.process(y);
  y = frontEnd.selectivityPeak.process(y);
  return y;
}

void RadioMixerNode::init(Radio1938&, RadioInitContext&) {}

void RadioMixerNode::reset(Radio1938&) {}

float RadioMixerNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext&) {
  // Real LO/mixing happens inside the oversampled IF core.
  (void)radio;
  return y;
}

static int computeIfOversampleFactor(float outputFs,
                                     float ifCenterHz,
                                     float bwHz) {
  float safeFs = std::max(outputFs, 1.0f);
  float safeBw = std::max(bwHz, 1.0f);
  float guardHz = std::max(16000.0f, 1.5f * safeBw + 12000.0f);
  float minInternalFs = (ifCenterHz + guardHz) / 0.48f;
  return std::max(8, static_cast<int>(std::ceil(minInternalFs / safeFs)));
}

static float selectSourceCarrierHz(float internalFs, float ifCenterHz) {
  float maxCarrier = 0.48f * internalFs - ifCenterHz - 8000.0f;
  if (maxCarrier <= 12000.0f) return 12000.0f;
  return std::clamp(0.5f * maxCarrier, 12000.0f, 40000.0f);
}

void RadioIFStripNode::init(Radio1938& radio, RadioInitContext&) {
  setBandwidth(radio, radio.bwHz, radio.tuning.tuneOffsetHz);
}

void RadioIFStripNode::reset(Radio1938& radio) {
  auto& ifStrip = radio.ifStrip;
  ifStrip.rfPhase = 0.0f;
  ifStrip.loPhase = 0.0f;
  ifStrip.prevSourceI = 0.0f;
  ifStrip.prevSourceQ = 0.0f;
  ifStrip.bp1.reset();
  ifStrip.bp2.reset();
}

void RadioIFStripNode::setBandwidth(Radio1938& radio, float bwHz, float tuneHz) {
  auto& ifStrip = radio.ifStrip;
  auto& demod = radio.demod;
  float sampleRate = std::max(radio.sampleRate, 1.0f);
  float safeBw = std::max(bwHz, ifStrip.ifMinBwHz);
  ifStrip.oversampleFactor =
      computeIfOversampleFactor(sampleRate, ifStrip.ifCenterHz, safeBw);
  ifStrip.internalSampleRate = sampleRate * ifStrip.oversampleFactor;
  ifStrip.sourceCarrierHz =
      selectSourceCarrierHz(ifStrip.internalSampleRate, ifStrip.ifCenterHz);
  ifStrip.loFrequencyHz = ifStrip.sourceCarrierHz + ifStrip.ifCenterHz + tuneHz;

  float stageBandwidthHz = std::max(safeBw * 1.55f, 600.0f);
  float stageQ = std::max(0.35f, ifStrip.ifCenterHz / stageBandwidthHz);
  ifStrip.bp1.setBandpass(ifStrip.internalSampleRate, ifStrip.ifCenterHz, stageQ);
  ifStrip.bp2.setBandpass(ifStrip.internalSampleRate, ifStrip.ifCenterHz, stageQ);

  float senseLow = ifStrip.ifCenterHz - 0.5f * safeBw;
  float senseHigh = ifStrip.ifCenterHz + 0.5f * safeBw;
  demod.am.setSenseWindow(senseLow, senseHigh);
  if (demod.am.fs > 0.0f) {
    demod.am.setBandwidth(safeBw, tuneHz);
  }
}

float RadioIFStripNode::process(Radio1938& radio,
                                float y,
                                const RadioSampleContext&) {
  (void)radio;
  return y;
}

void RadioDemodNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& demod = radio.demod;
  demod.am.init(radio.ifStrip.internalSampleRate, initCtx.tunedBw,
                radio.tuning.tuneOffsetHz);
}

void RadioDemodNode::reset(Radio1938& radio) { radio.demod.am.reset(); }

static float processSuperhetEnvelopeFrame(Radio1938& radio,
                                          const RadioSampleContext& ctx) {
  auto& frontEnd = radio.frontEnd;
  auto& ifStrip = radio.ifStrip;
  auto& demod = radio.demod.am;
  if (!ifStrip.enabled || ifStrip.internalSampleRate <= 0.0f ||
      ifStrip.oversampleFactor <= 0) {
    return radio.sourceFrame.i;
  }

  float currI = radio.sourceFrame.i;
  float currQ = radio.sourceFrame.q;
  float prevI = ifStrip.prevSourceI;
  float prevQ = ifStrip.prevSourceQ;
  float avcT = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float rfGain =
      frontEnd.rfGain * std::max(0.35f, 1.0f - frontEnd.avcGainDepth * avcT);
  float ifGain =
      ifStrip.stageGain * std::max(0.25f, 1.0f - ifStrip.avcGainDepth * avcT);
  float rfStep =
      kRadioTwoPi * (ifStrip.sourceCarrierHz / ifStrip.internalSampleRate);
  float loStep =
      kRadioTwoPi * (ifStrip.loFrequencyHz / ifStrip.internalSampleRate);
  float noiseAmp =
      ctx.derived.demodIfNoiseAmp / std::sqrt(static_cast<float>(ifStrip.oversampleFactor));
  float audioAcc = 0.0f;

  for (int step = 0; step < ifStrip.oversampleFactor; ++step) {
    float t =
        static_cast<float>(step + 1) / static_cast<float>(ifStrip.oversampleFactor);
    float envI = prevI + (currI - prevI) * t;
    float envQ = prevQ + (currQ - prevQ) * t;
    float rf =
        rfGain * (envI * std::cos(ifStrip.rfPhase) - envQ * std::sin(ifStrip.rfPhase));
    ifStrip.rfPhase = wrapPhase(ifStrip.rfPhase + rfStep);
    float lo = std::cos(ifStrip.loPhase);
    ifStrip.loPhase = wrapPhase(ifStrip.loPhase + loStep);
    float ifSample = 2.0f * rf * lo * ifGain;
    ifSample = ifStrip.bp1.process(ifSample);
    ifSample = ifStrip.bp2.process(ifSample);
    audioAcc += demod.process(AMDetectorSampleInput{ifSample, noiseAmp});
  }

  ifStrip.prevSourceI = currI;
  ifStrip.prevSourceQ = currQ;
  return audioAcc / static_cast<float>(ifStrip.oversampleFactor);
}

float RadioDemodNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext& ctx) {
  auto& demod = radio.demod;
  (void)y;
  y = processSuperhetEnvelopeFrame(radio, ctx);
  radio.controlSense.controlVoltageSense =
      clampf(demod.am.avcEnv / std::max(demod.am.controlVoltageRef, 1e-6f),
             0.0f, 1.25f);
  radio.controlSense.tuningErrorSense = demod.am.afcError;
  return y;
}

void RadioReceiverCircuitNode::init(Radio1938& radio, RadioInitContext&) {
  auto& receiver = radio.receiverCircuit;
  if (receiver.couplingHpHz > 0.0f) {
    float couplingScale =
        std::max(0.25f, (1.0f + receiver.couplingCapTolerance) *
                             (1.0f + receiver.gridLeakTolerance));
    float couplingHpHz =
        std::clamp(receiver.couplingHpHz / couplingScale, 30.0f,
                   radio.sampleRate * 0.25f);
    receiver.couplingHp.setHighpass(radio.sampleRate, couplingHpHz,
                                    kRadioBiquadQ);
  }
  if (receiver.interstagePeakHz > 0.0f &&
      std::fabs(receiver.interstagePeakGainDb) > 1e-4f) {
    float interstagePeakHz = std::clamp(
        receiver.interstagePeakHz *
            (1.0f + 0.5f * receiver.plateLoadTolerance -
             0.35f * receiver.toneCapTolerance),
        120.0f, radio.sampleRate * 0.45f);
    receiver.interstagePeak.setPeaking(radio.sampleRate, interstagePeakHz,
                                       receiver.interstagePeakQ,
                                       receiver.interstagePeakGainDb);
  }
  if (receiver.presenceDipHz > 0.0f &&
      std::fabs(receiver.presenceDipGainDb) > 1e-4f) {
    float presenceDipHz = std::clamp(
        receiver.presenceDipHz *
            (1.0f + 0.4f * receiver.transformerTolerance -
             0.25f * receiver.plateLoadTolerance),
        120.0f, radio.sampleRate * 0.45f);
    receiver.presenceDip.setPeaking(radio.sampleRate, presenceDipHz,
                                    receiver.presenceDipQ,
                                    receiver.presenceDipGainDb);
  }
  if (receiver.transformerLpHz > 0.0f) {
    float transformerScale =
        std::max(0.25f, (1.0f + receiver.toneCapTolerance) *
                             (1.0f + receiver.transformerTolerance));
    float transformerLpHz =
        std::clamp(receiver.transformerLpHz / transformerScale, 120.0f,
                   radio.sampleRate * 0.45f);
    receiver.transformerLp.setLowpass(radio.sampleRate, transformerLpHz,
                                      kRadioBiquadQ);
  }
  if (receiver.postTransformerLpHz > 0.0f) {
    receiver.postTransformerLp.setLowpass(radio.sampleRate,
                                          receiver.postTransformerLpHz,
                                          kRadioBiquadQ);
  }
  receiver.stageAtk = std::exp(-1.0f / (radio.sampleRate * 0.010f));
  receiver.stageRel = std::exp(-1.0f / (radio.sampleRate * 0.180f));
}

void RadioReceiverCircuitNode::reset(Radio1938& radio) {
  auto& receiver = radio.receiverCircuit;
  receiver.stageEnv = 0.0f;
  receiver.couplingHp.reset();
  receiver.interstagePeak.reset();
  receiver.presenceDip.reset();
  receiver.transformerLp.reset();
  receiver.postTransformerLp.reset();
}

float RadioReceiverCircuitNode::process(Radio1938& radio,
                                        float y,
                                        const RadioSampleContext&) {
  auto& receiver = radio.receiverCircuit;
  const auto& controlBus = radio.controlBus;
  if (!receiver.enabled) return y;

  if (receiver.couplingHpHz > 0.0f) {
    y = receiver.couplingHp.process(y);
  }
  if (receiver.interstagePeakHz > 0.0f &&
      std::fabs(receiver.interstagePeakGainDb) > 1e-4f) {
    y = receiver.interstagePeak.process(y);
  }
  if (receiver.presenceDipHz > 0.0f &&
      std::fabs(receiver.presenceDipGainDb) > 1e-4f) {
    y = receiver.presenceDip.process(y);
  }
  if (receiver.transformerLpHz > 0.0f) {
    y = receiver.transformerLp.process(y);
  }

  y *= std::max(receiver.voltageGain, 0.0f);

  if (receiver.driveBase <= 0.0f) {
    return y;
  }

  float a = std::fabs(y);
  if (a > receiver.stageEnv) {
    receiver.stageEnv =
        receiver.stageAtk * receiver.stageEnv + (1.0f - receiver.stageAtk) * a;
  } else {
    receiver.stageEnv =
        receiver.stageRel * receiver.stageEnv + (1.0f - receiver.stageRel) * a;
  }

  float controlVoltageT = clampf(controlBus.controlVoltage, 0.0f, 1.25f);
  float supplySagT = clampf(controlBus.supplySag, 0.0f, 1.0f);
  float detectorHold = clampf(controlVoltageT / 1.25f, 0.0f, 1.0f);

  float stageGain = 1.0f -
                    receiver.controlVoltageGainDepth * detectorHold -
                    receiver.supplySagGainDepth * supplySagT;
  y *= std::max(0.45f, stageGain);

  float drive = receiver.driveBase *
                (1.0f + 0.10f * receiver.plateLoadTolerance -
                 0.08f * receiver.transformerTolerance);
  drive *= 1.0f - receiver.supplySagDriveDepth * supplySagT;
  drive *= 1.0f - 0.04f * detectorHold;
  float bias = receiver.asymBiasBase *
               (1.0f + 0.35f * receiver.gridLeakTolerance -
                0.20f * receiver.plateLoadTolerance);
  float shaped = asymmetricSaturate(y, std::max(0.2f, drive), bias, 0.18f);

  float gridT = clampf((receiver.stageEnv - receiver.gridConductionStart) /
                           std::max(1e-6f, receiver.stageInputRef),
                       0.0f, 1.0f);
  float conductionGain = 1.0f - receiver.gridConductionDepth * gridT;
  float out = shaped * std::max(0.55f, conductionGain);
  if (receiver.postTransformerLpHz > 0.0f) {
    float softened = receiver.postTransformerLp.process(out);
    out = 0.60f * out + 0.40f * softened;
  }
  return out;
}

void RadioToneNode::init(Radio1938& radio, RadioInitContext&) {
  auto& tone = radio.tone;
  tone.presence.setPeaking(radio.sampleRate, tone.presenceHz, tone.presenceQ,
                           tone.presenceGainDb);
  tone.tiltLp.setLowpass(radio.sampleRate, tone.tiltSplitHz, kRadioBiquadQ);
}

void RadioToneNode::reset(Radio1938& radio) {
  auto& tone = radio.tone;
  tone.presence.reset();
  tone.tiltLp.reset();
}

float RadioToneNode::process(Radio1938& radio,
                             float y,
                             const RadioSampleContext&) {
  auto& tone = radio.tone;
  if (tone.presenceHz <= 0.0f) return y;
  return tone.presence.process(y);
}

void RadioPowerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& power = radio.power;
  power.sagAtk =
      std::exp(-1.0f / (radio.sampleRate * (power.sagAttackMs / 1000.0f)));
  power.sagRel =
      std::exp(-1.0f / (radio.sampleRate * (power.sagReleaseMs / 1000.0f)));
  if (power.postLpHz > 0.0f) {
    power.postLpf.setLowpass(radio.sampleRate, power.postLpHz, kRadioBiquadQ);
  } else {
    power.postLpf = Biquad{};
  }
  power.satOsLpIn = Biquad{};
  power.satOsLpOut = Biquad{};
}

void RadioPowerNode::reset(Radio1938& radio) {
  auto& power = radio.power;
  power.sagEnv = 0.0f;
  power.rectifierPhase = 0.0f;
  power.satOsPrev = 0.0f;
  power.postLpf.reset();
  power.satOsLpIn.reset();
  power.satOsLpOut.reset();
}

float RadioPowerNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext&) {
  auto& power = radio.power;
  auto& controlSense = radio.controlSense;
  float load = std::fabs(y);
  if (load > power.sagEnv) {
    power.sagEnv = power.sagAtk * power.sagEnv + (1.0f - power.sagAtk) * load;
  } else {
    power.sagEnv = power.sagRel * power.sagEnv + (1.0f - power.sagRel) * load;
  }
  float powerT = clampf((power.sagEnv - power.sagStart) /
                            std::max(1e-6f, power.sagEnd - power.sagStart),
                        0.0f, 1.0f);
  float supplyGain =
      std::clamp(1.0f - power.gainSagPerPower * powerT, power.gainMin,
                 power.gainMax);
  y *= supplyGain;
  if (power.postLpHz > 0.0f) {
    y = power.postLpf.process(y);
  }
  controlSense.powerSagSense = power.sagEnv;
  return y;
}

void RadioInterferenceDerivedNode::init(Radio1938& radio, RadioInitContext&) {
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseDerived = radio.noiseDerived;
  if (radio.noiseWeight <= 0.0f) {
    noiseDerived.baseNoiseAmp = 0.0f;
    noiseDerived.baseCrackleAmp = 0.0f;
    noiseDerived.baseHumAmp = 0.0f;
    noiseDerived.crackleRate = 0.0f;
    return;
  }

  float scale = std::clamp(radio.noiseWeight / noiseConfig.noiseWeightRef, 0.0f,
                           noiseConfig.noiseWeightScaleMax);
  noiseDerived.baseNoiseAmp = radio.noiseWeight;
  noiseDerived.baseCrackleAmp = noiseConfig.crackleAmpScale * scale;
  noiseDerived.baseHumAmp = noiseConfig.humAmpScale * scale;
  noiseDerived.crackleRate = noiseConfig.crackleRateScale * scale;
}

void RadioInterferenceDerivedNode::reset(Radio1938&) {}

void RadioInterferenceDerivedNode::update(Radio1938& radio,
                                          RadioSampleContext& ctx) {
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseDerived = radio.noiseDerived;
  float postNoiseScale = std::max(radio.globals.postNoiseMix, 0.0f);
  ctx.derived.demodIfNoiseAmp =
      noiseDerived.baseNoiseAmp * radio.globals.ifNoiseMix;
  ctx.derived.noiseAmp =
      noiseDerived.baseNoiseAmp * postNoiseScale + radio.globals.noiseFloorAmp;
  ctx.derived.crackleAmp = noiseDerived.baseCrackleAmp * postNoiseScale;
  ctx.derived.crackleRate = noiseDerived.crackleRate;
  ctx.derived.humAmp = noiseDerived.baseHumAmp * postNoiseScale;
  ctx.derived.humToneEnabled = noiseConfig.enableHumTone;
}

void RadioNoiseNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseRuntime = radio.noiseRuntime;
  noiseRuntime.hum.setFs(radio.sampleRate, initCtx.tunedBw);
  noiseRuntime.hum.humHz = noiseConfig.humHzDefault;
}

void RadioNoiseNode::reset(Radio1938& radio) { radio.noiseRuntime.hum.reset(); }

float RadioNoiseNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext& ctx) {
  NoiseInput noiseIn{};
  noiseIn.programSample = y;
  noiseIn.noiseAmp = ctx.derived.noiseAmp;
  noiseIn.crackleAmp = ctx.derived.crackleAmp;
  noiseIn.crackleRate = ctx.derived.crackleRate;
  noiseIn.humAmp = ctx.derived.humAmp;
  noiseIn.humToneEnabled = ctx.derived.humToneEnabled;
  return y + radio.noiseRuntime.hum.process(noiseIn);
}

void RadioSpeakerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& speakerStage = radio.speakerStage;
  float osFs = radio.sampleRate * radio.globals.oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  speakerStage.osLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  speakerStage.osLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
  speakerStage.speaker.init(osFs);
  speakerStage.speaker.drive = speakerStage.drive;
}

void RadioSpeakerNode::reset(Radio1938& radio) {
  auto& speakerStage = radio.speakerStage;
  speakerStage.osPrev = 0.0f;
  speakerStage.osLpIn.reset();
  speakerStage.osLpOut.reset();
  speakerStage.speaker.reset();
}

float RadioSpeakerNode::process(Radio1938& radio,
                                float y,
                                const RadioSampleContext&) {
  auto& speakerStage = radio.speakerStage;
  speakerStage.speaker.drive = std::max(speakerStage.drive, 0.0f);
  y *= radio.makeupGain;
  y = processOversampled2x(y, speakerStage.osPrev, speakerStage.osLpIn,
                           speakerStage.osLpOut, [&](float v) {
                             bool clipped = false;
                             float out = speakerStage.speaker.process(v, clipped);
                             if (clipped) radio.diagnostics.markSpeakerClip();
                             return out;
                           });
  return y;
}

void RadioCabinetNode::init(Radio1938& radio, RadioInitContext&) {
  auto& cabinet = radio.cabinet;
  float panelHzDerived = std::clamp(
      cabinet.panelHz * (1.0f + 0.52f * cabinet.panelStiffnessTolerance),
      90.0f, radio.sampleRate * 0.22f);
  float chassisHzDerived = std::clamp(
      cabinet.chassisHz * (1.0f + 0.28f * cabinet.panelStiffnessTolerance -
                           0.18f * cabinet.baffleLeakTolerance),
      panelHzDerived + 80.0f, radio.sampleRate * 0.30f);
  float cavityDipHzDerived = std::clamp(
      cabinet.cavityDipHz * (1.0f + 0.35f * cabinet.cavityTolerance),
      chassisHzDerived + 140.0f, radio.sampleRate * 0.42f);
  float rearDelayMsDerived = std::max(
      0.4f, cabinet.rearDelayMs *
                (1.0f + 0.30f * cabinet.rearPathTolerance +
                 0.12f * cabinet.baffleLeakTolerance));
  cabinet.rearMixApplied = std::clamp(
      cabinet.rearMix * (1.0f + 0.42f * cabinet.baffleLeakTolerance -
                         0.25f * cabinet.grilleClothTolerance),
      0.0f, 0.12f);

  if (cabinet.panelHz > 0.0f && cabinet.panelQ > 0.0f) {
    cabinet.panel.setPeaking(radio.sampleRate, panelHzDerived, cabinet.panelQ,
                             cabinet.panelGainDb);
  } else {
    cabinet.panel = Biquad{};
  }
  if (cabinet.chassisHz > 0.0f && cabinet.chassisQ > 0.0f) {
    cabinet.chassis.setPeaking(radio.sampleRate, chassisHzDerived,
                               cabinet.chassisQ, cabinet.chassisGainDb);
  } else {
    cabinet.chassis = Biquad{};
  }
  if (cabinet.cavityDipHz > 0.0f && cabinet.cavityDipQ > 0.0f) {
    cabinet.cavityDip.setPeaking(radio.sampleRate, cavityDipHzDerived,
                                 cabinet.cavityDipQ, cabinet.cavityDipGainDb);
  } else {
    cabinet.cavityDip = Biquad{};
  }
  if (cabinet.grilleLpHz > 0.0f) {
    float grilleLpHzDerived = std::clamp(
        cabinet.grilleLpHz /
            std::max(0.35f, 1.0f + 0.55f * cabinet.grilleClothTolerance),
        cavityDipHzDerived + 220.0f, radio.sampleRate * 0.45f);
    cabinet.grilleLp.setLowpass(radio.sampleRate, grilleLpHzDerived,
                                kRadioBiquadQ);
  } else {
    cabinet.grilleLp = Biquad{};
  }
  if (cabinet.rearMixApplied > 0.0f) {
    if (cabinet.rearHpHz > 0.0f) {
      cabinet.rearHp.setHighpass(radio.sampleRate, cabinet.rearHpHz,
                                 kRadioBiquadQ);
    } else {
      cabinet.rearHp = Biquad{};
    }
    if (cabinet.rearLpHz > 0.0f) {
      cabinet.rearLp.setLowpass(radio.sampleRate, cabinet.rearLpHz,
                                kRadioBiquadQ);
    } else {
      cabinet.rearLp = Biquad{};
    }
    int rearSamples =
        std::max(cabinet.minBufferSamples,
                 static_cast<int>(std::ceil(rearDelayMsDerived * 0.001f *
                                            radio.sampleRate)) +
                     cabinet.bufferGuardSamples);
    cabinet.buf.assign(static_cast<size_t>(rearSamples), 0.0f);
  } else {
    cabinet.rearHp = Biquad{};
    cabinet.rearLp = Biquad{};
    cabinet.buf.clear();
  }
  cabinet.clarifier1 = Biquad{};
  cabinet.clarifier2 = Biquad{};
  cabinet.clarifier3 = Biquad{};
  cabinet.index = 0;
}

void RadioCabinetNode::reset(Radio1938& radio) {
  auto& cabinet = radio.cabinet;
  cabinet.panel.reset();
  cabinet.chassis.reset();
  cabinet.cavityDip.reset();
  cabinet.grilleLp.reset();
  cabinet.rearHp.reset();
  cabinet.rearLp.reset();
  cabinet.clarifier1.reset();
  cabinet.clarifier2.reset();
  cabinet.clarifier3.reset();
  cabinet.index = 0;
  std::fill(cabinet.buf.begin(), cabinet.buf.end(), 0.0f);
}

float RadioCabinetNode::process(Radio1938& radio,
                                float y,
                                const RadioSampleContext&) {
  auto& cabinet = radio.cabinet;
  if (!cabinet.enabled) return y;

  float out = cabinet.panel.process(y);
  out = cabinet.chassis.process(out);
  out = cabinet.cavityDip.process(out);
  if (!cabinet.buf.empty()) {
    float rear = cabinet.buf[static_cast<size_t>(cabinet.index)];
    cabinet.buf[static_cast<size_t>(cabinet.index)] = y;
    cabinet.index = (cabinet.index + 1) % static_cast<int>(cabinet.buf.size());
    rear = cabinet.rearHp.process(rear);
    rear = cabinet.rearLp.process(rear);
    out -= rear * cabinet.rearMixApplied;
  }
  if (cabinet.grilleLpHz > 0.0f) {
    out = cabinet.grilleLp.process(out);
  }
  return out;
}

void RadioFinalLimiterNode::init(Radio1938& radio, RadioInitContext&) {
  auto& limiter = radio.finalLimiter;
  limiter.attackCoeff =
      std::exp(-1.0f / (radio.sampleRate * (limiter.attackMs / 1000.0f)));
  limiter.releaseCoeff =
      std::exp(-1.0f / (radio.sampleRate * (limiter.releaseMs / 1000.0f)));
  limiter.delaySamples = std::max(
      1, static_cast<int>(std::lround(
             radio.sampleRate * (limiter.lookaheadMs / 1000.0f))));
  limiter.delayBuf.assign(static_cast<size_t>(limiter.delaySamples), 0.0f);
  limiter.requiredGainBuf.assign(static_cast<size_t>(limiter.delaySamples), 1.0f);
  limiter.delayWriteIndex = 0;
  float osFs = radio.sampleRate * radio.globals.oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  limiter.osLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  limiter.osLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
}

void RadioFinalLimiterNode::reset(Radio1938& radio) {
  auto& limiter = radio.finalLimiter;
  limiter.gain = 1.0f;
  limiter.targetGain = 1.0f;
  limiter.osPrev = 0.0f;
  limiter.observedPeak = 0.0f;
  limiter.delayWriteIndex = 0;
  std::fill(limiter.delayBuf.begin(), limiter.delayBuf.end(), 0.0f);
  std::fill(limiter.requiredGainBuf.begin(), limiter.requiredGainBuf.end(), 1.0f);
  limiter.osLpIn.reset();
  limiter.osLpOut.reset();
}

float RadioFinalLimiterNode::process(Radio1938& radio,
                                     float y,
                                     const RadioSampleContext&) {
  auto& limiter = radio.finalLimiter;
  if (!limiter.enabled) return y;
  float limitedIn = y * radio.presentationGain;

  float peak = 0.0f;
  float mid = 0.5f * (limiter.osPrev + limitedIn);
  float s0 = limiter.osLpIn.process(mid);
  s0 = limiter.osLpOut.process(s0);
  peak = std::max(peak, std::fabs(s0));

  float s1 = limiter.osLpIn.process(limitedIn);
  s1 = limiter.osLpOut.process(s1);
  peak = std::max(peak, std::fabs(s1));

  limiter.osPrev = limitedIn;
  limiter.observedPeak = peak;

  float requiredGain = 1.0f;
  if (peak > limiter.threshold && peak > 1e-9f) {
    requiredGain = limiter.threshold / peak;
  }

  float delayed = limitedIn;
  if (!limiter.delayBuf.empty()) {
    size_t writeIndex = static_cast<size_t>(limiter.delayWriteIndex);
    delayed = limiter.delayBuf[writeIndex];
    limiter.delayBuf[writeIndex] = limitedIn;
    limiter.requiredGainBuf[writeIndex] = requiredGain;
    limiter.delayWriteIndex =
        (limiter.delayWriteIndex + 1) % static_cast<int>(limiter.delayBuf.size());
    limiter.targetGain = 1.0f;
    for (float gainCandidate : limiter.requiredGainBuf) {
      limiter.targetGain = std::min(limiter.targetGain, gainCandidate);
    }
  } else {
    limiter.targetGain = requiredGain;
  }

  if (limiter.targetGain < limiter.gain) {
    limiter.gain = limiter.targetGain;
  } else {
    limiter.gain =
        limiter.releaseCoeff * limiter.gain +
        (1.0f - limiter.releaseCoeff) * limiter.targetGain;
  }

  float out = delayed * limiter.gain;
  float gainReduction = 1.0f - limiter.gain;
  float gainReductionDb = (limiter.gain < 0.999999f) ? -lin2db(limiter.gain) : 0.0f;

  radio.diagnostics.finalLimiterPeak =
      std::max(radio.diagnostics.finalLimiterPeak, peak);
  radio.diagnostics.finalLimiterGain =
      std::min(radio.diagnostics.finalLimiterGain, limiter.gain);
  radio.diagnostics.finalLimiterMaxGainReduction =
      std::max(radio.diagnostics.finalLimiterMaxGainReduction, gainReduction);
  radio.diagnostics.finalLimiterMaxGainReductionDb =
      std::max(radio.diagnostics.finalLimiterMaxGainReductionDb, gainReductionDb);
  radio.diagnostics.finalLimiterGainReductionSum += gainReduction;
  radio.diagnostics.finalLimiterGainReductionDbSum += gainReductionDb;
  radio.diagnostics.processedSamples++;
  if (limiter.gain < 0.999f) {
    radio.diagnostics.finalLimiterActive = true;
    radio.diagnostics.finalLimiterActiveSamples++;
  }

  if (radio.calibration.enabled) {
    if (std::fabs(limitedIn) > radio.globals.outputClipThreshold) {
      radio.calibration.preLimiterClipCount++;
    }
    if (std::fabs(out) > radio.globals.outputClipThreshold) {
      radio.calibration.postLimiterClipCount++;
    }
    radio.calibration.totalSamples++;
    radio.calibration.limiterGainReductionSum += gainReduction;
    radio.calibration.limiterGainReductionDbSum += gainReductionDb;
    if (limiter.gain < 0.999f) {
      radio.calibration.limiterActiveSamples++;
    }
    radio.calibration.limiterMaxGainReduction =
        std::max(radio.calibration.limiterMaxGainReduction, gainReduction);
    radio.calibration.limiterMaxGainReductionDb =
        std::max(radio.calibration.limiterMaxGainReductionDb, gainReductionDb);
  }

  return out;
}

void RadioOutputClipNode::init(Radio1938& radio, RadioInitContext&) {
  auto& output = radio.output;
  float osFs = radio.sampleRate * radio.globals.oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  output.clipOsLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  output.clipOsLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
}

void RadioOutputClipNode::reset(Radio1938& radio) {
  auto& output = radio.output;
  output.clipOsPrev = 0.0f;
  output.clipOsLpIn.reset();
  output.clipOsLpOut.reset();
}

float RadioOutputClipNode::process(Radio1938& radio,
                                   float y,
                                   const RadioSampleContext&) {
  auto& output = radio.output;
  return processOversampled2x(y, output.clipOsPrev, output.clipOsLpIn,
                              output.clipOsLpOut, [&](float v) {
                                float t = radio.globals.outputClipThreshold;
                                float av = std::fabs(v);
                                if (av > t) radio.diagnostics.markOutputClip();
                                float clipped = softClip(v, t);
                                return std::clamp(clipped, -t, t);
                              });
}

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

Radio1938::PresentationPathStep*
Radio1938::RadioExecutionGraph::findPresentationPath(StageId id) {
  for (auto& step : presentationPathSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

const Radio1938::PresentationPathStep*
Radio1938::RadioExecutionGraph::findPresentationPath(StageId id) const {
  for (const auto& step : presentationPathSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

bool Radio1938::RadioExecutionGraph::isEnabled(StageId id) const {
  if (const auto* step = findBlock(id)) return step->enabled;
  if (const auto* step = findSampleControl(id)) return step->enabled;
  if (const auto* step = findProgramPath(id)) return step->enabled;
  if (const auto* step = findPresentationPath(id)) return step->enabled;
  return false;
}

void Radio1938::RadioExecutionGraph::setEnabled(StageId id, bool value) {
  if (auto* step = findBlock(id)) {
    step->enabled = value;
    return;
  }
  if (auto* step = findSampleControl(id)) {
    step->enabled = value;
    return;
  }
  if (auto* step = findProgramPath(id)) {
    step->enabled = value;
    return;
  }
  if (auto* step = findPresentationPath(id)) {
    step->enabled = value;
  }
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

static void updateCalibrationSnapshot(Radio1938& radio);

template <size_t StepCount>
static RadioBlockControl runBlockPrepare(
    Radio1938& radio,
    const std::array<BlockStep, StepCount>& steps,
    uint32_t frames) {
  RadioBlockControl block{};
  for (const auto& step : steps) {
    if (!step.enabled || !step.prepare) continue;
    step.prepare(radio, block, frames);
  }
  return block;
}

template <size_t StepCount>
static void runSampleControl(
    Radio1938& radio,
    RadioSampleContext& ctx,
    const std::array<SampleControlStep, StepCount>& steps) {
  for (const auto& step : steps) {
    if (!step.enabled || !step.update) continue;
    step.update(radio, ctx);
  }
}

template <size_t StepCount>
static float runProgramPath(
    Radio1938& radio,
    float y,
    const RadioSampleContext& ctx,
    const std::array<ProgramPathStep, StepCount>& steps) {
  for (const auto& step : steps) {
    if (!step.enabled || !step.process) continue;
    float in = y;
    y = step.process(radio, y, ctx);
    updateStageCalibration(radio, step.id, in, y);
  }
  return y;
}

template <size_t StepCount>
static float runProgramPathFromIndex(
    Radio1938& radio,
    float y,
    const RadioSampleContext& ctx,
    const std::array<ProgramPathStep, StepCount>& steps,
    size_t startIndex) {
  for (size_t i = startIndex; i < StepCount; ++i) {
    const auto& step = steps[i];
    if (!step.enabled || !step.process) continue;
    float in = y;
    y = step.process(radio, y, ctx);
    updateStageCalibration(radio, step.id, in, y);
  }
  return y;
}

template <size_t StepCount>
static float runPresentationPath(
    Radio1938& radio,
    float y,
    const RadioSampleContext& ctx,
    const std::array<PresentationPathStep, StepCount>& steps) {
  for (const auto& step : steps) {
    if (!step.enabled || !step.process) continue;
    float in = y;
    y = step.process(radio, y, ctx);
    updateStageCalibration(radio, step.id, in, y);
  }
  return y;
}

static constexpr size_t kMixerProgramStartIndex = 2;

template <typename InputSampleFn, typename OutputSampleFn>
static void processRadioFrames(Radio1938& radio,
                               uint32_t frames,
                               size_t programStartIndex,
                               InputSampleFn&& inputSample,
                               OutputSampleFn&& outputSample) {
  if (frames == 0 || radio.graph.bypass) return;

  radio.diagnostics.reset();
  RadioBlockControl block = runBlockPrepare(radio, radio.graph.blockSteps, frames);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    RadioSampleContext ctx{};
    ctx.block = &block;
    runSampleControl(radio, ctx, radio.graph.sampleControlSteps);
    float x = inputSample(frame);
    float y = runProgramPathFromIndex(radio, x, ctx, radio.graph.programPathSteps,
                                      programStartIndex);
    y = runPresentationPath(radio, y, ctx, radio.graph.presentationPathSteps);
    outputSample(frame, y);
  }

  if (radio.diagnostics.processedSamples > 0) {
    float invCount = 1.0f / static_cast<float>(radio.diagnostics.processedSamples);
    radio.diagnostics.finalLimiterDutyCycle =
        radio.diagnostics.finalLimiterActiveSamples * invCount;
    radio.diagnostics.finalLimiterAverageGainReduction =
        static_cast<float>(radio.diagnostics.finalLimiterGainReductionSum * invCount);
    radio.diagnostics.finalLimiterAverageGainReductionDb = static_cast<float>(
        radio.diagnostics.finalLimiterGainReductionDbSum * invCount);
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

static void applyPhilco37116XPreset(Radio1938& radio) {
  radio.makeupGain = 1.0f;
  radio.presentationGain = 1.0f;
  radio.globals.ifNoiseMix = 0.22f;
  radio.globals.postNoiseMix = 0.11f;
  radio.globals.noiseFloorAmp = 0.0024f;
  radio.globals.inputPad = 1.0f;
  radio.globals.enableAutoLevel = false;
  radio.globals.autoTargetDb = -21.0f;
  radio.globals.autoMaxBoostDb = 2.5f;
  radio.globals.outputClipThreshold = 0.98f;

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

  radio.mixer.conversionGain = 1.10f;
  radio.mixer.tuneLossDepth = 0.22f;
  radio.mixer.loFeedthrough = 0.0025f;
  radio.mixer.drive = 0.88f;
  radio.mixer.bias = 0.0f;

  radio.ifStrip.enabled = true;
  radio.ifStrip.ifMinBwHz = 2400.0f;
  radio.ifStrip.stageGain = 1.0f;
  radio.ifStrip.avcGainDepth = 0.18f;
  radio.ifStrip.ifCenterHz = 470000.0f;

  radio.demod.am.audioDiodeDrop = 0.0100f;
  radio.demod.am.avcDiodeDrop = 0.0080f;
  radio.demod.am.detectorChargeResNorm = 1.0f;
  radio.demod.am.audioDischargeMs = 0.10f;
  radio.demod.am.avcChargeMs = 90.0f;
  radio.demod.am.avcReleaseMs = 1500.0f;
  radio.demod.am.detectorGain = 1.0f;
  radio.demod.am.controlVoltageRef = 0.75f;
  radio.demod.am.dcMs = 450.0f;
  radio.demod.am.senseLowHz = 0.0f;
  radio.demod.am.senseHighHz = 0.0f;
  radio.demod.am.audioHpHz = 110.0f;
  radio.demod.am.detLpScale = 0.48f;
  radio.demod.am.detLpMinHz = 120.0f;
  radio.demod.am.audioEnvelopeLpHz = 0.0f;
  radio.demod.am.avcSenseLpHz = 420.0f;
  radio.demod.am.afcSenseLpHz = 34.0f;

  radio.receiverCircuit.enabled = true;
  radio.receiverCircuit.couplingCapTolerance = 0.0f;
  radio.receiverCircuit.gridLeakTolerance = 0.0f;
  radio.receiverCircuit.plateLoadTolerance = 0.0f;
  radio.receiverCircuit.toneCapTolerance = 0.0f;
  radio.receiverCircuit.transformerTolerance = 0.0f;
  radio.receiverCircuit.couplingHpHz = 116.0f;
  radio.receiverCircuit.voltageGain = 4.5f;
  radio.receiverCircuit.interstagePeakHz = 1325.0f;
  radio.receiverCircuit.interstagePeakQ = 0.82f;
  radio.receiverCircuit.interstagePeakGainDb = 0.0f;
  radio.receiverCircuit.transformerLpHz = 0.0f;
  radio.receiverCircuit.presenceDipHz = 2480.0f;
  radio.receiverCircuit.presenceDipQ = 1.05f;
  radio.receiverCircuit.presenceDipGainDb = 0.0f;
  radio.receiverCircuit.postTransformerLpHz = 0.0f;
  radio.receiverCircuit.stageInputRef = 0.18f;
  radio.receiverCircuit.driveBase = 0.0f;
  radio.receiverCircuit.asymBiasBase = 0.0030f;
  radio.receiverCircuit.controlVoltageGainDepth = 0.0f;
  radio.receiverCircuit.supplySagGainDepth = 0.0f;
  radio.receiverCircuit.supplySagDriveDepth = 0.0f;
  radio.receiverCircuit.gridConductionStart = 0.14f;
  radio.receiverCircuit.gridConductionDepth = 0.0f;

  radio.tone.presenceHz = 0.0f;
  radio.tone.presenceQ = 0.78f;
  radio.tone.presenceGainDb = 0.0f;
  radio.tone.tiltSplitHz = 0.0f;

  radio.power.sagStart = 0.06f;
  radio.power.sagEnd = 0.22f;
  radio.power.rippleDepth = 0.0f;
  radio.power.satDrive = 1.0f;
  radio.power.satMix = 0.0f;
  radio.power.sagAttackMs = 60.0f;
  radio.power.sagReleaseMs = 900.0f;
  radio.power.rectifierMinHz = 80.0f;
  radio.power.rippleSecondHarmonicMix = 0.0f;
  radio.power.gainSagPerPower = 0.015f;
  radio.power.rippleGainBase = 0.0f;
  radio.power.rippleGainDepth = 0.0f;
  radio.power.gainMin = 0.92f;
  radio.power.gainMax = 1.02f;
  radio.power.supplyDriveDepth = 0.0f;
  radio.power.supplyBiasDepth = 0.0f;
  radio.power.postLpHz = 0.0f;

  radio.speakerStage.drive = 1.0f;
  radio.speakerStage.speaker.filterQ = kRadioBiquadQ;
  radio.speakerStage.speaker.suspensionHz = 175.0f;
  radio.speakerStage.speaker.suspensionQ = 0.70f;
  radio.speakerStage.speaker.suspensionGainDb = 0.25f;
  radio.speakerStage.speaker.coneBodyHz = 480.0f;
  radio.speakerStage.speaker.coneBodyQ = 0.88f;
  radio.speakerStage.speaker.coneBodyGainDb = 0.0f;
  radio.speakerStage.speaker.upperBreakupHz = 0.0f;
  radio.speakerStage.speaker.upperBreakupQ = 1.05f;
  radio.speakerStage.speaker.upperBreakupGainDb = 0.00f;
  radio.speakerStage.speaker.coneDipHz = 0.0f;
  radio.speakerStage.speaker.coneDipQ = 0.95f;
  radio.speakerStage.speaker.coneDipGainDb = 0.0f;
  radio.speakerStage.speaker.topLpHz = 4200.0f;
  radio.speakerStage.speaker.limit = 0.99f;
  radio.speakerStage.speaker.asymBias = 0.0f;
  radio.speakerStage.speaker.excursionRef = 0.28f;
  radio.speakerStage.speaker.complianceLossDepth = 0.05f;
  radio.speakerStage.speaker.hfLossDepth = 0.0f;
  radio.speakerStage.speaker.hfLossLpHz = 0.0f;

  radio.cabinet.enabled = true;
  radio.cabinet.panelHz = 205.0f;
  radio.cabinet.panelQ = 0.82f;
  radio.cabinet.panelGainDb = 0.25f;
  radio.cabinet.chassisHz = 455.0f;
  radio.cabinet.chassisQ = 1.05f;
  radio.cabinet.chassisGainDb = 0.10f;
  radio.cabinet.cavityDipHz = 960.0f;
  radio.cabinet.cavityDipQ = 0.95f;
  radio.cabinet.cavityDipGainDb = -0.18f;
  radio.cabinet.grilleLpHz = 0.0f;
  radio.cabinet.rearDelayMs = 1.2f;
  radio.cabinet.rearMix = 0.03f;
  radio.cabinet.rearHpHz = 170.0f;
  radio.cabinet.rearLpHz = 820.0f;
  radio.cabinet.clarifier1Hz = 0.0f;
  radio.cabinet.clarifier1Q = 0.78f;
  radio.cabinet.clarifier1Coupling = 0.0f;
  radio.cabinet.clarifier2Hz = 0.0f;
  radio.cabinet.clarifier2Q = 0.92f;
  radio.cabinet.clarifier2Coupling = 0.0f;
  radio.cabinet.clarifier3Hz = 0.0f;
  radio.cabinet.clarifier3Q = 1.08f;
  radio.cabinet.clarifier3Coupling = 0.0f;

  radio.noiseConfig.enableHumTone = true;
  radio.noiseConfig.humAmpScale = 0.0016f;
  radio.noiseConfig.crackleAmpScale = 0.0012f;
  radio.noiseConfig.crackleRateScale = 0.012f;
  radio.noiseRuntime.hum.crackleDecayMs = 4.5f;
  radio.noiseRuntime.hum.noiseHpHz = 320.0f;
  radio.noiseRuntime.hum.noiseLpHz = 3300.0f;
  radio.noiseRuntime.hum.hissMaskDepth = 0.06f;
  radio.noiseRuntime.hum.burstMaskDepth = 0.01f;

  radio.finalLimiter.enabled = true;
  radio.finalLimiter.threshold = 0.995f;
  radio.finalLimiter.lookaheadMs = 1.2f;
  radio.finalLimiter.attackMs = 0.08f;
  radio.finalLimiter.releaseMs = 120.0f;
}

static void applySetIdentity(Radio1938& radio) {
  const uint32_t seed = radio.identity.seed;
  const float depth = std::max(0.0f, radio.identity.driftDepth);

  auto& receiver = radio.receiverCircuit;
  receiver.couplingCapTolerance = 0.12f * depth *
                                  seededSignedUnit(seed, 0x1001u);
  receiver.gridLeakTolerance = 0.08f * depth *
                               seededSignedUnit(seed, 0x1002u);
  receiver.plateLoadTolerance = 0.06f * depth *
                                seededSignedUnit(seed, 0x1003u);
  receiver.toneCapTolerance = 0.14f * depth *
                              seededSignedUnit(seed, 0x1004u);
  receiver.transformerTolerance = 0.08f * depth *
                                  seededSignedUnit(seed, 0x1005u);

  auto& speaker = radio.speakerStage.speaker;
  speaker.suspensionComplianceTolerance =
      0.10f * depth * seededSignedUnit(seed, 0x2001u);
  speaker.coneMassTolerance =
      0.08f * depth * seededSignedUnit(seed, 0x2002u);
  speaker.breakupTolerance =
      0.07f * depth * seededSignedUnit(seed, 0x2003u);
  speaker.voiceCoilTolerance =
      0.05f * depth * seededSignedUnit(seed, 0x2004u);

  auto& cabinet = radio.cabinet;
  cabinet.panelStiffnessTolerance =
      0.10f * depth * seededSignedUnit(seed, 0x3001u);
  cabinet.baffleLeakTolerance =
      0.12f * depth * seededSignedUnit(seed, 0x3002u);
  cabinet.cavityTolerance =
      0.08f * depth * seededSignedUnit(seed, 0x3003u);
  cabinet.grilleClothTolerance =
      0.06f * depth * seededSignedUnit(seed, 0x3004u);
  cabinet.rearPathTolerance =
      0.10f * depth * seededSignedUnit(seed, 0x3005u);
}

static void refreshIdentityDependentStages(Radio1938& radio) {
  applySetIdentity(radio);
  RadioInitContext initCtx{};
  RadioReceiverCircuitNode::init(radio, initCtx);
  RadioSpeakerNode::init(radio, initCtx);
  RadioCabinetNode::init(radio, initCtx);
  RadioReceiverCircuitNode::reset(radio);
  RadioSpeakerNode::reset(radio);
  RadioCabinetNode::reset(radio);
  if (radio.calibration.enabled) {
    radio.resetCalibration();
  }
}

std::string_view Radio1938::presetName(Preset preset) {
  switch (preset) {
    case Preset::Philco37116X:
      return "philco_37_116x";
  }
  return "philco_37_116x";
}

Radio1938::Radio1938() { applyRadioBaseDefaults(*this); }

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
  if (presetNameValue == "philco_37_116x") {
    applyPreset(Preset::Philco37116X);
    return true;
  }
  return false;
}

void Radio1938::applyPreset(Preset presetValue) {
  preset = presetValue;
  switch (presetValue) {
    case Preset::Philco37116X:
      applyPhilco37116XPreset(*this);
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
  channels = std::max(1, ch);
  sampleRate = sr;
  bwHz = bw;
  noiseWeight = noise;
  switch (preset) {
    case Preset::Philco37116X:
      applyPhilco37116XPreset(*this);
      break;
  }
  applySetIdentity(*this);
  RadioInitContext initCtx{};
  lifecycle.configure(*this, initCtx);
  lifecycle.allocate(*this, initCtx);
  lifecycle.initializeDependentState(*this, initCtx);
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
      *this, frames, kMixerProgramStartIndex,
      [&](uint32_t frame) {
        float x = sampleInterleavedToMono(samples, frame, channels);
        sourceFrame.setReal(x);
        return x;
      },
      [&](uint32_t frame, float y) {
        writeMonoToInterleaved(samples, frame, channels, y);
      });
}

void Radio1938::processIqBaseband(const float* iqInterleaved,
                                  float* outSamples,
                                  uint32_t frames) {
  if (!iqInterleaved || !outSamples || frames == 0) return;
  processRadioFrames(
      *this, frames, kMixerProgramStartIndex,
      [&](uint32_t frame) {
        size_t base = static_cast<size_t>(frame) * 2u;
        float i = iqInterleaved[base];
        float q = iqInterleaved[base + 1u];
        sourceFrame.setComplex(i, q);
        return i;
      },
      [&](uint32_t frame, float y) {
        writeMonoToInterleaved(outSamples, frame, channels, y);
      });
}
