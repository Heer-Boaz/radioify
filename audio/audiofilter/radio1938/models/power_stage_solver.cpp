#include "power_stage_solver.h"
#include "power_stage_internal.h"

#include "../../math/signal_math.h"
#include "../../models/tube_models.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace power_stage_internal {

TubeGridBranchEval evaluateTubeGridBranch(double acGridVolts, double biasVolts,
                                          double gridLeakConductance,
                                          double gridCurrentConductance) {
  double slope = gridLeakConductance;
  double current = acGridVolts * gridLeakConductance;
  double conductingVolts = biasVolts + acGridVolts;
  if (conductingVolts > 0.0f) {
    current += conductingVolts * gridCurrentConductance;
    slope += gridCurrentConductance;
  }
  TubeGridBranchEval result{};
  result.current = current;
  result.slope = slope;
  return result;
}

TriodeLutView makeTriodeLutView(const KorenTriodeLut& lut) {
  TriodeLutView view{};
  if (!lut.valid()) {
    return view;
  }
  view.currentAmps = lut.currentAmps.data();
  view.vgkMin = lut.vgkMin;
  view.vgkMax = lut.vgkMax;
  view.vpkMin = lut.vpkMin;
  view.vpkMax = lut.vpkMax;
  view.vgkInvStep = lut.vgkInvStep;
  view.vpkInvStep = lut.vpkInvStep;
  view.vgkBins = lut.vgkBins;
  view.vpkBins = lut.vpkBins;
  view.valid = true;
  return view;
}

KorenTriodePlateEval evaluateTriodePlateFast(double vgk, double vpk,
                                             const KorenTriodeModel& model,
                                             const KorenTriodeLut& lut,
                                             const TriodeLutView& view) {
  if (!view.valid) {
    return evaluateKorenTriodePlateRuntime(vgk, vpk, model, lut);
  }

  // Keep interpolation continuous at the Newton solver's voltage resolution.
  // Float quantization of plate voltage/current can stall a smaller update.
  double clampedVgk = std::clamp<double>(vgk, view.vgkMin, view.vgkMax);
  double clampedVpk = std::clamp<double>(vpk, view.vpkMin, view.vpkMax);
  double x = (clampedVgk - view.vgkMin) * view.vgkInvStep;
  double y = (clampedVpk - view.vpkMin) * view.vpkInvStep;
  int x0 = std::clamp(static_cast<int>(x), 0, view.vgkBins - 2);
  int y0 = std::clamp(static_cast<int>(y), 0, view.vpkBins - 2);
  double tx = x - x0;
  double ty = y - y0;
  int x1 = x0 + 1;
  int y1 = y0 + 1;

  size_t row0 = static_cast<size_t>(y0) * static_cast<size_t>(view.vgkBins);
  size_t row1 = static_cast<size_t>(y1) * static_cast<size_t>(view.vgkBins);
  double i00 = view.currentAmps[row0 + static_cast<size_t>(x0)];
  double i10 = view.currentAmps[row0 + static_cast<size_t>(x1)];
  double i01 = view.currentAmps[row1 + static_cast<size_t>(x0)];
  double i11 = view.currentAmps[row1 + static_cast<size_t>(x1)];

  double i0 = i00 + (i10 - i00) * tx;
  double i1 = i01 + (i11 - i01) * tx;

  KorenTriodePlateEval eval{};
  eval.currentAmps = i0 + (i1 - i0) * ty;
  // Newton needs the derivative of the interpolated current being solved,
  // not a separately interpolated derivative of the original tube equation.
  eval.conductanceSiemens =
      (vpk >= view.vpkMin && vpk <= view.vpkMax) ? (i1 - i0) * view.vpkInvStep : 0.0;
  return eval;
}

OutputTubePairEval evaluateOutputTubePair(const Radio1938::PowerNodeState& power,
                                          const TriodeLutView& outputTriodeLut,
                                          float outputPlateQuiescent, float gridA,
                                          float gridB, double primaryVoltage) {
  double plateA = outputPlateQuiescent - 0.5 * primaryVoltage;
  double plateB = outputPlateQuiescent + 0.5 * primaryVoltage;
  KorenTriodePlateEval evalA = evaluateTriodePlateFast(
      power.outputTubeBiasVolts + gridA, plateA, power.outputTubeTriodeModel,
      power.outputTubeTriodeLut, outputTriodeLut);
  KorenTriodePlateEval evalB = evaluateTriodePlateFast(
      power.outputTubeBiasVolts + gridB, plateB, power.outputTubeTriodeModel,
      power.outputTubeTriodeLut, outputTriodeLut);

  OutputTubePairEval result{};
  result.plateCurrentA = evalA.currentAmps;
  result.plateCurrentB = evalB.currentAmps;
  result.driveCurrent = 0.5 * (result.plateCurrentA - result.plateCurrentB);
  result.driveSlope = -0.25 * (evalA.conductanceSiemens + evalB.conductanceSiemens);
  return result;
}

}  // namespace power_stage_internal

