#include "radio_presets.h"

#include "../math/signal_math.h"

#include <cassert>
#include <cmath>

namespace {

void applyPhilco37116Preset(Radio1938& radio) {
  radio.identity.driftDepth = 0.06f;
  radio.globals.ifNoiseMix = 0.22f;
  radio.globals.inputPad = 1.0f;
  radio.globals.enableAutoLevel = false;
  radio.globals.autoTargetDb = -21.0f;
  radio.globals.autoMaxBoostDb = 2.5f;

  radio.tuning.safeBwMinHz = 2400.0f;
  radio.tuning.safeBwMaxHz = 9000.0f;
  radio.tuning.preBwScale = 1.00f;
  radio.tuning.postBwScale = 1.00f;
  radio.tuning.smoothTau = 0.05f;
  radio.tuning.updateEps = 0.25f;
  radio.tuning.magneticTuningEnabled = true;
  // Philco's service data specifies magnetic-tuning capture within roughly
  // 5 kHz of station center. The discriminator remains the proportional
  // element; these values define its acquisition window and available pull.
  radio.tuning.afcCaptureHz = 5000.0f;
  radio.tuning.afcMaxCorrectionHz = 5000.0f;
  radio.tuning.afcDeadband = 0.015f;
  radio.tuning.afcResponseMs = 240.0f;

  radio.frontEnd.secondTunedCircuitEnabled = true;
  radio.frontEnd.inputHpHz = 115.0f;
  radio.frontEnd.rfGain = 1.0f;
  radio.frontEnd.avcGainDepth = 0.18f;
  radio.frontEnd.selectivityPeakHz = 0.0f;
  radio.frontEnd.selectivityPeakQ = 0.707f;
  radio.frontEnd.selectivityPeakGainDb = 0.0f;
  radio.frontEnd.antennaInductanceHenries = 0.011f;
  radio.frontEnd.antennaLoadResistanceOhms = 2200.0f;
  radio.frontEnd.rfInductanceHenries = 0.016f;
  radio.frontEnd.rfLoadResistanceOhms = 3300.0f;

  radio.mixer.rfGridDriveVolts = 1.0f;
  radio.mixer.loGridDriveVolts = 18.0f;
  radio.mixer.loGridBiasVolts = -15.0f;
  radio.mixer.avcGridDriveVolts = 24.0f;
  radio.mixer.plateSupplyVolts = 250.0f;
  radio.mixer.plateDcVolts = 215.0f;
  radio.mixer.screenVolts = 160.0f;
  radio.mixer.biasVolts = -6.0f;
  radio.mixer.cutoffVolts = -45.0f;
  radio.mixer.plateCurrentAmps = 0.0035f;
  radio.mixer.mutualConductanceSiemens = 0.0011f;
  radio.mixer.acLoadResistanceOhms = 10000.0f;
  radio.mixer.plateKneeVolts = 22.0f;
  radio.mixer.gridSoftnessVolts = 2.0f;

  radio.ifStrip.enabled = true;
  radio.ifStrip.ifMinBwHz = 2400.0f;
  radio.ifStrip.primaryInductanceHenries = 0.00022f;
  radio.ifStrip.secondaryInductanceHenries = 0.00025f;
  radio.ifStrip.secondaryLoadResistanceOhms = 680.0f;
  // Reduced-order IF gain stands in for the missing plate-voltage swing of the
  // 470 kHz strip. Keep enough detector drive for the 6J5/6F6/6B4 chain so the
  // physical speaker-reference scaling is not forced to make the whole radio
  // sound abnormally quiet.
  // This is calibrated against the physical speaker reference, nominal SINAD,
  // and the measured audio passband rather than against listening loudness.
  radio.ifStrip.stageGain = 4.6f;
  radio.ifStrip.avcGainDepth = 0.18f;
  radio.ifStrip.ifCenterHz = 470000.0f;
  radio.ifStrip.interstageCouplingCoeff = 0.15f;
  radio.ifStrip.outputCouplingCoeff = 0.11f;

  radio.demod.am.audioDiodeDrop = 0.0100f;
  radio.demod.am.avcDiodeDrop = 0.0080f;
  radio.demod.am.audioJunctionSlopeVolts = 0.0045f;
  radio.demod.am.avcJunctionSlopeVolts = 0.0040f;
  radio.demod.am.detectorStorageCapFarads = 350e-12f;
  radio.demod.am.audioChargeResistanceOhms = 5100.0f;
  radio.demod.am.audioDischargeResistanceOhms = 160000.0f;
  radio.demod.am.avcChargeResistanceOhms = 1000000.0f;
  radio.demod.am.avcDischargeResistanceOhms =
      parallelResistance(1000000.0f, 1000000.0f);
  radio.demod.am.avcFilterCapFarads = 0.15e-6f;
  radio.demod.am.controlVoltageRef = 3.0f;
  radio.demod.am.senseLowHz = 0.0f;
  radio.demod.am.senseHighHz = 0.0f;
  radio.demod.am.afcSenseLpHz = 34.0f;

  radio.receiverCircuit.enabled = true;
  radio.receiverCircuit.volumeControlResistanceOhms = 2000000.0f;
  radio.receiverCircuit.volumeControlTapResistanceOhms = 1000000.0f;
  radio.receiverCircuit.volumeControlPosition = 1.0f;
  radio.receiverCircuit.volumeControlLoudnessResistanceOhms = 490000.0f;
  // Philco Service Bulletin 258 shows the tapped 2 M volume control feeding a
  // small 110 mmfd loudness / compensation branch, not a broad 15 nF shunt.
  radio.receiverCircuit.volumeControlLoudnessCapFarads = 110e-12f;
  radio.receiverCircuit.couplingCapFarads = 0.01e-6f;
  radio.receiverCircuit.gridLeakResistanceOhms = 1000000.0f;
  radio.receiverCircuit.tubePlateSupplyVolts = 250.0f;
  radio.receiverCircuit.tubePlateDcVolts = 90.0f;
  radio.receiverCircuit.tubeScreenVolts = 0.0f;
  radio.receiverCircuit.tubeBiasVolts = -8.0f;
  radio.receiverCircuit.tubePlateCurrentAmps = 0.009f;
  radio.receiverCircuit.tubeMutualConductanceSiemens = 0.0026f;
  radio.receiverCircuit.tubeMu = 20.0f;
  radio.receiverCircuit.tubeTriodeConnected = true;
  radio.receiverCircuit.tubeLoadResistanceOhms = 15000.0f;
  radio.receiverCircuit.tubePlateKneeVolts = 16.0f;
  radio.receiverCircuit.tubeGridSoftnessVolts = 0.6f;

  radio.tone.presenceHz = 2100.0f;
  radio.tone.presenceQ = 0.82f;
  radio.tone.presenceGainDb = 0.30f;
  radio.tone.tiltSplitHz = 900.0f;
  radio.tone.tiltDepthDb = 0.9f;

  radio.power.topology =
      Radio1938::PowerNodeState::Topology::TransformerCoupledPushPull;
  radio.power.sagStart = 0.06f;
  radio.power.sagEnd = 0.22f;
  radio.power.rippleDepth = 0.012f;
  radio.power.sagAttackMs = 60.0f;
  radio.power.sagReleaseMs = 900.0f;
  radio.power.rectifierMinHz = 80.0f;
  radio.power.rippleSecondHarmonicMix = 0.0f;
  radio.power.gainSagPerPower = 0.015f;
  radio.power.rippleGainBase = 0.18f;
  radio.power.rippleGainDepth = 0.08f;
  radio.power.gainMin = 0.92f;
  radio.power.gainMax = 1.02f;
  radio.power.supplyDriveDepth = 0.01f;
  radio.power.supplyBiasDepth = 0.0f;
  radio.power.postLpHz = 0.0f;
  radio.power.gridCouplingCapFarads = 0.02e-6f;
  radio.power.gridLeakResistanceOhms = 100000.0f;
  radio.power.tubePlateSupplyVolts = 250.0f;
  radio.power.tubeScreenVolts = 250.0f;
  radio.power.tubeBiasVolts = -20.0f;
  radio.power.tubePlateCurrentAmps = 0.021f;
  radio.power.tubeMutualConductanceSiemens = 0.0026f;
  radio.power.tubeMu = 6.8f;
  radio.power.tubeTriodeConnected = true;
  radio.power.tubeAcLoadResistanceOhms = 4000.0f;
  radio.power.tubePlateKneeVolts = 24.0f;
  radio.power.tubeGridSoftnessVolts = 0.8f;
  radio.power.tubeGridCurrentResistanceOhms = 1000.0f;
  radio.power.outputGridLeakResistanceOhms = 250000.0f;
  radio.power.outputGridCurrentResistanceOhms = 1200.0f;
  // Leakage belongs in the tens-of-millihenries range here; 0.45 H darkens the
  // power-chain like a total primary inductance, not a leakage term.
  radio.power.interstagePrimaryLeakageInductanceHenries = 0.045f;
  radio.power.interstageMagnetizingInductanceHenries = 15.0f;
  radio.power.interstagePrimaryResistanceOhms = 430.0f;
  radio.power.tubePlateDcVolts =
      radio.power.tubePlateSupplyVolts -
      radio.power.tubePlateCurrentAmps *
          radio.power.interstagePrimaryResistanceOhms;
  radio.power.interstageTurnsRatioPrimaryToSecondary = 1.0f / 6.0f;
  radio.power.interstagePrimaryCoreLossResistanceOhms = 220000.0f;
  radio.power.interstagePrimaryShuntCapFarads = 0.0f;
  radio.power.interstageSecondaryLeakageInductanceHenries = 0.040f;
  radio.power.interstageSecondaryResistanceOhms = 296.0f;
  radio.power.interstageSecondaryShuntCapFarads = 0.0f;
  radio.power.interstageIntegrationSubsteps = 8;
  radio.power.outputTubePlateSupplyVolts = 325.0f;
  radio.power.outputTubeScreenVolts = 325.0f;
  radio.power.outputTubeBiasVolts = -68.0f;
  radio.power.outputTubeCutoffVolts = 0.0f;
  radio.power.outputTubePlateCurrentAmps = 0.040f;
  radio.power.outputTubeMutualConductanceSiemens = 0.00525f;
  radio.power.outputTubeMu = 4.2f;
  // A slightly lighter reflected load keeps the output pair in a more
  // believable clean region while preserving the Philco's expected power.
  radio.power.outputTubePlateToPlateLoadOhms = 3300.0f;
  radio.power.outputTubePlateKneeVolts = 18.0f;
  radio.power.outputTubeGridSoftnessVolts = 2.0f;
  radio.power.outputTransformerPrimaryLeakageInductanceHenries = 35e-3f;
  radio.power.outputTransformerMagnetizingInductanceHenries = 20.0f;
  radio.power.outputTransformerTurnsRatioPrimaryToSecondary =
      std::sqrt(3300.0f / 3.9f);
  radio.power.outputTransformerPrimaryResistanceOhms = 235.0f;
  radio.power.outputTubePlateDcVolts =
      radio.power.outputTubePlateSupplyVolts -
      radio.power.outputTubePlateCurrentAmps *
          (0.5f * radio.power.outputTransformerPrimaryResistanceOhms);
  radio.power.outputTransformerPrimaryCoreLossResistanceOhms = 90000.0f;
  radio.power.outputTransformerPrimaryShuntCapFarads = 0.0f;
  radio.power.outputTransformerSecondaryLeakageInductanceHenries = 60e-6f;
  radio.power.outputTransformerSecondaryResistanceOhms = 0.32f;
  radio.power.outputTransformerSecondaryShuntCapFarads = 0.0f;
  radio.power.outputTransformerIntegrationSubsteps = 8;
  radio.power.outputLoadResistanceOhms = 3.9f;
  radio.power.nominalOutputPowerWatts = 0.0f;
  assert(std::fabs(radio.power.tubePlateSupplyVolts -
                   radio.power.tubePlateCurrentAmps *
                       radio.power.interstagePrimaryResistanceOhms -
                   radio.power.tubePlateDcVolts) < 1.0f);
  assert(std::fabs(radio.power.outputTubePlateSupplyVolts -
                   radio.power.outputTubePlateCurrentAmps *
                       (0.5f * radio.power.outputTransformerPrimaryResistanceOhms) -
                   radio.power.outputTubePlateDcVolts) < 1.0f);
  // Downstream listening-level calibration only. The patent-anchored cabinet
  // response has more low-frequency transient gain than the former flat fit;
  // keep nominal program peaks below the digital safety clip without changing
  // the physical speaker reference used by the power-stage calibration.
  radio.output.digitalMakeupGain = 1.03f;

  radio.globals.oversampleFactor = 2.0f;
  radio.globals.oversampleCutoffFraction = 0.45f;
  radio.globals.outputClipThreshold = 1.0f;
  radio.globals.postNoiseMix = 0.35f;
  radio.globals.noiseFloorAmp = 0.0f;

  radio.noiseConfig.enableHumTone = true;
  radio.noiseConfig.humHzDefault = 60.0f;
  radio.noiseConfig.noiseWeightRef = 0.15f;
  radio.noiseConfig.noiseWeightScaleMax = 2.0f;
  radio.noiseConfig.humAmpScale = 0.008f;
  radio.noiseConfig.crackleAmpScale = 0.025f;
  radio.noiseConfig.crackleRateScale = 1.2f;

  radio.noiseRuntime.hum.noiseHpHz = 500.0f;
  radio.noiseRuntime.hum.noiseLpHz = 5500.0f;
  radio.noiseRuntime.hum.filterQ = kRadioBiquadQ;
  radio.noiseRuntime.hum.scAttackMs = 2.0f;
  radio.noiseRuntime.hum.scReleaseMs = 80.0f;
  radio.noiseRuntime.hum.crackleDecayMs = 8.0f;
  radio.noiseRuntime.hum.sidechainMaskRef = 0.15f;
  radio.noiseRuntime.hum.hissMaskDepth = 0.5f;
  radio.noiseRuntime.hum.burstMaskDepth = 0.3f;
  radio.noiseRuntime.hum.pinkFastPole = 0.85f;
  radio.noiseRuntime.hum.pinkSlowPole = 0.97f;
  radio.noiseRuntime.hum.brownStep = 0.02f;
  radio.noiseRuntime.hum.hissDriftPole = 0.9992f;
  radio.noiseRuntime.hum.hissDriftNoise = 0.002f;
  radio.noiseRuntime.hum.hissDriftSlowPole = 0.9998f;
  radio.noiseRuntime.hum.hissDriftSlowNoise = 0.001f;
  radio.noiseRuntime.hum.whiteMix = 0.35f;
  radio.noiseRuntime.hum.pinkFastMix = 0.45f;
  radio.noiseRuntime.hum.pinkDifferenceMix = 0.12f;
  radio.noiseRuntime.hum.pinkFastSubtract = 0.6f;
  radio.noiseRuntime.hum.brownMix = 0.08f;
  radio.noiseRuntime.hum.hissBase = 0.7f;
  radio.noiseRuntime.hum.hissDriftDepth = 0.3f;
  radio.noiseRuntime.hum.hissDriftSlowMix = 0.04f;
  radio.noiseRuntime.hum.humSecondHarmonicMix = 0.42f;

  radio.speakerStage.drive = 1.0f;
  radio.speakerStage.speaker.suspensionHz = 65.0f;
  radio.speakerStage.speaker.suspensionQ = 0.90f;
  radio.speakerStage.speaker.suspensionGainDb = 1.5f;
  radio.speakerStage.speaker.coneBodyHz = 1200.0f;
  radio.speakerStage.speaker.coneBodyQ = 0.50f;
  radio.speakerStage.speaker.coneBodyGainDb = 0.25f;
  radio.speakerStage.speaker.upperBreakupHz = 2600.0f;
  radio.speakerStage.speaker.upperBreakupQ = 1.05f;
  radio.speakerStage.speaker.upperBreakupGainDb = 0.25f;
  radio.speakerStage.speaker.coneDipHz = 1850.0f;
  radio.speakerStage.speaker.coneDipQ = 0.85f;
  radio.speakerStage.speaker.coneDipGainDb = -0.35f;
  radio.speakerStage.speaker.topLpHz = 5000.0f;
  radio.speakerStage.speaker.hfLossLpHz = 4000.0f;
  radio.speakerStage.speaker.hfLossDepth = 0.12f;
  radio.speakerStage.speaker.filterQ = kRadioBiquadQ;
  radio.speakerStage.speaker.drive = 1.0f;
  radio.speakerStage.speaker.limit = 0.0f;
  // Philco 37-116 uses a 14-inch type-W electrodynamic speaker with a 3.9 ohm
  // voice-coil load. Philco described its two-section cone as a stiff treble
  // center with a flexible bass rim. Exact T/S data is not published, so keep
  // the motor close to the documented load while fitting the remaining
  // parameters to that large prewar field-coil construction.
  radio.speakerStage.speaker.voiceCoilResistanceOhms = 3.2f;
  radio.speakerStage.speaker.voiceCoilInductanceHenries = 0.12e-3f;
  radio.speakerStage.speaker.movingMassKg = 0.0153f;
  radio.speakerStage.speaker.mechanicalQ = 5.2f;
  radio.speakerStage.speaker.electricalQ = 2.5f;
  radio.speakerStage.speaker.forceFactorBl = 0.0f;
  radio.speakerStage.speaker.suspensionComplianceMetersPerNewton = 0.0f;
  radio.speakerStage.speaker.mechanicalDampingNsPerMeter = 0.0f;
  radio.speakerStage.speaker.excursionRef = 8.0f;
  radio.speakerStage.speaker.complianceLossDepth = 0.05f;
  radio.speakerStage.speaker.asymBias = 0.06f;

  radio.cabinet.enabled = true;
  // Philco patent US2059929 measures the family cabinet resonance from 70 to
  // 150 Hz, peaking at 95 Hz. The untreated response rises by roughly 8-10 dB
  // there. This peaking section is the resonance before the absorbers below.
  radio.cabinet.panelHz = 95.0f;
  radio.cabinet.panelQ = 1.10f;
  radio.cabinet.panelGainDb = 8.0f;
  // Do not invent unrelated mid-band cabinet modes: no model-specific
  // measurement supports the old 650 Hz and 900 Hz fitted sections.
  radio.cabinet.chassisHz = 0.0f;
  radio.cabinet.chassisQ = 0.0f;
  radio.cabinet.chassisGainDb = 0.0f;
  radio.cabinet.cavityDipHz = 0.0f;
  radio.cabinet.cavityDipQ = 0.0f;
  radio.cabinet.cavityDipGainDb = 0.0f;
  // The sound diffuser and cloth should not duplicate the Type-W cone's
  // 5 kHz roll-off. Keep only a mild upper-octave loss here.
  radio.cabinet.grilleLpHz = 8000.0f;
  // The patent cabinet is open-backed and approximately 12 inches deep. A
  // 1.8 ms rear path represents one depth out and back; its deliberately low
  // mix avoids pretending that one fixed listener/wall placement is exact.
  radio.cabinet.rearDelayMs = 1.80f;
  radio.cabinet.rearMix = 0.08f;
  radio.cabinet.rearHpHz = 0.0f;
  radio.cabinet.rearLpHz = 1200.0f;
  // Service Bulletin 258 specifies three identical Type-K clarifiers (part
  // 36-1155). Philco's patent measures its damped six-inch absorber at
  // approximately 108 Hz and shows that clarifiers absorb rather than radiate.
  // Three 3 dB damping sections flatten the modeled 95 Hz boom and produce
  // 9 dB maximum reduction, close to the patent's approximately 10 dB trace.
  // Q=1.5 follows the second-order width implied by the patent's roughly
  // 8 dB one-octave diaphragm-response drop; attenuation is fitted separately.
  radio.cabinet.clarifier1Hz = 108.0f;
  radio.cabinet.clarifier1Q = 1.50f;
  radio.cabinet.clarifier1AttenuationDb = 3.0f;
  radio.cabinet.clarifier2Hz = 108.0f;
  radio.cabinet.clarifier2Q = 1.50f;
  radio.cabinet.clarifier2AttenuationDb = 3.0f;
  radio.cabinet.clarifier3Hz = 108.0f;
  radio.cabinet.clarifier3Q = 1.50f;
  radio.cabinet.clarifier3AttenuationDb = 3.0f;

  radio.finalLimiter.enabled = false;
  radio.finalLimiter.threshold = 1.0f;
  radio.finalLimiter.lookaheadMs = 2.0f;
  radio.finalLimiter.attackMs = 0.5f;
  radio.finalLimiter.releaseMs = 80.0f;
}

void applyTypical1930sPreset(Radio1938& radio) {
  // Representative mass-market 1938 table receiver, anchored to Philco's
  // Model 38-12C rather than claimed as a statistical average of the decade.
  // Philco documented a five-tube 470 kHz superhet, a single 41 output
  // pentode, 2 W audio output, and the five-inch BO-1 field-coil speaker.
  radio.identity.driftDepth = 0.09f;
  radio.globals.ifNoiseMix = 0.27f;
  radio.globals.inputPad = 1.0f;
  radio.globals.enableAutoLevel = false;
  radio.globals.autoTargetDb = -21.0f;
  radio.globals.autoMaxBoostDb = 2.5f;

  radio.tuning.safeBwMinHz = 2200.0f;
  radio.tuning.safeBwMaxHz = 6500.0f;
  radio.tuning.preBwScale = 1.0f;
  radio.tuning.postBwScale = 1.0f;
  radio.tuning.smoothTau = 0.05f;
  radio.tuning.updateEps = 0.25f;
  radio.tuning.magneticTuningEnabled = false;
  radio.tuning.afcCaptureHz = 0.0f;
  radio.tuning.afcMaxCorrectionHz = 0.0f;
  radio.tuning.afcDeadband = 0.0f;
  radio.tuning.afcResponseMs = 0.0f;

  // The 38-12 has no separate RF-amplifier stage. Its tuned antenna secondary
  // feeds the 6A7 converter directly, so the second RF tank used by the
  // high-end receiver is explicitly absent.
  radio.frontEnd.secondTunedCircuitEnabled = false;
  radio.frontEnd.inputHpHz = 180.0f;
  radio.frontEnd.rfGain = 0.86f;
  radio.frontEnd.avcGainDepth = 0.24f;
  radio.frontEnd.selectivityPeakHz = 0.0f;
  radio.frontEnd.selectivityPeakQ = kRadioBiquadQ;
  radio.frontEnd.selectivityPeakGainDb = 0.0f;
  radio.frontEnd.antennaInductanceHenries = 0.011f;
  radio.frontEnd.antennaLoadResistanceOhms = 1800.0f;
  radio.frontEnd.rfInductanceHenries = 0.011f;
  radio.frontEnd.rfLoadResistanceOhms = 1800.0f;

  radio.mixer.rfGridDriveVolts = 0.82f;
  radio.mixer.loGridDriveVolts = 16.0f;
  radio.mixer.loGridBiasVolts = -14.0f;
  radio.mixer.avcGridDriveVolts = 22.0f;
  radio.mixer.plateSupplyVolts = 205.0f;
  radio.mixer.plateDcVolts = 175.0f;
  radio.mixer.screenVolts = 105.0f;
  radio.mixer.biasVolts = -6.0f;
  radio.mixer.cutoffVolts = -42.0f;
  radio.mixer.plateCurrentAmps = 0.0025f;
  radio.mixer.mutualConductanceSiemens = 0.00085f;
  radio.mixer.acLoadResistanceOhms = 12000.0f;
  radio.mixer.plateKneeVolts = 20.0f;
  radio.mixer.gridSoftnessVolts = 2.0f;

  radio.ifStrip.enabled = true;
  radio.ifStrip.ifMinBwHz = 2200.0f;
  radio.ifStrip.primaryInductanceHenries = 0.00022f;
  radio.ifStrip.secondaryInductanceHenries = 0.00025f;
  radio.ifStrip.secondaryLoadResistanceOhms = 560.0f;
  radio.ifStrip.stageGain = 1.00f;
  radio.ifStrip.avcGainDepth = 0.24f;
  radio.ifStrip.ifCenterHz = 470000.0f;
  radio.ifStrip.interstageCouplingCoeff = 0.13f;
  radio.ifStrip.outputCouplingCoeff = 0.10f;

  radio.demod.am.audioDiodeDrop = 0.0100f;
  radio.demod.am.avcDiodeDrop = 0.0080f;
  radio.demod.am.audioJunctionSlopeVolts = 0.0045f;
  radio.demod.am.avcJunctionSlopeVolts = 0.0040f;
  radio.demod.am.detectorStorageCapFarads = 350e-12f;
  radio.demod.am.audioChargeResistanceOhms = 5100.0f;
  radio.demod.am.audioDischargeResistanceOhms = 160000.0f;
  radio.demod.am.avcChargeResistanceOhms = 1000000.0f;
  radio.demod.am.avcDischargeResistanceOhms =
      parallelResistance(1000000.0f, 1000000.0f);
  radio.demod.am.avcFilterCapFarads = 0.15e-6f;
  radio.demod.am.controlVoltageRef = 3.0f;
  radio.demod.am.senseLowHz = 0.0f;
  radio.demod.am.senseHighHz = 0.0f;
  radio.demod.am.afcSenseLpHz = 34.0f;

  // Service Bulletin 284 lists a 500 k volume control, a 4 M first-audio grid
  // leak, a 190 k plate load, and 0.01 uF coupling capacitors around the 75.
  radio.receiverCircuit.enabled = true;
  radio.receiverCircuit.volumeControlResistanceOhms = 500000.0f;
  radio.receiverCircuit.volumeControlTapResistanceOhms = 0.0f;
  radio.receiverCircuit.volumeControlPosition = 1.0f;
  radio.receiverCircuit.volumeControlLoudnessResistanceOhms = 0.0f;
  radio.receiverCircuit.volumeControlLoudnessCapFarads = 0.0f;
  radio.receiverCircuit.couplingCapFarads = 0.01e-6f;
  radio.receiverCircuit.gridLeakResistanceOhms = 4000000.0f;
  radio.receiverCircuit.tubePlateSupplyVolts = 200.0f;
  radio.receiverCircuit.tubePlateDcVolts = 90.0f;
  radio.receiverCircuit.tubeScreenVolts = 0.0f;
  radio.receiverCircuit.tubeBiasVolts = -2.0f;
  radio.receiverCircuit.tubePlateCurrentAmps = 0.00058f;
  radio.receiverCircuit.tubeMutualConductanceSiemens = 0.00110f;
  radio.receiverCircuit.tubeMu = 100.0f;
  radio.receiverCircuit.tubeTriodeConnected = true;
  radio.receiverCircuit.tubeLoadResistanceOhms = 190000.0f;
  radio.receiverCircuit.tubePlateKneeVolts = 12.0f;
  radio.receiverCircuit.tubeGridSoftnessVolts = 0.30f;

  // This set has no user tone or loudness circuit. The remaining colour comes
  // from its documented audio coupling, output transformer, speaker and case.
  radio.tone.presenceHz = 0.0f;
  radio.tone.presenceQ = 0.0f;
  radio.tone.presenceGainDb = 0.0f;
  radio.tone.tiltSplitHz = 0.0f;
  radio.tone.tiltDepthDb = 0.0f;

  radio.power.topology =
      Radio1938::PowerNodeState::Topology::CapCoupledSingleEnded;
  radio.power.sagStart = 0.04f;
  radio.power.sagEnd = 0.16f;
  radio.power.rippleDepth = 0.018f;
  radio.power.sagAttackMs = 45.0f;
  radio.power.sagReleaseMs = 700.0f;
  radio.power.rectifierMinHz = 80.0f;
  radio.power.rippleSecondHarmonicMix = 0.0f;
  radio.power.gainSagPerPower = 0.025f;
  radio.power.rippleGainBase = 0.18f;
  radio.power.rippleGainDepth = 0.10f;
  radio.power.gainMin = 0.89f;
  radio.power.gainMax = 1.02f;
  radio.power.supplyDriveDepth = 0.02f;
  radio.power.supplyBiasDepth = 0.0f;
  radio.power.postLpHz = 0.0f;
  radio.power.gridCouplingCapFarads = 0.01e-6f;
  radio.power.gridLeakResistanceOhms = 490000.0f;

  // No driver tube or phase-splitting transformer exists in the 38-12.
  radio.power.tubePlateSupplyVolts = 0.0f;
  radio.power.tubePlateDcVolts = 0.0f;
  radio.power.tubeScreenVolts = 0.0f;
  radio.power.tubeBiasVolts = 0.0f;
  radio.power.tubePlateCurrentAmps = 0.0f;
  radio.power.tubeMutualConductanceSiemens = 0.0f;
  radio.power.tubeMu = 0.0f;
  radio.power.tubeTriodeConnected = false;
  radio.power.tubeAcLoadResistanceOhms = 0.0f;
  radio.power.tubePlateKneeVolts = 0.0f;
  radio.power.tubeGridSoftnessVolts = 0.0f;
  radio.power.tubeGridCurrentResistanceOhms = 1200.0f;
  radio.power.interstagePrimaryLeakageInductanceHenries = 0.0f;
  radio.power.interstageMagnetizingInductanceHenries = 0.0f;
  radio.power.interstageTurnsRatioPrimaryToSecondary = 0.0f;
  radio.power.interstagePrimaryResistanceOhms = 0.0f;
  radio.power.interstagePrimaryCoreLossResistanceOhms = 0.0f;
  radio.power.interstagePrimaryShuntCapFarads = 0.0f;
  radio.power.interstageSecondaryLeakageInductanceHenries = 0.0f;
  radio.power.interstageSecondaryResistanceOhms = 0.0f;
  radio.power.interstageSecondaryShuntCapFarads = 0.0f;
  radio.power.interstageIntegrationSubsteps = 1;
  radio.power.tubeTriodeLut = {};

  // RCA's 1937 data places a single 41 between 1.5 W at 180 V and 3.4 W at
  // 250 V. Philco rates this receiver at 2 W. The operating point below sits
  // between those published cases and includes the BO-1 transformer's 450-ohm
  // primary winding resistance.
  radio.power.outputGridLeakResistanceOhms = 490000.0f;
  radio.power.outputGridCurrentResistanceOhms = 1200.0f;
  radio.power.outputTubePlateSupplyVolts = 190.0f;
  radio.power.outputTubeScreenVolts = 185.0f;
  radio.power.outputTubeBiasVolts = -13.7f;
  radio.power.outputTubeCutoffVolts = -24.0f;
  radio.power.outputTubePlateCurrentAmps = 0.0200f;
  radio.power.outputTubeMutualConductanceSiemens = 0.00185f;
  radio.power.outputTubeMu = 0.0f;
  radio.power.outputTubePlateToPlateLoadOhms = 0.0f;
  radio.power.outputTubePlateKneeVolts = 20.0f;
  radio.power.outputTubeGridSoftnessVolts = 0.75f;
  radio.power.outputTubeTriodeLut = {};

  // Philco's BO-1 data gives a five-inch cone, 3.5-ohm voice-coil impedance
  // and 450-ohm output-transformer primary DC resistance. RCA's 180 V 41
  // operating point specifies a 9 k primary load, which fixes the turns ratio.
  radio.power.outputTransformerPrimaryLeakageInductanceHenries = 0.080f;
  radio.power.outputTransformerMagnetizingInductanceHenries = 7.0f;
  radio.power.outputTransformerTurnsRatioPrimaryToSecondary =
      std::sqrt(9000.0f / 3.5f);
  radio.power.outputTransformerPrimaryResistanceOhms = 450.0f;
  radio.power.outputTransformerPrimaryCoreLossResistanceOhms = 60000.0f;
  radio.power.outputTransformerPrimaryShuntCapFarads = 0.0f;
  radio.power.outputTransformerSecondaryLeakageInductanceHenries = 90e-6f;
  radio.power.outputTransformerSecondaryResistanceOhms = 0.30f;
  radio.power.outputTransformerSecondaryShuntCapFarads = 0.0f;
  radio.power.outputTransformerIntegrationSubsteps = 4;
  radio.power.outputLoadResistanceOhms = 3.5f;
  radio.power.nominalOutputPowerWatts = 0.0f;
  // Normal programme listening is below the 41's 2 W / roughly 10% distortion
  // rating. Restore playback level only after the physical speaker reference,
  // rather than driving the small output stage continuously at its limit. The
  // compact speaker/cabinet response peaks in the 150-400 Hz passband, so the
  // listening-level calibration is anchored to that strongest band instead
  // of the 1 kHz reference alone.
  radio.output.digitalMakeupGain = 1.95f;

  radio.globals.oversampleFactor = 2.0f;
  radio.globals.oversampleCutoffFraction = 0.45f;
  radio.globals.outputClipThreshold = 1.0f;
  radio.globals.postNoiseMix = 0.35f;
  radio.globals.noiseFloorAmp = 0.0f;

  radio.noiseConfig.enableHumTone = true;
  radio.noiseConfig.humHzDefault = 60.0f;
  radio.noiseConfig.noiseWeightRef = 0.15f;
  radio.noiseConfig.noiseWeightScaleMax = 2.0f;
  radio.noiseConfig.humAmpScale = 0.010f;
  radio.noiseConfig.crackleAmpScale = 0.025f;
  radio.noiseConfig.crackleRateScale = 1.2f;

  radio.noiseRuntime.hum.noiseHpHz = 500.0f;
  radio.noiseRuntime.hum.noiseLpHz = 5000.0f;
  radio.noiseRuntime.hum.filterQ = kRadioBiquadQ;
  radio.noiseRuntime.hum.scAttackMs = 2.0f;
  radio.noiseRuntime.hum.scReleaseMs = 80.0f;
  radio.noiseRuntime.hum.crackleDecayMs = 8.0f;
  radio.noiseRuntime.hum.sidechainMaskRef = 0.15f;
  radio.noiseRuntime.hum.hissMaskDepth = 0.5f;
  radio.noiseRuntime.hum.burstMaskDepth = 0.3f;
  radio.noiseRuntime.hum.pinkFastPole = 0.85f;
  radio.noiseRuntime.hum.pinkSlowPole = 0.97f;
  radio.noiseRuntime.hum.brownStep = 0.02f;
  radio.noiseRuntime.hum.hissDriftPole = 0.9992f;
  radio.noiseRuntime.hum.hissDriftNoise = 0.002f;
  radio.noiseRuntime.hum.hissDriftSlowPole = 0.9998f;
  radio.noiseRuntime.hum.hissDriftSlowNoise = 0.001f;
  radio.noiseRuntime.hum.whiteMix = 0.35f;
  radio.noiseRuntime.hum.pinkFastMix = 0.45f;
  radio.noiseRuntime.hum.pinkDifferenceMix = 0.12f;
  radio.noiseRuntime.hum.pinkFastSubtract = 0.6f;
  radio.noiseRuntime.hum.brownMix = 0.08f;
  radio.noiseRuntime.hum.hissBase = 0.7f;
  radio.noiseRuntime.hum.hissDriftDepth = 0.3f;
  radio.noiseRuntime.hum.hissDriftSlowMix = 0.04f;
  radio.noiseRuntime.hum.humSecondHarmonicMix = 0.42f;

  // Exact Thiele-Small data and anechoic curves do not survive for BO-1.
  // These are conservative reduced-order values for its documented five-inch
  // field-coil construction, intentionally distinct from the 14-inch Type-W.
  radio.speakerStage.drive = 1.0f;
  radio.speakerStage.speaker.suspensionHz = 115.0f;
  radio.speakerStage.speaker.suspensionQ = 1.10f;
  radio.speakerStage.speaker.suspensionGainDb = 2.5f;
  radio.speakerStage.speaker.coneBodyHz = 1450.0f;
  radio.speakerStage.speaker.coneBodyQ = 0.65f;
  radio.speakerStage.speaker.coneBodyGainDb = 0.8f;
  radio.speakerStage.speaker.upperBreakupHz = 2850.0f;
  radio.speakerStage.speaker.upperBreakupQ = 1.0f;
  radio.speakerStage.speaker.upperBreakupGainDb = 0.7f;
  radio.speakerStage.speaker.coneDipHz = 2100.0f;
  radio.speakerStage.speaker.coneDipQ = 0.85f;
  radio.speakerStage.speaker.coneDipGainDb = -0.3f;
  radio.speakerStage.speaker.topLpHz = 6000.0f;
  radio.speakerStage.speaker.hfLossLpHz = 4500.0f;
  radio.speakerStage.speaker.hfLossDepth = 0.12f;
  radio.speakerStage.speaker.filterQ = kRadioBiquadQ;
  radio.speakerStage.speaker.drive = 1.0f;
  radio.speakerStage.speaker.limit = 0.0f;
  radio.speakerStage.speaker.voiceCoilResistanceOhms = 2.9f;
  radio.speakerStage.speaker.voiceCoilInductanceHenries = 0.09e-3f;
  radio.speakerStage.speaker.movingMassKg = 0.0045f;
  radio.speakerStage.speaker.mechanicalQ = 4.0f;
  radio.speakerStage.speaker.electricalQ = 1.8f;
  radio.speakerStage.speaker.forceFactorBl = 0.0f;
  radio.speakerStage.speaker.suspensionComplianceMetersPerNewton = 0.0f;
  radio.speakerStage.speaker.mechanicalDampingNsPerMeter = 0.0f;
  radio.speakerStage.speaker.excursionRef = 3.2f;
  radio.speakerStage.speaker.complianceLossDepth = 0.10f;
  radio.speakerStage.speaker.asymBias = 0.09f;

  radio.cabinet.enabled = true;
  // The compact open-back C cabinet is only about 6.25 inches deep. The
  // sub-millisecond rear path follows that dimension; the broad low-mid panel
  // rise is an explicit inference, not a claimed factory response trace.
  radio.cabinet.panelHz = 155.0f;
  radio.cabinet.panelQ = 1.15f;
  radio.cabinet.panelGainDb = 4.0f;
  radio.cabinet.chassisHz = 0.0f;
  radio.cabinet.chassisQ = 0.0f;
  radio.cabinet.chassisGainDb = 0.0f;
  radio.cabinet.cavityDipHz = 0.0f;
  radio.cabinet.cavityDipQ = 0.0f;
  radio.cabinet.cavityDipGainDb = 0.0f;
  radio.cabinet.grilleLpHz = 8000.0f;
  radio.cabinet.rearDelayMs = 0.95f;
  radio.cabinet.rearMix = 0.14f;
  radio.cabinet.rearHpHz = 90.0f;
  radio.cabinet.rearLpHz = 1000.0f;
  radio.cabinet.clarifier1Hz = 0.0f;
  radio.cabinet.clarifier1Q = 0.0f;
  radio.cabinet.clarifier1AttenuationDb = 0.0f;
  radio.cabinet.clarifier2Hz = 0.0f;
  radio.cabinet.clarifier2Q = 0.0f;
  radio.cabinet.clarifier2AttenuationDb = 0.0f;
  radio.cabinet.clarifier3Hz = 0.0f;
  radio.cabinet.clarifier3Q = 0.0f;
  radio.cabinet.clarifier3AttenuationDb = 0.0f;

  radio.finalLimiter.enabled = false;
  radio.finalLimiter.threshold = 1.0f;
  radio.finalLimiter.lookaheadMs = 2.0f;
  radio.finalLimiter.attackMs = 0.5f;
  radio.finalLimiter.releaseMs = 80.0f;
}

}  // namespace

void applyRadioReceiverProfile(Radio1938& radio,
                               RadioReceiverProfile profile) {
  switch (profile) {
    case RadioReceiverProfile::Philco37116:
      applyPhilco37116Preset(radio);
      break;
    case RadioReceiverProfile::Typical1930s:
      applyTypical1930sPreset(radio);
      break;
  }
}
