#include "radio.h"

#include <algorithm>
#include <cmath>

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

static inline float diodeColor(float x, float drop) {
  float mag = std::fabs(x);
  if (drop <= 0.0f) return x;
  float sign = (x >= 0.0f) ? 1.0f : -1.0f;
  if (mag <= drop) {
    return sign * mag * kRadioDiodeColorLeak;
  }
  float shaped = mag - drop;
  float curve = 1.0f + kRadioDiodeColorCurve * shaped;
  return sign * shaped * curve;
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

void NoiseHum::setFs(float newFs, float noiseBwHz) {
  fs = newFs;
  noiseLpHz = (noiseBwHz > 0.0f) ? noiseBwHz : noiseLpHz;
  float safeLp = std::clamp(noiseLpHz, noiseHpHz + 200.0f, fs * 0.45f);
  hp.setHighpass(fs, noiseHpHz, filterQ);
  lp.setLowpass(fs, safeLp, filterQ);
  crackleHp.setHighpass(fs, noiseHpHz, filterQ);
  crackleLp.setLowpass(fs, safeLp, filterQ);
  float lightningLpHz = std::clamp(safeLp * lightningLpScale,
                                   lightningHpHz + lightningLpMinGapHz,
                                   fs * 0.45f);
  lightningHp.setHighpass(fs, lightningHpHz, filterQ);
  lightningLp.setLowpass(fs, lightningLpHz, filterQ);
  float safeMotorLp = std::min(motorLpHz, fs * 0.45f);
  motorHp.setHighpass(fs, motorHpHz, filterQ);
  motorLp.setLowpass(fs, safeMotorLp, filterQ);
  hp.reset();
  lp.reset();
  crackleHp.reset();
  crackleLp.reset();
  lightningHp.reset();
  lightningLp.reset();
  motorHp.reset();
  motorLp.reset();
  scAtk = std::exp(-1.0f / (fs * (scAttackMs / 1000.0f)));
  scRel = std::exp(-1.0f / (fs * (scReleaseMs / 1000.0f)));
  crackleDecay = std::exp(-1.0f / (fs * (crackleDecayMs / 1000.0f)));
  lightningDecay = std::exp(-1.0f / (fs * (lightningDecayMs / 1000.0f)));
  motorDecay = std::exp(-1.0f / (fs * (motorDecayMs / 1000.0f)));
}

void NoiseHum::reset() {
  humPhase = 0.0f;
  scEnv = 0.0f;
  crackleEnv = 0.0f;
  lightningEnv = 0.0f;
  motorEnv = 0.0f;
  motorPhase = 0.0f;
  pinkFast = 0.0f;
  pinkSlow = 0.0f;
  brown = 0.0f;
  hissDrift = 0.0f;
  hissDriftSlow = 0.0f;
  hp.reset();
  lp.reset();
  crackleHp.reset();
  crackleLp.reset();
  lightningHp.reset();
  lightningLp.reset();
  motorHp.reset();
  motorLp.reset();
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

  float noiseScale =
      std::clamp(in.noiseAmp / noiseScaleRef, 0.0f, noiseScaleMax);

  float l = 0.0f;
  if (in.lightningRate > 0.0f && in.lightningAmp > 0.0f && fs > 0.0f) {
    float chance = in.lightningRate / fs;
    if (dist01(rng) < chance) {
      lightningEnv = 1.0f;
    }
    float raw = dist(rng) * lightningEnv;
    lightningEnv *= lightningDecay;
    raw = lightningHp.process(raw);
    raw = lightningLp.process(raw);
    l = raw * in.lightningAmp * noiseScale * burstMask;
  }

  float m = 0.0f;
  if (in.motorRate > 0.0f && in.motorAmp > 0.0f && fs > 0.0f) {
    float chance = in.motorRate / fs;
    if (dist01(rng) < chance) {
      motorEnv = 1.0f;
    }
    motorEnv *= motorDecay;
    motorPhase += kRadioTwoPi * (in.motorBuzzHz / fs);
    if (motorPhase > kRadioTwoPi) motorPhase -= kRadioTwoPi;
    float buzz = motorBuzzBase + motorBuzzDepth * std::sin(motorPhase);
    float raw = dist(rng) * motorEnv * buzz;
    raw = motorHp.process(raw);
    raw = motorLp.process(raw);
    m = raw * in.motorAmp * noiseScale * burstMask;
  }

  float h = 0.0f;
  if (in.humToneEnabled && in.humAmp > 0.0f && fs > 0.0f) {
    humPhase += kRadioTwoPi * (humHz / fs);
    if (humPhase > kRadioTwoPi) humPhase -= kRadioTwoPi;
    h = std::sin(humPhase) + humSecondHarmonicMix * std::sin(2.0f * humPhase);
    h *= in.humAmp * hissMask;
  }

  return n + c + l + m + h;
}

void AMDetector::init(float newFs, float newBw, float newTuneHz) {
  fs = newFs;
  osFs = fs * static_cast<float>(osFactor);
  carrierHz = std::min(initCarrierHz, fs * initCarrierMaxFraction);
  phaseStep = kRadioTwoPi * (carrierHz / osFs);
  tuneOffsetHz = newTuneHz;
  setBandwidth(newBw, newTuneHz);

  agcAtk = std::exp(-1.0f / (fs * (agcAttackMs / 1000.0f)));
  agcRel = std::exp(-1.0f / (fs * (agcReleaseMs / 1000.0f)));
  agcGainAtk = std::exp(-1.0f / (fs * (agcGainAttackMs / 1000.0f)));
  agcGainRel = std::exp(-1.0f / (fs * (agcGainReleaseMs / 1000.0f)));

  detChargeCoeff = std::exp(-1.0f / (osFs * (detChargeMs / 1000.0f)));
  detReleaseCoeff = std::exp(-1.0f / (osFs * (detReleaseMs / 1000.0f)));
  avcChargeCoeff = std::exp(-1.0f / (osFs * (avcChargeMs / 1000.0f)));
  avcReleaseCoeff = std::exp(-1.0f / (osFs * (avcReleaseMs / 1000.0f)));

  dcCoeff = std::exp(-1.0f / (fs * (dcMs / 1000.0f)));

  reset();
}

void AMDetector::setBandwidth(float newBw, float newTuneHz) {
  bwHz = newBw;
  osFs = fs * static_cast<float>(osFactor);
  phaseStep = kRadioTwoPi * (carrierHz / osFs);
  float halfBw = std::clamp(bwHz * ifHalfBwScale, ifHalfBwMinHz,
                            fs * ifHalfBwMaxFraction);
  tuneOffsetHz = std::clamp(newTuneHz, -halfBw, halfBw);
  float skewT =
      clampf(tuneOffsetHz / std::max(halfBw, 1.0f), -1.0f, 1.0f);
  float ifCenter = carrierHz + tuneOffsetHz;
  ifCenter = std::clamp(ifCenter, ifCenterMinHz + halfBw,
                        fs * ifMaxFraction - halfBw);
  float lowSpan = halfBw * (1.0f + ifSkewPositiveExpand * skewT -
                            ifSkewNegativeShrink * std::max(0.0f, -skewT));
  float highSpan = halfBw * (1.0f - ifSkewPositiveShrink * std::max(0.0f, skewT) +
                             ifSkewNegativeExpand * -std::min(0.0f, skewT));
  lowSpan = std::clamp(lowSpan, ifSpanClampMinHz, fs * ifSpanClampMaxFraction);
  highSpan = std::clamp(highSpan, ifSpanClampMinHz, fs * ifSpanClampMaxFraction);
  float ifLow = std::clamp(ifCenter - lowSpan, ifCenterMinHz, fs * ifGuardFraction);
  float ifHigh =
      std::clamp(ifCenter + highSpan, ifLow + ifSpanMinHz, fs * ifMaxFraction);
  ifHp1.setHighpass(osFs, ifLow, kRadioBiquadQ);
  ifHp2.setHighpass(osFs, ifLow, kRadioBiquadQ);
  ifLp1.setLowpass(osFs, ifHigh, kRadioBiquadQ);
  ifLp2.setLowpass(osFs, ifHigh, kRadioBiquadQ);

  float detLpHz = std::clamp(bwHz * detLpScale, detLpMinHz, fs * 0.45f);
  audioHp.setHighpass(fs, audioHpHz, kRadioBiquadQ);
  audioLp1.setLowpass(fs, detLpHz, kRadioBiquadQ);
}

void AMDetector::reset() {
  phase = 0.0f;
  agcEnv = 0.0f;
  agcGainDb = 0.0f;
  envelopeCap = 0.0f;
  avcCap = 0.0f;
  dcEnv = 0.0f;
  ifHp1.reset();
  ifHp2.reset();
  ifLp1.reset();
  ifLp2.reset();
  audioHp.reset();
  audioLp1.reset();
}

float AMDetector::process(const AMDetectorSampleInput& in) {
  float mod = std::clamp(in.signal * modIndex, -modulationClamp, modulationClamp);
  float mistuneT = clampf(std::fabs(tuneOffsetHz) /
                              std::max(1.0f, bwHz * mistuneNormDenomScale),
                          0.0f, 1.0f);
  float agcLin = db2lin(agcGainDb);
  float envAccum = 0.0f;
  float rectifiedAccum = 0.0f;
  float avcImpulsePeak = 0.0f;

  for (int i = 0; i < osFactor; ++i) {
    phase += phaseStep;
    if (phase > kRadioTwoPi) phase -= kRadioTwoPi;

    float carrier = std::sin(phase);
    float ifSample = (1.0f + mod) * carrier * carrierGain;
    if (in.ifNoiseAmp > 0.0f) {
      ifSample += dist(rng) * in.ifNoiseAmp;
    }

    ifSample = ifHp1.process(ifSample);
    ifSample = ifHp2.process(ifSample);
    ifSample = ifLp1.process(ifSample);
    ifSample = ifLp2.process(ifSample);

    float sensed = ifSample * agcLin;
    float rectified = std::max(0.0f, sensed - diodeDrop);
    rectified = rectified / (1.0f + detectorCompression * rectified);

    float detRelease = detReleaseCoeff +
                       (1.0f - detReleaseCoeff) *
                           (detReleaseMistuneMix * mistuneT);
    if (rectified > envelopeCap) {
      envelopeCap =
          detChargeCoeff * envelopeCap + (1.0f - detChargeCoeff) * rectified;
    } else {
      envelopeCap =
          detRelease * envelopeCap + (1.0f - detRelease) * rectified;
    }

    float avcImpulse = std::max(0.0f, rectified - envelopeCap);
    avcImpulsePeak = std::max(avcImpulsePeak, avcImpulse);

    float avcSense =
        envelopeCap + avcImpulse * (avcImpulseBase + avcImpulseMistune * mistuneT);
    if (avcSense > avcCap) {
      avcCap = avcChargeCoeff * avcCap + (1.0f - avcChargeCoeff) * avcSense;
    } else {
      avcCap = avcReleaseCoeff * avcCap + (1.0f - avcReleaseCoeff) * avcSense;
    }

    envAccum += envelopeCap;
    rectifiedAccum += rectified;
  }

  agcEnv = avcCap;
  float avcDb = lin2db(avcCap);
  float consonantPullDb =
      clampf(avcImpulsePeak * consonantPullScale, 0.0f, consonantPullMaxDb);
  float targetGainDb =
      std::clamp(agcTargetDb - avcDb - consonantPullDb -
                     mistuneGainPenaltyDb * mistuneT,
                 agcMinGainDb, agcMaxGainDb);
  if (targetGainDb < agcGainDb) {
    agcGainDb = agcGainAtk * agcGainDb + (1.0f - agcGainAtk) * targetGainDb;
  } else {
    agcGainDb = agcGainRel * agcGainDb + (1.0f - agcGainRel) * targetGainDb;
  }

  float env = envAccum / static_cast<float>(osFactor);
  float rectifiedAvg = rectifiedAccum / static_cast<float>(osFactor);
  float detected = env + detectorDetailMix * (rectifiedAvg - env);
  dcEnv = dcCoeff * dcEnv + (1.0f - dcCoeff) * detected;
  float out = detected - dcEnv;
  out = audioHp.process(out);
  out = audioLp1.process(out);
  out *= detGain * detectorMakeupGain;
  float overloadT = clampf((env - overloadThreshold) / overloadRange +
                               avcImpulsePeak * overloadImpulseScale,
                           0.0f, 1.0f);
  float negLimit = negLimitBase - negLimitOverloadDepth * overloadT -
                   negLimitMistuneDepth * mistuneT;
  float posLimit = posLimitBase - posLimitOverloadDepth * overloadT;
  negLimit = std::max(negLimitMin, negLimit);
  posLimit = std::max(negLimit + posLimitGuard, posLimit);
  if (out < -negLimit) {
    float excess = -out - negLimit;
    out = -(negLimit + excess / (1.0f + negOverloadSoftness * overloadT));
  }
  if (out > posLimit) {
    float excess = out - posLimit;
    out = posLimit + excess / (1.0f + posOverloadSoftness * overloadT);
  }
  return out;
}

void SpeakerSim::init(float fs) {
  boxResBass.setPeaking(fs, boxResBassHz, boxResBassQ, boxResBassGainDb);
  boxResLowMid.setPeaking(fs, boxResLowMidHz, boxResLowMidQ,
                          boxResLowMidGainDb);
  cabNasal.setPeaking(fs, cabNasalHz, cabNasalQ, cabNasalGainDb);
  panelRes.setPeaking(fs, panelResHz, panelResQ, panelResGainDb);
  hornPeak.setPeaking(fs, hornPeakHz, hornPeakQ, hornPeakGainDb);
  paperPeak.setPeaking(fs, paperPeakHz, paperPeakQ, paperPeakGainDb);
  coneDip.setPeaking(fs, coneDipHz, coneDipQ, coneDipGainDb);
  backWaveHp.setHighpass(fs, backWaveHpHz, filterQ);
  backWaveLp.setLowpass(fs, backWaveLpHz, filterQ);
  int delaySamples = std::max(
      2, static_cast<int>(std::lround(fs * (backWaveDelayMs / 1000.0f))));
  backWaveBuf.assign(static_cast<size_t>(delaySamples), 0.0f);
  backWaveIndex = 0;
}

void SpeakerSim::reset() {
  boxResBass.reset();
  boxResLowMid.reset();
  cabNasal.reset();
  panelRes.reset();
  hornPeak.reset();
  paperPeak.reset();
  coneDip.reset();
  backWaveHp.reset();
  backWaveLp.reset();
  backWaveIndex = 0;
  std::fill(backWaveBuf.begin(), backWaveBuf.end(), 0.0f);
}

float SpeakerSim::process(float x, bool& clipped) {
  float cone = boxResBass.process(x);
  cone = boxResLowMid.process(cone);
  float y = cone;
  if (!backWaveBuf.empty()) {
    float rear = backWaveBuf[static_cast<size_t>(backWaveIndex)];
    backWaveBuf[static_cast<size_t>(backWaveIndex)] = cone;
    backWaveIndex = (backWaveIndex + 1) % static_cast<int>(backWaveBuf.size());
    rear = backWaveHp.process(rear);
    rear = backWaveLp.process(rear);
    y -= rear * backWaveMix;
  }
  y = cabNasal.process(y);
  y = panelRes.process(y);
  y = hornPeak.process(y);
  y = paperPeak.process(y);
  y = coneDip.process(y);
  float biased = y + asymBias;
  float yd =
      std::tanh(drive * biased) / std::tanh(drive) -
      std::tanh(drive * asymBias) / std::tanh(drive);
  y = (1.0f - mix) * y + mix * yd;
  clipped = std::fabs(y) > limit;
  return softClip(y, limit);
}

float RadioTuningNode::applyFilters(Radio1938& radio, float tuneHz, float bwHz) {
  auto& tuning = radio.tuning;
  auto& frontEnd = radio.frontEnd;
  auto& power = radio.power;
  auto& speakerStage = radio.speakerStage;
  auto& adjacent = radio.adjacent;
  float safeBw = std::clamp(bwHz, tuning.safeBwMinHz, tuning.safeBwMaxHz);
  float tuneNorm =
      (safeBw > 0.0f) ? clampf(tuneHz / (safeBw * 0.5f), -1.0f, 1.0f) : 0.0f;
  tuning.tuneOffsetNorm = tuneNorm;
  float detuneT = std::clamp(std::fabs(tuneNorm), 0.0f, 1.0f);
  float tunedBw = std::clamp(
      safeBw * (1.0f - tuning.tunedBwMistuneDepth * detuneT), tuning.tunedBwMinHz,
      safeBw);
  float preBw =
      std::clamp(tunedBw * tuning.preBwScale, tunedBw, radio.sampleRate * 0.45f);
  float postBw =
      std::clamp(tunedBw * tuning.postBwScale, preBw, radio.sampleRate * 0.45f);

  frontEnd.preLpfIn.setLowpass(radio.sampleRate, preBw, kRadioBiquadQ);
  frontEnd.preLpfOut.setLowpass(radio.sampleRate, preBw, kRadioBiquadQ);
  power.postLpf.setLowpass(radio.sampleRate, postBw, kRadioBiquadQ);
  speakerStage.postLpf.setLowpass(radio.sampleRate, postBw, kRadioBiquadQ);

  float shift = 1.0f + tuneNorm * tuning.rippleShiftScale;
  float rippleLowHz = std::clamp(
      tuning.ifRippleLowHz * shift, tuning.ifRippleLowMinHz, tunedBw * 0.95f);
  float rippleHighHz = std::clamp(
      tuning.ifRippleHighHz * shift, tuning.ifRippleHighMinHz, tunedBw * 0.95f);
  frontEnd.ifRippleLow.setPeaking(radio.sampleRate, rippleLowHz, tuning.ifRippleLowQ,
                                  tuning.ifRippleLowGainDb);
  frontEnd.ifRippleHigh.setPeaking(radio.sampleRate, rippleHighHz,
                                   tuning.ifRippleHighQ,
                                   tuning.ifRippleHighGainDb);

  float tiltScale = 1.0f - tuning.tiltDetuneDepth * detuneT;
  float tiltHz = std::clamp(
      tuning.tiltHz * (1.0f + tuneNorm * tuning.tiltTuneScale) * tiltScale,
      tuning.tiltMinHz, tunedBw * 0.95f);
  frontEnd.ifTiltLp.setLowpass(radio.sampleRate, tiltHz, kRadioBiquadQ);

  float adjLpHz = std::clamp(tunedBw * tuning.adjLpScale, tuning.adjLpMinHz,
                             tunedBw * tuning.adjLpMaxScale);
  adjacent.lp.setLowpass(radio.sampleRate, adjLpHz, kRadioBiquadQ);

  tuning.tunedBw = tunedBw;
  return tunedBw;
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
  block.offT = std::fabs(block.tuneNorm);
  block.offHz = std::fabs(tuning.tuneSmoothedHz);
  block.cosmeticOffT = clampf((block.offT - tuning.mistuneCosmeticStart) /
                                  tuning.mistuneCosmeticRange,
                              0.0f, 1.0f);

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
  float receptionScale = reception.receptionBase +
                         reception.receptionOffTScale * block.offT +
                         reception.receptionFadeScale * fadeT;
  ctx.control.noiseScale *= receptionScale;
  ctx.control.noiseScale = std::clamp(ctx.control.noiseScale,
                                      reception.noiseScaleMin,
                                      reception.noiseScaleMax);
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
  frontEnd.ifRippleLow.reset();
  frontEnd.ifRippleHigh.reset();
  frontEnd.ifTiltLp.reset();
}

float RadioFrontEndNode::process(Radio1938& radio,
                                 float x,
                                 const RadioSampleContext& ctx) {
  auto& frontEnd = radio.frontEnd;
  auto& tuning = radio.tuning;
  const auto& block = *ctx.block;
  float y = frontEnd.hpf.process(x);
  y = frontEnd.preLpfIn.process(y);
  y = frontEnd.preLpfOut.process(y);
  y = frontEnd.ifRippleLow.process(y);
  y = frontEnd.ifRippleHigh.process(y);
  float low = frontEnd.ifTiltLp.process(y);
  float high = y - low;
  float tiltMix = frontEnd.ifTiltMix;
  if (block.tuneNorm != 0.0f) {
    float extra = block.cosmeticOffT * tuning.tuneTiltExtra;
    if (block.tuneNorm > 0.0f) {
      float highGain = std::max(0.0f, 1.0f - tiltMix - extra);
      return low + high * highGain;
    }
    float lowGain = std::max(0.0f, 1.0f - extra);
    float highGain = std::max(0.0f, 1.0f - tiltMix);
    return low * lowGain + high * highGain;
  }
  return low + high * (1.0f - tiltMix);
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
  adjacent.atk =
      std::exp(-1.0f / (radio.sampleRate * (adjacent.attackMs / 1000.0f)));
  adjacent.rel =
      std::exp(-1.0f / (radio.sampleRate * (adjacent.releaseMs / 1000.0f)));
}

void RadioAdjacentNode::reset(Radio1938& radio) {
  auto& adjacent = radio.adjacent;
  adjacent.phase = 0.0f;
  adjacent.env = 0.0f;
  adjacent.hp.reset();
  adjacent.lp.reset();
}

float RadioAdjacentNode::process(Radio1938& radio,
                                 float y,
                                 const RadioSampleContext& ctx) {
  auto& adjacent = radio.adjacent;
  const auto& block = *ctx.block;
  if (!(adjacent.mix > 0.0f && block.offT > 0.0f)) {
    return y;
  }

  float adjSource = y;
  adjacent.phase += kRadioTwoPi * (adjacent.beatHz / radio.sampleRate);
  if (adjacent.phase > kRadioTwoPi) adjacent.phase -= kRadioTwoPi;
  float mod = 1.0f - 0.5f * adjacent.modDepth +
              0.5f * adjacent.modDepth * std::sin(adjacent.phase);
  float adj = adjacent.hp.process(adjSource);
  float splatter = std::tanh(adj * adjacent.drive) - adj;
  float adjOut = adj + adjacent.splatterMix * splatter;
  adjOut = adjacent.lp.process(adjOut);
  adjOut *= mod;
  float adjAbs = std::fabs(adjOut);
  if (adjAbs > adjacent.env) {
    adjacent.env = adjacent.atk * adjacent.env + (1.0f - adjacent.atk) * adjAbs;
  } else {
    adjacent.env = adjacent.rel * adjacent.env + (1.0f - adjacent.rel) * adjAbs;
  }
  float adjGain =
      (adjacent.env > 1e-6f) ? (adjacent.target / adjacent.env) : adjacent.maxGain;
  adjGain = std::clamp(adjGain, adjacent.minGain, adjacent.maxGain);
  adjOut *= adjGain;
  float mix =
      std::clamp(adjacent.mix * (block.cosmeticOffT * block.cosmeticOffT), 0.0f,
                 adjacent.maxMix);
  return y + mix * adjOut;
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
  return diodeColor(y, demod.diodeColorDrop);
}

void RadioToneNode::init(Radio1938& radio, RadioInitContext&) {
  auto& tone = radio.tone;
  tone.midBoost.setPeaking(radio.sampleRate, tone.midBoostHz, tone.midBoostQ,
                           tone.midBoostGainDb);
  tone.lowMidDip.setPeaking(radio.sampleRate, tone.lowMidDipHz, tone.lowMidDipQ,
                            tone.lowMidDipGainDb);
  tone.presBoost.setPeaking(radio.sampleRate, tone.presBoostHz, tone.presBoostQ,
                            tone.presBoostGainDb);
  tone.lowLp.setLowpass(radio.sampleRate, tone.lowLpHz, kRadioBiquadQ);
  tone.highHp.setHighpass(radio.sampleRate, tone.highHpHz, kRadioBiquadQ);
  tone.comp.setFs(radio.sampleRate);
  tone.comp.thresholdDb = tone.compThresholdDb;
  tone.comp.ratio = tone.compRatio;
  tone.comp.setTimes(tone.compAttackMs, tone.compReleaseMs);
  tone.atk = std::exp(-1.0f / (radio.sampleRate * (tone.attackMs / 1000.0f)));
  tone.rel = std::exp(-1.0f / (radio.sampleRate * (tone.releaseMs / 1000.0f)));
}

void RadioToneNode::reset(Radio1938& radio) {
  auto& tone = radio.tone;
  tone.env = 0.0f;
  tone.midBoost.reset();
  tone.lowMidDip.reset();
  tone.presBoost.reset();
  tone.lowLp.reset();
  tone.highHp.reset();
  tone.comp.reset();
}

float RadioToneNode::process(Radio1938& radio,
                             float y,
                             const RadioSampleContext&) {
  auto& tone = radio.tone;
  float toneLevel = std::fabs(y);
  if (toneLevel > tone.env) {
    tone.env = tone.atk * tone.env + (1.0f - tone.atk) * toneLevel;
  } else {
    tone.env = tone.rel * tone.env + (1.0f - tone.rel) * toneLevel;
  }
  float loudT = clampf((tone.env - tone.loudnessEnvStart) / tone.loudnessEnvRange,
                       0.0f, 1.0f);
  float lowBand = tone.lowLp.process(y);
  float highBand = tone.highHp.process(y);
  float midBand = y - lowBand - highBand;
  float lowGain = tone.lowBaseGain + tone.lowGainDepth * loudT;
  float midGain = tone.midBaseGain + tone.midGainDepth * loudT;
  float highGain = tone.highBaseGain + tone.highGainDepth * loudT;
  y = lowBand * lowGain + midBand * midGain + highBand * highGain;
  y = tone.midBoost.process(y);
  y = tone.lowMidDip.process(y);
  y = tone.presBoost.process(y);
  y = tone.comp.process(y);
  return y * radio.globals.compMakeupGain;
}

void RadioPowerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& power = radio.power;
  power.sagAtk =
      std::exp(-1.0f / (radio.sampleRate * (power.sagAttackMs / 1000.0f)));
  power.sagRel =
      std::exp(-1.0f / (radio.sampleRate * (power.sagReleaseMs / 1000.0f)));
  power.atk = std::exp(-1.0f / (radio.sampleRate * (power.attackMs / 1000.0f)));
  power.rel = std::exp(-1.0f / (radio.sampleRate * (power.releaseMs / 1000.0f)));
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
  power.env = 0.0f;
  power.rectifierPhase = 0.0f;
  power.subharmonicPhase = 0.0f;
  power.satOsPrev = 0.0f;
  power.postLpf.reset();
  power.satOsLpIn.reset();
  power.satOsLpOut.reset();
  radio.runtime.powerSag = 0.0f;
}

float RadioPowerNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext&) {
  auto& power = radio.power;
  auto& runtime = radio.runtime;
  auto& noiseConfig = radio.noiseConfig;
  float powerLevel = std::fabs(y);
  if (powerLevel > power.env) {
    power.env = power.atk * power.env + (1.0f - power.atk) * powerLevel;
  } else {
    power.env = power.rel * power.env + (1.0f - power.rel) * powerLevel;
  }
  float powerT =
      clampf((power.env - power.powerEnvStart) / power.powerEnvRange, 0.0f, 1.0f);
  float rectHz = std::max(power.rectifierMinHz, noiseConfig.humHzDefault * 2.0f);
  power.rectifierPhase += kRadioTwoPi * (rectHz / radio.sampleRate);
  power.subharmonicPhase +=
      kRadioTwoPi * ((rectHz * power.rectifierSubharmonic) / radio.sampleRate);
  if (power.rectifierPhase > kRadioTwoPi) {
    power.rectifierPhase -= kRadioTwoPi;
  }
  if (power.subharmonicPhase > kRadioTwoPi) {
    power.subharmonicPhase -= kRadioTwoPi;
  }
  float ripple =
      std::sin(power.rectifierPhase) +
      power.rippleSecondHarmonicMix * std::sin(2.0f * power.rectifierPhase) +
      power.rippleSubharmonicMix * std::sin(power.subharmonicPhase);
  float powerGain =
      1.0f - power.gainSagPerPower * powerT +
      ripple * power.rippleDepth *
          (power.rippleGainBase + power.rippleGainDepth * powerT);
  powerGain = std::clamp(powerGain, power.gainMin, power.gainMax);
  float powerBias =
      ripple * power.biasDepth * (power.biasBase + power.biasPowerDepth * powerT);
  y = y * powerGain + powerBias;
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
  float level = std::fabs(y);
  if (level > power.sagEnv) {
    power.sagEnv = power.sagAtk * power.sagEnv + (1.0f - power.sagAtk) * level;
  } else {
    power.sagEnv = power.sagRel * power.sagEnv + (1.0f - power.sagRel) * level;
  }
  float sagT = clampf((power.sagEnv - power.sagStart) /
                          (power.sagEnd - power.sagStart),
                      0.0f, 1.0f);
  runtime.powerSag = power.sagEnv;
  return y * (1.0f - power.sagDepth * sagT);
}

