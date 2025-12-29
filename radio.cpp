#include "radio.h"

#include <algorithm>
#include <cmath>

static constexpr bool kEnableRadioArtifacts = true;
static constexpr bool kEnableRoomEarlyReflections = true;

static inline float clampf(float x, float a, float b) {
  return std::min(std::max(x, a), b);
}

static inline float db2lin(float db) {
  return std::pow(10.0f, db / 20.0f);
}

static inline float lin2db(float x) {
  return 20.0f * std::log10(std::max(x, 1e-12f));
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
  float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
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
  float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
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
  float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
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

void RadioDSP::init(int ch, float sr, float bw, float presence) {
  channels = ch;
  sampleRate = sr;
  presenceDb = presence;
  lpHz = bw;
  hpHz = 120.0f;
  env = 0.0f;

  hp.assign(channels, {});
  lp.assign(channels, {});
  peq.assign(channels, {});
  for (int i = 0; i < channels; ++i) {
    hp[i].setHighpass(sampleRate, hpHz, 0.707f);
    lp[i].setLowpass(sampleRate, lpHz, 0.707f);
    peq[i].setPeaking(sampleRate, 900.0f, 1.0f, presenceDb);
  }
}

float RadioDSP::computeGainDb(float envDb) {
  if (envDb <= thresholdDb) return 0.0f;
  float over = envDb - thresholdDb;
  float compressed = over / ratio;
  return (thresholdDb + compressed) - envDb;
}

void RadioDSP::process(float* samples, uint32_t frames) {
  if (!samples || frames == 0) return;
  float attackCoeff = std::exp(-1.0f / (attack * sampleRate));
  float releaseCoeff = std::exp(-1.0f / (release * sampleRate));
  float noiseAmp = 0.0015f * noiseWeight;

  for (uint32_t f = 0; f < frames; ++f) {
    float peak = 0.0f;
    for (int c = 0; c < channels; ++c) {
      float v = samples[f * channels + c];
      peak = std::max(peak, std::fabs(v));
    }

    if (peak > env) {
      env = attackCoeff * env + (1.0f - attackCoeff) * peak;
    } else {
      env = releaseCoeff * env + (1.0f - releaseCoeff) * peak;
    }

    float envDb = 20.0f * std::log10(std::max(env, 1e-6f));
    float gainDb = computeGainDb(envDb);
    float gain = std::pow(10.0f, gainDb / 20.0f);

    for (int c = 0; c < channels; ++c) {
      float v = samples[f * channels + c];
      v = hp[c].process(v);
      v = lp[c].process(v);
      v = peq[c].process(v);

      v *= gain;

      if (noiseWeight > 0.0f) {
        v += dist(rng) * noiseAmp;
      }

      if (v > limit) v = limit;
      if (v < -limit) v = -limit;
      samples[f * channels + c] = v;
    }
  }
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

static inline float softClip(float x, float t = 0.98f) {
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
  hp.setHighpass(fs, noiseHpHz, 0.707f);
  lp.setLowpass(fs, safeLp, 0.707f);
  crackleHp.setHighpass(fs, noiseHpHz, 0.707f);
  crackleLp.setLowpass(fs, safeLp, 0.707f);
  hp.reset();
  lp.reset();
  crackleHp.reset();
  crackleLp.reset();
  float atkMs = 10.0f;
  float relMs = 250.0f;
  scAtk = std::exp(-1.0f / (fs * (atkMs / 1000.0f)));
  scRel = std::exp(-1.0f / (fs * (relMs / 1000.0f)));
  float crackleMs = 12.0f;
  crackleDecay = std::exp(-1.0f / (fs * (crackleMs / 1000.0f)));
}

void NoiseHum::reset() {
  humPhase = 0.0f;
  scEnv = 0.0f;
  crackleEnv = 0.0f;
  hp.reset();
  lp.reset();
  crackleHp.reset();
  crackleLp.reset();
}

float NoiseHum::process(float programSample) {
  float a = std::fabs(programSample);
  if (a > scEnv) {
    scEnv = scAtk * scEnv + (1.0f - scAtk) * a;
  } else {
    scEnv = scRel * scEnv + (1.0f - scRel) * a;
  }

  float start = 0.01f;
  float end = 0.05f;
  float t = clampf((scEnv - start) / (end - start), 0.0f, 1.0f);
  float duckDb = 2.0f;
  float duckGain = db2lin(-duckDb * t);

  float n = dist(rng);
  n = hp.process(n);
  n = lp.process(n);
  n *= noiseAmp * duckGain;

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
    c = raw * crackleAmp * duckGain;
  }

  constexpr float twoPi = 6.283185307f;
  humPhase += twoPi * (humHz / fs);
  if (humPhase > twoPi) humPhase -= twoPi;
  float h = std::sin(humPhase) + 0.35f * std::sin(2.0f * humPhase);
  h *= humAmp;

  return n + c + h;
}

void AMDetector::init(float newFs, float newBw) {
  fs = newFs;
  bwHz = newBw;
  carrierHz = std::min(12000.0f, fs * 0.35f);
  float halfBw = std::clamp(bwHz * 0.95f, 1500.0f, fs * 0.2f);
  float ifLow = std::clamp(carrierHz - halfBw, 1000.0f, fs * 0.45f);
  float ifHigh = std::clamp(carrierHz + halfBw, ifLow + 800.0f, fs * 0.48f);
  ifHp1.setHighpass(fs, ifLow, 0.707f);
  ifHp2.setHighpass(fs, ifLow, 0.707f);
  ifLp1.setLowpass(fs, ifHigh, 0.707f);
  ifLp2.setLowpass(fs, ifHigh, 0.707f);

  float detLpHz = std::clamp(bwHz * 0.9f, 2000.0f, fs * 0.45f);
  detLp1.setLowpass(fs, detLpHz, 0.707f);
  detLp2.setLowpass(fs, detLpHz, 0.707f);
  detLp3.setLowpass(fs, detLpHz, 0.707f);

  float agcAtkMs = 6.0f;
  float agcRelMs = 160.0f;
  agcAtk = std::exp(-1.0f / (fs * (agcAtkMs / 1000.0f)));
  agcRel = std::exp(-1.0f / (fs * (agcRelMs / 1000.0f)));
  agcGainAtk = agcAtk;
  agcGainRel = agcRel;

  float dcMs = 450.0f;
  dcCoeff = std::exp(-1.0f / (fs * (dcMs / 1000.0f)));

  reset();
}

void AMDetector::reset() {
  phase = 0.0f;
  agcEnv = 0.0f;
  agcGainDb = 0.0f;
  dcEnv = 0.0f;
  ifHp1.reset();
  ifHp2.reset();
  ifLp1.reset();
  ifLp2.reset();
  detLp1.reset();
  detLp2.reset();
  detLp3.reset();
}

float AMDetector::process(float x) {
  float mod = std::clamp(x * modIndex, -0.98f, 0.98f);
  constexpr float twoPi = 6.283185307f;
  phase += twoPi * (carrierHz / fs);
  if (phase > twoPi) phase -= twoPi;
  float carrier = std::sin(phase);
  float ifSample = (1.0f + mod) * carrier * carrierGain;

  ifSample = ifHp1.process(ifSample);
  ifSample = ifHp2.process(ifSample);
  ifSample = ifLp1.process(ifSample);
  ifSample = ifLp2.process(ifSample);

  float level = std::fabs(ifSample);
  if (level > agcEnv) {
    agcEnv = agcAtk * agcEnv + (1.0f - agcAtk) * level;
  } else {
    agcEnv = agcRel * agcEnv + (1.0f - agcRel) * level;
  }
  float envDb = lin2db(agcEnv);
  float targetGainDb = std::clamp(agcTargetDb - envDb, agcMinGainDb, agcMaxGainDb);
  if (targetGainDb < agcGainDb) {
    agcGainDb = agcGainAtk * agcGainDb + (1.0f - agcGainAtk) * targetGainDb;
  } else {
    agcGainDb = agcGainRel * agcGainDb + (1.0f - agcGainRel) * targetGainDb;
  }
  float ifAgc = ifSample * db2lin(agcGainDb);

  float det = ifAgc * carrier;
  float env = detLp1.process(det);
  env = detLp2.process(env);
  env = detLp3.process(env);
  dcEnv = dcCoeff * dcEnv + (1.0f - dcCoeff) * env;
  float out = (env - dcEnv) * detGain;
  if (diodeDrop > 0.0f) {
    float a = std::fabs(out);
    if (a <= diodeDrop) {
      out = 0.0f;
    } else {
      out = std::copysign(a - diodeDrop, out);
    }
  }
  return out;
}

void SpeakerSim::init(float fs) {
  boxRes.setPeaking(fs, 145.0f, 0.85f, 2.6f);
  boxRes2.setPeaking(fs, 430.0f, 1.05f, 1.4f);
  coneDip.setPeaking(fs, 2600.0f, 1.05f, -2.0f);
}

void SpeakerSim::reset() {
  boxRes.reset();
  boxRes2.reset();
  coneDip.reset();
}

float SpeakerSim::process(float x) {
  float y = boxRes.process(x);
  y = boxRes2.process(y);
  y = coneDip.process(y);
  float yd = std::tanh(drive * y) / std::tanh(drive);
  y = (1.0f - mix) * y + mix * yd;
  return softClip(y, limit);
}

void Radio1938::init(int ch, float sr, float bw, float noise) {
  channels = std::max(1, ch);
  sampleRate = sr;
  bwHz = bw;
  noiseWeight = noise;
  detuneBuf.assign(64, 0.0f);
  detuneIndex = 0;

  float safeBw = std::clamp(bwHz, 4200.0f, 5600.0f);
  hpf.setHighpass(sampleRate, 140.0f, 0.707f);
  lpf1.setLowpass(sampleRate, safeBw, 0.707f);
  lpf2.setLowpass(sampleRate, safeBw, 0.707f);
  postLpf1.setLowpass(sampleRate, safeBw, 0.707f);
  postLpf2.setLowpass(sampleRate, safeBw, 0.707f);
  tuneOffsetNorm = (safeBw > 0.0f)
    ? std::clamp(tuneOffsetHz / (safeBw * 0.5f), -1.0f, 1.0f)
    : 0.0f;
  float shift = 1.0f + tuneOffsetNorm * 0.18f;
  float ripple1Hz = std::clamp(950.0f * shift, 500.0f, safeBw * 0.95f);
  float ripple2Hz = std::clamp(2300.0f * shift, 800.0f, safeBw * 0.95f);
  float tiltHz = std::clamp(1700.0f * (1.0f + tuneOffsetNorm * 0.15f), 900.0f, safeBw * 0.95f);
  ifRipple1.setPeaking(sampleRate, ripple1Hz, 0.9f, 0.5f);
  ifRipple2.setPeaking(sampleRate, ripple2Hz, 1.1f, -0.6f);
  ifTiltLp.setLowpass(sampleRate, tiltHz, 0.707f);
  mpTiltLp.setLowpass(sampleRate, 1800.0f, 0.707f);
  float amNyquist = std::min(amSampleRate * 0.5f, sampleRate * 0.45f);
  float amCut = std::clamp(amNyquist * 0.96f, 2500.0f, sampleRate * 0.45f);
  amRateLp1.setLowpass(sampleRate, amCut, 0.707f);
  amRateLp2.setLowpass(sampleRate, amCut, 0.707f);
  amStep = (amSampleRate > 0.0f) ? (amSampleRate / sampleRate) : 0.0f;
  float mpMaxDelay = std::max(0.0f, mpDelayMs + mpDelayModMs);
  int mpSamples = std::max(8, static_cast<int>(std::ceil(mpMaxDelay * 0.001f * sampleRate)) + 4);
  mpBuf.assign(static_cast<size_t>(mpSamples), 0.0f);
  mpIndex = 0;
  midBoost.setPeaking(sampleRate, 1500.0f, 1.0f, 4.0f);
  lowMidDip.setPeaking(sampleRate, 420.0f, 1.0f, -2.5f);
  presBoost.setPeaking(sampleRate, 3200.0f, 0.9f, 1.5f);

  comp.setFs(sampleRate);
  comp.thresholdDb = -22.0f;
  comp.ratio = 2.4f;
  comp.setTimes(90.0f, 900.0f);

  float sagAtkMs = 60.0f;
  float sagRelMs = 900.0f;
  sagAtk = std::exp(-1.0f / (sampleRate * (sagAtkMs / 1000.0f)));
  sagRel = std::exp(-1.0f / (sampleRate * (sagRelMs / 1000.0f)));

  sat.drive = 1.40f;
  sat.mix = 0.50f;

  am.init(sampleRate, safeBw);
  speaker.init(sampleRate);
  roomTapSamples.clear();
  roomTapGains.clear();
  const float tapMs[] = {6.0f, 10.5f, 16.0f, 23.0f, 31.0f};
  const float tapGain[] = {0.32f, 0.26f, 0.20f, 0.15f, 0.11f};
  int maxTap = 1;
  for (size_t i = 0; i < sizeof(tapMs) / sizeof(tapMs[0]); ++i) {
    int samples = std::max(1, static_cast<int>(std::lround(tapMs[i] * 0.001f * sampleRate)));
    roomTapSamples.push_back(samples);
    roomTapGains.push_back(tapGain[i]);
    maxTap = std::max(maxTap, samples);
  }
  int baseRoom = std::max(1, static_cast<int>(std::lround(sampleRate * 0.012f)));
  roomDelaySamples = std::max(baseRoom, maxTap);
  roomBuf.assign(static_cast<size_t>(roomDelaySamples + 2), 0.0f);
  roomIndex = 0;
  roomLp.setLowpass(sampleRate, roomLpHz, 0.707f);

  noiseHum.setFs(sampleRate, safeBw);
  noiseHum.noiseAmp = noiseWeight;
  noiseHum.humHz = 50.0f;
  if (noiseWeight <= 0.0f) {
    noiseHum.humAmp = 0.0f;
    noiseHum.crackleRate = 0.0f;
    noiseHum.crackleAmp = 0.0f;
  } else {
    float scale = std::clamp(noiseWeight / 0.015f, 0.0f, 2.0f);
    noiseHum.humAmp = 0.0015f * scale;
    noiseHum.crackleRate = 0.9f * scale;
    noiseHum.crackleAmp = 0.015f * scale;
  }
  noiseBase = noiseHum.noiseAmp;
  crackleBase = noiseHum.crackleAmp;
  heteroBaseScale = (noiseWeight > 0.0f)
    ? std::clamp(noiseWeight / 0.015f, 0.15f, 1.0f)
    : 0.0f;

  reset();
}

void Radio1938::reset() {
  fadePhase = 0.0f;
  fadePhase2 = 0.0f;
  noisePhase = 0.0f;
  noisePhase2 = 0.0f;
  detunePhase = 0.0f;
  detunePhase2 = 0.0f;
  heteroPhase = 0.0f;
  heteroDriftPhase = 0.0f;
  amPhase = 0.0f;
  amPrev = 0.0f;
  amHold = 0.0f;
  mpPhase = 0.0f;
  mpPhase2 = 0.0f;
  mpDelayPhase = 0.0f;
  mpIndex = 0;
  std::fill(mpBuf.begin(), mpBuf.end(), 0.0f);
  detuneIndex = 0;
  std::fill(detuneBuf.begin(), detuneBuf.end(), 0.0f);
  sagEnv = 0.0f;
  roomIndex = 0;
  std::fill(roomBuf.begin(), roomBuf.end(), 0.0f);
  hpf.reset();
  lpf1.reset();
  lpf2.reset();
  postLpf1.reset();
  postLpf2.reset();
  ifRipple1.reset();
  ifRipple2.reset();
  ifTiltLp.reset();
  mpTiltLp.reset();
  amRateLp1.reset();
  amRateLp2.reset();
  midBoost.reset();
  lowMidDip.reset();
  presBoost.reset();
  comp.reset();
  am.reset();
  speaker.reset();
  roomLp.reset();
  noiseHum.reset();
}

void Radio1938::process(float* samples, uint32_t frames) {
  if (!samples || frames == 0) return;
  constexpr float twoPi = 6.283185307f;
  for (uint32_t f = 0; f < frames; ++f) {
    float inL = samples[f * channels];
    float inR = (channels > 1) ? samples[f * channels + 1] : inL;
    float x = (channels > 1) ? 0.5f * (inL + inR) : inL;

    float y = hpf.process(x);
    y = lpf1.process(y);
    y = lpf2.process(y);
    y = ifRipple1.process(y);
    y = ifRipple2.process(y);
    float low = ifTiltLp.process(y);
    float high = y - low;
    float tiltMix = ifTiltMix;
    if (tuneOffsetNorm != 0.0f) {
      float extra = std::abs(tuneOffsetNorm) * tuneTiltExtra;
      if (tuneOffsetNorm > 0.0f) {
        float highGain = std::max(0.0f, 1.0f - tiltMix - extra);
        y = low + high * highGain;
      } else {
        float lowGain = std::max(0.0f, 1.0f - extra);
        float highGain = std::max(0.0f, 1.0f - tiltMix);
        y = low * lowGain + high * highGain;
      }
    } else {
      y = low + high * (1.0f - tiltMix);
    }
    if (kEnableRadioArtifacts && !mpBuf.empty() && mpMix > 0.0f) {
      mpPhase += twoPi * (mpRate / sampleRate);
      mpPhase2 += twoPi * (mpRate2 / sampleRate);
      mpDelayPhase += twoPi * (mpDelayRate / sampleRate);
      if (mpPhase > twoPi) mpPhase -= twoPi;
      if (mpPhase2 > twoPi) mpPhase2 -= twoPi;
      if (mpDelayPhase > twoPi) mpDelayPhase -= twoPi;

      float mixLfo = std::sin(mpPhase);
      float mix = mpMix * (1.0f + mpDepth * mixLfo);
      mix = std::clamp(mix, 0.0f, 0.35f);

      float delayMs = mpDelayMs + mpDelayModMs * std::sin(mpDelayPhase);
      float maxDelay = static_cast<float>(mpBuf.size() - 2);
      float delaySamples = std::clamp(delayMs * 0.001f * sampleRate, 1.0f, maxDelay);
      float read = static_cast<float>(mpIndex) - delaySamples;
      while (read < 0.0f) read += static_cast<float>(mpBuf.size());
      int i0 = static_cast<int>(read);
      int i1 = (i0 + 1) % static_cast<int>(mpBuf.size());
      float frac = read - static_cast<float>(i0);
      float delayed = mpBuf[static_cast<size_t>(i0)] * (1.0f - frac) +
        mpBuf[static_cast<size_t>(i1)] * frac;

      mpBuf[static_cast<size_t>(mpIndex)] = y;
      mpIndex = (mpIndex + 1) % static_cast<int>(mpBuf.size());

      float mpLow = mpTiltLp.process(delayed);
      float mpHigh = delayed - mpLow;
      delayed = mpLow + mpHigh * (1.0f - mpTiltMix);

      float phaseMix = std::sin(mpPhase2);
      float direct = y * (1.0f - 0.5f * mix);
      y = direct + delayed * mix * phaseMix;
    } else if (!mpBuf.empty()) {
      mpBuf[static_cast<size_t>(mpIndex)] = y;
      mpIndex = (mpIndex + 1) % static_cast<int>(mpBuf.size());
    }
    if (amStep > 0.0f && amStep < 1.0f) {
      y = amRateLp1.process(y);
      y = amRateLp2.process(y);
      float prevPhase = amPhase;
      amPhase += amStep;
      if (amPhase >= 1.0f) {
        float t = (1.0f - prevPhase) / amStep;
        amHold = amPrev + (y - amPrev) * t;
        amPhase -= 1.0f;
      }
      amPrev = y;
      y = amHold;
    }
    if (kEnableRadioArtifacts && !detuneBuf.empty()) {
      detunePhase += twoPi * (detuneRate / sampleRate);
      detunePhase2 += twoPi * (detuneRate2 / sampleRate);
      if (detunePhase > twoPi) detunePhase -= twoPi;
      if (detunePhase2 > twoPi) detunePhase2 -= twoPi;
      float lfo = 0.6f * std::sin(detunePhase) + 0.4f * std::sin(detunePhase2);
      float delay = detuneBaseDelay + detuneDepth * lfo;
      float maxDelay = static_cast<float>(detuneBuf.size() - 2);
      delay = std::clamp(delay, 0.25f, maxDelay);
      float read = static_cast<float>(detuneIndex) - delay;
      while (read < 0.0f) read += static_cast<float>(detuneBuf.size());
      int i0 = static_cast<int>(read);
      int i1 = (i0 + 1) % static_cast<int>(detuneBuf.size());
      float frac = read - static_cast<float>(i0);
      float delayed = detuneBuf[static_cast<size_t>(i0)] * (1.0f - frac) +
        detuneBuf[static_cast<size_t>(i1)] * frac;
      detuneBuf[static_cast<size_t>(detuneIndex)] = y;
      detuneIndex = (detuneIndex + 1) % static_cast<int>(detuneBuf.size());
      y = delayed;
    }
    // y = am.process(y); // Cannot revert these changes
    y = midBoost.process(y);
    y = lowMidDip.process(y);
    y = presBoost.process(y);
    y = comp.process(y);
    y = sat.process(y);
    y = postLpf1.process(y);
    y = postLpf2.process(y);
    float level = std::fabs(y);
    if (level > sagEnv) {
      sagEnv = sagAtk * sagEnv + (1.0f - sagAtk) * level;
    } else {
      sagEnv = sagRel * sagEnv + (1.0f - sagRel) * level;
    }
    float sagT = clampf((sagEnv - sagStart) / (sagEnd - sagStart), 0.0f, 1.0f);
    float sagGain = 1.0f - sagDepth * sagT;
    y *= sagGain;
    if (kEnableRadioArtifacts) {
      float noiseScale = 1.0f;
      fadePhase += twoPi * (fadeRate / sampleRate);
      fadePhase2 += twoPi * (fadeRate2 / sampleRate);
      if (fadePhase > twoPi) fadePhase -= twoPi;
      if (fadePhase2 > twoPi) fadePhase2 -= twoPi;
      float fadeLfo = 0.6f * std::sin(fadePhase) + 0.4f * std::sin(fadePhase2);
      float fade = 1.0f + fadeDepth * fadeLfo;
      fade = std::clamp(fade, 0.65f, 1.35f);
      y *= fade;

      noisePhase += twoPi * (noiseRate / sampleRate);
      noisePhase2 += twoPi * (noiseRate2 / sampleRate);
      if (noisePhase > twoPi) noisePhase -= twoPi;
      if (noisePhase2 > twoPi) noisePhase2 -= twoPi;
      float noiseLfo = 0.6f * std::sin(noisePhase) + 0.4f * std::sin(noisePhase2);
      noiseScale = 1.0f + noiseDepth * noiseLfo;
      noiseScale = std::clamp(noiseScale, 0.4f, 1.6f);
      noiseHum.noiseAmp = noiseBase * noiseScale;
      noiseHum.crackleAmp = crackleBase * noiseScale;

      if (heteroDepth > 0.0f && heteroBaseScale > 0.0f) {
        heteroDriftPhase += twoPi * (heteroDriftHz / sampleRate);
        if (heteroDriftPhase > twoPi) heteroDriftPhase -= twoPi;
        float drift = 1.0f + 0.12f * std::sin(heteroDriftPhase);
        float heteroHz = heteroBaseHz * drift;
        heteroPhase += twoPi * (heteroHz / sampleRate);
        if (heteroPhase > twoPi) heteroPhase -= twoPi;
        float hetero = std::sin(heteroPhase);
        float quietT = clampf(
          (sagEnv - heteroGateStart) / std::max(1e-6f, heteroGateEnd - heteroGateStart),
          0.0f,
          1.0f
        );
        float quiet = 1.0f - quietT;
        quiet *= quiet;
        float amp = heteroDepth * heteroBaseScale * (0.6f + 0.4f * noiseScale) * quiet;
        y += hetero * amp;
      }
    }
    y += noiseHum.process(y);
    y = speaker.process(y); // Cannot revert these changes
    if (!roomBuf.empty()) {
      float dry = y;
      float roomOut = 0.0f;
      if (roomMix > 0.0f && kEnableRoomEarlyReflections && !roomTapSamples.empty()) {
        int size = static_cast<int>(roomBuf.size());
        int writeIndex = roomIndex;
        size_t tapCount = std::min(roomTapSamples.size(), roomTapGains.size());
        for (size_t i = 0; i < tapCount; ++i) {
          int tap = roomTapSamples[i];
          int idx = writeIndex - tap;
          while (idx < 0) idx += size;
          roomOut += roomBuf[static_cast<size_t>(idx)] * roomTapGains[i];
        }
      } else if (roomMix > 0.0f) {
        roomOut = roomBuf[static_cast<size_t>(roomIndex)];
      }
      roomBuf[static_cast<size_t>(roomIndex)] = dry;
      roomIndex = (roomIndex + 1) % static_cast<int>(roomBuf.size());
      if (roomMix > 0.0f) {
        roomOut = roomLp.process(roomOut);
        y += roomMix * roomOut;
      }
    }
    y = softClip(y, 0.985f);

    for (int c = 0; c < channels; ++c) {
      samples[f * channels + c] = y;
    }
  }
}
