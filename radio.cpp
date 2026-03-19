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

static void applyAMDetectorDefaults(AMDetector& detector) {
  detector.fs = 48000.0f;
  detector.osFactor = 1;
  detector.bwHz = 4800.0f;
  detector.diodeDrop = 0.0045f;
  detector.audioChargeMs = 0.05f;
  detector.audioReleaseMs = 0.80f;
  detector.avcAttackMs = 80.0f;
  detector.avcReleaseMs = 1200.0f;
  detector.detectorGain = 1.0f;
  detector.controlVoltageRef = 0.18f;
  detector.dcMs = 450.0f;
  detector.ifLowBaseHz = 120.0f;
  detector.ifMinBwHz = 1200.0f;
  detector.ifMaxFraction = 0.45f;
  detector.audioHpHz = 220.0f;
  detector.detLpScale = 1.10f;
  detector.detLpMinHz = 3000.0f;
}

static void applySpeakerSimDefaults(SpeakerSim& speaker) {
  speaker.drive = 1.0f;
  speaker.limit = 0.95f;
  speaker.asymBias = 0.02f;
  speaker.filterQ = kRadioBiquadQ;
  speaker.suspensionHz = 165.0f;
  speaker.suspensionQ = 0.70f;
  speaker.suspensionGainDb = 0.85f;
  speaker.coneBodyHz = 470.0f;
  speaker.coneBodyQ = 0.88f;
  speaker.coneBodyGainDb = 0.35f;
  speaker.upperBreakupHz = 1620.0f;
  speaker.upperBreakupQ = 1.05f;
  speaker.upperBreakupGainDb = 0.45f;
  speaker.coneDipHz = 3180.0f;
  speaker.coneDipQ = 0.95f;
  speaker.coneDipGainDb = -1.75f;
  speaker.topLpHz = 3350.0f;
  speaker.excursionRef = 0.18f;
}

static void applyRadioBaseDefaults(Radio1938& radio) {
  radio.sampleRate = 48000.0f;
  radio.channels = 1;
  radio.bwHz = 4800.0f;
  radio.noiseWeight = 0.012f;
  radio.makeupGain = 1.0f;
  radio.presentationGain = 1.0f;
  radio.preset = Radio1938::Preset::Philco37116X;
  radio.initialized = false;

  radio.controlBus.controlVoltageAttackMs = 2.4f;
  radio.controlBus.controlVoltageReleaseMs = 36.0f;
  radio.controlBus.supplySagAttackMs = 10.0f;
  radio.controlBus.supplySagReleaseMs = 220.0f;

  radio.identity.driftDepth = 1.0f;

  applyNoiseHumDefaults(radio.noiseRuntime.hum);
  applyAMDetectorDefaults(radio.demod.am);
  applySpeakerSimDefaults(radio.speakerStage.speaker);

  radio.globals.oversampleFactor = 2.0f;
  radio.globals.ifNoiseMix = 0.22f;
  radio.globals.postNoiseMix = 0.14f;
  radio.globals.noiseFloorAmp = 0.0022f;
  radio.globals.inputPad = 0.76f;
  radio.globals.enableAutoLevel = true;
  radio.globals.autoTargetDb = -21.0f;
  radio.globals.autoMaxBoostDb = 4.0f;
  radio.globals.satClipDelta = 0.03f;
  radio.globals.satClipMinLevel = 0.70f;
  radio.globals.outputClipThreshold = 0.98f;
  radio.globals.oversampleCutoffFraction = 0.45f;

  radio.tuning.safeBwMinHz = 4200.0f;
  radio.tuning.safeBwMaxHz = 5600.0f;
  radio.tuning.preBwScale = 1.08f;
  radio.tuning.postBwScale = 1.18f;
  radio.tuning.adjLpScale = 0.78f;
  radio.tuning.adjLpMinHz = 2200.0f;
  radio.tuning.adjLpMaxScale = 0.90f;
  radio.tuning.smoothTau = 0.05f;
  radio.tuning.updateEps = 0.25f;

  radio.input.autoEnvAttackMs = 8.0f;
  radio.input.autoEnvReleaseMs = 220.0f;
  radio.input.autoGainAttackMs = 140.0f;
  radio.input.autoGainReleaseMs = 1200.0f;

  radio.reception.fadeRateFast = 0.08f;
  radio.reception.fadeRateSlow = 0.05f;
  radio.reception.noiseRateFast = 0.04f;
  radio.reception.noiseRateSlow = 0.031f;
  radio.reception.fadeDepth = 0.05f;
  radio.reception.noiseDepth = 0.20f;
  radio.reception.lfoMixA = 0.6f;
  radio.reception.lfoMixB = 0.4f;
  radio.reception.fadeMin = 0.65f;
  radio.reception.fadeMax = 1.35f;
  radio.reception.receptionBase = 0.6f;
  radio.reception.receptionOffTScale = 0.6f;
  radio.reception.receptionFadeScale = 0.4f;
  radio.reception.noiseScaleMin = 0.3f;
  radio.reception.noiseScaleMax = 1.6f;

  radio.frontEnd.inputHpHz = 115.0f;

  radio.multipath.mixRate = 0.035f;
  radio.multipath.blendRate = 0.027f;
  radio.multipath.delayRate = 0.041f;
  radio.multipath.mix = 0.12f;
  radio.multipath.depth = 0.08f;
  radio.multipath.delayMs = 5.5f;
  radio.multipath.delayModMs = 1.5f;
  radio.multipath.tiltMix = 0.35f;
  radio.multipath.tiltLpHz = 1800.0f;
  radio.multipath.maxMix = 0.35f;
  radio.multipath.lfoMixA = 0.6f;
  radio.multipath.lfoMixB = 0.4f;
  radio.multipath.directMixDepth = 0.5f;

  radio.detune.lfoRateFast = 0.13f;
  radio.detune.lfoRateSlow = 0.09f;
  radio.detune.depth = 0.0f;
  radio.detune.baseDelay = 2.0f;
  radio.detune.minDelay = 0.25f;
  radio.detune.lfoMixA = 0.6f;
  radio.detune.lfoMixB = 0.4f;

  radio.adjacent.beatHz = 980.0f;
  radio.adjacent.modDepth = 0.12f;
  radio.adjacent.mix = 0.010f;
  radio.adjacent.hpHz = 250.0f;
  radio.adjacent.maxMix = 0.20f;

  radio.receiverCircuit.enabled = true;
  radio.receiverCircuit.couplingHpHz = 125.0f;
  radio.receiverCircuit.interstagePeakHz = 1480.0f;
  radio.receiverCircuit.interstagePeakQ = 0.82f;
  radio.receiverCircuit.interstagePeakGainDb = 0.45f;
  radio.receiverCircuit.transformerLpHz = 3150.0f;
  radio.receiverCircuit.presenceDipHz = 2550.0f;
  radio.receiverCircuit.presenceDipQ = 1.05f;
  radio.receiverCircuit.presenceDipGainDb = -0.45f;
  radio.receiverCircuit.driveBase = 1.02f;
  radio.receiverCircuit.asymBiasBase = 0.003f;
  radio.receiverCircuit.controlVoltageGainDepth = 0.12f;
  radio.receiverCircuit.supplySagGainDepth = 0.10f;
  radio.receiverCircuit.supplySagDriveDepth = 0.12f;

  radio.tone.presenceHz = 1650.0f;
  radio.tone.presenceQ = 0.78f;
  radio.tone.presenceGainDb = 0.28f;
  radio.tone.tiltSplitHz = 1450.0f;

  radio.power.sagStart = 0.06f;
  radio.power.sagEnd = 0.22f;
  radio.power.rippleDepth = 0.010f;
  radio.power.satDrive = 1.12f;
  radio.power.satMix = 0.24f;
  radio.power.sagAttackMs = 60.0f;
  radio.power.sagReleaseMs = 900.0f;
  radio.power.rectifierMinHz = 80.0f;
  radio.power.rippleSecondHarmonicMix = 0.28f;
  radio.power.gainSagPerPower = 0.018f;
  radio.power.rippleGainBase = 0.35f;
  radio.power.rippleGainDepth = 0.65f;
  radio.power.gainMin = 0.88f;
  radio.power.gainMax = 1.04f;

  radio.heterodyne.enabled = true;
  radio.heterodyne.driftHz = 0.06f;
  radio.heterodyne.gateStart = 0.015f;
  radio.heterodyne.gateEnd = 0.05f;
  radio.heterodyne.depth = 0.00035f;
  radio.heterodyne.noiseWeightRef = 0.015f;
  radio.heterodyne.heteroBaseScaleMin = 0.10f;
  radio.heterodyne.heteroBaseScaleMax = 0.85f;
  radio.heterodyne.gateHz = 180.0f;
  radio.heterodyne.maxHz = 2500.0f;
  radio.heterodyne.driftDepth = 0.12f;

  radio.noiseConfig.enableHumTone = true;
  radio.noiseConfig.humHzDefault = 50.0f;
  radio.noiseConfig.noiseWeightRef = 0.015f;
  radio.noiseConfig.noiseWeightScaleMax = 2.0f;
  radio.noiseConfig.humAmpScale = 0.0018f;
  radio.noiseConfig.crackleAmpScale = 0.008f;
  radio.noiseConfig.crackleRateScale = 0.50f;

  radio.speakerStage.drive = 0.72f;

  radio.cabinet.enabled = true;
  radio.cabinet.panelHz = 185.0f;
  radio.cabinet.panelQ = 0.82f;
  radio.cabinet.panelGainDb = 0.55f;
  radio.cabinet.chassisHz = 430.0f;
  radio.cabinet.chassisQ = 1.05f;
  radio.cabinet.chassisGainDb = 0.30f;
  radio.cabinet.cavityDipHz = 980.0f;
  radio.cabinet.cavityDipQ = 0.95f;
  radio.cabinet.cavityDipGainDb = -0.55f;
  radio.cabinet.grilleLpHz = 2950.0f;
  radio.cabinet.rearDelayMs = 1.6f;
  radio.cabinet.rearMix = 0.06f;
  radio.cabinet.rearHpHz = 170.0f;
  radio.cabinet.rearLpHz = 1100.0f;
  radio.cabinet.rearMixApplied = 0.06f;

  radio.room.enableEarlyReflections = true;
  radio.room.enableTail = false;
  radio.room.mix = 0.025f;
  radio.room.lpHz = 1800.0f;
  radio.room.tailMix = 0.0f;
  radio.room.tailFeedback = 0.28f;
  radio.room.tailMs = 45.0f;
  radio.room.tailLpHz = 1600.0f;
  radio.room.tapMs = {6.0f, 10.5f, 16.0f, 23.0f, 31.0f};
  radio.room.tapGain = {0.18f, 0.14f, 0.10f, 0.07f, 0.05f};
  radio.room.baseDelayMs = 12.0f;

  radio.finalLimiter.enabled = true;
  radio.finalLimiter.threshold = 0.88f;
  radio.finalLimiter.lookaheadMs = 3.5f;
  radio.finalLimiter.attackMs = 0.20f;
  radio.finalLimiter.releaseMs = 160.0f;
}

