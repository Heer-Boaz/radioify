#include "../../../radio.h"
#include "../../math/linear_solvers.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

namespace {

constexpr float kReceiverNodeLinkOhms = 1e-3f;
constexpr int kReceiverWiperNode = 0;
constexpr int kReceiverTapNode = 1;
constexpr int kReceiverGridNode = 2;

struct ReceiverInputNetworkSolve {
  float inputCurrent = 0.0f;
  float wiperVoltage = 0.0f;
  float tapVoltage = 0.0f;
  float gridVoltage = 0.0f;
  float couplingCapVoltage = 0.0f;
};

struct ReceiverVolumeControlDerived {
  float totalResistance = 0.0f;
  float wiperResistance = 0.0f;
  float tapResistance = 0.0f;
  float loudnessBlend = 0.0f;
  int sourceNode = kReceiverWiperNode;
  int groundedNode = kReceiverTapNode;
  float sourceResistance = kReceiverNodeLinkOhms;
  float interNodeResistance = kReceiverNodeLinkOhms;
  float groundedResistance = kReceiverNodeLinkOhms;
};

ReceiverVolumeControlDerived deriveReceiverVolumeControl(
    const Radio1938::ReceiverCircuitNodeState& receiver) {
  ReceiverVolumeControlDerived control{};
  control.totalResistance =
      std::max(receiver.volumeControlResistanceOhms, kReceiverNodeLinkOhms);
  control.wiperResistance =
      clampf(receiver.volumeControlPosition, 0.0f, 1.0f) *
      control.totalResistance;
  control.tapResistance =
      std::max(receiver.volumeControlTapResistanceOhms, 0.0f);
  if (control.tapResistance > 0.0f) {
    control.loudnessBlend =
        clampf((control.tapResistance - control.wiperResistance) /
                   control.tapResistance,
               0.0f, 1.0f);
  }

  if (control.wiperResistance >= control.tapResistance) {
    control.sourceNode = kReceiverWiperNode;
    control.groundedNode = kReceiverTapNode;
    control.sourceResistance =
        std::max(control.totalResistance - control.wiperResistance,
                 kReceiverNodeLinkOhms);
    control.interNodeResistance =
        std::max(control.wiperResistance - control.tapResistance,
                 kReceiverNodeLinkOhms);
    control.groundedResistance =
        std::max(control.tapResistance, kReceiverNodeLinkOhms);
  } else {
    control.sourceNode = kReceiverTapNode;
    control.groundedNode = kReceiverWiperNode;
    control.sourceResistance =
        std::max(control.totalResistance - control.tapResistance,
                 kReceiverNodeLinkOhms);
    control.interNodeResistance =
        std::max(control.tapResistance - control.wiperResistance,
                 kReceiverNodeLinkOhms);
    control.groundedResistance =
        std::max(control.wiperResistance, kReceiverNodeLinkOhms);
  }
  return control;
}

template <size_t N>
void addGroundBranch(float (&a)[N][N], int node, float resistanceOhms) {
  if (resistanceOhms <= 0.0f) return;
  a[node][node] += 1.0f / std::max(resistanceOhms, 1e-9f);
}

template <size_t N>
void addSourceBranch(float (&a)[N][N],
                     float (&b)[N],
                     int node,
                     float resistanceOhms,
                     float sourceVoltage) {
  if (resistanceOhms <= 0.0f) return;
  float conductance = 1.0f / std::max(resistanceOhms, 1e-9f);
  a[node][node] += conductance;
  b[node] += conductance * sourceVoltage;
}

template <size_t N>
void addNodeBranch(float (&a)[N][N],
                   int aNode,
                   int bNode,
                   float resistanceOhms) {
  if (resistanceOhms <= 0.0f) return;
  float conductance = 1.0f / std::max(resistanceOhms, 1e-9f);
  a[aNode][aNode] += conductance;
  a[bNode][bNode] += conductance;
  a[aNode][bNode] -= conductance;
  a[bNode][aNode] -= conductance;
}

template <size_t N>
void stampReceiverVolumeControl(const ReceiverVolumeControlDerived& control,
                                float sourceVoltage,
                                float (&a)[N][N],
                                float (&b)[N]) {
  addSourceBranch(a, b, control.sourceNode, control.sourceResistance,
                  sourceVoltage);
  addNodeBranch(a, control.sourceNode, control.groundedNode,
                control.interNodeResistance);
  addGroundBranch(a, control.groundedNode, control.groundedResistance);
}

template <size_t N>
void stampReceiverLoudnessResistor(
    const Radio1938::ReceiverCircuitNodeState& receiver,
    const ReceiverVolumeControlDerived& control,
    float (&a)[N][N]) {
  if (control.loudnessBlend <= 0.0f ||
      receiver.volumeControlLoudnessResistanceOhms <= 0.0f) {
    return;
  }
  a[kReceiverTapNode][kReceiverTapNode] +=
      control.loudnessBlend /
      std::max(receiver.volumeControlLoudnessResistanceOhms, 1e-9f);
}

void stampReceiverLoudnessCap(
    const Radio1938::ReceiverCircuitNodeState& receiver,
    const ReceiverVolumeControlDerived& control,
    float dt,
    float (&a)[3][3],
    float (&b)[3]) {
  if (control.loudnessBlend <= 0.0f ||
      receiver.volumeControlLoudnessCapFarads <= 0.0f) {
    return;
  }
  float loudnessGc =
      control.loudnessBlend * receiver.volumeControlLoudnessCapFarads /
      std::max(dt, 1e-9f);
  a[kReceiverTapNode][kReceiverTapNode] += loudnessGc;
  b[kReceiverTapNode] += loudnessGc * receiver.volumeControlTapVoltage;
}

std::array<float, 2> computeReceiverControlDcNodes(
    const Radio1938::ReceiverCircuitNodeState& receiver,
    float sourceVoltage) {
  ReceiverVolumeControlDerived control = deriveReceiverVolumeControl(receiver);
  float a[2][2] = {};
  float b[2] = {};
  stampReceiverVolumeControl(control, sourceVoltage, a, b);
  stampReceiverLoudnessResistor(receiver, control, a);

  float det = a[0][0] * a[1][1] - a[0][1] * a[1][0];
  assert(std::fabs(det) >= 1e-12f);
  float wiperVoltage = (b[0] * a[1][1] - a[0][1] * b[1]) / det;
  float tapVoltage = (a[0][0] * b[1] - b[0] * a[1][0]) / det;
  assert(std::isfinite(wiperVoltage) && std::isfinite(tapVoltage));
  return {wiperVoltage, tapVoltage};
}

ReceiverInputNetworkSolve solveReceiverInputNetwork(
    const Radio1938::ReceiverCircuitNodeState& receiver,
    float dt,
    float sourceVoltage) {
  ReceiverInputNetworkSolve result{};
  ReceiverVolumeControlDerived control = deriveReceiverVolumeControl(receiver);
  float a[3][3] = {};
  float b[3] = {};
  stampReceiverVolumeControl(control, sourceVoltage, a, b);
  stampReceiverLoudnessResistor(receiver, control, a);
  stampReceiverLoudnessCap(receiver, control, dt, a, b);
  addGroundBranch(a, kReceiverGridNode, receiver.gridLeakResistanceOhms);
  float couplingGc = receiver.couplingCapFarads / std::max(dt, 1e-9f);
  a[kReceiverWiperNode][kReceiverWiperNode] += couplingGc;
  a[kReceiverGridNode][kReceiverGridNode] += couplingGc;
  a[kReceiverWiperNode][kReceiverGridNode] -= couplingGc;
  a[kReceiverGridNode][kReceiverWiperNode] -= couplingGc;
  b[kReceiverWiperNode] += couplingGc * receiver.couplingCapVoltage;
  b[kReceiverGridNode] -= couplingGc * receiver.couplingCapVoltage;
  float nodeVoltages[3] = {};
  bool solved = solveLinear3x3(a, b, nodeVoltages);
  assert(solved);
  (void)solved;
  result.wiperVoltage = nodeVoltages[kReceiverWiperNode];
  result.tapVoltage = nodeVoltages[kReceiverTapNode];
  result.gridVoltage = nodeVoltages[kReceiverGridNode];
  result.couplingCapVoltage =
      nodeVoltages[kReceiverWiperNode] - nodeVoltages[kReceiverGridNode];
  result.inputCurrent =
      (sourceVoltage - nodeVoltages[control.sourceNode]) /
      std::max(control.sourceResistance, kReceiverNodeLinkOhms);
  return result;
}

float computeReceiverDetectorLoadConductance(
    const Radio1938::ReceiverCircuitNodeState& receiver) {
  if (!receiver.enabled) return 0.0f;

  float totalResistance = std::max(receiver.volumeControlResistanceOhms, 1e-6f);
  float volumePosition = clampf(receiver.volumeControlPosition, 0.0f, 1.0f);
  float wiperResistance = volumePosition * totalResistance;
  float sourceToWiperResistance =
      std::max(totalResistance - wiperResistance, 0.0f);

  float detectorLoadResistance = totalResistance;
  float gridLeakResistance = std::max(receiver.gridLeakResistanceOhms, 1e-6f);
  float gridPathResistance =
      std::max(sourceToWiperResistance + gridLeakResistance, 1e-6f);
  detectorLoadResistance =
      parallelResistance(detectorLoadResistance, gridPathResistance);

  if (receiver.volumeControlTapResistanceOhms > 0.0f &&
      receiver.volumeControlLoudnessResistanceOhms > 0.0f) {
    float tapResistance = receiver.volumeControlTapResistanceOhms;
    float loudnessBlend =
        clampf((tapResistance - wiperResistance) / tapResistance, 0.0f, 1.0f);
    if (loudnessBlend > 0.0f) {
      float loudnessResistance =
          std::max(receiver.volumeControlLoudnessResistanceOhms /
                       std::max(loudnessBlend, 1e-6f),
                   1e-6f);
      detectorLoadResistance =
          parallelResistance(detectorLoadResistance, loudnessResistance);
    }
  }

  return 1.0f / std::max(detectorLoadResistance, 1e-6f);
}

void updateDetectorLoadState(Radio1938::ReceiverCircuitNodeState& receiver,
                             const ReceiverInputNetworkSolve& solve) {
  receiver.gridVoltage = solve.gridVoltage;
  receiver.volumeControlTapVoltage = solve.tapVoltage;
  receiver.couplingCapVoltage = solve.couplingCapVoltage;
  receiver.warmStartPending = false;
}

void primeReceiverInputWarmStart(Radio1938::ReceiverCircuitNodeState& receiver,
                                 float detectorNode) {
  auto dcNodes = computeReceiverControlDcNodes(receiver, detectorNode);
  receiver.volumeControlTapVoltage = dcNodes[1];
  // Warm start at the control-network DC operating point so the first-audio
  // coupling capacitor does not inject a false startup transient into the grid.
  receiver.couplingCapVoltage = dcNodes[0];
  receiver.gridVoltage = 0.0f;
}

}  // namespace

void RadioReceiverInputNetworkNode::init(Radio1938& radio, RadioInitContext&) {
  radio.receiverCircuit.detectorLoadConductance =
      computeReceiverDetectorLoadConductance(radio.receiverCircuit);
}

void RadioReceiverInputNetworkNode::reset(Radio1938& radio) {
  auto& receiver = radio.receiverCircuit;
  receiver.detectorLoadConductance =
      computeReceiverDetectorLoadConductance(receiver);
}

float RadioReceiverInputNetworkNode::run(Radio1938& radio,
                                         float y,
                                         RadioSampleContext&) {
  auto& receiver = radio.receiverCircuit;
  receiver.detectorLoadConductance =
      computeReceiverDetectorLoadConductance(receiver);
  if (!receiver.enabled) return y;

  float detectorNode = radio.demod.am.detectorNode;
  float dt = 1.0f / std::max(radio.sampleRate, 1.0f);
  if (receiver.warmStartPending) {
    primeReceiverInputWarmStart(receiver, detectorNode);
  }

  auto solve = solveReceiverInputNetwork(receiver, dt, detectorNode);
  updateDetectorLoadState(receiver, solve);
  return y;
}