namespace {

float deriveSpeakerCompliance(float movingMassKg, float suspensionHz) {
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return 1.0f / std::max(movingMassKg * w0 * w0, 1e-9f);
}

float deriveSpeakerMechanicalDamping(float movingMassKg,
                                     float suspensionHz,
                                     float suspensionQ) {
  float q = std::max(suspensionQ, 0.35f);
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return (movingMassKg * w0) / q;
}

float deriveSpeakerVoiceCoilInductance(float voiceCoilResistanceOhms,
                                       float topLpHz) {
  float lpHz = std::max(topLpHz, 2500.0f);
  return voiceCoilResistanceOhms / (kRadioTwoPi * lpHz);
}

float deriveSpeakerForceFactor(float movingMassKg,
                               float suspensionHz,
                               float voiceCoilResistanceOhms) {
  constexpr float kElectricalQ = 0.95f;
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return std::sqrt(std::max(w0 * movingMassKg * voiceCoilResistanceOhms /
                                kElectricalQ,
                            1e-9f));
}

float deriveSpeakerMechanicalDampingFromQ(float movingMassKg,
                                          float suspensionHz,
                                          float mechanicalQ) {
  float q = std::max(mechanicalQ, 0.35f);
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return (movingMassKg * w0) / q;
}

float deriveSpeakerForceFactorFromQ(float movingMassKg,
                                    float suspensionHz,
                                    float voiceCoilResistanceOhms,
                                    float electricalQ) {
  float q = std::max(electricalQ, 0.35f);
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return std::sqrt(std::max(w0 * movingMassKg * voiceCoilResistanceOhms / q,
                            1e-9f));
}

OutputPrimarySolveResult solveOutputPrimaryAffineWithLut(
    const AffineTransformerProjection& projection, const Radio1938::PowerNodeState& power,
    const power_stage_internal::TriodeLutView& outputTriodeLut,
    float outputPlateQuiescent, float gridA, float gridB, float initialPrimaryVoltage) {
  double primaryVoltage = initialPrimaryVoltage;
  constexpr float kPrimaryVoltageTolerance = 5e-5f;
  int iterations = 0;

  for (int iter = 0; iter < 8; ++iter) {
    iterations = iter + 1;
    power_stage_internal::OutputTubePairEval pair =
        power_stage_internal::evaluateOutputTubePair(
            power, outputTriodeLut, outputPlateQuiescent, gridA, gridB, primaryVoltage);

    double f = projection.base.primaryVoltage +
               projection.slope.primaryVoltage * pair.driveCurrent - primaryVoltage;
    double df = projection.slope.primaryVoltage * pair.driveSlope - 1.0;
    assert(std::isfinite(df) && std::fabs(df) >= 1e-9f);

    double deltaPrimaryVoltage = f / df;
    primaryVoltage -= deltaPrimaryVoltage;
    assert(std::isfinite(primaryVoltage));
    if (std::fabs(deltaPrimaryVoltage) < kPrimaryVoltageTolerance) break;
  }

  power_stage_internal::OutputTubePairEval finalPair =
      power_stage_internal::evaluateOutputTubePair(
          power, outputTriodeLut, outputPlateQuiescent, gridA, gridB, primaryVoltage);
  OutputPrimarySolveResult result{};
  result.primaryVoltage = primaryVoltage;
  result.driveCurrent = finalPair.driveCurrent;
  result.plateCurrentA = finalPair.plateCurrentA;
  result.plateCurrentB = finalPair.plateCurrentB;
  result.iterations = iterations;
  return result;
}

}  // namespace

OutputPrimarySolveResult solveOutputPrimaryAffine(
    const AffineTransformerProjection& projection, const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent, float gridA, float gridB, float initialPrimaryVoltage) {
  const power_stage_internal::TriodeLutView outputTriodeLut =
      power_stage_internal::makeTriodeLutView(power.outputTubeTriodeLut);
  return solveOutputPrimaryAffineWithLut(projection, power, outputTriodeLut,
                                         outputPlateQuiescent, gridA, gridB,
                                         initialPrimaryVoltage);
}