void RadioInterferenceDerivedNode::init(Radio1938& radio, RadioInitContext&) {
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseDerived = radio.noiseDerived;
  if (radio.noiseWeight <= 0.0f) {
    noiseDerived.baseNoiseAmp = 0.0f;
    noiseDerived.baseCrackleAmp = 0.0f;
    noiseDerived.baseLightningAmp = 0.0f;
    noiseDerived.baseMotorAmp = 0.0f;
    noiseDerived.baseHumAmp = 0.0f;
    noiseDerived.crackleRate = 0.0f;
    noiseDerived.lightningRate = 0.0f;
    noiseDerived.motorRate = 0.0f;
    return;
  }

  float scale = std::clamp(radio.noiseWeight / noiseConfig.noiseWeightRef, 0.0f,
                           noiseConfig.noiseWeightScaleMax);
  noiseDerived.baseNoiseAmp = radio.noiseWeight;
  noiseDerived.baseCrackleAmp = noiseConfig.crackleAmpScale * scale;
  noiseDerived.baseLightningAmp = noiseConfig.lightningAmpScale * scale;
  noiseDerived.baseMotorAmp = noiseConfig.motorAmpScale * scale;
  noiseDerived.baseHumAmp = noiseConfig.humAmpScale * scale;
  noiseDerived.crackleRate = noiseConfig.crackleRateScale * scale;
  noiseDerived.lightningRate = noiseConfig.lightningRateScale * scale;
  noiseDerived.motorRate = noiseConfig.motorRateScale * scale;
}