static inline float smoothSharedState(float state,
                                      float target,
                                      float attackCoeff,
                                      float releaseCoeff) {
  if (target > state) {
    return attackCoeff * state + (1.0f - attackCoeff) * target;
  }
  return releaseCoeff * state + (1.0f - releaseCoeff) * target;
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
  audioChargeCoeff = std::exp(-1.0f / (fs * (audioChargeMs / 1000.0f)));
  audioReleaseCoeff = std::exp(-1.0f / (fs * (audioReleaseMs / 1000.0f)));
  avcAttackCoeff = std::exp(-1.0f / (fs * (avcAttackMs / 1000.0f)));
  avcReleaseCoeff = std::exp(-1.0f / (fs * (avcReleaseMs / 1000.0f)));
  dcCoeff = std::exp(-1.0f / (fs * (dcMs / 1000.0f)));
  setBandwidth(newBw, newTuneHz);
  reset();
}

void AMDetector::setBandwidth(float newBw, float newTuneHz) {
  bwHz = newBw;
  tuneOffsetHz = newTuneHz;
  float maxIfHz = std::max(ifLowBaseHz + ifMinBwHz + 100.0f, fs * ifMaxFraction);
  float safeBw = std::clamp(bwHz, ifMinBwHz, maxIfHz - 40.0f);
  float ifLow = std::clamp(ifLowBaseHz + tuneOffsetHz, 40.0f, maxIfHz - safeBw);
  float ifHigh = std::clamp(ifLow + safeBw, ifLow + 400.0f, maxIfHz);
  ifHp1.setHighpass(fs, ifLow, kRadioBiquadQ);
  ifHp2.setHighpass(fs, ifLow, kRadioBiquadQ);
  ifLp1.setLowpass(fs, ifHigh, kRadioBiquadQ);
  ifLp2.setLowpass(fs, ifHigh, kRadioBiquadQ);

  float detLpHz = std::clamp(safeBw * detLpScale, detLpMinHz, fs * 0.45f);
  audioHp.setHighpass(fs, audioHpHz, kRadioBiquadQ);
  audioLp1.setLowpass(fs, detLpHz, kRadioBiquadQ);
}