namespace {

struct SingleEndedPrimarySolveResult {
  float primaryVoltage = 0.0f;
  float driveCurrent = 0.0f;
  float plateCurrent = 0.0f;
  int iterations = 0;
};

float evaluateSingleEndedPlateCurrent(
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float outputGridVolts,
    float primaryVoltage) {
  float gridControlledCurrent =
      power.outputTubePentodeCurrentForGrid.eval(
          power.outputTubeBiasVolts + outputGridVolts);
  // The 41 screen is fed from B+: use the same screen-current scaling as
  // deviceTubePlateCurrent, with the instantaneous supply perturbation.
  const float screenVolts = std::max(0.0f, power.outputTubeScreenVolts +
      outputPlateQuiescent - power.outputTubeQuiescentPlateVolts);
  gridControlledCurrent *= screenVolts / power.outputTubeScreenVolts;
  float plateVoltage = std::max(outputPlateQuiescent - primaryVoltage, 0.0f);
  float plateFeedbackCurrent =
      (plateVoltage - power.outputTubeQuiescentPlateVolts) /
      requirePositiveFinite(power.outputTubePlateResistanceOhms);
  return std::max(gridControlledCurrent + plateFeedbackCurrent, 0.0f);
}

SingleEndedPrimarySolveResult solveSingleEndedPrimaryAffine(
    const AffineTransformerProjection& projection,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float outputGridVolts,
    float initialPrimaryVoltage) {
  float primaryVoltage = initialPrimaryVoltage;
  constexpr float kPrimaryVoltageTolerance = 5e-5f;
  int iterations = 0;

  for (int iter = 0; iter < 4; ++iter) {
    iterations = iter + 1;
    float plateCurrent = evaluateSingleEndedPlateCurrent(
        power, outputPlateQuiescent, outputGridVolts, primaryVoltage);
    float driveCurrent =
        plateCurrent - power.outputTubeQuiescentPlateCurrentAmps;
    float f = projection.base.primaryVoltage +
              projection.slope.primaryVoltage * driveCurrent - primaryVoltage;
    bool plateConducting = plateCurrent > 0.0f &&
                           outputPlateQuiescent - primaryVoltage > 0.0f;
    float driveSlope =
        plateConducting
            ? -1.0f /
                  requirePositiveFinite(power.outputTubePlateResistanceOhms)
            : 0.0f;
    float df = projection.slope.primaryVoltage * driveSlope - 1.0f;
    assert(std::isfinite(df) && std::fabs(df) >= 1e-9f);
    float deltaPrimaryVoltage = f / df;
    primaryVoltage -= deltaPrimaryVoltage;
    assert(std::isfinite(primaryVoltage));
    if (std::fabs(deltaPrimaryVoltage) < kPrimaryVoltageTolerance) break;
  }

  float plateCurrent = evaluateSingleEndedPlateCurrent(
      power, outputPlateQuiescent, outputGridVolts, primaryVoltage);
  SingleEndedPrimarySolveResult result{};
  result.primaryVoltage = primaryVoltage;
  result.driveCurrent =
      plateCurrent - power.outputTubeQuiescentPlateCurrentAmps;
  result.plateCurrent = plateCurrent;
  result.iterations = iterations;
  return result;
}

}  // namespace

void configureSpeakerElectromechanics(SpeakerSim& speaker,
                                      float sampleRate,
                                      float nominalLoadOhms) {
  speaker.electricalSampleRate = sampleRate;
  speaker.nominalLoadOhms = std::max(nominalLoadOhms, 1e-3f);
  speaker.effectiveVoiceCoilResistanceOhms =
      speaker.voiceCoilResistanceOhms > 0.0f
          ? speaker.voiceCoilResistanceOhms
          : 0.82f * speaker.nominalLoadOhms;
  speaker.effectiveMovingMassKg =
      speaker.movingMassKg > 0.0f ? speaker.movingMassKg : 0.014f;
  speaker.effectiveSuspensionComplianceMetersPerNewton =
      speaker.suspensionComplianceMetersPerNewton > 0.0f
          ? speaker.suspensionComplianceMetersPerNewton
          : deriveSpeakerCompliance(speaker.effectiveMovingMassKg,
                                    speaker.suspensionHz);
  if (speaker.mechanicalDampingNsPerMeter > 0.0f) {
    speaker.effectiveMechanicalDampingNsPerMeter =
        speaker.mechanicalDampingNsPerMeter;
  } else if (speaker.mechanicalQ > 0.0f) {
    speaker.effectiveMechanicalDampingNsPerMeter =
        deriveSpeakerMechanicalDampingFromQ(speaker.effectiveMovingMassKg,
                                            speaker.suspensionHz,
                                            speaker.mechanicalQ);
  } else {
    speaker.effectiveMechanicalDampingNsPerMeter =
        deriveSpeakerMechanicalDamping(speaker.effectiveMovingMassKg,
                                       speaker.suspensionHz,
                                       speaker.suspensionQ);
  }
  speaker.effectiveVoiceCoilInductanceHenries =
      speaker.voiceCoilInductanceHenries > 0.0f
          ? speaker.voiceCoilInductanceHenries
          : deriveSpeakerVoiceCoilInductance(
                speaker.effectiveVoiceCoilResistanceOhms, speaker.topLpHz);
  if (speaker.forceFactorBl > 0.0f) {
    speaker.effectiveForceFactorBl = speaker.forceFactorBl;
  } else if (speaker.electricalQ > 0.0f) {
    speaker.effectiveForceFactorBl = deriveSpeakerForceFactorFromQ(
        speaker.effectiveMovingMassKg, speaker.suspensionHz,
        speaker.effectiveVoiceCoilResistanceOhms, speaker.electricalQ);
  } else {
    speaker.effectiveForceFactorBl =
        deriveSpeakerForceFactor(speaker.effectiveMovingMassKg,
                                 speaker.suspensionHz,
                                 speaker.effectiveVoiceCoilResistanceOhms);
  }
  constexpr float kLoadSenseMs = 1.0f;
  speaker.loadSenseCoeff =
      std::exp(-1.0f /
               (std::max(sampleRate, 1.0f) * (kLoadSenseMs / 1000.0f)));
  speaker.effectiveLoadOhms = speaker.nominalLoadOhms;
}

