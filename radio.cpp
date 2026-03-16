#include "radio.h"

#include <algorithm>
#include <cmath>

using LifecycleStep = Radio1938::LifecycleStep;
using PipelineStep = Radio1938::PipelineStep;

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

float NoiseHum::process(float programSample) {
  float programAbs = std::fabs(programSample);
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
  n *= noiseAmp * hissMask;

  float c = 0.0f;
  if (crackleRate > 0.0f && crackleAmp > 0.0f && fs > 0.0f) {
    float chance = crackleRate / fs;
    if (dist01(rng) < chance) {
      crackleEnv = 1.0f;
    }
    float raw = dist(rng) * crackleEnv;
    crackleEnv *= crackleDecay;
    raw = crackleHp.process(raw);
    raw = crackleLp.process(raw);
    c = raw * crackleAmp * burstMask;
  }

  float noiseScale = std::clamp(noiseAmp / noiseScaleRef, 0.0f, noiseScaleMax);

  float l = 0.0f;
  if (lightningRate > 0.0f && lightningAmp > 0.0f && fs > 0.0f) {
    float chance = lightningRate / fs;
    if (dist01(rng) < chance) {
      lightningEnv = 1.0f;
    }
    float raw = dist(rng) * lightningEnv;
    lightningEnv *= lightningDecay;
    raw = lightningHp.process(raw);
    raw = lightningLp.process(raw);
    l = raw * lightningAmp * noiseScale * burstMask;
  }

  float m = 0.0f;
  if (motorRate > 0.0f && motorAmp > 0.0f && fs > 0.0f) {
    float chance = motorRate / fs;
    if (dist01(rng) < chance) {
      motorEnv = 1.0f;
    }
    motorEnv *= motorDecay;
    motorPhase += kRadioTwoPi * (motorBuzzHz / fs);
    if (motorPhase > kRadioTwoPi) motorPhase -= kRadioTwoPi;
    float buzz = motorBuzzBase + motorBuzzDepth * std::sin(motorPhase);
    float raw = dist(rng) * motorEnv * buzz;
    raw = motorHp.process(raw);
    raw = motorLp.process(raw);
    m = raw * motorAmp * noiseScale * burstMask;
  }

  float h = 0.0f;
  if (humToneEnabled && humAmp > 0.0f && fs > 0.0f) {
    humPhase += kRadioTwoPi * (humHz / fs);
    if (humPhase > kRadioTwoPi) humPhase -= kRadioTwoPi;
    h = std::sin(humPhase) + humSecondHarmonicMix * std::sin(2.0f * humPhase);
    h *= humAmp * hissMask;
  }

  return n + c + l + m + h;
}

void AMDetector::init(float newFs, float newBw, float newTuneHz) {
  fs = newFs;
  carrierHz = std::min(initCarrierHz, fs * initCarrierMaxFraction);
  tuneOffsetHz = newTuneHz;
  setBandwidth(newBw, newTuneHz);

  agcAtk = std::exp(-1.0f / (fs * (agcAttackMs / 1000.0f)));
  agcRel = std::exp(-1.0f / (fs * (agcReleaseMs / 1000.0f)));
  agcGainAtk = std::exp(-1.0f / (fs * (agcGainAttackMs / 1000.0f)));
  agcGainRel = std::exp(-1.0f / (fs * (agcGainReleaseMs / 1000.0f)));

  detChargeCoeff = std::exp(-1.0f / (fs * (detChargeMs / 1000.0f)));
  detReleaseCoeff = std::exp(-1.0f / (fs * (detReleaseMs / 1000.0f)));
  avcChargeCoeff = std::exp(-1.0f / (fs * (avcChargeMs / 1000.0f)));
  avcReleaseCoeff = std::exp(-1.0f / (fs * (avcReleaseMs / 1000.0f)));

  dcCoeff = std::exp(-1.0f / (fs * (dcMs / 1000.0f)));

  reset();
}