void AMDetector::reset() {
  audioEnv = 0.0f;
  avcEnv = 0.0f;
  dcEnv = 0.0f;
  ifHp1.reset();
  ifHp2.reset();
  ifLp1.reset();
  ifLp2.reset();
  audioHp.reset();
  audioLp1.reset();
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

  ifSample = ifHp1.process(ifSample);
  ifSample = ifHp2.process(ifSample);
  ifSample = ifLp1.process(ifSample);
  ifSample = ifLp2.process(ifSample);

  float rectified = std::max(0.0f, ifSample - diodeDrop);
  if (rectified > audioEnv) {
    audioEnv =
        audioChargeCoeff * audioEnv + (1.0f - audioChargeCoeff) * rectified;
  } else {
    audioEnv =
        audioReleaseCoeff * audioEnv + (1.0f - audioReleaseCoeff) * rectified;
  }

  if (audioEnv > avcEnv) {
    avcEnv = avcAttackCoeff * avcEnv + (1.0f - avcAttackCoeff) * audioEnv;
  } else {
    avcEnv = avcReleaseCoeff * avcEnv + (1.0f - avcReleaseCoeff) * audioEnv;
  }

  dcEnv = dcCoeff * dcEnv + (1.0f - dcCoeff) * audioEnv;
  float out = audioEnv - dcEnv;
  out = audioHp.process(out);
  out = audioLp1.process(out);
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
  float upperBreakupHzDerived =
      std::clamp(upperBreakupHz *
                     (1.0f + 0.40f * breakupTolerance +
                      0.10f * voiceCoilTolerance),
                 coneBodyHzDerived + 240.0f, fs * 0.42f);
  float coneDipHzDerived =
      std::clamp(coneDipHz *
                     (1.0f + 0.28f * breakupTolerance -
                      0.08f * coneMassTolerance),
                 upperBreakupHzDerived + 180.0f, fs * 0.46f);
  float topLpHzDerived =
      std::clamp(topLpHz /
                     std::max(0.35f, 1.0f + 0.40f * voiceCoilTolerance +
                                         0.24f * breakupTolerance),
                 coneDipHzDerived + 220.0f, fs * 0.45f);

  suspensionRes.setPeaking(fs, suspensionHzDerived, suspensionQ, suspensionGainDb);
  coneBody.setPeaking(fs, coneBodyHzDerived, coneBodyQ, coneBodyGainDb);
  upperBreakup.setPeaking(fs, upperBreakupHzDerived, upperBreakupQ,
                          upperBreakupGainDb);
  coneDip.setPeaking(fs, coneDipHzDerived, coneDipQ, coneDipGainDb);
  topLp.setLowpass(fs, topLpHzDerived, filterQ);
  excursionAtk = std::exp(-1.0f / (fs * 0.010f));
  excursionRel = std::exp(-1.0f / (fs * 0.120f));
}

void SpeakerSim::reset() {
  suspensionRes.reset();
  coneBody.reset();
  upperBreakup.reset();
  coneDip.reset();
  topLp.reset();
  excursionEnv = 0.0f;
}

float SpeakerSim::process(float x, bool& clipped) {
  float y = x;
  y = suspensionRes.process(y);
  y = coneBody.process(y);
  y = upperBreakup.process(y);
  y = coneDip.process(y);
  y = topLp.process(y);

  float a = std::fabs(y);
  if (a > excursionEnv) {
    excursionEnv = excursionAtk * excursionEnv + (1.0f - excursionAtk) * a;
  } else {
    excursionEnv = excursionRel * excursionEnv + (1.0f - excursionRel) * a;
  }

  float excursionT =
      clampf(excursionEnv / std::max(excursionRef, 1e-6f), 0.0f, 1.0f);
  float stageDrive = std::max(0.2f, drive * (1.0f + 0.35f * excursionT));
  float stageBias = asymBias * (1.0f + 0.2f * excursionT);
  y = asymmetricSaturate(y, stageDrive, stageBias, 0.15f + 0.20f * excursionT);
  clipped = std::fabs(y) > limit;
  return softClip(y, limit);
}

float RadioTuningNode::applyFilters(Radio1938& radio, float tuneHz, float bwHz) {
  auto& tuning = radio.tuning;
  auto& frontEnd = radio.frontEnd;
  auto& power = radio.power;
  auto& adjacent = radio.adjacent;
  float safeBw = std::clamp(bwHz, tuning.safeBwMinHz, tuning.safeBwMaxHz);
  float tuneNorm =
      (safeBw > 0.0f) ? clampf(tuneHz / (safeBw * 0.5f), -1.0f, 1.0f) : 0.0f;
  tuning.tuneOffsetNorm = tuneNorm;
  float preBw =
      std::clamp(safeBw * tuning.preBwScale, 1200.0f, radio.sampleRate * 0.45f);
  float postBw =
      std::clamp(safeBw * tuning.postBwScale, preBw, radio.sampleRate * 0.45f);

  frontEnd.preLpfIn.setLowpass(radio.sampleRate, preBw, kRadioBiquadQ);
  frontEnd.preLpfOut.setLowpass(radio.sampleRate, preBw, kRadioBiquadQ);
  power.postLpf.setLowpass(radio.sampleRate, postBw, kRadioBiquadQ);

  float adjLpHz = std::clamp(safeBw * tuning.adjLpScale, tuning.adjLpMinHz,
                             safeBw * tuning.adjLpMaxScale);
  adjacent.lp.setLowpass(radio.sampleRate, adjLpHz, kRadioBiquadQ);

  tuning.tunedBw = safeBw;
  return safeBw;
}

void RadioTuningNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& tuning = radio.tuning;
  initCtx.tunedBw = applyFilters(radio, tuning.tuneOffsetHz, radio.bwHz);
  tuning.tuneAppliedHz = tuning.tuneOffsetHz;
  tuning.bwAppliedHz = radio.bwHz;
  tuning.tuneSmoothedHz = tuning.tuneOffsetHz;
  tuning.bwSmoothedHz = radio.bwHz;
}

void RadioTuningNode::reset(Radio1938&) {}