void resetSpeakerElectromechanics(SpeakerSim& speaker) {
  speaker.electricalCurrentAmps = 0.0f;
  speaker.coneVelocityMetersPerSecond = 0.0f;
  speaker.coneDisplacementMeters = 0.0f;
  speaker.backEmfVolts = 0.0f;
  speaker.coilTemperatureRiseKelvin = 0.0f;
  speaker.acousticDriveVolts = 0.0f;
  speaker.solverNonconvergenceCount = 0;
  speaker.solverMaxIterations = 0;
  speaker.solverMaxResidualVolts = 0.0f;
  speaker.loadSenseVoltage = 0.0f;
  speaker.loadSenseCurrent = 0.0f;
  speaker.effectiveLoadOhms = std::max(speaker.nominalLoadOhms, 1e-3f);
}

namespace {

struct SpeakerConstitutive {
  double bl;
  double blDerivative;
  double restoringForce;
  double stiffness;
};

SpeakerConstitutive speakerConstitutive(const SpeakerSim& s, double x) {
  const double motorScale = s.motorTravelMeters > 0.0f
                               ? 1.0 / s.motorTravelMeters : 0.0;
  const double suspensionScale = s.suspensionTravelMeters > 0.0f
                                    ? 1.0 / s.suspensionTravelMeters : 0.0;
  const double u = x * motorScale;
  const double w = x * suspensionScale;
  const double bl = s.effectiveForceFactorBl / (1.0 + u * u);
  const double k = 1.0 / s.effectiveSuspensionComplianceMetersPerNewton;
  // Positive Bl and a convex spring potential at every displacement. The
  // same Bl(x) multiplies current (force) and velocity (back EMF).
  return {bl, -2.0 * bl * u * motorScale / (1.0 + u * u),
          k * x * (1.0 + w * w), k * (1.0 + 3.0 * w * w)};
}

double speakerHotResistance(const SpeakerSim& s) {
  constexpr double kCopperTemperatureCoefficient = 0.00393;
  return s.effectiveVoiceCoilResistanceOhms *
         (1.0 + kCopperTemperatureCoefficient * s.coilTemperatureRiseKelvin);
}

SpeakerMotion solveSpeakerMotion(const SpeakerElectricalLinearization& l,
                                 double volts) {
  const double drive = volts + l.rhsElectrical;
  return {(drive * l.mechanicalCoeff - l.electricalCoupling * l.rhsMechanical) /
              l.determinant,
          (l.electricalCoeff * l.rhsMechanical + l.forceFactorBl * drive) /
              l.determinant};
}

double speakerResidualVolts(const SpeakerSim& s, const SpeakerMotion& next,
                            double dt, double volts) {
  const auto law = speakerConstitutive(
      s, s.coneDisplacementMeters + dt * next.velocityMetersPerSecond);
  const double electrical = s.effectiveVoiceCoilInductanceHenries / dt *
      (next.currentAmps - s.electricalCurrentAmps) +
      speakerHotResistance(s) * next.currentAmps +
      law.bl * next.velocityMetersPerSecond - volts;
  const double mechanical = s.effectiveMovingMassKg / dt *
      (next.velocityMetersPerSecond - s.coneVelocityMetersPerSecond) +
      s.effectiveMechanicalDampingNsPerMeter * next.velocityMetersPerSecond +
      law.restoringForce - law.bl * next.currentAmps;
  return std::max(std::fabs(electrical), std::fabs(mechanical) *
      s.effectiveVoiceCoilResistanceOhms / s.effectiveForceFactorBl);
}

constexpr unsigned kSpeakerMaxIterations = 8;

bool speakerConverged(SpeakerSim& s, unsigned iteration, double residual,
                       double volts) {
  const bool converged = residual <= 1e-6 * (1.0 + std::fabs(volts));
  if (converged || iteration == kSpeakerMaxIterations) {
    s.solverMaxIterations = std::max(s.solverMaxIterations, iteration);
    s.solverMaxResidualVolts = std::max(s.solverMaxResidualVolts,
                                       static_cast<float>(residual));
    if (!converged) ++s.solverNonconvergenceCount;
  }
  return converged;
}

}  // namespace