void RadioInterferenceDerivedNode::reset(Radio1938&) {}

void RadioInterferenceDerivedNode::update(Radio1938& radio,
                                          RadioSampleContext& ctx) {
  auto& heterodyne = radio.heterodyne;
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseDerived = radio.noiseDerived;
  auto& runtime = radio.runtime;
  float postNoiseScale =
      std::max(ctx.control.noiseScale * radio.globals.postNoiseMix, 0.06f);
  ctx.derived.demodIfNoiseAmp =
      noiseDerived.baseNoiseAmp * ctx.control.noiseScale * radio.globals.ifNoiseMix;
  ctx.derived.noiseAmp =
      std::max(noiseDerived.baseNoiseAmp * postNoiseScale,
               radio.globals.noiseFloorAmp);
  ctx.derived.crackleAmp = noiseDerived.baseCrackleAmp * postNoiseScale;
  ctx.derived.crackleRate = noiseDerived.crackleRate;
  ctx.derived.lightningAmp = noiseDerived.baseLightningAmp * postNoiseScale;
  ctx.derived.lightningRate = noiseDerived.lightningRate;
  ctx.derived.motorAmp = noiseDerived.baseMotorAmp * postNoiseScale;
  ctx.derived.motorRate = noiseDerived.motorRate;
  ctx.derived.motorBuzzHz = noiseConfig.motorBuzzHz;
  ctx.derived.humAmp = noiseDerived.baseHumAmp * postNoiseScale;
  ctx.derived.humToneEnabled = noiseConfig.enableHumTone;
  float quietT =
      clampf((runtime.powerSag - heterodyne.gateStart) /
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
              (heterodyne.quietNoiseBase +
               heterodyne.quietNoiseDepth * ctx.control.noiseScale) *
              ctx.derived.quieting;
  amp *= gate;
  amp *= block.cosmeticOffT;
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
  noiseIn.lightningAmp = ctx.derived.lightningAmp;
  noiseIn.lightningRate = ctx.derived.lightningRate;
  noiseIn.motorAmp = ctx.derived.motorAmp;
  noiseIn.motorRate = ctx.derived.motorRate;
  noiseIn.motorBuzzHz = ctx.derived.motorBuzzHz;
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
  speakerStage.postLpf.reset();
  speakerStage.osLpIn.reset();
  speakerStage.osLpOut.reset();
  speakerStage.speaker.reset();
}

float RadioSpeakerNode::process(Radio1938& radio,
                                float y,
                                const RadioSampleContext&) {
  auto& speakerStage = radio.speakerStage;
  y *= radio.makeupGain;
  y = processOversampled2x(y, speakerStage.osPrev, speakerStage.osLpIn,
                           speakerStage.osLpOut, [&](float v) {
                             bool clipped = false;
                             float out = speakerStage.speaker.process(v, clipped);
                             if (clipped) radio.diagnostics.markSpeakerClip();
                             return out;
                           });
  return speakerStage.postLpf.process(y);
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
  limiter.releaseCoeff =
      std::exp(-1.0f / (radio.sampleRate * (limiter.releaseMs / 1000.0f)));
  float osFs = radio.sampleRate * radio.globals.oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  limiter.osLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  limiter.osLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
}

void RadioFinalLimiterNode::reset(Radio1938& radio) {
  auto& limiter = radio.finalLimiter;
  limiter.gain = 1.0f;
  limiter.osPrev = 0.0f;
  limiter.osObservedPeak = 0.0f;
  limiter.osLpIn.reset();
  limiter.osLpOut.reset();
}

float RadioFinalLimiterNode::process(Radio1938& radio,
                                     float y,
                                     const RadioSampleContext&) {
  auto& limiter = radio.finalLimiter;
  if (!limiter.enabled) return y;

  float peak = 0.0f;
  float mid = 0.5f * (limiter.osPrev + y);
  float s0 = limiter.osLpIn.process(mid);
  s0 = limiter.osLpOut.process(s0);
  peak = std::max(peak, std::fabs(s0));

  float s1 = limiter.osLpIn.process(y);
  s1 = limiter.osLpOut.process(s1);
  peak = std::max(peak, std::fabs(s1));

  limiter.osPrev = y;
  limiter.osObservedPeak = peak;

  float targetGain = 1.0f;
  if (peak > limiter.threshold && peak > 1e-9f) {
    targetGain = limiter.threshold / peak;
  }

  if (targetGain < limiter.gain) {
    limiter.gain = targetGain;
  } else {
    limiter.gain =
        limiter.releaseCoeff * limiter.gain +
        (1.0f - limiter.releaseCoeff) * targetGain;
  }

  radio.diagnostics.finalLimiterPeak =
      std::max(radio.diagnostics.finalLimiterPeak, peak);
  radio.diagnostics.finalLimiterGain =
      std::min(radio.diagnostics.finalLimiterGain, limiter.gain);
  if (limiter.gain < 0.999f) {
    radio.diagnostics.finalLimiterActive = true;
  }

  return y * limiter.gain;
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
                                return softClip(v, t);
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
    y = step.process(radio, y, ctx);
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
    y = step.process(radio, y, ctx);
  }
  return y;
}

