#ifndef RADIOIFY_TOOLS_RECEIVER_INPUT_REFERENCE_H
#define RADIOIFY_TOOLS_RECEIVER_INPUT_REFERENCE_H

#include "radio.h"

#include <algorithm>
#include <complex>

// Independent reference for the tapped volume control, built from series and
// parallel impedances only: no conductance matrix and no tiny-resistor
// stand-ins, so coincident wiper and tap (and both end stops) stay exact.
// `s` is the Laplace variable, or the exact z-domain image of a trapezoidal
// capacitor when comparing against the discrete solver.
struct ReceiverInputReference {
  std::complex<double> grid, inputCurrent;
};

inline ReceiverInputReference receiverInputReference(
    const Radio1938::ReceiverCircuitNodeState& r, std::complex<double> s,
    bool withLoudnessBranch = true) {
  using C = std::complex<double>;
  const double total = r.volumeControlResistanceOhms;
  const double w = std::clamp<double>(r.volumeControlPosition, 0, 1) * total;
  const double t = r.volumeControlTapResistanceOhms;
  const C yw = C{1, 0} / (double(r.gridLeakResistanceOhms) +
                          C{1, 0} / (s * double(r.couplingCapFarads)));
  // Series R-C loudness branch from the tap; C = 0 is a plain resistor.
  const double rl = r.volumeControlLoudnessResistanceOhms;
  const double cl = r.volumeControlLoudnessCapFarads;
  const C yt = !withLoudnessBranch || t <= 0 ? C{}
               : cl > 0                     ? C{1, 0} / (rl + C{1, 0} / (s * cl))
               : rl > 0                     ? C{1.0 / rl, 0}
                                            : C{};
  const bool wiperAbove = w >= t;
  const double bottom = std::min(w, t), middle = std::abs(w - t);
  const C bottomZ = bottom / (C{1, 0} + bottom * (wiperAbove ? yt : yw));
  const C lowerZ = middle + bottomZ;
  const C upperZ = lowerZ / (C{1, 0} + lowerZ * (wiperAbove ? yw : yt));
  const C current = C{1, 0} / (total - std::max(w, t) + upperZ);
  const C upperV = current * upperZ;
  const C lowerV = std::abs(lowerZ) > 0 ? upperV * bottomZ / lowerZ : C{};
  return {(wiperAbove ? upperV : lowerV) * yw * double(r.gridLeakResistanceOhms), current};
}

#endif  // RADIOIFY_TOOLS_RECEIVER_INPUT_REFERENCE_H