void RadioTuningNode::prepare(Radio1938& radio,
                              RadioBlockControl& block,
                              uint32_t frames) {
  auto& tuning = radio.tuning;
  auto& demod = radio.demod;
  auto& noiseRuntime = radio.noiseRuntime;
  block.sampleRate = radio.sampleRate;
  float rate = std::max(1.0f, radio.sampleRate);
  float tick =
      1.0f - std::exp(-static_cast<float>(frames) / (rate * tuning.smoothTau));
  tuning.tuneSmoothedHz += tick * (tuning.tuneOffsetHz - tuning.tuneSmoothedHz);
  tuning.bwSmoothedHz += tick * (radio.bwHz - tuning.bwSmoothedHz);

  float safeBw =
      std::clamp(tuning.bwSmoothedHz, tuning.safeBwMinHz, tuning.safeBwMaxHz);
  float bwHalf = 0.5f * std::max(1.0f, safeBw);
  block.tuneNorm = clampf(tuning.tuneSmoothedHz / bwHalf, -1.0f, 1.0f);
  block.offHz = std::fabs(tuning.tuneSmoothedHz);

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

void RadioReceptionControlNode::init(Radio1938&, RadioInitContext&) {}

void RadioReceptionControlNode::reset(Radio1938& radio) {
  auto& reception = radio.reception;
  reception.fadePhaseFast = 0.0f;
  reception.fadePhaseSlow = 0.0f;
  reception.noisePhaseFast = 0.0f;
  reception.noisePhaseSlow = 0.0f;
}

void RadioReceptionControlNode::update(Radio1938& radio,
                                       RadioSampleContext& ctx) {
  auto& reception = radio.reception;
  const auto& block = *ctx.block;

  reception.fadePhaseFast +=
      kRadioTwoPi * (reception.fadeRateFast / radio.sampleRate);
  reception.fadePhaseSlow +=
      kRadioTwoPi * (reception.fadeRateSlow / radio.sampleRate);
  if (reception.fadePhaseFast > kRadioTwoPi) {
    reception.fadePhaseFast -= kRadioTwoPi;
  }
  if (reception.fadePhaseSlow > kRadioTwoPi) {
    reception.fadePhaseSlow -= kRadioTwoPi;
  }
  float fadeLfo = reception.lfoMixA * std::sin(reception.fadePhaseFast) +
                  reception.lfoMixB * std::sin(reception.fadePhaseSlow);
  ctx.control.receptionGain =
      std::clamp(1.0f + reception.fadeDepth * fadeLfo, reception.fadeMin,
                 reception.fadeMax);

  reception.noisePhaseFast +=
      kRadioTwoPi * (reception.noiseRateFast / radio.sampleRate);
  reception.noisePhaseSlow +=
      kRadioTwoPi * (reception.noiseRateSlow / radio.sampleRate);
  if (reception.noisePhaseFast > kRadioTwoPi) {
    reception.noisePhaseFast -= kRadioTwoPi;
  }
  if (reception.noisePhaseSlow > kRadioTwoPi) {
    reception.noisePhaseSlow -= kRadioTwoPi;
  }
  float noiseLfo = reception.lfoMixA * std::sin(reception.noisePhaseFast) +
                   reception.lfoMixB * std::sin(reception.noisePhaseSlow);
  ctx.control.noiseScale = 1.0f + reception.noiseDepth * noiseLfo;
  float fadeT =
      clampf((1.0f - ctx.control.receptionGain) / (1.0f - reception.fadeMin), 0.0f,
             1.0f);
  float tuneOffNorm = std::fabs(block.tuneNorm);
  float receptionScale = reception.receptionBase +
                         reception.receptionOffTScale * tuneOffNorm +
                         reception.receptionFadeScale * fadeT;
  ctx.control.noiseScale *= receptionScale;
  ctx.control.noiseScale = std::clamp(ctx.control.noiseScale,
                                      reception.noiseScaleMin,
                                      reception.noiseScaleMax);
}

void RadioControlBusNode::init(Radio1938& radio, RadioInitContext&) {
  auto& controlBus = radio.controlBus;
  float sampleRate = std::max(radio.sampleRate, 1.0f);
  controlBus.controlVoltageAtk = std::exp(
      -1.0f / (sampleRate * (controlBus.controlVoltageAttackMs / 1000.0f)));
  controlBus.controlVoltageRel = std::exp(
      -1.0f / (sampleRate * (controlBus.controlVoltageReleaseMs / 1000.0f)));
  controlBus.supplySagAtk =
      std::exp(-1.0f / (sampleRate * (controlBus.supplySagAttackMs / 1000.0f)));
  controlBus.supplySagRel = std::exp(
      -1.0f / (sampleRate * (controlBus.supplySagReleaseMs / 1000.0f)));
}

void RadioControlBusNode::reset(Radio1938& radio) {
  radio.controlSense.reset();
  radio.controlBus.reset();
}

void RadioControlBusNode::update(Radio1938& radio, RadioSampleContext&) {
  auto& controlBus = radio.controlBus;
  const auto& controlSense = radio.controlSense;
  const auto& power = radio.power;
  float controlTarget = clampf(controlSense.controlVoltageSense, 0.0f, 1.25f);
  float supplyTarget =
      clampf((controlSense.powerSagSense - power.sagStart) /
                 std::max(1e-6f, power.sagEnd - power.sagStart),
             0.0f, 1.0f);
  controlBus.controlVoltage =
      smoothSharedState(controlBus.controlVoltage, controlTarget,
                        controlBus.controlVoltageAtk, controlBus.controlVoltageRel);
  controlBus.supplySag =
      smoothSharedState(controlBus.supplySag, supplyTarget, controlBus.supplySagAtk,
                        controlBus.supplySagRel);
}

void RadioFrontEndNode::init(Radio1938& radio, RadioInitContext&) {
  radio.frontEnd.hpf.setHighpass(radio.sampleRate, radio.frontEnd.inputHpHz,
                                 kRadioBiquadQ);
}

void RadioFrontEndNode::reset(Radio1938& radio) {
  auto& frontEnd = radio.frontEnd;
  frontEnd.hpf.reset();
  frontEnd.preLpfIn.reset();
  frontEnd.preLpfOut.reset();
}

float RadioFrontEndNode::process(Radio1938& radio,
                                 float x,
                                 const RadioSampleContext&) {
  auto& frontEnd = radio.frontEnd;
  float y = frontEnd.hpf.process(x);
  y = frontEnd.preLpfIn.process(y);
  y = frontEnd.preLpfOut.process(y);
  return y;
}

void RadioMultipathNode::init(Radio1938& radio, RadioInitContext&) {
  auto& multipath = radio.multipath;
  multipath.tiltLp.setLowpass(radio.sampleRate, multipath.tiltLpHz, kRadioBiquadQ);
  float mpMaxDelay = std::max(0.0f, multipath.delayMs + multipath.delayModMs);
  int mpSamples =
      std::max(multipath.minBufferSamples,
               static_cast<int>(std::ceil(mpMaxDelay * 0.001f * radio.sampleRate)) +
                   multipath.bufferGuardSamples);
  multipath.buf.assign(static_cast<size_t>(mpSamples), 0.0f);
  multipath.index = 0;
  multipath.mix = 0.00f;
}

void RadioMultipathNode::reset(Radio1938& radio) {
  auto& multipath = radio.multipath;
  multipath.mixPhase = 0.0f;
  multipath.blendPhase = 0.0f;
  multipath.delayPhase = 0.0f;
  multipath.index = 0;
  std::fill(multipath.buf.begin(), multipath.buf.end(), 0.0f);
  multipath.tiltLp.reset();
}

float RadioMultipathNode::process(Radio1938& radio,
                                  float y,
                                  const RadioSampleContext&) {
  auto& multipath = radio.multipath;
  if (!multipath.buf.empty() && multipath.mix > 0.0f) {
    multipath.mixPhase +=
        kRadioTwoPi * (multipath.mixRate / radio.sampleRate);
    multipath.blendPhase +=
        kRadioTwoPi * (multipath.blendRate / radio.sampleRate);
    multipath.delayPhase +=
        kRadioTwoPi * (multipath.delayRate / radio.sampleRate);
    if (multipath.mixPhase > kRadioTwoPi) multipath.mixPhase -= kRadioTwoPi;
    if (multipath.blendPhase > kRadioTwoPi) {
      multipath.blendPhase -= kRadioTwoPi;
    }
    if (multipath.delayPhase > kRadioTwoPi) {
      multipath.delayPhase -= kRadioTwoPi;
    }

    float mixLfo = std::sin(multipath.mixPhase);
    float mix = multipath.mix * (1.0f + multipath.depth * mixLfo);
    mix = std::clamp(mix, 0.0f, multipath.maxMix);

    float delayMs =
        multipath.delayMs + multipath.delayModMs * std::sin(multipath.delayPhase);
    float maxDelay = static_cast<float>(multipath.buf.size() - 2);
    float delaySamples =
        std::clamp(delayMs * 0.001f * radio.sampleRate, 1.0f, maxDelay);
    float read = static_cast<float>(multipath.index) - delaySamples;
    while (read < 0.0f) read += static_cast<float>(multipath.buf.size());
    int i0 = static_cast<int>(read);
    int i1 = (i0 + 1) % static_cast<int>(multipath.buf.size());
    float frac = read - static_cast<float>(i0);
    float delayed = multipath.buf[static_cast<size_t>(i0)] * (1.0f - frac) +
                    multipath.buf[static_cast<size_t>(i1)] * frac;

    multipath.buf[static_cast<size_t>(multipath.index)] = y;
    multipath.index =
        (multipath.index + 1) % static_cast<int>(multipath.buf.size());

    float mpLow = multipath.tiltLp.process(delayed);
    float mpHigh = delayed - mpLow;
    delayed = mpLow + mpHigh * (1.0f - multipath.tiltMix);

    float phaseMix = std::sin(multipath.blendPhase);
    float direct = y * (1.0f - multipath.directMixDepth * mix);
    return direct + delayed * mix * phaseMix;
  }

  if (!multipath.buf.empty()) {
    multipath.buf[static_cast<size_t>(multipath.index)] = y;
    multipath.index =
        (multipath.index + 1) % static_cast<int>(multipath.buf.size());
  }
  return y;
}

void RadioDetuneNode::init(Radio1938& radio, RadioInitContext&) {
  radio.detune.buf.assign(static_cast<size_t>(radio.detune.bufferSamples), 0.0f);
  radio.detune.index = 0;
}

void RadioDetuneNode::reset(Radio1938& radio) {
  auto& detune = radio.detune;
  detune.lfoPhaseFast = 0.0f;
  detune.lfoPhaseSlow = 0.0f;
  detune.index = 0;
  std::fill(detune.buf.begin(), detune.buf.end(), 0.0f);
}

float RadioDetuneNode::process(Radio1938& radio,
                               float y,
                               const RadioSampleContext&) {
  auto& detune = radio.detune;
  if (detune.buf.empty() || detune.depth <= 0.0f) return y;

  detune.lfoPhaseFast += kRadioTwoPi * (detune.lfoRateFast / radio.sampleRate);
  detune.lfoPhaseSlow += kRadioTwoPi * (detune.lfoRateSlow / radio.sampleRate);
  if (detune.lfoPhaseFast > kRadioTwoPi) {
    detune.lfoPhaseFast -= kRadioTwoPi;
  }
  if (detune.lfoPhaseSlow > kRadioTwoPi) {
    detune.lfoPhaseSlow -= kRadioTwoPi;
  }
  float lfo = detune.lfoMixA * std::sin(detune.lfoPhaseFast) +
              detune.lfoMixB * std::sin(detune.lfoPhaseSlow);
  float delay = detune.baseDelay + detune.depth * lfo;
  float maxDelay = static_cast<float>(detune.buf.size() - 2);
  delay = std::clamp(delay, detune.minDelay, maxDelay);
  float read = static_cast<float>(detune.index) - delay;
  while (read < 0.0f) read += static_cast<float>(detune.buf.size());
  int i0 = static_cast<int>(read);
  int i1 = (i0 + 1) % static_cast<int>(detune.buf.size());
  float frac = read - static_cast<float>(i0);
  float delayed = detune.buf[static_cast<size_t>(i0)] * (1.0f - frac) +
                  detune.buf[static_cast<size_t>(i1)] * frac;
  detune.buf[static_cast<size_t>(detune.index)] = y;
  detune.index = (detune.index + 1) % static_cast<int>(detune.buf.size());
  return delayed;
}

void RadioAdjacentNode::init(Radio1938& radio, RadioInitContext&) {
  auto& adjacent = radio.adjacent;
  adjacent.hp.setHighpass(radio.sampleRate, adjacent.hpHz, kRadioBiquadQ);
}

void RadioAdjacentNode::reset(Radio1938& radio) {
  auto& adjacent = radio.adjacent;
  adjacent.phase = 0.0f;
  adjacent.hp.reset();
  adjacent.lp.reset();
}

float RadioAdjacentNode::process(Radio1938& radio,
                                 float y,
                                 const RadioSampleContext& ctx) {
  auto& adjacent = radio.adjacent;
  const auto& block = *ctx.block;
  float overlap = clampf(std::fabs(block.tuneNorm), 0.0f, 1.0f);
  if (!(adjacent.mix > 0.0f && overlap > 0.0f)) {
    return y;
  }

  adjacent.phase += kRadioTwoPi * (adjacent.beatHz / radio.sampleRate);
  if (adjacent.phase > kRadioTwoPi) adjacent.phase -= kRadioTwoPi;
  float beat = 1.0f + adjacent.modDepth * std::sin(adjacent.phase);
  float adj = adjacent.hp.process(y);
  adj = adjacent.lp.process(adj);
  float mix = std::clamp(adjacent.mix * overlap, 0.0f, adjacent.maxMix);
  return y + mix * beat * adj;
}

void RadioDemodNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& demod = radio.demod;
  demod.am.init(radio.sampleRate, initCtx.tunedBw, radio.tuning.tuneOffsetHz);
}

