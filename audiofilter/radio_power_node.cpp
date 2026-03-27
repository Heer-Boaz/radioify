#include "../radio.h"

#include <algorithm>
#include <cassert>
#include <cmath>

void RadioPowerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& power = radio.power;
  power.tubePlateDcVolts =
      power.tubePlateSupplyVolts -
      power.tubePlateCurrentAmps * power.interstagePrimaryResistanceOhms;
  power.outputTubePlateDcVolts =
      power.outputTubePlateSupplyVolts -
      power.outputTubePlateCurrentAmps *
          (0.5f * power.outputTransformerPrimaryResistanceOhms);
  assert(std::fabs(power.tubePlateSupplyVolts -
                   power.tubePlateCurrentAmps *
                       power.interstagePrimaryResistanceOhms -
                   power.tubePlateDcVolts) < 1.0f);
  assert(std::fabs(power.outputTubePlateSupplyVolts -
                   power.outputTubePlateCurrentAmps *
                       (0.5f * power.outputTransformerPrimaryResistanceOhms) -
                   power.outputTubePlateDcVolts) < 1.0f);
  power.sagAtk =
      std::exp(-1.0f / (radio.sampleRate * (power.sagAttackMs / 1000.0f)));
  power.sagRel =
      std::exp(-1.0f / (radio.sampleRate * (power.sagReleaseMs / 1000.0f)));
  if (power.postLpHz > 0.0f) {
    power.postLpf.setLowpass(radio.sampleRate, power.postLpHz, kRadioBiquadQ);
  } else {
    power.postLpf = Biquad{};
  }
  power.driverSourceResistanceOhms = parallelResistance(
      radio.receiverCircuit.tubeLoadResistanceOhms,
      radio.receiverCircuit.tubePlateResistanceOhms);
  assert(std::isfinite(power.driverSourceResistanceOhms) &&
         power.driverSourceResistanceOhms > 0.0f);
  TriodeOperatingPoint driverOp = solveTriodeOperatingPoint(
      power.tubePlateSupplyVolts, power.interstagePrimaryResistanceOhms,
      power.tubeBiasVolts, power.tubePlateDcVolts, power.tubePlateCurrentAmps,
      power.tubeMutualConductanceSiemens, power.tubeMu);
  power.tubeQuiescentPlateVolts = driverOp.plateVolts;
  power.tubeQuiescentPlateCurrentAmps = driverOp.plateCurrentAmps;
  power.tubePlateResistanceOhms = driverOp.rpOhms;
  power.tubeTriodeModel = driverOp.model;
  configureRuntimeTriodeLut(power.tubeTriodeLut, power.tubeTriodeModel,
                            power.tubeBiasVolts, power.tubePlateSupplyVolts,
                            96.0f, 18.0f);
  assert((std::fabs(power.tubeQuiescentPlateVolts - power.tubePlateDcVolts) <=
          power.operatingPointToleranceVolts) &&
         "6F6 driver operating point diverged from the preset target");

  TriodeOperatingPoint outputOp = solveTriodeOperatingPoint(
      power.outputTubePlateSupplyVolts,
      0.5f * power.outputTransformerPrimaryResistanceOhms,
      power.outputTubeBiasVolts, power.outputTubePlateDcVolts,
      power.outputTubePlateCurrentAmps,
      power.outputTubeMutualConductanceSiemens, power.outputTubeMu);
  power.outputTubeQuiescentPlateVolts = outputOp.plateVolts;
  power.outputTubeQuiescentPlateCurrentAmps = outputOp.plateCurrentAmps;
  power.outputTubePlateResistanceOhms = outputOp.rpOhms;
  power.outputTubeTriodeModel = outputOp.model;
  configureRuntimeTriodeLut(power.outputTubeTriodeLut,
                            power.outputTubeTriodeModel,
                            power.outputTubeBiasVolts,
                            power.outputTubePlateSupplyVolts, 120.0f, 24.0f);
  assert((std::fabs(power.outputTubeQuiescentPlateVolts -
                    power.outputTubePlateDcVolts) <=
          power.outputOperatingPointToleranceVolts) &&
         "6B4 output operating point diverged from the preset target");

  power.tubePlateVoltage = power.tubeQuiescentPlateVolts;
  power.outputGridAVolts = 0.0f;
  power.outputGridBVolts = 0.0f;
  power.interstageTransformer.configure(
      radio.sampleRate, power.interstagePrimaryLeakageInductanceHenries,
      power.interstageMagnetizingInductanceHenries,
      power.interstageTurnsRatioPrimaryToSecondary,
      power.interstagePrimaryResistanceOhms,
      power.interstagePrimaryCoreLossResistanceOhms,
      power.interstagePrimaryShuntCapFarads,
      power.interstageSecondaryLeakageInductanceHenries,
      power.interstageSecondaryResistanceOhms,
      power.interstageSecondaryShuntCapFarads,
      power.interstageIntegrationSubsteps);
  power.outputTransformer.configure(
      radio.sampleRate, power.outputTransformerPrimaryLeakageInductanceHenries,
      power.outputTransformerMagnetizingInductanceHenries,
      power.outputTransformerTurnsRatioPrimaryToSecondary,
      power.outputTransformerPrimaryResistanceOhms,
      power.outputTransformerPrimaryCoreLossResistanceOhms,
      power.outputTransformerPrimaryShuntCapFarads,
      power.outputTransformerSecondaryLeakageInductanceHenries,
      power.outputTransformerSecondaryResistanceOhms,
      power.outputTransformerSecondaryShuntCapFarads,
      power.outputTransformerIntegrationSubsteps);
  {
    FixedLoadAffineTransformerProjection outputAffine =
        buildFixedLoadAffineProjection(power.outputTransformer,
                                       power.outputLoadResistanceOhms, 0.0f);
    power.outputTransformerAffineReady = true;
    power.outputTransformerAffineStateA = outputAffine.stateA;
    power.outputTransformerAffineSlope = outputAffine.slope;
  }
  // Derive the digital speaker reference from the modeled 6B4/output-transformer
  // combination instead of a hand-tuned watt scalar. This keeps the digital
  // scale anchored to the clean power the current tube/load model can actually
  // produce.
  power.nominalOutputPowerWatts = estimateOutputStageNominalPowerWatts(power);
  assert(std::isfinite(power.nominalOutputPowerWatts) &&
         power.nominalOutputPowerWatts > 0.0f);
  radio.output.digitalReferenceSpeakerVoltsPeak = std::sqrt(
      2.0f * power.nominalOutputPowerWatts * power.outputLoadResistanceOhms);
}