static void applyMid30sDocumentaryPreset(Radio1938& radio) {
  radio.makeupGain = 1.0f;

  radio.globals.ifNoiseMix = 0.28f;
  radio.globals.postNoiseMix = 0.17f;
  radio.globals.noiseFloorAmp = 0.0032f;
  radio.globals.compMakeupGain = 1.08f;
  radio.globals.enableAutoLevel = false;
  radio.globals.autoTargetDb = -21.0f;
  radio.globals.autoMaxBoostDb = 2.5f;

  radio.tuning.safeBwMinHz = 3400.0f;
  radio.tuning.safeBwMaxHz = 4000.0f;
  radio.tuning.tunedBwMistuneDepth = 0.34f;
  radio.tuning.preBwScale = 1.00f;
  radio.tuning.postBwScale = 1.04f;

  radio.frontEnd.ifTiltMix = 0.14f;

  radio.multipath.mix = 0.0f;
  radio.multipath.depth = 0.0f;

  radio.adjacent.mix = 0.008f;

  radio.demod.diodeColorDrop = 0.008f;

  radio.tone.midBoostHz = 1400.0f;
  radio.tone.midBoostGainDb = 1.2f;
  radio.tone.lowMidDipGainDb = -0.5f;
  radio.tone.presBoostGainDb = -1.2f;
  radio.tone.compThresholdDb = -12.0f;
  radio.tone.compRatio = 1.25f;
  radio.tone.compAttackMs = 12.0f;
  radio.tone.compReleaseMs = 220.0f;
  radio.tone.lowBaseGain = 0.76f;
  radio.tone.lowGainDepth = 0.10f;
  radio.tone.midBaseGain = 0.86f;
  radio.tone.midGainDepth = 0.10f;
  radio.tone.highBaseGain = 0.33f;
  radio.tone.highGainDepth = 0.14f;

  radio.power.satMix = 0.21f;

  radio.noiseConfig.enableHumTone = true;
  radio.noiseConfig.humAmpScale = 0.0022f;
  radio.noiseConfig.crackleAmpScale = 0.0075f;
  radio.noiseConfig.lightningAmpScale = 0.014f;
  radio.noiseConfig.motorAmpScale = 0.0038f;

  radio.speakerStage.drive = 0.66f;
  radio.speakerStage.speaker.mix = 0.14f;
  radio.speakerStage.speaker.backWaveMix = 0.03f;
  radio.speakerStage.speaker.boxResBassGainDb = 0.35f;
  radio.speakerStage.speaker.boxResLowMidGainDb = 0.10f;
  radio.speakerStage.speaker.panelResGainDb = 0.05f;
  radio.speakerStage.speaker.hornPeakGainDb = 0.12f;
  radio.speakerStage.speaker.paperPeakGainDb = 0.02f;
  radio.speakerStage.speaker.coneDipGainDb = -0.85f;

  radio.room.enableEarlyReflections = false;
  radio.room.mix = 0.0f;
  radio.room.enableTail = false;
  radio.room.tailMix = 0.0f;
}