void RadioDemodNode::reset(Radio1938& radio) { radio.demod.am.reset(); }

float RadioDemodNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext& ctx) {
  auto& demod = radio.demod;
  y = demod.am.process(
      AMDetectorSampleInput{y, ctx.derived.demodIfNoiseAmp});
  radio.controlSense.controlVoltageSense =
      clampf(demod.am.avcEnv / std::max(demod.am.controlVoltageRef, 1e-6f),
             0.0f, 1.25f);
  return y;
}

void RadioReceiverCircuitNode::init(Radio1938& radio, RadioInitContext&) {
  auto& receiver = radio.receiverCircuit;
  float couplingScale =
      std::max(0.25f, (1.0f + receiver.couplingCapTolerance) *
                           (1.0f + receiver.gridLeakTolerance));
  float couplingHpHz =
      std::clamp(receiver.couplingHpHz / couplingScale, 60.0f,
                 radio.sampleRate * 0.25f);
  float interstagePeakHz = std::clamp(
      receiver.interstagePeakHz *
          (1.0f + 0.5f * receiver.plateLoadTolerance -
           0.35f * receiver.toneCapTolerance),
      500.0f, radio.sampleRate * 0.45f);
  float transformerScale =
      std::max(0.25f, (1.0f + receiver.toneCapTolerance) *
                           (1.0f + receiver.transformerTolerance));
  float transformerLpHz =
      std::clamp(receiver.transformerLpHz / transformerScale,
                 interstagePeakHz + 400.0f, radio.sampleRate * 0.45f);
  float presenceDipHz = std::clamp(
      receiver.presenceDipHz *
          (1.0f + 0.4f * receiver.transformerTolerance -
           0.25f * receiver.plateLoadTolerance),
      900.0f, radio.sampleRate * 0.45f);

  receiver.couplingHp.setHighpass(radio.sampleRate, couplingHpHz, kRadioBiquadQ);
  receiver.interstagePeak.setPeaking(radio.sampleRate, interstagePeakHz,
                                     receiver.interstagePeakQ,
                                     receiver.interstagePeakGainDb);
  receiver.presenceDip.setPeaking(radio.sampleRate, presenceDipHz,
                                  receiver.presenceDipQ,
                                  receiver.presenceDipGainDb);
  receiver.transformerLp.setLowpass(radio.sampleRate, transformerLpHz,
                                    kRadioBiquadQ);
}

void RadioReceiverCircuitNode::reset(Radio1938& radio) {
  auto& receiver = radio.receiverCircuit;
  receiver.couplingHp.reset();
  receiver.interstagePeak.reset();
  receiver.presenceDip.reset();
  receiver.transformerLp.reset();
}

