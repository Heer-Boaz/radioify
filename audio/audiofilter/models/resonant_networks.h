#ifndef RADIOIFY_AUDIOFILTER_MODELS_RESONANT_NETWORKS_H
#define RADIOIFY_AUDIOFILTER_MODELS_RESONANT_NETWORKS_H

#include <array>

// Narrowband envelope of one parallel tuned RF circuit on the broadcast dial.
// The gang and trimmers align the circuit to the dial frequency, so it is
// centred in the receiver frame and both quadratures share one real system.
// L, unloaded coil Q and the reflected shunt load set the loaded Q there:
// 1/QL = 1/Qcoil + w*L/Rshunt, with envelope half-bandwidth w/(2*QL).
// Gain at resonance is one; the caller declares the absolute stage gain.
// A first-order digital filter cannot follow this curve once the half-
// bandwidth is a sizeable part of the envelope rate, so a minimum-phase biquad
// matches its magnitude at DC, .1, .2, .3 fs and Nyquist (within .005 dB up to
// .3 fs, .35 dB up to .45 fs). The phase differs from the circuit by a nearly
// constant sub-sample advance, which delays every sideband alike.
class TunedRfCircuit {
 public:
  void configure(double sampleRate, double dialHz, double inductanceHenries,
                 double coilQ, double shuntLoadOhms);
  std::array<float, 2> process(float i, float q);
  void reset();
  double loadedQ() const { return loadedQ_; }
  double halfBandwidthHz() const { return halfBandwidthHz_; }

 private:
  std::array<double, 3> b_{1, 0, 0};
  std::array<double, 2> a_{};
  double loadedQ_ = 0, halfBandwidthHz_ = 0;
  std::array<std::array<double, 2>, 2> state_{};
};

#endif  // RADIOIFY_AUDIOFILTER_MODELS_RESONANT_NETWORKS_H
