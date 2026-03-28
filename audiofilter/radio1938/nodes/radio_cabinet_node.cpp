#include "../../../radio.h"

#include <algorithm>
#include <cassert>
#include <cmath>

void RadioCabinetNode::init(Radio1938& radio, RadioInitContext&) {
  auto& cabinet = radio.cabinet;
  float panelHzDerived =
      cabinet.panelHz * (1.0f + 0.52f * cabinet.panelStiffnessTolerance);
  float chassisHzDerived =
      cabinet.chassisHz * (1.0f + 0.28f * cabinet.panelStiffnessTolerance -
                           0.18f * cabinet.baffleLeakTolerance);
  float cavityDipHzDerived =
      cabinet.cavityDipHz * (1.0f + 0.35f * cabinet.cavityTolerance);
  float rearDelayMsDerived =
      cabinet.rearDelayMs *
      (1.0f + 0.30f * cabinet.rearPathTolerance +
       0.12f * cabinet.baffleLeakTolerance);
  cabinet.rearMixApplied =
      cabinet.rearMix * (1.0f + 0.42f * cabinet.baffleLeakTolerance -
                         0.25f * cabinet.grilleClothTolerance);

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
    float grilleLpHzDerived =
        cabinet.grilleLpHz / (1.0f + 0.55f * cabinet.grilleClothTolerance);
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
        static_cast<int>(std::ceil(rearDelayMsDerived * 0.001f *
                                   radio.sampleRate)) +
        cabinet.bufferGuardSamples;
    if (rearSamples > 0) {
      cabinet.buf.assign(static_cast<size_t>(rearSamples), 0.0f);
    } else {
      cabinet.buf.clear();
    }
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

float RadioCabinetNode::run(Radio1938& radio, float y, RadioSampleContext&) {
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