SpeakerElectricalLinearization linearizeSpeakerElectricalLoad(
    const SpeakerSim& s, double dt, const SpeakerMotion& estimate) {
  const double v = estimate.velocityMetersPerSecond;
  const auto law = speakerConstitutive(s, s.coneDisplacementMeters + dt * v);
  SpeakerElectricalLinearization l{};
  l.dt = dt;
  l.forceFactorBl = law.bl;
  l.electricalCoeff = s.effectiveVoiceCoilInductanceHenries / dt +
                      speakerHotResistance(s);
  l.electricalCoupling = law.bl + law.blDerivative * dt * v;
  const double tangent = law.stiffness - law.blDerivative * estimate.currentAmps;
  l.mechanicalCoeff = s.effectiveMovingMassKg / dt +
                      s.effectiveMechanicalDampingNsPerMeter + dt * tangent;
  l.rhsElectrical = s.effectiveVoiceCoilInductanceHenries / dt *
                     s.electricalCurrentAmps + law.blDerivative * dt * v * v;
  l.rhsMechanical = s.effectiveMovingMassKg / dt *
                     s.coneVelocityMetersPerSecond - law.restoringForce +
                     dt * tangent * v;
  l.determinant = l.electricalCoeff * l.mechanicalCoeff +
                   l.electricalCoupling * law.bl;
  assert(std::isfinite(l.determinant) && l.determinant > 0.0);
  l.load.conductanceSiemens = static_cast<float>(l.mechanicalCoeff / l.determinant);
  l.load.currentAmps = static_cast<float>(
      (l.rhsElectrical * l.mechanicalCoeff - l.electricalCoupling * l.rhsMechanical) /
      l.determinant);
  return l;
}

void commitSpeakerElectricalLoad(SpeakerSim& speaker,
                                 const SpeakerElectricalLinearization& l,
                                 float appliedVolts) {
  const auto next = solveSpeakerMotion(l, appliedVolts);
  const float currentAmps = static_cast<float>(next.currentAmps);
  const float velocityMetersPerSecond = static_cast<float>(next.velocityMetersPerSecond);
  const float displacementMeters = static_cast<float>(
      speaker.coneDisplacementMeters + l.dt * next.velocityMetersPerSecond);
  const auto law = speakerConstitutive(speaker, displacementMeters);
  const float backEmfVolts = static_cast<float>(law.bl * next.velocityMetersPerSecond);
  // Far-field pressure is proportional to acceleration. Preserve the existing
  // voltage normalization M*Re0/Bl0, but derive it from the actual nonlinear
  // motion instead of a second, linear mechanical observer.
  speaker.acousticDriveVolts = static_cast<float>(
      (law.bl * next.currentAmps - law.restoringForce -
       speaker.effectiveMechanicalDampingNsPerMeter * next.velocityMetersPerSecond) *
      speaker.effectiveVoiceCoilResistanceOhms / speaker.effectiveForceFactorBl);
  if (speaker.thermalCapacityJoulesPerKelvin > 0.0f &&
      speaker.thermalResistanceKelvinPerWatt > 0.0f) {
    const double heating = next.currentAmps * next.currentAmps *
                            speakerHotResistance(speaker);
    const double dtOverC = l.dt / speaker.thermalCapacityJoulesPerKelvin;
    speaker.coilTemperatureRiseKelvin = static_cast<float>(
        (speaker.coilTemperatureRiseKelvin + dtOverC * heating) /
        (1.0 + dtOverC / speaker.thermalResistanceKelvinPerWatt));
  }
  speaker.electricalCurrentAmps = currentAmps;
  speaker.coneVelocityMetersPerSecond = velocityMetersPerSecond;
  speaker.coneDisplacementMeters = displacementMeters;
  speaker.backEmfVolts = backEmfVolts;

  float coeff = clampf(speaker.loadSenseCoeff, 0.0f, 0.99999f);
  speaker.loadSenseVoltage =
      coeff * speaker.loadSenseVoltage + (1.0f - coeff) * std::fabs(appliedVolts);
  speaker.loadSenseCurrent =
      coeff * speaker.loadSenseCurrent + (1.0f - coeff) * std::fabs(currentAmps);
  if (speaker.loadSenseCurrent > 1e-5f && speaker.loadSenseVoltage > 1e-5f) {
    float minLoad = 0.70f * std::max(speaker.nominalLoadOhms, 1e-3f);
    float maxLoad = 4.5f * std::max(speaker.nominalLoadOhms, 1e-3f);
    speaker.effectiveLoadOhms = std::clamp(
        speaker.loadSenseVoltage / speaker.loadSenseCurrent, minLoad, maxLoad);
  }
}