float RadioReceiverCircuitNode::process(Radio1938& radio,
                                        float y,
                                        const RadioSampleContext&) {
  auto& receiver = radio.receiverCircuit;
  const auto& controlBus = radio.controlBus;
  if (!receiver.enabled) return y;

  y = receiver.couplingHp.process(y);
  y = receiver.interstagePeak.process(y);
  y = receiver.presenceDip.process(y);
  y = receiver.transformerLp.process(y);

  float controlVoltageT = clampf(controlBus.controlVoltage, 0.0f, 1.25f);
  float supplySagT = clampf(controlBus.supplySag, 0.0f, 1.0f);
  float stageGain = 1.0f - receiver.controlVoltageGainDepth * controlVoltageT -
                    receiver.supplySagGainDepth * supplySagT;
  y *= std::max(0.5f, stageGain);

  float drive = receiver.driveBase *
                (1.0f + 0.12f * receiver.plateLoadTolerance -
                 0.08f * receiver.transformerTolerance);
  drive *= 1.0f - receiver.supplySagDriveDepth * supplySagT;
  float bias = receiver.asymBiasBase *
               (1.0f + 0.45f * receiver.gridLeakTolerance -
                0.20f * receiver.plateLoadTolerance);
  return asymmetricSaturate(y, std::max(0.2f, drive), bias, 0.2f);
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
  float lowBand = tone.tiltLp.process(y);
  float highBand = y - lowBand;
  y = lowBand * 1.01f + highBand * 0.96f;
  y = tone.presence.process(y);
  return y;
}

void RadioPowerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& power = radio.power;
  power.sagAtk =
      std::exp(-1.0f / (radio.sampleRate * (power.sagAttackMs / 1000.0f)));
  power.sagRel =
      std::exp(-1.0f / (radio.sampleRate * (power.sagReleaseMs / 1000.0f)));
  power.sat.drive = power.satDrive;
  power.sat.mix = power.satMix;
  float osFs = radio.sampleRate * radio.globals.oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  power.satOsLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  power.satOsLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
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
  auto& noiseConfig = radio.noiseConfig;
  float load = std::fabs(y);
  if (load > power.sagEnv) {
    power.sagEnv = power.sagAtk * power.sagEnv + (1.0f - power.sagAtk) * load;
  } else {
    power.sagEnv = power.sagRel * power.sagEnv + (1.0f - power.sagRel) * load;
  }
  float powerT = clampf((power.sagEnv - power.sagStart) /
                            std::max(1e-6f, power.sagEnd - power.sagStart),
                        0.0f, 1.0f);
  float rectHz = std::max(power.rectifierMinHz, noiseConfig.humHzDefault * 2.0f);
  power.rectifierPhase += kRadioTwoPi * (rectHz / radio.sampleRate);
  if (power.rectifierPhase > kRadioTwoPi) {
    power.rectifierPhase -= kRadioTwoPi;
  }
  float rippleAmp =
      power.rippleDepth * (power.rippleGainBase + power.rippleGainDepth * powerT);
  float ripple = rippleAmp *
                 (std::sin(power.rectifierPhase) +
                  power.rippleSecondHarmonicMix *
                      std::sin(2.0f * power.rectifierPhase));
  float supplyGain =
      std::clamp(1.0f - power.gainSagPerPower * powerT, power.gainMin,
                 power.gainMax);
  y *= supplyGain;
  y += ripple;
  power.sat.drive = power.satDrive;
  power.sat.mix = power.satMix;
  y = processOversampled2x(y, power.satOsPrev, power.satOsLpIn, power.satOsLpOut,
                           [&](float v) {
                             float out = power.sat.process(v);
                             float level = std::max(std::fabs(out), std::fabs(v));
                             if (level > radio.globals.satClipMinLevel &&
                                 std::fabs(out - v) > radio.globals.satClipDelta) {
                               radio.diagnostics.markPowerClip();
                             }
                             return out;
                           });
  y = power.postLpf.process(y);
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
  auto& heterodyne = radio.heterodyne;
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseDerived = radio.noiseDerived;
  const auto& controlBus = radio.controlBus;
  float postNoiseScale =
      std::max(ctx.control.noiseScale * radio.globals.postNoiseMix, 0.06f);
  ctx.derived.demodIfNoiseAmp =
      noiseDerived.baseNoiseAmp * ctx.control.noiseScale * radio.globals.ifNoiseMix;
  ctx.derived.noiseAmp =
      std::max(noiseDerived.baseNoiseAmp * postNoiseScale,
               radio.globals.noiseFloorAmp);
  ctx.derived.crackleAmp = noiseDerived.baseCrackleAmp * postNoiseScale;
  ctx.derived.crackleRate = noiseDerived.crackleRate;
  ctx.derived.humAmp = noiseDerived.baseHumAmp * postNoiseScale;
  ctx.derived.humToneEnabled = noiseConfig.enableHumTone;
  float quietT =
      clampf((controlBus.supplySag - heterodyne.gateStart) /
                 std::max(1e-6f, heterodyne.gateEnd - heterodyne.gateStart),
             0.0f, 1.0f);
  ctx.derived.quieting = 1.0f - quietT;
  ctx.derived.quieting *= ctx.derived.quieting;
}

void RadioHeterodyneNode::init(Radio1938& radio, RadioInitContext&) {
  auto& heterodyne = radio.heterodyne;
  heterodyne.heteroBaseScale =
      (radio.noiseWeight > 0.0f)
          ? std::clamp(radio.noiseWeight / heterodyne.noiseWeightRef,
                       heterodyne.heteroBaseScaleMin,
                       heterodyne.heteroBaseScaleMax)
          : 0.0f;
}

void RadioHeterodyneNode::reset(Radio1938& radio) {
  auto& heterodyne = radio.heterodyne;
  heterodyne.phase = 0.0f;
  heterodyne.driftPhase = 0.0f;
}

