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
  Biquad lightningHp;
  Biquad lightningLp;
  Biquad motorHp;
  Biquad motorLp;
  float fs = 48000.0f;
  float noiseAmp = 0.015f;
  float noiseHpHz = 220.0f; // 500.0f
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
  float lightningRate = 0.03f;
  float lightningAmp = 0.045f;
  float lightningEnv = 0.0f;
  float lightningDecay = 0.0f;
  float motorRate = 0.20f;
  float motorAmp = 0.010f;
  float motorEnv = 0.0f;
  float motorDecay = 0.0f;
  float motorPhase = 0.0f;
  float motorBuzzHz = 18.0f;

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
  float diodeDrop = 0.008f;
  float detGain = 2.70f;
  float ifNoiseAmp = 0.0f;

  enum class Mode {
    Envelope,
    Iq,
  };
  Mode mode = Mode::Iq;

  std::mt19937 rng{0x1942u};
  std::uniform_real_distribution<float> dist{-1.0f, 1.0f};

  Biquad ifHp1;
  Biquad ifHp2;
  Biquad ifLp1;
  Biquad ifLp2;
  Biquad detLp1;
  Biquad detLp2;
  Biquad detLp3;
  Biquad detLp1Q;
  Biquad detLp2Q;
  Biquad detLp3Q;

  float phase = 0.0f;
  float agcEnv = 0.0f;
  float agcGainDb = 0.0f;
  float agcTargetDb = -18.0f;
  float agcMaxGainDb = 12.0f;
  float agcMinGainDb = -4.0f;
  float agcAtk = 0.0f;
  float agcRel = 0.0f;
  float agcGainAtk = 0.0f;
  float agcGainRel = 0.0f;

  float dcEnv = 0.0f;
  float dcCoeff = 0.0f;

  void init(float newFs, float newBw);
  void setBandwidth(float newBw);
  void reset();
  float process(float x);
};

struct SpeakerSim {
  Biquad boxRes;
  Biquad boxRes2;
  Biquad coneDip;
  float drive = 1.05f;
  float mix = 0.24f;
  float limit = 0.93f;
  bool clipTriggered = false;

  void init(float fs);
  void reset();
  float process(float x);
};

struct Radio1938 {
  float sampleRate = 48000.0f;
  int channels = 1;
  float bwHz = 4800.0f;
  float noiseWeight = 0.012f;
  float makeupGain = 1.0f; // baseline unity gain
  bool clipTriggered = false;

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
  float fadeDepth = 0.05f;
  float noiseDepth = 0.20f;
  float detuneDepth = 0.0f;
  float heteroDepth = 0.00035f;
  float detuneBaseDelay = 2.0f;
  std::vector<float> detuneBuf;
  int detuneIndex = 0;
  float noiseBase = 0.0f;
  float crackleBase = 0.0f;
  float lightningBase = 0.0f;
  float motorBase = 0.0f;
  float humBase = 0.0f;
  float heteroBaseScale = 0.0f;

  float tuneOffsetHz = 0.0f;
  float tuneOffsetNorm = 0.0f;
  float tunedBw = 0.0f;
  float tuneAppliedHz = 0.0f;
  float bwAppliedHz = 0.0f;
  float tuneSmoothedHz = 0.0f;
  float bwSmoothedHz = 0.0f;
  float tuneTiltExtra = 0.14f;

  float autoEnv = 0.0f;
  float autoGainDb = 0.0f;
  float autoEnvAtk = 0.0f;
  float autoEnvRel = 0.0f;
  float autoGainAtk = 0.0f;
  float autoGainRel = 0.0f;

  float adjPhase = 0.0f;
  float adjBeatHz = 980.0f;
  float adjModDepth = 0.12f;
  float adjMix = 0.010f;
  float adjDrive = 1.40f;
  float adjSplatterMix = 0.14f;
  float adjEnv = 0.0f;
  float adjAtk = 0.0f;
  float adjRel = 0.0f;
  float adjTarget = 0.18f;
  float adjMinGain = 0.25f;
  float adjMaxGain = 4.0f;

  float mpPhase = 0.0f;
  float mpPhase2 = 0.0f;
  float mpDelayPhase = 0.0f;
  float mpRate = 0.035f;
  float mpRate2 = 0.027f;
  float mpDelayRate = 0.041f;
  float mpMix = 0.12f;
  float mpDepth = 0.08f;
  float mpDelayMs = 5.5f;
  float mpDelayModMs = 1.5f;
  float mpTiltMix = 0.35f;
  std::vector<float> mpBuf;
  int mpIndex = 0;

  float amSampleRate = 11025.0f;
  float amStep = 0.0f;
  float amPhase = 0.0f;
  float amPrev = 0.0f;
  float amHold = 0.0f;

  float sagEnv = 0.0f;
  float sagAtk = 0.0f;
  float sagRel = 0.0f;
  float sagStart = 0.06f;
  float sagEnd = 0.22f;
  float sagDepth = 0.04f;

  float ifTiltMix = 0.10f;
  float roomMix = 0.025f;
  float roomLpHz = 1800.0f;
  int roomIndex = 0;
  int roomDelaySamples = 0;
  std::vector<float> roomBuf;
  std::vector<int> roomTapSamples;
  std::vector<float> roomTapGains;
  float roomTailMix = 0.0f;
  float roomTailFeedback = 0.28f;
  float roomTailMs = 45.0f;
  float roomTailLpHz = 1600.0f;
  int roomTailIndex = 0;
  std::vector<float> roomTailBuf;

  Biquad ifRipple1;
  Biquad ifRipple2;
  Biquad ifTiltLp;
  Biquad adjHp;
  Biquad adjLp;
  Biquad mpTiltLp;
  Biquad amRateLp1;
  Biquad amRateLp2;
  Biquad roomLp;
  Biquad roomTailLp;

  Biquad hpf;
  Biquad lpf1;
  Biquad lpf2;
  Biquad postLpf1;
  Biquad postLpf2;
  Biquad satOsLp1;
  Biquad satOsLp2;
  Biquad speakerOsLp1;
  Biquad speakerOsLp2;
  Biquad clipOsLp1;
  Biquad clipOsLp2;
  float satOsPrev = 0.0f;
  float speakerOsPrev = 0.0f;
  float clipOsPrev = 0.0f;
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