void AMDetector::setBandwidth(float newBw, float newTuneHz) {
  bwHz = newBw;
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
  ifHpIn.setHighpass(fs, ifLow, kRadioBiquadQ);
  ifHpOut.setHighpass(fs, ifLow, kRadioBiquadQ);
  ifLpIn.setLowpass(fs, ifHigh, kRadioBiquadQ);
  ifLpOut.setLowpass(fs, ifHigh, kRadioBiquadQ);

  float detLpHz = std::clamp(bwHz * detLpScale, detLpMinHz, fs * 0.45f);
  detAudioLpIn.setLowpass(fs, detLpHz, kRadioBiquadQ);
  detAudioLpOut.setLowpass(fs, detLpHz, kRadioBiquadQ);
  detAudioRippleLp.setLowpass(fs, detLpHz, kRadioBiquadQ);
  detQuadratureLpIn.setLowpass(fs, detLpHz, kRadioBiquadQ);
  detQuadratureLpOut.setLowpass(fs, detLpHz, kRadioBiquadQ);
  detQuadratureRippleLp.setLowpass(fs, detLpHz, kRadioBiquadQ);
}

void AMDetector::reset() {
  phase = 0.0f;
  rxPhase = 0.0f;
  agcEnv = 0.0f;
  agcGainDb = 0.0f;
  detectorCap = 0.0f;
  avcCap = 0.0f;
  dcEnv = 0.0f;
  ifNoiseAmp = 0.0f;
  ifHpIn.reset();
  ifHpOut.reset();
  ifLpIn.reset();
  ifLpOut.reset();
  detAudioLpIn.reset();
  detAudioLpOut.reset();
  detAudioRippleLp.reset();
  detQuadratureLpIn.reset();
  detQuadratureLpOut.reset();
  detQuadratureRippleLp.reset();
}