float RadioHeterodyneNode::process(Radio1938& radio,
                                   float y,
                                   const RadioSampleContext& ctx) {
  auto& heterodyne = radio.heterodyne;
  const auto& block = *ctx.block;

  if (!(heterodyne.enabled && heterodyne.depth > 0.0f &&
        heterodyne.heteroBaseScale > 0.0f)) {
    return y;
  }

  if (block.offHz < heterodyne.gateHz) return y;

  heterodyne.driftPhase +=
      kRadioTwoPi * (heterodyne.driftHz / radio.sampleRate);
  if (heterodyne.driftPhase > kRadioTwoPi) {
    heterodyne.driftPhase -= kRadioTwoPi;
  }
  float drift =
      1.0f + heterodyne.driftDepth * std::sin(heterodyne.driftPhase);
  float heteroHz = std::min(block.offHz, heterodyne.maxHz) * drift;
  heterodyne.phase += kRadioTwoPi * (heteroHz / radio.sampleRate);
  if (heterodyne.phase > kRadioTwoPi) heterodyne.phase -= kRadioTwoPi;
  float hetero = std::sin(heterodyne.phase);
  float gate = clampf((block.offHz - heterodyne.gateHz) /
                          (heterodyne.maxHz - heterodyne.gateHz),
                      0.0f, 1.0f);
  gate *= gate;
  float amp = heterodyne.depth * heterodyne.heteroBaseScale *
              ctx.derived.quieting * gate *
              clampf(std::fabs(block.tuneNorm), 0.0f, 1.0f);
  return y + hetero * amp;
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
  float supplySagT = clampf(radio.controlBus.supplySag, 0.0f, 1.0f);
  float controlVoltageT = clampf(radio.controlBus.controlVoltage, 0.0f, 1.25f);
  speakerStage.speaker.drive =
      speakerStage.drive *
      (1.0f - 0.08f * supplySagT - 0.04f * controlVoltageT);
  y *= radio.makeupGain * (1.0f - 0.03f * supplySagT);
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
  float grilleLpHzDerived = std::clamp(
      cabinet.grilleLpHz /
          std::max(0.35f, 1.0f + 0.55f * cabinet.grilleClothTolerance),
      cavityDipHzDerived + 220.0f, radio.sampleRate * 0.45f);
  float rearDelayMsDerived = std::max(
      0.4f, cabinet.rearDelayMs *
                (1.0f + 0.30f * cabinet.rearPathTolerance +
                 0.12f * cabinet.baffleLeakTolerance));
  cabinet.rearMixApplied = std::clamp(
      cabinet.rearMix * (1.0f + 0.42f * cabinet.baffleLeakTolerance -
                         0.25f * cabinet.grilleClothTolerance),
      0.0f, 0.12f);

  cabinet.panel.setPeaking(radio.sampleRate, panelHzDerived, cabinet.panelQ,
                           cabinet.panelGainDb);
  cabinet.chassis.setPeaking(radio.sampleRate, chassisHzDerived,
                             cabinet.chassisQ, cabinet.chassisGainDb);
  cabinet.cavityDip.setPeaking(radio.sampleRate, cavityDipHzDerived,
                               cabinet.cavityDipQ, cabinet.cavityDipGainDb);
  cabinet.grilleLp.setLowpass(radio.sampleRate, grilleLpHzDerived,
                              kRadioBiquadQ);
  cabinet.rearHp.setHighpass(radio.sampleRate, cabinet.rearHpHz, kRadioBiquadQ);
  cabinet.rearLp.setLowpass(radio.sampleRate, cabinet.rearLpHz, kRadioBiquadQ);
  int rearSamples =
      std::max(cabinet.minBufferSamples,
               static_cast<int>(std::ceil(rearDelayMsDerived * 0.001f *
                                          radio.sampleRate)) +
                   cabinet.bufferGuardSamples);
  cabinet.buf.assign(static_cast<size_t>(rearSamples), 0.0f);
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
  out = cabinet.grilleLp.process(out);
  return out;
}

void RadioRoomNode::init(Radio1938& radio, RadioInitContext&) {
  auto& room = radio.room;
  room.tapSamples.clear();
  room.tapGains.clear();
  int maxTap = 1;
  for (size_t i = 0; i < room.tapMs.size(); ++i) {
    int samples = std::max(
        1, static_cast<int>(std::lround(room.tapMs[i] * 0.001f * radio.sampleRate)));
    room.tapSamples.push_back(samples);
    room.tapGains.push_back(room.tapGain[i]);
    maxTap = std::max(maxTap, samples);
  }
  int baseRoom = std::max(
      1, static_cast<int>(std::lround(radio.sampleRate * (room.baseDelayMs / 1000.0f))));
  room.delaySamples = std::max(baseRoom, maxTap);
  room.buf.assign(static_cast<size_t>(room.delaySamples + 2), 0.0f);
  room.index = 0;
  room.lp.setLowpass(radio.sampleRate, room.lpHz, kRadioBiquadQ);
  if (room.enableTail) {
    int tailSamples =
        std::max(room.tailMinSamples,
                 static_cast<int>(std::lround(room.tailMs * 0.001f * radio.sampleRate)));
    room.tailBuf.assign(static_cast<size_t>(tailSamples), 0.0f);
    room.tailIndex = 0;
    room.tailLp.setLowpass(radio.sampleRate, room.tailLpHz, kRadioBiquadQ);
  } else {
    room.tailBuf.clear();
    room.tailIndex = 0;
  }
}

void RadioRoomNode::reset(Radio1938& radio) {
  auto& room = radio.room;
  room.index = 0;
  std::fill(room.buf.begin(), room.buf.end(), 0.0f);
  room.tailIndex = 0;
  std::fill(room.tailBuf.begin(), room.tailBuf.end(), 0.0f);
  room.lp.reset();
  room.tailLp.reset();
}

