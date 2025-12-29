#ifndef RADIO_H
#define RADIO_H

#include <cstdint>
#include <random>
#include <vector>

struct Biquad {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  float z1 = 0.0f;
  float z2 = 0.0f;

  float process(float x);
  void reset();
  void setLowpass(float sampleRate, float freq, float q);
  void setHighpass(float sampleRate, float freq, float q);
  void setPeaking(float sampleRate, float freq, float q, float gainDb);
};

struct RadioDSP {
  float sampleRate = 48000.0f;
  int channels = 1;
  float noiseWeight = 0.015f;
  float presenceDb = 2.5f;
  float lpHz = 5000.0f;
  float hpHz = 120.0f;

  std::vector<Biquad> hp;
  std::vector<Biquad> lp;
  std::vector<Biquad> peq;

  float env = 0.0f;
  float attack = 0.03f;
  float release = 0.40f;
  float thresholdDb = -18.0f;
  float ratio = 4.0f;
  float limit = 0.98f;

  std::mt19937 rng{0x2a4f5a1u};
  std::uniform_real_distribution<float> dist{-1.0f, 1.0f};

  void init(int ch, float sr, float bw, float presence);
  float computeGainDb(float envDb);
  void process(float* samples, uint32_t frames);
};

struct Compressor {
  float fs = 48000.0f;
  float thresholdDb = -24.0f;
  float ratio = 4.0f;
  float attackMs = 12.0f;
  float releaseMs = 180.0f;

  float env = 0.0f;
  float gainDb = 0.0f;
  float atkCoeff = 0.0f;
  float relCoeff = 0.0f;
  float gainAtkCoeff = 0.0f;
  float gainRelCoeff = 0.0f;

  void setFs(float newFs);
  void setTimes(float aMs, float rMs);
  void reset();
  float process(float x);
};

struct Saturator {
  float drive = 1.35f;
  float mix = 0.45f;

  float process(float x);
};

struct NoiseHum {
  std::mt19937 rng{0x1938u};
  std::uniform_real_distribution<float> dist{-1.0f, 1.0f};
  std::uniform_real_distribution<float> dist01{0.0f, 1.0f};
  Biquad hp;
  Biquad lp;
  Biquad crackleHp;
  Biquad crackleLp;
  float fs = 48000.0f;
  float noiseAmp = 0.015f;
  float noiseHpHz = 500.0f;
  float noiseLpHz = 5500.0f;
  float humAmp = 0.0015f;
  float humHz = 50.0f;
  float humPhase = 0.0f;
  float scEnv = 0.0f;
  float scAtk = 0.0f;
  float scRel = 0.0f;
  float crackleRate = 0.9f;
  float crackleAmp = 0.015f;
  float crackleEnv = 0.0f;
  float crackleDecay = 0.0f;

  void setFs(float newFs, float noiseBwHz);
  void reset();
  float process(float programSample);
};

struct AMDetector {
  float fs = 48000.0f;
  float bwHz = 4800.0f;
  float carrierHz = 12000.0f;
  float modIndex = 0.80f;
  float carrierGain = 0.40f;
  float diodeDrop = 0.02f;
  float detGain = 3.20f;

  Biquad ifHp1;
  Biquad ifHp2;
  Biquad ifLp1;
  Biquad ifLp2;
  Biquad detLp1;
  Biquad detLp2;
  Biquad detLp3;

  float phase = 0.0f;
  float agcEnv = 0.0f;
  float agcGainDb = 0.0f;
  float agcTargetDb = -14.0f;
  float agcMaxGainDb = 22.0f;
  float agcMinGainDb = -6.0f;
  float agcAtk = 0.0f;
  float agcRel = 0.0f;
  float agcGainAtk = 0.0f;
  float agcGainRel = 0.0f;

  float dcEnv = 0.0f;
  float dcCoeff = 0.0f;

  void init(float newFs, float newBw);
  void reset();
  float process(float x);
};

struct SpeakerSim {
  Biquad boxRes;
  Biquad boxRes2;
  Biquad coneDip;
  float drive = 1.18f;
  float mix = 0.35f;
  float limit = 0.93f;

  void init(float fs);
  void reset();
  float process(float x);
};

struct Radio1938 {
  float sampleRate = 48000.0f;
  int channels = 1;
  float bwHz = 4800.0f;
  float noiseWeight = 0.012f;

  float fadePhase = 0.0f;
  float fadePhase2 = 0.0f;
  float noisePhase = 0.0f;
  float noisePhase2 = 0.0f;
  float detunePhase = 0.0f;
  float detunePhase2 = 0.0f;
  float heteroPhase = 0.0f;
  float heteroDriftPhase = 0.0f;
  float fadeRate = 0.08f;
  float fadeRate2 = 0.05f;
  float noiseRate = 0.04f;
  float noiseRate2 = 0.031f;
  float detuneRate = 0.13f;
  float detuneRate2 = 0.09f;
  float heteroBaseHz = 950.0f;
  float heteroDriftHz = 0.06f;
  float heteroGateStart = 0.015f;
  float heteroGateEnd = 0.05f;
  float fadeDepth = 0.10f;
  float noiseDepth = 0.35f;
  float detuneDepth = 0.80f;
  float heteroDepth = 0.0007f;
  float detuneBaseDelay = 2.0f;
  std::vector<float> detuneBuf;
  int detuneIndex = 0;
  float noiseBase = 0.0f;
  float crackleBase = 0.0f;
  float heteroBaseScale = 0.0f;

  float sagEnv = 0.0f;
  float sagAtk = 0.0f;
  float sagRel = 0.0f;
  float sagStart = 0.06f;
  float sagEnd = 0.22f;
  float sagDepth = 0.07f;

  float ifTiltMix = 0.16f;
  float roomMix = 0.08f;
  float roomLpHz = 1800.0f;
  int roomIndex = 0;
  int roomDelaySamples = 0;
  std::vector<float> roomBuf;

  Biquad ifRipple1;
  Biquad ifRipple2;
  Biquad ifTiltLp;
  Biquad roomLp;

  Biquad hpf;
  Biquad lpf1;
  Biquad lpf2;
  Biquad postLpf1;
  Biquad postLpf2;
  Biquad midBoost;
  Biquad lowMidDip;
  Biquad presBoost;
  Compressor comp;
  Saturator sat;
  AMDetector am;
  SpeakerSim speaker;
  NoiseHum noiseHum;

  void init(int ch, float sr, float bw, float noise);
  void reset();
  void process(float* samples, uint32_t frames);
};

#endif