void advanceVoltageDrivenSpeaker(SpeakerSim& speaker, float volts, float dt) {
  SpeakerMotion estimate{speaker.electricalCurrentAmps,
                          speaker.coneVelocityMetersPerSecond};
  SpeakerElectricalLinearization load;
  for (unsigned iteration = 1; iteration <= kSpeakerMaxIterations; ++iteration) {
    load = linearizeSpeakerElectricalLoad(speaker, dt, estimate);
    estimate = solveSpeakerMotion(load, volts);
    if (speakerConverged(speaker, iteration,
                         speakerResidualVolts(speaker, estimate, dt, volts), volts)) break;
  }
  commitSpeakerElectricalLoad(speaker, load, volts);
}

namespace {

// Both tube topologies use this coupled Newton solve. The transformer and
// speaker history are frozen until the joint solution has converged.
template <class PrimarySolver>
CurrentDrivenTransformerSample advanceLoadedOutput(
    CurrentDrivenTransformer& transformer, SpeakerSim& speaker,
    float primaryLoad, PrimarySolver solvePrimary, int& totalIterations,
    int& maxIterations) {
  assert(transformer.integrationSubsteps == 1);
  SpeakerMotion estimate{speaker.electricalCurrentAmps,
                          speaker.coneVelocityMetersPerSecond};
  SpeakerElectricalLinearization load;
  CurrentDrivenTransformerSample sample;
  for (unsigned iteration = 1; iteration <= kSpeakerMaxIterations; ++iteration) {
    load = linearizeSpeakerElectricalLoad(speaker, transformer.dtSub, estimate);
    const auto projection = buildAffineProjection(transformer, load.load, primaryLoad);
    const auto primary = solvePrimary(projection);
    totalIterations += primary.iterations;
    maxIterations = std::max(maxIterations, primary.iterations);
    // The transformer is linear for this frozen Norton load. Its affine
    // projection already contains the complete next state at any drive;
    // evaluating it avoids solving the same transformer a third time.
    sample = evalAffineProjection(projection, primary.driveCurrent);
    estimate = solveSpeakerMotion(load, sample.secondaryVoltage);
    if (speakerConverged(speaker, iteration,
          speakerResidualVolts(speaker, estimate, transformer.dtSub,
                                sample.secondaryVoltage), sample.secondaryVoltage)) break;
  }
  transformer.primaryCurrent = sample.primaryCurrent;
  transformer.secondaryCurrent = sample.secondaryCurrent;
  transformer.primaryVoltage = sample.primaryVoltage;
  transformer.secondaryVoltage = sample.secondaryVoltage;
  commitSpeakerElectricalLoad(speaker, load, sample.secondaryVoltage);
  return sample;
}

}  // namespace