float RadioRoomNode::process(Radio1938& radio,
                             float y,
                             const RadioSampleContext&) {
  auto& room = radio.room;
  if (room.buf.empty()) return y;

  float dry = y;
  float roomOut = 0.0f;
  if (room.mix > 0.0f && room.enableEarlyReflections && !room.tapSamples.empty()) {
    int size = static_cast<int>(room.buf.size());
    int writeIndex = room.index;
    size_t tapCount = std::min(room.tapSamples.size(), room.tapGains.size());
    for (size_t i = 0; i < tapCount; ++i) {
      int tap = room.tapSamples[i];
      int idx = writeIndex - tap;
      while (idx < 0) idx += size;
      roomOut += room.buf[static_cast<size_t>(idx)] * room.tapGains[i];
    }
  } else if (room.mix > 0.0f) {
    roomOut = room.buf[static_cast<size_t>(room.index)];
  }

  room.buf[static_cast<size_t>(room.index)] = dry;
  room.index = (room.index + 1) % static_cast<int>(room.buf.size());

  if (room.mix > 0.0f) {
    roomOut = room.lp.process(roomOut);
    y += room.mix * roomOut;
  }

  if (room.enableTail && room.tailMix > 0.0f && !room.tailBuf.empty()) {
    float tailSample = room.tailBuf[static_cast<size_t>(room.tailIndex)];
    float tailLp = room.tailLp.process(tailSample);
    room.tailBuf[static_cast<size_t>(room.tailIndex)] =
        dry + tailLp * room.tailFeedback;
    room.tailIndex = (room.tailIndex + 1) % static_cast<int>(room.tailBuf.size());
    y += room.tailMix * tailLp;
  }
  return y;
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

template <size_t StepCount>
static RadioBlockControl runBlockPrepare(
    Radio1938& radio,
    const std::array<BlockStep, StepCount>& steps,
    uint32_t frames) {
  RadioBlockControl block{};
  block.sampleRate = radio.sampleRate;
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

static void applyPhilco37116XPreset(Radio1938& radio) {
  radio.makeupGain = 0.88f;
  radio.presentationGain = 1.03f;

  radio.graph.setEnabled(StageId::Multipath, false);
  radio.graph.setEnabled(StageId::Detune, false);
  radio.graph.setEnabled(StageId::Room, false);

  radio.globals.ifNoiseMix = 0.28f;
  radio.globals.postNoiseMix = 0.17f;
  radio.globals.noiseFloorAmp = 0.0032f;
  radio.globals.inputPad = 1.0f;
  radio.globals.enableAutoLevel = false;
  radio.globals.autoTargetDb = -21.0f;
  radio.globals.autoMaxBoostDb = 2.5f;
  radio.globals.outputClipThreshold = 0.98f;

  radio.tuning.safeBwMinHz = 3400.0f;
  radio.tuning.safeBwMaxHz = 4000.0f;
  radio.tuning.preBwScale = 1.00f;
  radio.tuning.postBwScale = 1.04f;
  radio.tuning.adjLpScale = 0.80f;

  radio.frontEnd.inputHpHz = 115.0f;

  radio.multipath.mix = 0.0f;
  radio.multipath.depth = 0.0f;
  radio.detune.depth = 0.0f;

  radio.adjacent.mix = 0.008f;
  radio.adjacent.modDepth = 0.10f;
  radio.adjacent.beatHz = 920.0f;

  radio.demod.am.detectorGain = 4.2f;
  radio.demod.am.diodeDrop = 0.0060f;
  radio.demod.am.audioChargeMs = 0.05f;
  radio.demod.am.audioReleaseMs = 0.35f;
  radio.demod.am.avcAttackMs = 90.0f;
  radio.demod.am.avcReleaseMs = 1500.0f;
  radio.demod.am.controlVoltageRef = 0.18f;

  radio.controlBus.controlVoltageAttackMs = 2.4f;
  radio.controlBus.controlVoltageReleaseMs = 36.0f;
  radio.controlBus.supplySagAttackMs = 10.0f;
  radio.controlBus.supplySagReleaseMs = 220.0f;

  radio.receiverCircuit.couplingCapTolerance = 0.0f;
  radio.receiverCircuit.gridLeakTolerance = 0.0f;
  radio.receiverCircuit.plateLoadTolerance = 0.0f;
  radio.receiverCircuit.toneCapTolerance = 0.0f;
  radio.receiverCircuit.transformerTolerance = 0.0f;
  radio.receiverCircuit.couplingHpHz = 116.0f;
  radio.receiverCircuit.interstagePeakHz = 1325.0f;
  radio.receiverCircuit.interstagePeakGainDb = 0.24f;
  radio.receiverCircuit.transformerLpHz = 3520.0f;
  radio.receiverCircuit.presenceDipHz = 2480.0f;
  radio.receiverCircuit.presenceDipGainDb = -0.10f;
  radio.receiverCircuit.driveBase = 0.92f;
  radio.receiverCircuit.asymBiasBase = 0.0030f;
  radio.receiverCircuit.controlVoltageGainDepth = 0.02f;
  radio.receiverCircuit.supplySagGainDepth = 0.03f;
  radio.receiverCircuit.supplySagDriveDepth = 0.10f;

  radio.tone.presenceHz = 1600.0f;
  radio.tone.presenceGainDb = 0.14f;
  radio.tone.tiltSplitHz = 1450.0f;

  radio.power.sagAttackMs = 60.0f;
  radio.power.sagReleaseMs = 900.0f;
  radio.power.rippleDepth = 0.010f;
  radio.power.rippleSecondHarmonicMix = 0.28f;
  radio.power.gainSagPerPower = 0.020f;
  radio.power.rippleGainBase = 0.35f;
  radio.power.rippleGainDepth = 0.65f;
  radio.power.gainMin = 0.88f;
  radio.power.gainMax = 1.02f;
  radio.power.satDrive = 1.08f;
  radio.power.satMix = 0.08f;

  radio.noiseConfig.enableHumTone = true;
  radio.noiseConfig.humAmpScale = 0.0022f;
  radio.noiseConfig.crackleAmpScale = 0.0035f;
  radio.noiseConfig.crackleRateScale = 0.08f;

  radio.speakerStage.drive = 0.60f;
  radio.speakerStage.speaker.limit = 0.99f;
  radio.speakerStage.speaker.asymBias = 0.018f;
  radio.speakerStage.speaker.suspensionGainDb = 0.48f;
  radio.speakerStage.speaker.coneBodyGainDb = 0.18f;
  radio.speakerStage.speaker.upperBreakupGainDb = 0.02f;
  radio.speakerStage.speaker.coneDipGainDb = -0.60f;
  radio.speakerStage.speaker.topLpHz = 3380.0f;
  radio.speakerStage.speaker.excursionRef = 0.19f;

  radio.cabinet.panelHz = 190.0f;
  radio.cabinet.panelGainDb = 0.74f;
  radio.cabinet.chassisHz = 420.0f;
  radio.cabinet.chassisGainDb = 0.44f;
  radio.cabinet.cavityDipHz = 930.0f;
  radio.cabinet.cavityDipGainDb = -0.36f;
  radio.cabinet.grilleLpHz = 2680.0f;
  radio.cabinet.rearDelayMs = 1.5f;
  radio.cabinet.rearMix = 0.058f;
  radio.cabinet.rearLpHz = 980.0f;

  radio.room.enableEarlyReflections = false;
  radio.room.mix = 0.0f;
  radio.room.enableTail = false;
  radio.room.tailMix = 0.0f;

  radio.finalLimiter.threshold = 0.98f;
  radio.finalLimiter.lookaheadMs = 2.0f;
  radio.finalLimiter.attackMs = 0.20f;
  radio.finalLimiter.releaseMs = 160.0f;
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
      return "Tuning";
    case StageId::Input:
      return "Input";
    case StageId::Reception:
      return "Reception";
    case StageId::ControlBus:
      return "ControlBus";
    case StageId::InterferenceDerived:
      return "InterferenceDerived";
    case StageId::FrontEnd:
      return "FrontEnd";
    case StageId::Multipath:
      return "Multipath";
    case StageId::Detune:
      return "Detune";
    case StageId::Adjacent:
      return "Adjacent";
    case StageId::Demod:
      return "Demod";
    case StageId::ReceiverCircuit:
      return "ReceiverCircuit";
    case StageId::Tone:
      return "Tone";
    case StageId::Power:
      return "Power";
    case StageId::Noise:
      return "Noise";
    case StageId::Heterodyne:
      return "Heterodyne";
    case StageId::Speaker:
      return "Speaker";
    case StageId::Cabinet:
      return "Cabinet";
    case StageId::Room:
      return "Room";
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
  lifecycle.resetRuntime(*this);
  if (calibration.enabled) {
    resetCalibration();
  }
}

void Radio1938::process(float* samples, uint32_t frames) {
  if (!samples || frames == 0) return;
  if (graph.bypass) return;
  diagnostics.reset();
  RadioBlockControl block = runBlockPrepare(*this, graph.blockSteps, frames);
  for (uint32_t f = 0; f < frames; ++f) {
    float inL = samples[f * channels];
    float inR = (channels > 1) ? samples[f * channels + 1] : inL;
    float x = (channels > 1) ? 0.5f * (inL + inR) : inL;
    RadioSampleContext ctx{};
    ctx.block = &block;
    runSampleControl(*this, ctx, graph.sampleControlSteps);
    float y = runProgramPath(*this, x, ctx, graph.programPathSteps);
    y *= ctx.control.receptionGain;
    y = runPresentationPath(*this, y, ctx, graph.presentationPathSteps);
    for (int c = 0; c < channels; ++c) {
      samples[f * channels + c] = y;
    }
  }
  if (diagnostics.processedSamples > 0) {
    float invCount = 1.0f / static_cast<float>(diagnostics.processedSamples);
    diagnostics.finalLimiterDutyCycle =
        diagnostics.finalLimiterActiveSamples * invCount;
    diagnostics.finalLimiterAverageGainReduction =
        static_cast<float>(diagnostics.finalLimiterGainReductionSum * invCount);
    diagnostics.finalLimiterAverageGainReductionDb =
        static_cast<float>(diagnostics.finalLimiterGainReductionDbSum * invCount);
  }
  if (calibration.enabled) {
    updateCalibrationSnapshot(*this);
  }
}