void RadioPowerNode::reset(Radio1938& radio) {
  auto& power = radio.power;
  power.sagEnv = 0.0f;
  power.rectifierPhase = 0.0f;
  power.gridCouplingCapVoltage = 0.0f;
  power.gridVoltage = 0.0f;
  power.tubePlateVoltage = power.tubeQuiescentPlateVolts;
  power.outputGridAVolts = 0.0f;
  power.outputGridBVolts = 0.0f;
  power.interstageCt.primaryCurrent = 0.0f;
  power.interstageCt.primaryVoltage = 0.0f;
  power.interstageCt.secondaryACurrent = 0.0f;
  power.interstageCt.secondaryAVoltage = 0.0f;
  power.interstageCt.secondaryBCurrent = 0.0f;
  power.interstageCt.secondaryBVoltage = 0.0f;
  power.outputTransformer.reset();
  power.postLpf.reset();
}

float RadioPowerNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext&) {
  auto& power = radio.power;
  auto& controlSense = radio.controlSense;
  if (radio.calibration.enabled) {
    radio.calibration.validationSampleCount++;
  }
  float powerT = computePowerLoadT(power);
  float driverSupplyScale =
      computePowerBranchSupplyScale(radio, power.supplyDriveDepth);
  float outputSupplyScale = computePowerBranchSupplyScale(radio, 1.0f);
  float dt = 1.0f / requirePositiveFinite(radio.sampleRate);
  float previousCapVoltage = power.gridCouplingCapVoltage;
  power.gridVoltage = solveCapCoupledGridVoltage(
      y, previousCapVoltage, dt, power.gridCouplingCapFarads,
      power.driverSourceResistanceOhms, power.gridLeakResistanceOhms,
      power.tubeBiasVolts, power.tubeGridCurrentResistanceOhms);
  float seriesCurrent =
      (y - previousCapVoltage - power.gridVoltage) /
      (power.driverSourceResistanceOhms +
       dt / requirePositiveFinite(power.gridCouplingCapFarads));
  power.gridCouplingCapVoltage +=
      dt * (seriesCurrent / requirePositiveFinite(power.gridCouplingCapFarads));
  float controlGridVolts = power.tubeBiasVolts + power.gridVoltage;
  if (radio.calibration.enabled) {
    radio.calibration.driverGridVolts.accumulate(power.gridVoltage);
  }
  if (radio.calibration.enabled && controlGridVolts > 0.0f) {
    radio.calibration.driverGridPositiveSamples++;
  }
  assert(power.tubeTriodeConnected &&
         "Interstage driver solve requires triode-connected 6F6 operation");
  float driverPlateQuiescent =
      requirePositiveFinite(power.tubeQuiescentPlateVolts * driverSupplyScale);
  float driverQuiescentCurrent = static_cast<float>(
      evaluateKorenTriodePlateRuntime(power.tubeBiasVolts,
                                      driverPlateQuiescent,
                                      power.tubeTriodeModel,
                                      power.tubeTriodeLut)
          .currentAmps);
  auto interstageSolved = solveDriverInterstageCenterTappedNoCap(
      power.interstageTransformer, power, controlGridVolts,
      driverPlateQuiescent, driverQuiescentCurrent);

  power.interstageCt.primaryCurrent = interstageSolved.primaryCurrent;
  power.interstageCt.primaryVoltage = interstageSolved.primaryVoltage;
  power.interstageCt.secondaryACurrent = interstageSolved.secondaryACurrent;
  power.interstageCt.secondaryAVoltage = interstageSolved.secondaryAVoltage;
  power.interstageCt.secondaryBCurrent = interstageSolved.secondaryBCurrent;
  power.interstageCt.secondaryBVoltage = interstageSolved.secondaryBVoltage;

  power.tubePlateVoltage =
      driverPlateQuiescent - interstageSolved.primaryVoltage;

  power.outputGridAVolts = interstageSolved.secondaryAVoltage;
  power.outputGridBVolts = interstageSolved.secondaryBVoltage;

  float actualDriverCurrent = interstageSolved.driverPlateCurrentAbs;

  if (radio.calibration.enabled) {
    radio.calibration.driverPlateSwingVolts.accumulate(
        interstageSolved.primaryVoltage);
    radio.calibration.outputGridAVolts.accumulate(power.outputGridAVolts);
    radio.calibration.outputGridBVolts.accumulate(power.outputGridBVolts);
    float interstageDifferentialVolts =
        interstageSolved.secondaryAVoltage - interstageSolved.secondaryBVoltage;

    radio.calibration.interstageSecondaryPeakVolts =
        std::max(radio.calibration.interstageSecondaryPeakVolts,
                 std::fabs(interstageDifferentialVolts));

    radio.calibration.interstageSecondarySumSq +=
        static_cast<double>(interstageDifferentialVolts) *
        static_cast<double>(interstageDifferentialVolts);
  }
  if (radio.calibration.enabled) {
    bool outputGridAPositive =
        (power.outputTubeBiasVolts + power.outputGridAVolts) > 0.0f;
    bool outputGridBPositive =
        (power.outputTubeBiasVolts + power.outputGridBVolts) > 0.0f;
    if (outputGridAPositive) radio.calibration.outputGridAPositiveSamples++;
    if (outputGridBPositive) radio.calibration.outputGridBPositiveSamples++;
    if (outputGridAPositive || outputGridBPositive) {
      radio.calibration.outputGridPositiveSamples++;
    }
  }
  float outputPlateQuiescent =
      requirePositiveFinite(power.outputTubeQuiescentPlateVolts *
                            outputSupplyScale);
  const float outputPrimaryLoadResistance = 0.0f;
  AffineTransformerProjection affineOut{};
  if (power.outputTransformerAffineReady) {
    affineOut.base = evalFixedLoadAffineBase(
        power.outputTransformerAffineStateA, power.outputTransformer);
    affineOut.slope = power.outputTransformerAffineSlope;
  } else {
    affineOut = buildAffineProjection(
        power.outputTransformer, power.outputLoadResistanceOhms,
        outputPrimaryLoadResistance);
  }
  float solvedOutputPrimaryVoltage = solveOutputPrimaryVoltageAffine(
      affineOut, power, outputPlateQuiescent, power.outputGridAVolts,
      power.outputGridBVolts, power.outputTransformer.primaryVoltage);
  float outputPlateA =
      outputPlateQuiescent - 0.5f * solvedOutputPrimaryVoltage;
  float outputPlateB =
      outputPlateQuiescent + 0.5f * solvedOutputPrimaryVoltage;
  KorenTriodePlateEval outputEvalA = evaluateKorenTriodePlateRuntime(
      power.outputTubeBiasVolts + power.outputGridAVolts, outputPlateA,
      power.outputTubeTriodeModel, power.outputTubeTriodeLut);
  KorenTriodePlateEval outputEvalB = evaluateKorenTriodePlateRuntime(
      power.outputTubeBiasVolts + power.outputGridBVolts, outputPlateB,
      power.outputTubeTriodeModel, power.outputTubeTriodeLut);
  float plateCurrentA = static_cast<float>(outputEvalA.currentAmps);
  float plateCurrentB = static_cast<float>(outputEvalB.currentAmps);
  float driveCurrent = 0.5f * (plateCurrentA - plateCurrentB);
  auto outputSample = evalAffineProjection(affineOut, driveCurrent);
  power.outputTransformer.primaryVoltage = outputSample.primaryVoltage;
  power.outputTransformer.secondaryVoltage = outputSample.secondaryVoltage;
  power.outputTransformer.primaryCurrent = outputSample.primaryCurrent;
  power.outputTransformer.secondaryCurrent = outputSample.secondaryCurrent;
  float actualOutputPlateA =
      outputPlateQuiescent - 0.5f * outputSample.primaryVoltage;
  float actualOutputPlateB =
      outputPlateQuiescent + 0.5f * outputSample.primaryVoltage;
  float actualPlateCurrentA = static_cast<float>(
      evaluateKorenTriodePlateRuntime(
          power.outputTubeBiasVolts + power.outputGridAVolts, actualOutputPlateA,
          power.outputTubeTriodeModel, power.outputTubeTriodeLut)
          .currentAmps);
  float actualPlateCurrentB = static_cast<float>(
      evaluateKorenTriodePlateRuntime(
          power.outputTubeBiasVolts + power.outputGridBVolts, actualOutputPlateB,
          power.outputTubeTriodeModel, power.outputTubeTriodeLut)
          .currentAmps);
  y = outputSample.secondaryVoltage;
  if (power.postLpHz > 0.0f) {
    y = power.postLpf.process(y);
  }
  if (radio.calibration.enabled) {
    radio.calibration.outputPrimaryVolts.accumulate(outputSample.primaryVoltage);
    radio.calibration.speakerSecondaryVolts.accumulate(y);
    radio.calibration.maxDriverPlateCurrentAmps = std::max(
        radio.calibration.maxDriverPlateCurrentAmps, actualDriverCurrent);
    radio.calibration.maxOutputPlateCurrentAAmps = std::max(
        radio.calibration.maxOutputPlateCurrentAAmps, actualPlateCurrentA);
    radio.calibration.maxOutputPlateCurrentBAmps = std::max(
        radio.calibration.maxOutputPlateCurrentBAmps, actualPlateCurrentB);
    radio.calibration.maxSpeakerSecondaryVolts =
        std::max(radio.calibration.maxSpeakerSecondaryVolts, std::fabs(y));
    radio.calibration.maxSpeakerReferenceRatio = std::max(
        radio.calibration.maxSpeakerReferenceRatio,
        std::fabs(y) /
            std::max(radio.output.digitalReferenceSpeakerVoltsPeak, 1e-3f));
  }
  float quiescentSupplyCurrent =
      driverQuiescentCurrent + 2.0f * power.outputTubeQuiescentPlateCurrentAmps;
  float actualSupplyCurrent =
      actualDriverCurrent + actualPlateCurrentA + actualPlateCurrentB;
  float load = std::max(
      0.0f, (actualSupplyCurrent - quiescentSupplyCurrent) /
                requirePositiveFinite(quiescentSupplyCurrent));
  if (load > power.sagEnv) {
    power.sagEnv = power.sagAtk * power.sagEnv + (1.0f - power.sagAtk) * load;
  } else {
    power.sagEnv = power.sagRel * power.sagEnv + (1.0f - power.sagRel) * load;
  }
  controlSense.powerSagSense = power.sagEnv;
  advanceRectifierRipplePhase(radio);
  return y;
}