float AMDetector::process(float x) {
  float mod = std::clamp(x * modIndex, -modulationClamp, modulationClamp);
  float mistuneT = clampf(std::fabs(tuneOffsetHz) /
                              std::max(1.0f, bwHz * mistuneNormDenomScale),
                          0.0f, 1.0f);
  phase += kRadioTwoPi * (carrierHz / fs);
  if (phase > kRadioTwoPi) phase -= kRadioTwoPi;
  float rxCarrierHz =
      std::clamp(carrierHz + tuneOffsetHz, ifCenterMinHz, fs * ifGuardFraction);
  rxPhase += kRadioTwoPi * (rxCarrierHz / fs);
  if (rxPhase > kRadioTwoPi) rxPhase -= kRadioTwoPi;
  float carrier = std::sin(phase);
  float ifSample = (1.0f + mod) * carrier * carrierGain;
  if (ifNoiseAmp > 0.0f) {
    ifSample += dist(rng) * ifNoiseAmp;
  }

  ifSample = ifHpIn.process(ifSample);
  ifSample = ifHpOut.process(ifSample);
  ifSample = ifLpIn.process(ifSample);
  ifSample = ifLpOut.process(ifSample);

  float ifAgc = ifSample * db2lin(agcGainDb);
  float rectified = std::max(0.0f, ifAgc - diodeDrop);
  float detectorIn = rectified / (1.0f + detectorCompression * rectified);
  float detRelease =
      detReleaseCoeff + (1.0f - detReleaseCoeff) * (detReleaseMistuneMix * mistuneT);
  if (detectorIn > detectorCap) {
    detectorCap =
        detChargeCoeff * detectorCap + (1.0f - detChargeCoeff) * detectorIn;
  } else {
    detectorCap = detRelease * detectorCap + (1.0f - detRelease) * detectorIn;
  }

  float avcImpulse = std::max(0.0f, detectorIn - detectorCap);
  float avcSense =
      detectorCap + avcImpulse * (avcImpulseBase + avcImpulseMistune * mistuneT);
  if (avcSense > avcCap) {
    avcCap = avcChargeCoeff * avcCap + (1.0f - avcChargeCoeff) * avcSense;
  } else {
    avcCap = avcReleaseCoeff * avcCap + (1.0f - avcReleaseCoeff) * avcSense;
  }
  agcEnv = avcCap;
  float avcDb = lin2db(avcCap);
  float consonantPullDb =
      clampf(avcImpulse * consonantPullScale, 0.0f, consonantPullMaxDb);
  float targetGainDb =
      std::clamp(agcTargetDb - avcDb - consonantPullDb -
                     mistuneGainPenaltyDb * mistuneT,
                 agcMinGainDb, agcMaxGainDb);
  if (targetGainDb < agcGainDb) {
    agcGainDb = agcGainAtk * agcGainDb + (1.0f - agcGainAtk) * targetGainDb;
  } else {
    agcGainDb = agcGainRel * agcGainDb + (1.0f - agcGainRel) * targetGainDb;
  }

  float env = 0.0f;
  if (mode == Mode::Envelope) {
    float audio = detectorCap;
    dcEnv = dcCoeff * dcEnv + (1.0f - dcCoeff) * audio;
    env = audio - dcEnv;
    env = detAudioLpIn.process(env);
    env = detAudioLpOut.process(env);
    float detectorRipple = detAudioRippleLp.process(rectified - detectorCap);
    env += detectorRipple *
           (envelopeRippleBase + envelopeRippleMistune * mistuneT);
    return env * detGain;
  } else {
    float carrierI = std::sin(rxPhase);
    float carrierQ = std::cos(rxPhase);
    float mixI = ifAgc * carrierI;
    float mixQ = ifAgc * carrierQ;
    float i = detAudioLpIn.process(mixI);
    i = detAudioLpOut.process(i);
    float q = detQuadratureLpIn.process(mixQ);
    q = detQuadratureLpOut.process(q);
    env = std::sqrt(i * i + q * q);
    env *= iqLevelComp;  // Make up I/Q level loss.
    if (diodeDrop > 0.0f) {
      env = std::max(0.0f, env - diodeDrop);
    }
  }
  dcEnv = dcCoeff * dcEnv + (1.0f - dcCoeff) * env;
  float out = (env - dcEnv) * detGain;
  float overloadT = clampf((detectorIn - overloadThreshold) / overloadRange +
                               avcImpulse * overloadImpulseScale,
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
  clipTriggered = false;
}

float SpeakerSim::process(float x) {
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
  if (std::fabs(y) > limit) {
    clipTriggered = true;
  }
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

void RadioTuningNode::prepareBlock(Radio1938& radio,
                                   RadioFrameContext& ctx,
                                   uint32_t frames) {
  auto& tuning = radio.tuning;
  auto& demod = radio.demod;
  auto& noise = radio.noise;
  ctx.sampleRate = radio.sampleRate;
  float rate = std::max(1.0f, radio.sampleRate);
  float tick =
      1.0f - std::exp(-static_cast<float>(frames) / (rate * tuning.smoothTau));
  tuning.tuneSmoothedHz += tick * (tuning.tuneOffsetHz - tuning.tuneSmoothedHz);
  tuning.bwSmoothedHz += tick * (radio.bwHz - tuning.bwSmoothedHz);

  float safeBw =
      std::clamp(tuning.bwSmoothedHz, tuning.safeBwMinHz, tuning.safeBwMaxHz);
  float bwHalf = 0.5f * std::max(1.0f, safeBw);
  ctx.tuneNorm = clampf(tuning.tuneSmoothedHz / bwHalf, -1.0f, 1.0f);
  ctx.offT = std::fabs(ctx.tuneNorm);
  ctx.offHz = std::fabs(tuning.tuneSmoothedHz);
  ctx.cosmeticOffT = clampf((ctx.offT - tuning.mistuneCosmeticStart) /
                                tuning.mistuneCosmeticRange,
                            0.0f, 1.0f);

  if (std::fabs(tuning.tuneSmoothedHz - tuning.tuneAppliedHz) > tuning.updateEps ||
      std::fabs(tuning.bwSmoothedHz - tuning.bwAppliedHz) > tuning.updateEps) {
    float tunedBw = applyFilters(radio, tuning.tuneSmoothedHz, tuning.bwSmoothedHz);
    tuning.tuneAppliedHz = tuning.tuneSmoothedHz;
    tuning.bwAppliedHz = tuning.bwSmoothedHz;
    demod.am.setBandwidth(tunedBw, tuning.tuneSmoothedHz);
    noise.hum.setFs(radio.sampleRate, tunedBw);
  }
}

float RadioTuningNode::process(Radio1938&, float y, RadioFrameContext&) {
  return y;
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

void RadioInputNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioInputNode::process(Radio1938& radio, float x, RadioFrameContext&) {
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

void RadioReceptionNode::init(Radio1938&, RadioInitContext&) {}

void RadioReceptionNode::reset(Radio1938& radio) {
  auto& reception = radio.reception;
  reception.fadePhaseFast = 0.0f;
  reception.fadePhaseSlow = 0.0f;
  reception.noisePhaseFast = 0.0f;
  reception.noisePhaseSlow = 0.0f;
}

void RadioReceptionNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioReceptionNode::process(Radio1938& radio,
                                  float y,
                                  RadioFrameContext& ctx) {
  auto& reception = radio.reception;

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
  ctx.fade = std::clamp(1.0f + reception.fadeDepth * fadeLfo, reception.fadeMin,
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
  ctx.noiseScale = 1.0f + reception.noiseDepth * noiseLfo;
  float fadeT =
      clampf((1.0f - ctx.fade) / (1.0f - reception.fadeMin), 0.0f, 1.0f);
  float receptionScale = reception.receptionBase +
                         reception.receptionOffTScale * ctx.offT +
                         reception.receptionFadeScale * fadeT;
  ctx.noiseScale *= receptionScale;
  ctx.noiseScale =
      std::clamp(ctx.noiseScale, reception.noiseScaleMin, reception.noiseScaleMax);
  return y;
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

void RadioFrontEndNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioFrontEndNode::process(Radio1938& radio,
                                 float x,
                                 RadioFrameContext& ctx) {
  auto& frontEnd = radio.frontEnd;
  auto& tuning = radio.tuning;
  float y = frontEnd.hpf.process(x);
  y = frontEnd.preLpfIn.process(y);
  y = frontEnd.preLpfOut.process(y);
  y = frontEnd.ifRippleLow.process(y);
  y = frontEnd.ifRippleHigh.process(y);
  float low = frontEnd.ifTiltLp.process(y);
  float high = y - low;
  float tiltMix = frontEnd.ifTiltMix;
  if (ctx.tuneNorm != 0.0f) {
    float extra = ctx.cosmeticOffT * tuning.tuneTiltExtra;
    if (ctx.tuneNorm > 0.0f) {
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

void RadioMultipathNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioMultipathNode::process(Radio1938& radio,
                                  float y,
                                  RadioFrameContext&) {
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

void RadioDetuneNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioDetuneNode::process(Radio1938& radio, float y, RadioFrameContext&) {
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

void RadioAdjacentNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioAdjacentNode::process(Radio1938& radio,
                                 float y,
                                 RadioFrameContext& ctx) {
  auto& adjacent = radio.adjacent;
  if (!(adjacent.mix > 0.0f && ctx.offT > 0.0f)) {
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
      std::clamp(adjacent.mix * (ctx.cosmeticOffT * ctx.cosmeticOffT), 0.0f,
                 adjacent.maxMix);
  return y + mix * adjOut;
}

void RadioDemodNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& demod = radio.demod;
  // Keep the detector on coherent demod until the synthetic IF path is
  // oversampled high enough for a whistle-free envelope detector.
  demod.am.mode = AMDetector::Mode::Iq;
  demod.am.init(radio.sampleRate, initCtx.tunedBw, radio.tuning.tuneOffsetHz);
}

void RadioDemodNode::reset(Radio1938& radio) { radio.demod.am.reset(); }

void RadioDemodNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioDemodNode::process(Radio1938& radio,
                              float y,
                              RadioFrameContext& ctx) {
  auto& demod = radio.demod;
  auto& artifacts = radio.artifacts;
  demod.am.ifNoiseAmp =
      artifacts.noiseBase * ctx.noiseScale * radio.globals.ifNoiseMix;
  y = demod.am.process(y);
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

void RadioToneNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioToneNode::process(Radio1938& radio, float y, RadioFrameContext&) {
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
}

void RadioPowerNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioPowerNode::process(Radio1938& radio, float y, RadioFrameContext&) {
  auto& power = radio.power;
  auto& noise = radio.noise;
  float powerLevel = std::fabs(y);
  if (powerLevel > power.env) {
    power.env = power.atk * power.env + (1.0f - power.atk) * powerLevel;
  } else {
    power.env = power.rel * power.env + (1.0f - power.rel) * powerLevel;
  }
  float powerT =
      clampf((power.env - power.powerEnvStart) / power.powerEnvRange, 0.0f, 1.0f);
  float rectHz = std::max(power.rectifierMinHz, noise.hum.humHz * 2.0f);
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
                               radio.clipTriggered = true;
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
  return y * (1.0f - power.sagDepth * sagT);
}

void RadioArtifactNode::init(Radio1938& radio, RadioInitContext&) {
  auto& artifacts = radio.artifacts;
  auto& noise = radio.noise;
  artifacts.noiseBase = noise.hum.noiseAmp;
  artifacts.crackleBase = noise.hum.crackleAmp;
  artifacts.lightningBase = noise.hum.lightningAmp;
  artifacts.motorBase = noise.hum.motorAmp;
  artifacts.humBase = noise.hum.humAmp;
  artifacts.heteroBaseScale =
      (radio.noiseWeight > 0.0f)
          ? std::clamp(radio.noiseWeight / artifacts.noiseWeightRef,
                       artifacts.heteroBaseScaleMin, artifacts.heteroBaseScaleMax)
          : 0.0f;
}

void RadioArtifactNode::reset(Radio1938& radio) {
  radio.artifacts.heteroPhase = 0.0f;
  radio.artifacts.heteroDriftPhase = 0.0f;
}

void RadioArtifactNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioArtifactNode::process(Radio1938& radio,
                                 float y,
                                 RadioFrameContext& ctx) {
  auto& artifacts = radio.artifacts;
  auto& noise = radio.noise;
  auto& power = radio.power;

  y *= ctx.fade;

  float postNoiseScale =
      std::max(ctx.noiseScale * radio.globals.postNoiseMix, 0.06f);
  noise.hum.noiseAmp =
      std::max(artifacts.noiseBase * postNoiseScale, radio.globals.noiseFloorAmp);
  noise.hum.crackleAmp = artifacts.crackleBase * postNoiseScale;
  noise.hum.lightningAmp = artifacts.lightningBase * postNoiseScale;
  noise.hum.motorAmp = artifacts.motorBase * postNoiseScale;
  noise.hum.humAmp = artifacts.humBase * postNoiseScale;

  if (!(artifacts.enableHeterodyneWhine && artifacts.heteroDepth > 0.0f &&
        artifacts.heteroBaseScale > 0.0f)) {
    return y;
  }

  if (ctx.offHz < artifacts.heteroGateHz) return y;

  artifacts.heteroDriftPhase +=
      kRadioTwoPi * (artifacts.heteroDriftHz / radio.sampleRate);
  if (artifacts.heteroDriftPhase > kRadioTwoPi) {
    artifacts.heteroDriftPhase -= kRadioTwoPi;
  }
  float drift =
      1.0f + artifacts.heteroDriftDepth * std::sin(artifacts.heteroDriftPhase);
  float heteroHz = std::min(ctx.offHz, artifacts.heteroMaxHz) * drift;
  artifacts.heteroPhase += kRadioTwoPi * (heteroHz / radio.sampleRate);
  if (artifacts.heteroPhase > kRadioTwoPi) artifacts.heteroPhase -= kRadioTwoPi;
  float hetero = std::sin(artifacts.heteroPhase);
  float quietT =
      clampf((power.sagEnv - artifacts.heteroGateStart) /
                 std::max(1e-6f, artifacts.heteroGateEnd - artifacts.heteroGateStart),
             0.0f, 1.0f);
  float quiet = 1.0f - quietT;
  quiet *= quiet;
  float gate = clampf((ctx.offHz - artifacts.heteroGateHz) /
                          (artifacts.heteroMaxHz - artifacts.heteroGateHz),
                      0.0f, 1.0f);
  gate *= gate;
  float amp = artifacts.heteroDepth * artifacts.heteroBaseScale *
              (artifacts.quietNoiseBase + artifacts.quietNoiseDepth * ctx.noiseScale) *
              quiet;
  amp *= gate;
  amp *= ctx.cosmeticOffT;
  return y + hetero * amp;
}

void RadioNoiseNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& noise = radio.noise;
  noise.hum.setFs(radio.sampleRate, initCtx.tunedBw);
  noise.hum.noiseAmp = radio.noiseWeight;
  noise.hum.humHz = noise.humHzDefault;
  noise.hum.humToneEnabled = noise.enableHumTone;
  if (radio.noiseWeight <= 0.0f) {
    noise.hum.humAmp = 0.0f;
    noise.hum.crackleRate = 0.0f;
    noise.hum.crackleAmp = 0.0f;
    noise.hum.lightningRate = 0.0f;
    noise.hum.lightningAmp = 0.0f;
    noise.hum.motorRate = 0.0f;
    noise.hum.motorAmp = 0.0f;
  } else {
    float scale = std::clamp(radio.noiseWeight / noise.noiseWeightRef, 0.0f,
                             noise.noiseWeightScaleMax);
    noise.hum.humAmp = noise.humAmpScale * scale;
    noise.hum.crackleRate = noise.crackleRateScale * scale;
    noise.hum.crackleAmp = noise.crackleAmpScale * scale;
    noise.hum.lightningRate = noise.lightningRateScale * scale;
    noise.hum.lightningAmp = noise.lightningAmpScale * scale;
    noise.hum.motorRate = noise.motorRateScale * scale;
    noise.hum.motorAmp = noise.motorAmpScale * scale;
    noise.hum.motorBuzzHz = noise.motorBuzzHz;
  }
}

void RadioNoiseNode::reset(Radio1938& radio) { radio.noise.hum.reset(); }

void RadioNoiseNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioNoiseNode::process(Radio1938& radio, float y, RadioFrameContext&) {
  auto& noise = radio.noise;
  noise.hum.humToneEnabled = noise.enableHumTone;
  return y + noise.hum.process(y);
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

void RadioSpeakerNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioSpeakerNode::process(Radio1938& radio, float y, RadioFrameContext&) {
  auto& speakerStage = radio.speakerStage;
  y *= radio.makeupGain;
  y = processOversampled2x(y, speakerStage.osPrev, speakerStage.osLpIn,
                           speakerStage.osLpOut, [&](float v) {
                             float out = speakerStage.speaker.process(v);
                             if (speakerStage.speaker.clipTriggered) {
                               radio.clipTriggered = true;
                             }
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

void RadioRoomNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioRoomNode::process(Radio1938& radio, float y, RadioFrameContext&) {
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

void RadioOutputClipNode::prepareBlock(Radio1938&, RadioFrameContext&, uint32_t) {}

float RadioOutputClipNode::process(Radio1938& radio,
                                   float y,
                                   RadioFrameContext&) {
  auto& output = radio.output;
  return processOversampled2x(y, output.clipOsPrev, output.clipOsLpIn,
                              output.clipOsLpOut, [&](float v) {
                                float t = radio.globals.outputClipThreshold;
                                float av = std::fabs(v);
                                if (av > t) radio.clipTriggered = true;
                                return softClip(v, t);
                              });
}

template <size_t StageCount>
static void runInitFlow(
    Radio1938& radio,
    const std::array<LifecycleStep, StageCount>& stages,
    RadioInitContext& initCtx) {
  for (const auto& stage : stages) {
    if (stage.processor.init) stage.processor.init(radio, initCtx);
  }
}

template <size_t StageCount>
static void runResetFlow(
    Radio1938& radio,
    const std::array<LifecycleStep, StageCount>& stages) {
  for (const auto& stage : stages) {
    if (stage.processor.reset) stage.processor.reset(radio);
  }
}

template <size_t StageCount>
static RadioFrameContext runBlockFlow(
    Radio1938& radio,
    const std::array<PipelineStep, StageCount>& stages,
    uint32_t frames) {
  RadioFrameContext ctx{};
  ctx.sampleRate = radio.sampleRate;
  for (const auto& step : stages) {
    if (!step.enabled || !step.processor.block) continue;
    step.processor.block(radio, ctx, frames);
  }
  return ctx;
}

template <size_t StageCount>
static float runSampleFlow(Radio1938& radio,
                           float y,
                           RadioFrameContext& frame,
                           const std::array<PipelineStep, StageCount>& stages) {
  for (const auto& step : stages) {
    if (!step.enabled || !step.processor.sample) continue;
    y = step.processor.sample(radio, y, frame);
  }
  return y;
}

void Radio1938::init(int ch, float sr, float bw, float noise) {
  channels = std::max(1, ch);
  sampleRate = sr;
  bwHz = bw;
  noiseWeight = noise;
  RadioInitContext initCtx{};
  runInitFlow(*this, pipeline.initFlow, initCtx);
  reset();
}

void Radio1938::reset() {
  clipTriggered = false;
  speakerStage.speaker.clipTriggered = false;
  runResetFlow(*this, pipeline.resetFlow);
}

void Radio1938::process(float* samples, uint32_t frames) {
  if (!samples || frames == 0) return;
  if (pipeline.bypass) return;
  clipTriggered = false;
  speakerStage.speaker.clipTriggered = false;
  RadioFrameContext blockContext = runBlockFlow(*this, pipeline.blockFlow, frames);
  for (uint32_t f = 0; f < frames; ++f) {
    float inL = samples[f * channels];
    float inR = (channels > 1) ? samples[f * channels + 1] : inL;
    float x = (channels > 1) ? 0.5f * (inL + inR) : inL;
    auto writeOut = [&](float v) {
      for (int c = 0; c < channels; ++c) {
        samples[f * channels + c] = v;
      }
    };
    RadioFrameContext frame = blockContext;
    float y = runSampleFlow(*this, x, frame, pipeline.sampleFlow);
    writeOut(y);
  }
}