OutputStageSubstepResult runOutputStageSubsteps(
    CurrentDrivenTransformer transformer,
    SpeakerSim& speaker,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float outputPrimaryLoadResistance) {
  const int transformerSubsteps = std::max(transformer.integrationSubsteps, 1);
  transformer.integrationSubsteps = 1;

  float primaryVoltageSum = 0.0f;
  float secondaryVoltageSum = 0.0f;
  float acousticDriveSum = 0.0f;
  float actualPlateCurrentASum = 0.0f;
  float actualPlateCurrentBSum = 0.0f;
  int totalNewtonIterations = 0;
  int maxNewtonIterations = 0;
  const power_stage_internal::TriodeLutView outputTriodeLut =
      power_stage_internal::makeTriodeLutView(power.outputTubeTriodeLut);

  for (int step = 0; step < transformerSubsteps; ++step) {
    const auto outputSample = advanceLoadedOutput(
        transformer, speaker, outputPrimaryLoadResistance,
        [&](const AffineTransformerProjection& projection) {
          return solveOutputPrimaryAffineWithLut(
              projection, power, outputTriodeLut, outputPlateQuiescent,
              power.outputGridAVolts, power.outputGridBVolts,
              transformer.primaryVoltage);
        }, totalNewtonIterations, maxNewtonIterations);
    float actualOutputPlateA =
        outputPlateQuiescent - 0.5f * outputSample.primaryVoltage;
    float actualOutputPlateB =
        outputPlateQuiescent + 0.5f * outputSample.primaryVoltage;
    float actualPlateCurrentA = static_cast<float>(
        power_stage_internal::evaluateTriodePlateFast(
            power.outputTubeBiasVolts + power.outputGridAVolts,
            actualOutputPlateA, power.outputTubeTriodeModel,
            power.outputTubeTriodeLut, outputTriodeLut)
            .currentAmps);
    float actualPlateCurrentB = static_cast<float>(
        power_stage_internal::evaluateTriodePlateFast(
            power.outputTubeBiasVolts + power.outputGridBVolts,
            actualOutputPlateB, power.outputTubeTriodeModel,
            power.outputTubeTriodeLut, outputTriodeLut)
            .currentAmps);
    acousticDriveSum += speaker.acousticDriveVolts;
    primaryVoltageSum += outputSample.primaryVoltage;
    secondaryVoltageSum += outputSample.secondaryVoltage;
    actualPlateCurrentASum += actualPlateCurrentA;
    actualPlateCurrentBSum += actualPlateCurrentB;
  }

  // The loop above expands the configured substeps explicitly. Preserve that
  // configuration for the next audio sample after the one-step inner solves.
  transformer.integrationSubsteps = transformerSubsteps;
  OutputStageSubstepResult result{};
  result.transformer = transformer;
  result.averagePrimaryVoltage =
      primaryVoltageSum / static_cast<float>(transformerSubsteps);
  result.averageSecondaryVoltage =
      secondaryVoltageSum / static_cast<float>(transformerSubsteps);
  result.averageAcousticDriveVolts =
      acousticDriveSum / static_cast<float>(transformerSubsteps);
  result.averagePlateCurrentA =
      actualPlateCurrentASum / static_cast<float>(transformerSubsteps);
  result.averagePlateCurrentB =
      actualPlateCurrentBSum / static_cast<float>(transformerSubsteps);
  result.transformerSubsteps = transformerSubsteps;
  result.totalNewtonIterations = totalNewtonIterations;
  result.maxNewtonIterations = maxNewtonIterations;
  return result;
}

OutputStageSubstepResult runSingleEndedOutputStageSubsteps(
    CurrentDrivenTransformer transformer,
    SpeakerSim& speaker,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float outputGridVolts,
    float outputPrimaryLoadResistance) {
  const int transformerSubsteps = std::max(transformer.integrationSubsteps, 1);
  transformer.integrationSubsteps = 1;

  float primaryVoltageSum = 0.0f;
  float secondaryVoltageSum = 0.0f;
  float acousticDriveSum = 0.0f;
  float actualPlateCurrentSum = 0.0f;
  int totalIterations = 0;
  int maxIterations = 0;

  for (int step = 0; step < transformerSubsteps; ++step) {
    const auto outputSample = advanceLoadedOutput(
        transformer, speaker, outputPrimaryLoadResistance,
        [&](const AffineTransformerProjection& projection) {
          return solveSingleEndedPrimaryAffine(
              projection, power, outputPlateQuiescent, outputGridVolts,
              transformer.primaryVoltage);
        }, totalIterations, maxIterations);
    acousticDriveSum += speaker.acousticDriveVolts;
    primaryVoltageSum += outputSample.primaryVoltage;
    secondaryVoltageSum += outputSample.secondaryVoltage;
    actualPlateCurrentSum += evaluateSingleEndedPlateCurrent(
        power, outputPlateQuiescent, outputGridVolts, outputSample.primaryVoltage);
  }

  transformer.integrationSubsteps = transformerSubsteps;
  OutputStageSubstepResult result{};
  result.transformer = transformer;
  result.averagePrimaryVoltage =
      primaryVoltageSum / static_cast<float>(transformerSubsteps);
  result.averageSecondaryVoltage =
      secondaryVoltageSum / static_cast<float>(transformerSubsteps);
  result.averageAcousticDriveVolts =
      acousticDriveSum / static_cast<float>(transformerSubsteps);
  result.averagePlateCurrentA =
      actualPlateCurrentSum / static_cast<float>(transformerSubsteps);
  result.averagePlateCurrentB = 0.0f;
  result.transformerSubsteps = transformerSubsteps;
  result.totalNewtonIterations = totalIterations;
  result.maxNewtonIterations = maxIterations;
  return result;
}

