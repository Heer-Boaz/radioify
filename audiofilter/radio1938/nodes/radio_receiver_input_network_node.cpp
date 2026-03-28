#include "../../../radio.h"
#include "../../math/linear_solvers.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

namespace {

struct ReceiverInputNetworkSolve {
  float inputCurrent = 0.0f;
  float wiperVoltage = 0.0f;
  float tapVoltage = 0.0f;
  float gridVoltage = 0.0f;
  float couplingCapVoltage = 0.0f;
};

std::array<float, 2> computeReceiverControlDcNodes(
    const Radio1938::ReceiverCircuitNodeState& receiver,
    float sourceVoltage) {
  float totalResistance = receiver.volumeControlResistanceOhms;
  float wiperResistance = receiver.volumeControlPosition * totalResistance;
  float tapResistance = receiver.volumeControlTapResistanceOhms;
  float loudnessBlend = 0.0f;
  if (tapResistance > 0.0f) {
    loudnessBlend = clampf((tapResistance - wiperResistance) / tapResistance,
                           0.0f, 1.0f);
  }

  constexpr float kNodeLinkOhms = 1e-3f;
  auto addGroundBranch = [](float (&a)[2][2], int node, float resistanceOhms) {
    if (resistanceOhms <= 0.0f) return;
    a[node][node] += 1.0f / std::max(resistanceOhms, 1e-9f);
  };
  auto addSourceBranch = [](float (&a)[2][2], float (&b)[2], int node,
                            float resistanceOhms, float sourceVoltageIn) {
    if (resistanceOhms <= 0.0f) return;
    float conductance = 1.0f / std::max(resistanceOhms, 1e-9f);
    a[node][node] += conductance;
    b[node] += conductance * sourceVoltageIn;
  };
  auto addNodeBranch = [](float (&a)[2][2], int aNode, int bNode,
                          float resistanceOhms) {
    if (resistanceOhms <= 0.0f) return;
    float conductance = 1.0f / std::max(resistanceOhms, 1e-9f);
    a[aNode][aNode] += conductance;
    a[bNode][bNode] += conductance;
    a[aNode][bNode] -= conductance;
    a[bNode][aNode] -= conductance;
  };

  float a[2][2] = {};
  float b[2] = {};
  constexpr int kWiperNode = 0;
  constexpr int kTapNode = 1;
  if (wiperResistance >= tapResistance) {
    addSourceBranch(a, b, kWiperNode,
                    std::max(totalResistance - wiperResistance, kNodeLinkOhms),
                    sourceVoltage);
    addNodeBranch(a, kWiperNode, kTapNode,
                  std::max(wiperResistance - tapResistance, kNodeLinkOhms));
    addGroundBranch(a, kTapNode, std::max(tapResistance, kNodeLinkOhms));
  } else {
    addSourceBranch(a, b, kTapNode,
                    std::max(totalResistance - tapResistance, kNodeLinkOhms),
                    sourceVoltage);
    addNodeBranch(a, kTapNode, kWiperNode,
                  std::max(tapResistance - wiperResistance, kNodeLinkOhms));
    addGroundBranch(a, kWiperNode, std::max(wiperResistance, kNodeLinkOhms));
  }
  if (loudnessBlend > 0.0f &&
      receiver.volumeControlLoudnessResistanceOhms > 0.0f) {
    a[kTapNode][kTapNode] +=
        loudnessBlend /
        std::max(receiver.volumeControlLoudnessResistanceOhms, 1e-9f);
  }

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
  float totalResistance = receiver.volumeControlResistanceOhms;
  float wiperResistance = receiver.volumeControlPosition * totalResistance;
  float tapResistance = receiver.volumeControlTapResistanceOhms;
  float loudnessBlend = 0.0f;
  if (tapResistance > 0.0f) {
    loudnessBlend = clampf((tapResistance - wiperResistance) / tapResistance,
                           0.0f, 1.0f);
  }

  constexpr float kNodeLinkOhms = 1e-3f;
  auto addGroundBranch = [](float (&a)[3][3], int node, float resistanceOhms) {
    if (resistanceOhms <= 0.0f) return;
    a[node][node] += 1.0f / std::max(resistanceOhms, 1e-9f);
  };
  auto addSourceBranch = [](float (&a)[3][3], float (&b)[3], int node,
                            float resistanceOhms, float sourceVoltageIn) {
    if (resistanceOhms <= 0.0f) return;
    float conductance = 1.0f / std::max(resistanceOhms, 1e-9f);
    a[node][node] += conductance;
    b[node] += conductance * sourceVoltageIn;
  };
  auto addNodeBranch = [](float (&a)[3][3], int aNode, int bNode,
                          float resistanceOhms) {
    if (resistanceOhms <= 0.0f) return;
    float conductance = 1.0f / std::max(resistanceOhms, 1e-9f);
    a[aNode][aNode] += conductance;
    a[bNode][bNode] += conductance;
    a[aNode][bNode] -= conductance;
    a[bNode][aNode] -= conductance;
  };

  float a[3][3] = {};
  float b[3] = {};
  constexpr int kWiperNode = 0;
  constexpr int kTapNode = 1;
  constexpr int kGridNode = 2;
  int sourceNode = kWiperNode;
  float sourceResistance = kNodeLinkOhms;
  if (wiperResistance >= tapResistance) {
    sourceNode = kWiperNode;
    sourceResistance =
        std::max(totalResistance - wiperResistance, kNodeLinkOhms);
    addSourceBranch(a, b, kWiperNode, sourceResistance, sourceVoltage);
    addNodeBranch(a, kWiperNode, kTapNode,
                  std::max(wiperResistance - tapResistance, kNodeLinkOhms));
    addGroundBranch(a, kTapNode, std::max(tapResistance, kNodeLinkOhms));
  } else {
    sourceNode = kTapNode;
    sourceResistance = std::max(totalResistance - tapResistance, kNodeLinkOhms);
    addSourceBranch(a, b, kTapNode, sourceResistance, sourceVoltage);
    addNodeBranch(a, kTapNode, kWiperNode,
                  std::max(tapResistance - wiperResistance, kNodeLinkOhms));
    addGroundBranch(a, kWiperNode, std::max(wiperResistance, kNodeLinkOhms));
  }
  if (loudnessBlend > 0.0f &&
      receiver.volumeControlLoudnessResistanceOhms > 0.0f) {
    a[kTapNode][kTapNode] +=
        loudnessBlend /
        std::max(receiver.volumeControlLoudnessResistanceOhms, 1e-9f);
  }
  if (loudnessBlend > 0.0f &&
      receiver.volumeControlLoudnessCapFarads > 0.0f) {
    float loudnessGc =
        loudnessBlend * receiver.volumeControlLoudnessCapFarads /
        std::max(dt, 1e-9f);
    a[kTapNode][kTapNode] += loudnessGc;
    b[kTapNode] += loudnessGc * receiver.volumeControlTapVoltage;
  }
  addGroundBranch(a, kGridNode, receiver.gridLeakResistanceOhms);
  float couplingGc = receiver.couplingCapFarads / std::max(dt, 1e-9f);
  a[kWiperNode][kWiperNode] += couplingGc;
  a[kGridNode][kGridNode] += couplingGc;
  a[kWiperNode][kGridNode] -= couplingGc;
  a[kGridNode][kWiperNode] -= couplingGc;
  b[kWiperNode] += couplingGc * receiver.couplingCapVoltage;
  b[kGridNode] -= couplingGc * receiver.couplingCapVoltage;
  float nodeVoltages[3] = {};
  bool solved = solveLinear3x3(a, b, nodeVoltages);
  assert(solved);
  result.wiperVoltage = nodeVoltages[kWiperNode];
  result.tapVoltage = nodeVoltages[kTapNode];
  result.gridVoltage = nodeVoltages[kGridNode];
  result.couplingCapVoltage =
      nodeVoltages[kWiperNode] - nodeVoltages[kGridNode];
  result.inputCurrent =
      (sourceVoltage - nodeVoltages[sourceNode]) /
      std::max(sourceResistance, kNodeLinkOhms);
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
    auto dcNodes = computeReceiverControlDcNodes(receiver, detectorNode);
    receiver.volumeControlTapVoltage = dcNodes[1];
    receiver.couplingCapVoltage = dcNodes[0];
    receiver.gridVoltage = 0.0f;
  }

  auto solve = solveReceiverInputNetwork(receiver, dt, detectorNode);
  updateDetectorLoadState(receiver, solve);
  return y;
}
