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
  radio.tuning.stationFrequencyHz = 1000000.0f;
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

  // Antenna circuit, 6K7 RF stage and second tuned circuit before the 6L7.
  // Coil Q and the reflected antenna/plate loads are priors for a good 1937
  // broadcast coil, not measured Philco windings. At 1 MHz they give loaded
  // Q near 51 and 63. The resonance gain keeps the former effective level at
  // the default 5.5 kHz control position; it is not a measured antenna step-up.
  radio.frontEnd.secondTunedCircuitEnabled = true;
  radio.frontEnd.rfGain = 0.237f;
  // Bulletin 258, Fig. 2: RF 6K7G screen 84 V, grid return -3 V. The 6K7 has
  // the 78's remote-cutoff characteristic; it takes the full AVC line.
  radio.frontEnd.rfStageBiasVolts = -3.0f;
  radio.frontEnd.rfStageScreenVolts = 84.0f;
  radio.frontEnd.antennaInductanceHenries = 230e-6f;
  radio.frontEnd.antennaCoilQ = 100.0f;
  radio.frontEnd.antennaShuntLoadOhms = 150000.0f;
  radio.frontEnd.rfInductanceHenries = 230e-6f;
  radio.frontEnd.rfCoilQ = 100.0f;
  radio.frontEnd.rfShuntLoadOhms = 250000.0f;

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
  // 3 double-tuned transformers; expanded alignment peaks at 465/475 kHz
  // in Bulletin 258. L, effective RF losses and loading below are explicit
  // equivalent-circuit priors, not measured winding data.
  radio.ifStrip.transformerCount = 3;
  radio.ifStrip.primarySeriesResistanceOhms = 15.0f;
  radio.ifStrip.secondarySeriesResistanceOhms = 15.0f;
  radio.ifStrip.referenceTransimpedanceOhms = 17000.0f;
  radio.ifStrip.secondaryLoadResistanceOhms = 200000.0f;
  // IF voltage calibration: a hot-cathode detector needs volts of carrier,
  // followed by the physical volume control. The former sub-volt calibration
  // relied on an almost ideal diode. This remains an unmeasured gain prior.
  radio.ifStrip.stageGain = 46.0f;
  // Bulletin 258: 1st and 2nd IF 6K7G, both on the AVC line, screen 90 V,
  // grid return -3 V.
  radio.ifStrip.avcStageCount = 2;
  radio.ifStrip.stageBiasVolts = -3.0f;
  radio.ifStrip.stageScreenVolts = 90.0f;
  radio.ifStrip.ifCenterHz = 470000.0f;
  radio.ifStrip.interstageCouplingCoeff = 0.032f;
  radio.ifStrip.outputCouplingCoeff = 0.032f;

  // Hot-cathode diode family priors; no measured 6J5 grid-as-diode / 75
  // low-current characteristic was located. 850 K gives kT/e = 73 mV.
  radio.demod.am.audioDiode = {0.00045, 0.073, 0};
  // Bulletin 258, 72/72A/73: 110 pF -- 51 k -- 110 pF detector filter.
  radio.demod.am.detectorStorageCapFarads = 110e-12f;
  radio.demod.am.filterResistanceOhms = 51000;
  radio.demod.am.filterCapFarads = 110e-12f;
  radio.demod.am.audioChargeResistanceOhms = 5100.0f;
  radio.demod.am.audioDischargeResistanceOhms = 160000.0f;
  // Bulletin 258: the A.V.C. 6J5G has its plate grounded, so only its grid-
  // cathode diode works. It takes the last IF plate through 110 pF (80) and
  // returns through 20 k / 1 M with .01 uF (90-92); its cathode sits at +25 V
  // (Fig. 2). It is a delayed AVC rectifier, separate from the audio detector.
  // The documented ~28 V threshold is in real IF plate volts, which the
  // calibrated IF scale does not reproduce; 4 V keeps the detector at its
  // former 2.4 V working point at the nominal 0.12 V carrier. The 75 ms filter
  // is a prior: the AVC bus beyond 90-92 was not traced unambiguously.
  radio.demod.am.avcFromIfEnvelope = true;
  radio.demod.am.avcDelayBiasVolts = 4.0f;
  radio.demod.am.avcFilterResistanceOhms = 500000.0f;
  radio.demod.am.avcFilterCapFarads = 0.15e-6f;
  // Keep the normalized control bus, but make one physical AVC volt
  // impose one volt of mixer grid bias (avcT divides by 1.25).
  radio.demod.am.controlVoltageRef = radio.mixer.avcGridDriveVolts / 1.25f;
  radio.demod.am.senseLowHz = 0.0f;
  radio.demod.am.senseHighHz = 0.0f;
  radio.demod.am.afcSenseLpHz = 34.0f;

  radio.receiverCircuit.enabled = true;
  radio.receiverCircuit.volumeControlResistanceOhms = 2000000.0f;
  radio.receiverCircuit.volumeControlTapResistanceOhms = 1000000.0f;
  // Listener setting, not circuit data: just above the 1 M tap, where the
  // nominal carrier gives the former playback level. The tap network still
  // shapes the response here; lower settings add more bass compensation.
  radio.receiverCircuit.volumeControlPosition = 0.54f;
  // Bulletin 258, 102-105: the 1 M tap returns through 70 k (103) to the tone
  // switch capacitors .008 uF (104) and .006 uF (105) in series. The switch
  // grounds the 70 k end (plain 70 k), the junction (70 k + 8 nF), or neither
  // (70 k + 3.43 nF). Which knob position is which is undocumented; the
  // switch as drawn, both capacitors in series, is the chosen listening
  // setting here, not a known factory default.
  radio.receiverCircuit.volumeControlLoudnessResistanceOhms = 70000.0f;
  radio.receiverCircuit.volumeControlLoudnessCapFarads = 8e-9f * 6e-9f / 14e-9f;
  radio.receiverCircuit.couplingCapFarads = 0.01e-6f;
  radio.receiverCircuit.gridLeakResistanceOhms = 1000000.0f;
  radio.receiverCircuit.tubePlateSupplyVolts = 250.0f;
  radio.receiverCircuit.tubePlateDcVolts = 90.0f;
  // Circuit bias is an inferred DC prior, separate from the RCA characteristic.
  // The printed 90 V service reading used a loading 1000 ohms/V meter; it is
  // not a precision unloaded-voltage measurement.
  radio.receiverCircuit.tubeBiasVolts = -3.7f;
  radio.receiverCircuit.tubeTriodeModel = rca6J5Characteristic();
  radio.receiverCircuit.tubeMu = 20.0f;
  radio.receiverCircuit.tubeTriodeConnected = true;
  // Bulletin 258, resistors 97 and 99, with bypass capacitor 98.
  radio.receiverCircuit.tubeLoadResistanceOhms = 51000.0f;
  radio.receiverCircuit.tubeDcFeedResistanceOhms = 99000.0f;
  // Bulletin 258, 97X: 110 mmfd mica from the first-audio plate to ground.
  radio.receiverCircuit.tubePlateShuntCapFarads = 110e-12f;

  // No undocumented tone shaping: the set's tone comes from 102-107.
  radio.tone.presenceHz = 0.0f;
  radio.tone.presenceQ = 0.0f;
  radio.tone.presenceGainDb = 0.0f;
  radio.tone.tiltSplitHz = 0.0f;
  radio.tone.tiltDepthDb = 0.0f;

  radio.power.topology =
      Radio1938::PowerNodeState::Topology::TransformerCoupledPushPull;
  // Rider 7-31/32: 5U4, 16 uF reservoir, 4.75 H / 80 ohm choke,
  // 8 uF to CT, 10 uF bias bypass and 360+18 ohm field/bias return.
  // Field inductance and total unmodelled RF load are explicit priors.
  radio.powerSupply.config = {};
  radio.powerSupply.otherLoadAmps = 0.010f;
  radio.powerSupply.auxiliaryOtherLoadAmps = 0.030f;
  radio.powerSupply.screenCurrentRatio = 0;
  radio.power.postLpHz = 0.0f;
  // Bulletin 258: .05 uF (96) from the first-audio plate to the 6F6G driver,
  // 240 k (108) grid return. The 500 k expander potentiometer (106), ganged
  // with the I.F. expander, puts the grid on its wiper with 110 mmfd (107) at
  // its far end. Its mechanical law is undocumented: the wiper is set at the
  // coupling end (full audio fidelity), which leaves only the network's load.
  radio.power.gridCouplingCapFarads = 0.05e-6f;
  radio.power.gridLeakResistanceOhms = 240000.0f;
  radio.power.gridPotentiometerOhms = 500000.0f;
  radio.power.gridPotentiometerPosition = 1.0f;
  radio.power.gridPotentiometerCapFarads = 110e-12f;
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
  // Downstream listening level: reserve headroom for the period upper-mid
  // response rise. This does not change the electrical power reference.
  radio.output.digitalMakeupGain = 0.25f;

  radio.globals.oversampleFactor = 2.0f;
  radio.globals.oversampleCutoffFraction = 0.45f;
  radio.globals.outputClipThreshold = 1.0f;
  radio.globals.postNoiseMix = 0.35f;
  radio.globals.noiseFloorAmp = 0.0f;

  radio.noiseConfig.enableHumTone = false;
  radio.noiseConfig.humHzDefault = 60.0f;
  radio.noiseConfig.noiseWeightRef = 0.15f;
  radio.noiseConfig.noiseWeightScaleMax = 2.0f;
  radio.noiseConfig.humAmpScale = 0.008f;
  radio.noiseConfig.crackleAmpScale = 0.025f;
  radio.noiseConfig.crackleRateScale = 1.2f;

  radio.noiseRuntime.hum.noiseHpHz = 500.0f;
  radio.noiseRuntime.hum.noiseLpHz = 5500.0f;
  radio.noiseRuntime.hum.filterQ = kRadioBiquadQ;
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
  // Unsized 70-120 Hz period suspension prior (Seabert/Olson), NOT Type-W T/S.
  // The acoustic fit is a smooth K12 (Knowles Fig. 7A) family comparison.
  // Gains below compensate the complete motor + radiation + mounting chain;
  // they are not individual measured cone-mode amplitudes. See the evidence doc.
  radio.speakerStage.speaker.suspensionHz = 80.0f;
  radio.speakerStage.speaker.suspensionQ = 0.90f;
  radio.speakerStage.speaker.suspensionGainDb = 0.0f;
  radio.speakerStage.speaker.coneBodyHz = 650.0f;
  radio.speakerStage.speaker.coneBodyQ = 1.50f;
  radio.speakerStage.speaker.coneBodyGainDb = 2.2f;
  radio.speakerStage.speaker.upperBreakupHz = 3000.0f;
  radio.speakerStage.speaker.upperBreakupQ = 0.65f;
  radio.speakerStage.speaker.upperBreakupGainDb = 16.0f;
  radio.speakerStage.speaker.coneDipHz = 1850.0f;
  radio.speakerStage.speaker.coneDipQ = 0.85f;
  radio.speakerStage.speaker.coneDipGainDb = 0.0f;
  radio.speakerStage.speaker.topLpHz = 6500.0f;
  radio.speakerStage.speaker.hfLossLpHz = 4000.0f;
  radio.speakerStage.speaker.hfLossDepth = 0.0f;
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
  // Conservative nonlinear/thermal priors, not measured Type-W properties.
  radio.speakerStage.speaker.motorTravelMeters = 0.004f;
  radio.speakerStage.speaker.suspensionTravelMeters = 0.003f;
  radio.speakerStage.speaker.thermalCapacityJoulesPerKelvin = 1.0f;
  radio.speakerStage.speaker.thermalResistanceKelvinPerWatt = 4.0f;
  radio.speakerStage.speaker.asymBias = 0.06f;

  radio.cabinet.enabled = true;
  // Residual LF mounting fit against the whole K12 family response, with the
  // motor and the separate Bobb clarifier proxy included. This is NOT a
  // measured 37-116 baffle corner or a prediction from its exterior dimensions.
  radio.cabinet.mountingHpHz = 120.0f;
  radio.cabinet.mountingHpQ = kRadioBiquadQ;
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
  // The wideband fit already includes mounting. Neither original cloth loss
  // nor a listening-position rear path is measured; do not layer them on top.
  // The 37-116 catalog depth is 17.25 in, not the earlier 116X's 12 in.
  radio.cabinet.grilleLpHz = 0.0f;
  radio.cabinet.rearDelayMs = 0.0f;
  radio.cabinet.rearMix = 0.0f;
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
  radio.tuning.stationFrequencyHz = 1000000.0f;
  radio.tuning.smoothTau = 0.05f;
  radio.tuning.updateEps = 0.25f;
  radio.tuning.magneticTuningEnabled = false;
  radio.tuning.afcCaptureHz = 0.0f;
  radio.tuning.afcMaxCorrectionHz = 0.0f;
  radio.tuning.afcDeadband = 0.0f;
  radio.tuning.afcResponseMs = 0.0f;

  // The 38-12 has no separate RF-amplifier stage. Its tuned antenna secondary
  // feeds the 6A7 converter directly, so the second RF tank used by the
  // high-end receiver is explicitly absent. A plainer coil and the heavier
  // coupling of a short household antenna give loaded Q near 37 at 1 MHz
  // (priors, not measured 38-12 windings). The resonance gain keeps the
  // former effective level at the default 5.5 kHz control position.
  radio.frontEnd.secondTunedCircuitEnabled = false;
  radio.frontEnd.rfGain = 0.509f;
  // No RF amplifier, so no AVC-controlled tube before the 6A7 converter.
  // Its operating point is unused; reset it to the neutral defaults.
  radio.frontEnd.rfStageBiasVolts = -3.0f;
  radio.frontEnd.rfStageScreenVolts = 100.0f;
  radio.frontEnd.antennaInductanceHenries = 230e-6f;
  radio.frontEnd.antennaCoilQ = 80.0f;
  radio.frontEnd.antennaShuntLoadOhms = 100000.0f;
  radio.frontEnd.rfInductanceHenries = 0.0f;
  radio.frontEnd.rfCoilQ = 0.0f;
  radio.frontEnd.rfShuntLoadOhms = 0.0f;

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
  radio.ifStrip.transformerCount = 2;
  radio.ifStrip.primarySeriesResistanceOhms = 5.0f;
  radio.ifStrip.secondarySeriesResistanceOhms = 5.0f;
  radio.ifStrip.referenceTransimpedanceOhms = 40000.0f;
  radio.ifStrip.secondaryLoadResistanceOhms = 200000.0f;
  radio.ifStrip.stageGain = 20.0f;
  // Bulletin 284, Fig. 1: one 78 IF stage, screen 90 V, grid -2.3 V.
  radio.ifStrip.avcStageCount = 1;
  radio.ifStrip.stageBiasVolts = -2.3f;
  radio.ifStrip.stageScreenVolts = 90.0f;
  radio.ifStrip.ifCenterHz = 470000.0f;
  radio.ifStrip.interstageCouplingCoeff = 0.012f;
  radio.ifStrip.outputCouplingCoeff = 0.012f;

  // Hot-cathode diode family priors; no measured 6J5 grid-as-diode / 75
  // low-current characteristic was located. 850 K gives kT/e = 73 mV.
  radio.demod.am.audioDiode = {8.5e-05, 0.073, 0};
  // Bulletin 284 / Rider VIII 8-69, 12B/12C/13: 100 pF -- 51 k -- 100 pF.
  radio.demod.am.detectorStorageCapFarads = 100e-12f;
  radio.demod.am.filterResistanceOhms = 51000;
  radio.demod.am.filterCapFarads = 100e-12f;
  radio.demod.am.audioChargeResistanceOhms = 5100.0f;
  radio.demod.am.audioDischargeResistanceOhms = 0;  // 500 k volume pot is the DC load
  // Bulletin 284, 9/10: simple (undelayed) AVC. The 6A7 and 78 grid returns
  // take the volume-control DC through 2 M and 0.03 uF (60 ms).
  radio.demod.am.avcFromIfEnvelope = false;
  radio.demod.am.avcDelayBiasVolts = 0.0f;
  radio.demod.am.avcFilterResistanceOhms = 2000000.0f;
  radio.demod.am.avcFilterCapFarads = 0.03e-6f;
  // Keep the normalized control bus, but make one physical AVC volt
  // impose one volt of mixer grid bias (avcT divides by 1.25).
  radio.demod.am.controlVoltageRef = radio.mixer.avcGridDriveVolts / 1.25f;
  radio.demod.am.senseLowHz = 0.0f;
  radio.demod.am.senseHighHz = 0.0f;
  radio.demod.am.afcSenseLpHz = 34.0f;

  // Service Bulletin 284 lists a 500 k volume control, a 4 M first-audio grid
  // leak, a 190 k plate load, and 0.01 uF coupling capacitors around the 75.
  radio.receiverCircuit.enabled = true;
  radio.receiverCircuit.volumeControlResistanceOhms = 500000.0f;
  radio.receiverCircuit.volumeControlTapResistanceOhms = 0.0f;
  // Listener setting, not circuit data. The 38-12's simple AVC holds the
  // detector lower than the former weak AVC did; this untapped 500 k control
  // is turned up to keep the former playback level at the nominal carrier.
  radio.receiverCircuit.volumeControlPosition = 0.085f;
  radio.receiverCircuit.volumeControlLoudnessResistanceOhms = 0.0f;
  radio.receiverCircuit.volumeControlLoudnessCapFarads = 0.0f;
  radio.receiverCircuit.couplingCapFarads = 0.01e-6f;
  radio.receiverCircuit.gridLeakResistanceOhms = 4000000.0f;
  radio.receiverCircuit.tubePlateSupplyVolts = 200.0f;
  radio.receiverCircuit.tubePlateDcVolts = 90.0f;
  radio.receiverCircuit.tubeBiasVolts = -0.5f;
  radio.receiverCircuit.tubeMu = 100.0f;
  radio.receiverCircuit.tubeTriodeModel = rca75Characteristic();
  radio.receiverCircuit.tubeDcFeedResistanceOhms = 0.0f;
  radio.receiverCircuit.tubeTriodeConnected = true;
  radio.receiverCircuit.tubeLoadResistanceOhms = 190000.0f;
  // Rider VIII 8-69, part 17: 250 mmf mica from the 75 plate to ground.
  radio.receiverCircuit.tubePlateShuntCapFarads = 250e-12f;

  // This set has no user tone or loudness circuit. The remaining colour comes
  // from its documented audio coupling, output transformer, speaker and case.
  radio.tone.presenceHz = 0.0f;
  radio.tone.presenceQ = 0.0f;
  radio.tone.presenceGainDb = 0.0f;
  radio.tone.tiltSplitHz = 0.0f;
  radio.tone.tiltDepthDb = 0.0f;

  radio.power.topology =
      Radio1938::PowerNodeState::Topology::CapCoupledSingleEnded;
  // Bulletin 284: 84 rectifier, 12/4 uF filter, 1500 ohm field,
  // 250+70 ohm centre-tap return; no separate first-audio supply decoupling.
  radio.powerSupply.config = {};
  radio.powerSupply.config.rectifier.perveance = 0.00058f;
  radio.powerSupply.config.secondaryResistanceOhms = 400;
  radio.powerSupply.config.reservoirFarads = 12e-6f;
  radio.powerSupply.config.filterFarads = 4e-6f;
  radio.powerSupply.config.filterReturnsToCentreTap = false;
  radio.powerSupply.config.filterHenries = 10;  // field inductance prior
  radio.powerSupply.config.filterResistanceOhms = 1500;
  radio.powerSupply.config.returnFarads = 0;
  radio.powerSupply.config.returnHenries = 0;
  radio.powerSupply.config.returnResistanceOhms = 320;
  radio.powerSupply.config.auxiliaryResistanceOhms = 0;
  radio.powerSupply.config.auxiliaryHenries = 0;
  radio.powerSupply.config.auxiliaryFarads = 0;
  radio.powerSupply.config.audioResistanceOhms = 0;
  radio.powerSupply.config.audioFarads = 0;
  radio.powerSupply.otherLoadAmps = 0.020f;
  radio.powerSupply.auxiliaryOtherLoadAmps = 0;
  radio.powerSupply.screenCurrentRatio = 0.15f;
  radio.power.postLpHz = 0.0f;
  radio.power.gridCouplingCapFarads = 0.01e-6f;
  radio.power.gridLeakResistanceOhms = 490000.0f;
  // No potentiometer between the 75 and the 41 grid.
  radio.power.gridPotentiometerOhms = 0.0f;
  radio.power.gridPotentiometerPosition = 1.0f;
  radio.power.gridPotentiometerCapFarads = 0.0f;

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
  // Rider VIII 8-69, part 21: 0.01 uF across the output-transformer primary
  // (41 plate to B+), the fixed top cut of a pentode output stage.
  radio.power.outputTransformerPrimaryShuntCapFarads = 0.01e-6f;
  radio.power.outputTransformerSecondaryLeakageInductanceHenries = 90e-6f;
  radio.power.outputTransformerSecondaryResistanceOhms = 0.30f;
  radio.power.outputTransformerSecondaryShuntCapFarads = 0.0f;
  radio.power.outputTransformerIntegrationSubsteps = 4;
  radio.power.outputLoadResistanceOhms = 3.5f;
  radio.power.nominalOutputPowerWatts = 0.0f;
  // Normal programme listening is below the 41's 2 W / roughly 10% distortion
  // rating. Calibrate listening level after the physical speaker reference,
  // allowing headroom for the complete acoustic curve, including 2-3 kHz.
  radio.output.digitalMakeupGain = 0.60f;

  radio.globals.oversampleFactor = 2.0f;
  radio.globals.oversampleCutoffFraction = 0.45f;
  radio.globals.outputClipThreshold = 1.0f;
  radio.globals.postNoiseMix = 0.35f;
  radio.globals.noiseFloorAmp = 0.0f;

  radio.noiseConfig.enableHumTone = false;
  radio.noiseConfig.humHzDefault = 60.0f;
  radio.noiseConfig.noiseWeightRef = 0.15f;
  radio.noiseConfig.noiseWeightScaleMax = 2.0f;
  radio.noiseConfig.humAmpScale = 0.010f;
  radio.noiseConfig.crackleAmpScale = 0.025f;
  radio.noiseConfig.crackleRateScale = 1.2f;

  radio.noiseRuntime.hum.noiseHpHz = 500.0f;
  radio.noiseRuntime.hum.noiseLpHz = 5000.0f;
  radio.noiseRuntime.hum.filterQ = kRadioBiquadQ;
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

  // No exact BO-1 T/S or response was located. The 200 Hz choice is within
  // Knowles's stated 180-250 Hz natural-frequency prior for a small speaker.
  // Fit the full acoustic chain to the K5 small-cabinet example, not Quam's
  // strong bass in an unspecified mounting. These EQ sections are a smooth
  // residual fit; their gains are not claimed physical cone-mode measurements.
  radio.speakerStage.drive = 1.0f;
  radio.speakerStage.speaker.suspensionHz = 200.0f;
  radio.speakerStage.speaker.suspensionQ = 1.10f;
  radio.speakerStage.speaker.suspensionGainDb = 0.0f;
  radio.speakerStage.speaker.coneBodyHz = 1000.0f;
  radio.speakerStage.speaker.coneBodyQ = 1.50f;
  radio.speakerStage.speaker.coneBodyGainDb = 3.5f;
  radio.speakerStage.speaker.upperBreakupHz = 2800.0f;
  radio.speakerStage.speaker.upperBreakupQ = 0.90f;
  radio.speakerStage.speaker.upperBreakupGainDb = 14.4f;
  radio.speakerStage.speaker.coneDipHz = 4000.0f;
  radio.speakerStage.speaker.coneDipQ = 2.50f;
  radio.speakerStage.speaker.coneDipGainDb = -2.7f;
  radio.speakerStage.speaker.topLpHz = 6500.0f;
  radio.speakerStage.speaker.hfLossLpHz = 4500.0f;
  radio.speakerStage.speaker.hfLossDepth = 0.0f;
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
  // Conservative nonlinear/thermal priors, not measured BO-1 properties.
  radio.speakerStage.speaker.motorTravelMeters = 0.0015f;
  radio.speakerStage.speaker.suspensionTravelMeters = 0.001f;
  radio.speakerStage.speaker.thermalCapacityJoulesPerKelvin = 0.25f;
  radio.speakerStage.speaker.thermalResistanceKelvinPerWatt = 12.0f;
  radio.speakerStage.speaker.asymBias = 0.09f;

  radio.cabinet.enabled = true;
  // Smooth approximation to K5's cabinet-minus-large-baffle LF observations.
  // This is the TOTAL mounting difference, not an isolated dipole function.
  // Do not add a second cabinet transfer to the resulting in-cabinet curve.
  radio.cabinet.mountingHpHz = 235.0f;
  radio.cabinet.mountingHpQ = kRadioBiquadQ;
  radio.cabinet.panelHz = 0.0f;
  radio.cabinet.panelQ = 0.0f;
  radio.cabinet.panelGainDb = 0.0f;
  radio.cabinet.chassisHz = 0.0f;
  radio.cabinet.chassisQ = 0.0f;
  radio.cabinet.chassisGainDb = 0.0f;
  radio.cabinet.cavityDipHz = 0.0f;
  radio.cabinet.cavityDipQ = 0.0f;
  radio.cabinet.cavityDipGainDb = 0.0f;
  radio.cabinet.grilleLpHz = 0.0f;
  radio.cabinet.rearDelayMs = 0.0f;
  radio.cabinet.rearMix = 0.0f;
  radio.cabinet.rearHpHz = 0.0f;
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