float estimateOutputStageNominalPowerWatts(
    const Radio1938::PowerNodeState& power) {
  float loadResistance = requirePositiveFinite(power.outputLoadResistanceOhms);
  float outputPlateQuiescent =
      requirePositiveFinite(power.outputTubeQuiescentPlateVolts);
  float maxGridPeak =
      0.92f * std::max(std::fabs(power.outputTubeBiasVolts), 1.0f);
  constexpr int kAmplitudeSteps = 40;
  constexpr int kSamplesPerCycle = 96;
  constexpr int kSettleCycles = 6;
  constexpr int kMeasureCycles = 2;
  float bestSecondaryRms = 0.0f;
  const power_stage_internal::TriodeLutView outputTriodeLut =
      power_stage_internal::makeTriodeLutView(power.outputTubeTriodeLut);

  for (int step = 1; step <= kAmplitudeSteps; ++step) {
    float gridPeak =
        maxGridPeak * static_cast<float>(step) / static_cast<float>(kAmplitudeSteps);
    CurrentDrivenTransformer transformer = power.outputTransformer;
    transformer.clearState();
    double sumSq = 0.0;
    int sampleCount = 0;

    for (int cycle = 0; cycle < (kSettleCycles + kMeasureCycles); ++cycle) {
      for (int i = 0; i < kSamplesPerCycle; ++i) {
        float phase = kRadioTwoPi * static_cast<float>(i) /
                      static_cast<float>(kSamplesPerCycle);
        float gridA = gridPeak * std::sin(phase);
        float gridB = -gridA;
        AffineTransformerProjection projection =
            buildAffineProjection(transformer, loadResistance, 0.0f);
        OutputPrimarySolveResult outputSolve = solveOutputPrimaryAffineWithLut(
            projection, power, outputTriodeLut, outputPlateQuiescent, gridA,
            gridB, transformer.primaryVoltage);
        auto outputSample =
            transformer.step(outputSolve.driveCurrent, loadResistance, 0.0f);
        if (cycle >= kSettleCycles) {
          sumSq += static_cast<double>(outputSample.secondaryVoltage) *
                   static_cast<double>(outputSample.secondaryVoltage);
          sampleCount++;
        }
      }
    }

    float secondaryRms =
        static_cast<float>(std::sqrt(sumSq / std::max(sampleCount, 1)));
    bestSecondaryRms = std::max(bestSecondaryRms, secondaryRms);
  }

  return (bestSecondaryRms * bestSecondaryRms) / loadResistance;
}

float estimateSingleEndedOutputStageNominalPowerWatts(
    const Radio1938::PowerNodeState& power) {
  float loadResistance = requirePositiveFinite(power.outputLoadResistanceOhms);
  float outputPlateQuiescent =
      requirePositiveFinite(power.outputTubeQuiescentPlateVolts);
  float maxGridPeak =
      0.92f * std::max(std::fabs(power.outputTubeBiasVolts), 1.0f);
  constexpr int kAmplitudeSteps = 40;
  constexpr int kSamplesPerCycle = 96;
  constexpr int kSettleCycles = 6;
  constexpr int kMeasureCycles = 2;
  float bestSecondaryRms = 0.0f;

  for (int step = 1; step <= kAmplitudeSteps; ++step) {
    float gridPeak =
        maxGridPeak * static_cast<float>(step) / static_cast<float>(kAmplitudeSteps);
    CurrentDrivenTransformer transformer = power.outputTransformer;
    transformer.clearState();
    double sumSq = 0.0;
    int sampleCount = 0;

    for (int cycle = 0; cycle < (kSettleCycles + kMeasureCycles); ++cycle) {
      for (int i = 0; i < kSamplesPerCycle; ++i) {
        float phase = kRadioTwoPi * static_cast<float>(i) /
                      static_cast<float>(kSamplesPerCycle);
        float grid = gridPeak * std::sin(phase);
        AffineTransformerProjection projection =
            buildAffineProjection(transformer, loadResistance, 0.0f);
        SingleEndedPrimarySolveResult outputSolve =
            solveSingleEndedPrimaryAffine(projection, power,
                                           outputPlateQuiescent, grid,
                                           transformer.primaryVoltage);
        CurrentDrivenTransformerSample outputSample = transformer.step(
            outputSolve.driveCurrent, loadResistance, 0.0f);
        if (cycle >= kSettleCycles) {
          sumSq += static_cast<double>(outputSample.secondaryVoltage) *
                   static_cast<double>(outputSample.secondaryVoltage);
          sampleCount++;
        }
      }
    }

    float secondaryRms =
        static_cast<float>(std::sqrt(sumSq / std::max(sampleCount, 1)));
    bestSecondaryRms = std::max(bestSecondaryRms, secondaryRms);
  }

  return (bestSecondaryRms * bestSecondaryRms) / loadResistance;
}