std::string_view Radio1938::presetName(Preset preset) {
  switch (preset) {
    case Preset::Mid30sDocumentary:
      return "mid30s_documentary";
  }
  return "mid30s_documentary";
}

bool Radio1938::applyPreset(std::string_view presetNameValue) {
  if (presetNameValue == "mid30s_documentary") {
    applyPreset(Preset::Mid30sDocumentary);
    return true;
  }
  return false;
}

void Radio1938::applyPreset(Preset presetValue) {
  preset = presetValue;
  switch (presetValue) {
    case Preset::Mid30sDocumentary:
      applyMid30sDocumentaryPreset(*this);
      break;
  }
  if (!initialized) return;
  init(channels, sampleRate, bwHz, noiseWeight);
}

void Radio1938::init(int ch, float sr, float bw, float noise) {
  channels = std::max(1, ch);
  sampleRate = sr;
  bwHz = bw;
  noiseWeight = noise;
  switch (preset) {
    case Preset::Mid30sDocumentary:
      applyMid30sDocumentaryPreset(*this);
      break;
  }
  RadioInitContext initCtx{};
  lifecycle.configure(*this, initCtx);
  lifecycle.allocate(*this, initCtx);
  lifecycle.initializeDependentState(*this, initCtx);
  initialized = true;
  reset();
}

void Radio1938::reset() {
  diagnostics.reset();
  runtime.reset();
  lifecycle.resetRuntime(*this);
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
}
