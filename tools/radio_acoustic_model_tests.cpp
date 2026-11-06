#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

#include "audiofilter/radio1938/models/am_detector_internal.h"
#include "audiofilter/radio1938/models/power_stage_internal.h"
#include "audiofilter/radio1938/models/power_stage_solver.h"
#include "audiofilter/radio1938/models/power_supply.h"
#include "audiofilter/radio1938/models/receiver_input_network.h"
#include "audiofilter/radio1938/preview/radio_preview_pipeline.h"
#include "audiofilter/radio1938/radio_pipeline_types.h"
#include "radio.h"

namespace {
constexpr double kPi = 3.14159265358979323846;

void check(bool value, const char* message) {
  if (!value) {
    std::cerr << "radio_acoustic_model_tests: " << message << '\n';
    std::exit(1);
  }
}

SpeakerSim makeSpeaker(RadioReceiverProfile profile, float fs) {
  Radio1938 radio;
  radio.applyReceiverProfile(profile);
  auto speaker = radio.speakerStage.speaker;
  configureSpeakerElectromechanics(speaker, fs, radio.power.outputLoadResistanceOhms);
  resetSpeakerElectromechanics(speaker);
  return speaker;
}

struct ToneResult {
  double fundamental, third, impedance;
  float temperature;
};
ToneResult speakerTone(SpeakerSim speaker, float fs, float frequency, float volts) {
  std::complex<double> fundamental{}, third{};
  double voltageSq = 0, currentSq = 0;
  const int half = static_cast<int>(fs * .25);
  for (int i = 0; i < 2 * half; ++i) {
    const double phase = 2 * kPi * frequency * i / fs;
    const float v = volts * static_cast<float>(std::sin(phase));
    advanceVoltageDrivenSpeaker(speaker, v, 1.0f / fs);
    check(std::isfinite(speaker.acousticDriveVolts), "nonfinite cone acceleration");
    if (i >= half) {
      fundamental +=
          static_cast<double>(speaker.acousticDriveVolts) * std::polar(1.0, -phase);
      third +=
          static_cast<double>(speaker.acousticDriveVolts) * std::polar(1.0, -3 * phase);
      voltageSq += v * v;
      currentSq += speaker.electricalCurrentAmps * speaker.electricalCurrentAmps;
    }
  }
  check(speaker.solverNonconvergenceCount == 0, "speaker Newton solve did not converge");
  return {2 * std::abs(fundamental) / half, 2 * std::abs(third) / half,
          std::sqrt(voltageSq / currentSq), speaker.coilTemperatureRiseKelvin};
}

struct ReceiverInputReference {
  std::complex<double> grid, inputCurrent;
};

ReceiverInputReference receiverInputReference(
    const Radio1938::ReceiverCircuitNodeState& r, double fs, double hz) {
  // Independent series/parallel impedances, with the exact z-domain impedance
  // of a trapezoidal capacitor. No conductance-matrix stamps or tiny wires.
  using C = std::complex<double>;
  const C z = std::polar(1.0, -2 * kPi * hz / fs);
  const C s = 2 * fs * (C{1, 0} - z) / (C{1, 0} + z);
  const double total = r.volumeControlResistanceOhms;
  const double w = r.volumeControlPosition * total;
  const double t = r.volumeControlTapResistanceOhms;
  const double blend = t > 0 ? std::clamp((t - w) / t, 0.0, 1.0) : 0;
  const C yw = C{1, 0} / (double(r.gridLeakResistanceOhms) +
                          C{1, 0} / (s * double(r.couplingCapFarads)));
  const C yt = blend * (s * double(r.volumeControlLoudnessCapFarads) +
                        (r.volumeControlLoudnessResistanceOhms > 0
                             ? 1.0 / r.volumeControlLoudnessResistanceOhms
                             : 0));
  const bool wiperAbove = w >= t;
  const double bottom = std::min(w, t), middle = std::abs(w - t);
  const C bottomZ = bottom / (C{1, 0} + bottom * (wiperAbove ? yt : yw));
  const C lowerZ = middle + bottomZ;
  const C upperZ = lowerZ / (C{1, 0} + lowerZ * (wiperAbove ? yw : yt));
  const C current = C{1, 0} / (total - std::max(w, t) + upperZ);
  const C upperV = current * upperZ;
  const C lowerV = std::abs(lowerZ) > 0 ? upperV * bottomZ / lowerZ : C{};
  return {(wiperAbove ? upperV : lowerV) * yw * double(r.gridLeakResistanceOhms),
          current};
}

void testReceiverInput(bool validate) {
  for (auto profile :
       {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    for (float position : {0.0f, .25f, .4999f, .5f, .5001f, .75f, 1.0f}) {
      for (float fs : {44100.0f, 48000.0f, 96000.0f}) {
        for (double hz : {100.0, 1000.0, 5000.0}) {
          Radio1938 radio;
          radio.applyReceiverProfile(profile);
          radio.sampleRate = fs;
          auto& receiver = radio.receiverCircuit;
          receiver.volumeControlPosition = position;
          RadioSampleContext ctx{};
          radio.demod.am.detectorNode = 1;
          RadioReceiverInputNetworkNode::run(radio, 0, ctx);
          const float dcGrid = receiver.gridVoltage;
          const float dcTap = receiver.volumeControlTapVoltage;
          double square = 0;
          std::complex<double> fundamental{}, third{}, current{};
          const auto reference = receiverInputReference(receiver, fs, hz);
          for (int i = 0; i < static_cast<int>(fs); ++i) {
            const double phase = 2 * kPi * hz * i / fs;
            radio.demod.am.detectorNode = 1 + .1f * std::sin(phase);
            const auto step = receiverInputNetworkStep(receiver, 1.0 / fs);
            const double inputCurrent =
                step.inputConductance * radio.demod.am.detectorNode + step.historyCurrent;
            RadioReceiverInputNetworkNode::run(radio, 0, ctx);
            if (i >= static_cast<int>(fs / 2)) {
              square += receiver.gridVoltage * receiver.gridVoltage;
              fundamental +=
                  static_cast<double>(receiver.gridVoltage) * std::polar(1.0, -phase);
              third +=
                  static_cast<double>(receiver.gridVoltage) * std::polar(1.0, -3 * phase);
              current += inputCurrent * std::polar(1.0, -phase);
            }
          }
          const auto measuredGrid = fundamental * std::complex<double>{0, 40.0 / fs};
          const auto measuredCurrent = current * std::complex<double>{0, 40.0 / fs};
          if (validate) {
            check(std::isfinite(square) && std::abs(dcGrid) < 1e-7 && dcTap >= 0 &&
                      dcTap <= 1,
                  "receiver input lost its DC operating point at a pot boundary");
            check(std::abs(measuredGrid - reference.grid) < 4e-5,
                  "receiver input disagrees with independent RC impedance response");
            check(std::abs(measuredCurrent - reference.inputCurrent) < 1e-9,
                  "detector port current disagrees with the connected RC network");
            check(std::abs(third) < 1e-4 * std::max(std::abs(fundamental), 1.0),
                  "passive receiver input generated spurious harmonics");
          }
          if (fs == 48000 && hz == 1000)
            std::cout << "receiver_input," << radioReceiverProfileName(profile) << ','
                      << position << ",dc_grid=" << dcGrid << ",dc_tap=" << dcTap
                      << ",rms=" << std::sqrt(square / (fs / 2))
                      << ",gain=" << std::abs(measuredGrid) << ",h3="
                      << std::abs(third) / std::max(std::abs(fundamental), 1e-20) << '\n';
        }
      }
    }
  }
}

void testDetectorMultirateCoupling(bool validate) {
  constexpr int substeps = 16;
  constexpr double fs = 48000, dt = 1 / fs, fineDt = dt / substeps;
  for (auto profile :
       {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    for (float position : {.25f, 1.0f}) {
      for (double hz : {100.0, 1000.0, 5000.0}) {
        Radio1938 radio;
        radio.applyReceiverProfile(profile);
        auto coarse = radio.receiverCircuit;
        coarse.volumeControlPosition = position;
        primeReceiverInputNetwork(coarse, 2);
        auto fine = coarse;
        auto a = radio.demod.am, b = a;
        a.detectorStorageNode = b.detectorStorageNode = 2;
        a.filteredStorageNode = b.filteredStorageNode = 2;
        std::complex<double> coarseGrid{}, fineGrid{};
        for (int i = 0; i < 9600; ++i) {
          const auto port = receiverInputNetworkStep(coarse, dt);
          const double leak = a.audioDischargeResistanceOhms > 0 ? 1.0 / a.audioDischargeResistanceOhms : 0;
          const am_detector_internal::DetectorLoad coarseLoad{
              static_cast<float>(leak + port.inputConductance),
              static_cast<float>(port.historyCurrent)};
          double averageV = 0, averageGrid = 0;
          for (int j = 0; j < substeps; ++j) {
            const double phase = 2 * kPi * hz * (i + (j + .5) / substeps) / fs;
            const float drive = static_cast<float>(2 + .6 * std::sin(phase));
            const float previousA = a.filteredStorageNode;
            const float previousGrid = fine.gridVoltage;
            am_detector_internal::stepDetectorAudioNode(
                a, fineDt, drive, coarseLoad);
            const auto finePort = receiverInputNetworkStep(fine, fineDt);
            am_detector_internal::stepDetectorAudioNode(
                b, fineDt, drive,
                {static_cast<float>(leak + finePort.inputConductance),
                 static_cast<float>(finePort.historyCurrent)});
            advanceReceiverInputNetwork(fine, finePort, b.filteredStorageNode);
            averageV += .5 * (previousA + a.filteredStorageNode) / substeps;
            averageGrid += .5 * (previousGrid + fine.gridVoltage) / substeps;
          }
          advanceReceiverInputNetwork(coarse, port, averageV);
          if (i >= 4800) {
            const auto phase = std::polar(1.0, -2 * kPi * hz * i / fs);
            coarseGrid += double(coarse.gridVoltage) * phase;
            fineGrid += averageGrid * phase;
          }
        }
        const double error = std::abs(coarseGrid - fineGrid) / std::abs(fineGrid);
        std::cout << "receiver_multirate," << radioReceiverProfileName(profile) << ','
                  << position << ',' << hz << ",relative_error=" << error << '\n';
        if (validate)
          check(error < .02,
                "detector/input coupling differs from fully coupled 16x reference");
      }
    }
  }
}

void testDetectorInputCoupling() {
  for (auto profile :
       {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    double firstDetectorVoltage = 0;
    for (float gridLeak : {100000.0f, 4000000.0f}) {
      Radio1938 radio;
      radio.applyReceiverProfile(profile);
      radio.sampleRate = 48000;
      radio.ifStrip.enabled = false;
      auto& receiver = radio.receiverCircuit;
      receiver.gridLeakResistanceOhms = gridLeak;
      radio.demod.am.init(48000, 5500);
      RadioSampleContext ctx{};
      for (int i = 0; i < 24000; ++i) {
        radio.demod.am.run(2, 0, radio);
        RadioReceiverInputNetworkNode::run(radio, 0, ctx);
      }
      const double v = radio.demod.am.detectorNode;
      const auto port = receiverInputNetworkStep(receiver, 1.0 / 48000);
      check(std::abs(port.inputConductance * v + port.historyCurrent -
                     receiverInputNetworkDc(receiver).inputConductance * v) < 2e-10,
            "coupling capacitor incorrectly conducts DC into the grid leak");
      if (firstDetectorVoltage > 0)
        check(std::abs(v - firstDetectorVoltage) < 2e-5,
              "grid leak changed detector DC through a blocking capacitor");
      firstDetectorVoltage = v;
    }
  }
}

void testSpeaker(RadioReceiverProfile profile, float outputRate) {
  const bool small = profile == RadioReceiverProfile::Typical1930s;
  // Production runs the power stage at 2x with 4/8 transformer substeps.
  const float fs = (small ? 8 : 16) * outputRate;
  auto nonlinear = makeSpeaker(profile, fs);
  nonlinear.thermalCapacityJoulesPerKelvin = 0;
  auto linear = nonlinear;
  linear.motorTravelMeters = linear.suspensionTravelMeters = 0;
  for (float hz : {80.0f, 200.0f, 1000.0f, 5000.0f}) {
    const auto measured = speakerTone(linear, fs, hz, .01f);
    const std::complex<double> jw{0, 2 * kPi * hz};
    const auto mechanical =
        static_cast<double>(linear.effectiveMechanicalDampingNsPerMeter) +
        jw * static_cast<double>(linear.effectiveMovingMassKg) +
        1.0 / (jw *
               static_cast<double>(linear.effectiveSuspensionComplianceMetersPerNewton));
    const double bl = linear.effectiveForceFactorBl;
    const auto z = static_cast<double>(linear.effectiveVoiceCoilResistanceOhms) +
                   jw * static_cast<double>(linear.effectiveVoiceCoilInductanceHenries) +
                   bl * bl / mechanical;
    std::cout << "impedance," << radioReceiverProfileName(profile) << ',' << outputRate
              << ',' << hz << ',' << measured.impedance << ',' << std::abs(z) << '\n';
    check(std::fabs(20 * std::log10(measured.impedance / std::abs(z))) < .3,
          "small-signal load differs from analytic RLC impedance");
    const auto quiet = speakerTone(nonlinear, fs, hz, .01f);
    check(std::fabs(20 * std::log10(quiet.fundamental / measured.fundamental)) < .02,
          "nonlinearity changed the small-signal response");
  }
  const float bass = small ? 160.0f : 80.0f;
  const auto quiet = speakerTone(nonlinear, fs, bass, .05f);
  const auto loud = speakerTone(nonlinear, fs, bass, small ? 3.0f : 7.0f);
  const auto linearLoud = speakerTone(linear, fs, bass, small ? 3.0f : 7.0f);
  const double quietDistortion = quiet.third / quiet.fundamental;
  const double loudDistortion = loud.third / loud.fundamental;
  check(loudDistortion > 8 * quietDistortion && loudDistortion > .002,
        "bass distortion did not grow with cone excursion");
  check(loud.third > 10 * linearLoud.third,
        "harmonics originate in the numerical integrator");
  std::cout << radioReceiverProfileName(profile) << ',' << outputRate
            << ",bass_h3_percent," << 100 * loudDistortion << '\n';

  // A nonlinear spring has an x^4 potential. With no drive, the passive
  // electrical and mechanical system must dissipate its stored energy.
  auto ringing = nonlinear;
  ringing.coneDisplacementMeters = .5f * ringing.suspensionTravelMeters;
  const auto energy = [&]() {
    const double x = ringing.coneDisplacementMeters;
    const double w = x / ringing.suspensionTravelMeters;
    return .5 * ringing.effectiveVoiceCoilInductanceHenries *
               ringing.electricalCurrentAmps * ringing.electricalCurrentAmps +
           .5 * ringing.effectiveMovingMassKg * ringing.coneVelocityMetersPerSecond *
               ringing.coneVelocityMetersPerSecond +
           .5 * x * x / ringing.effectiveSuspensionComplianceMetersPerNewton *
               (1 + .5 * w * w);
  };
  const double initial = energy();
  double previous = initial;
  for (int i = 0; i < static_cast<int>(fs * .2); ++i) {
    advanceVoltageDrivenSpeaker(ringing, 0, 1 / fs);
    const double next = energy();
    check(next <= previous + initial * 1e-7, "undriven speaker created energy");
    previous = next;
  }
  check(previous < initial * .001, "speaker failed to settle");
}

void testThermalMemory() {
  constexpr float fs = 48000;
  auto speaker = makeSpeaker(RadioReceiverProfile::Typical1930s, fs);
  for (int i = 0; i < 4 * fs; ++i)
    advanceVoltageDrivenSpeaker(speaker, 3, 1 / fs);
  const float hot = speaker.coilTemperatureRiseKelvin;
  check(hot > 5 && hot < 80, "thermal model did not accumulate plausible heating");
  for (int i = 0; i < 4 * fs; ++i)
    advanceVoltageDrivenSpeaker(speaker, 0, 1 / fs);
  check(speaker.coilTemperatureRiseKelvin < .4f * hot &&
            speaker.coilTemperatureRiseKelvin > 0,
        "coil did not cool with thermal memory");
  resetSpeakerElectromechanics(speaker);
  check(speaker.coilTemperatureRiseKelvin == 0 && speaker.acousticDriveVolts == 0 &&
            speaker.coneDisplacementMeters == 0,
        "speaker reset retained energy");
}

struct RippleSpectrum {
  double mean = 0.0;
  std::complex<double> fundamental{}, second{};
};

RippleSpectrum measureSupplyRipple(float sampleRate, float mainsHz, float filterCap = 8e-6f) {
  RectifierSupply supply;
  RectifierSupply::Config config;
  config.filterFarads = filterCap;
  supply.configure(config, 2 * sampleRate, mainsHz);
  const RectifierSupply::Load load{.12, .018, .001};
  supply.start(325, load);
  RippleSpectrum result;
  const int frames = static_cast<int>(2 * sampleRate);
  for (int i = 0; i < frames; ++i) {
    supply.step(load);
    const double value = supply.volts(RectifierSupply::Main);
    const double phase = 2 * kPi * 2 * mainsHz * i / frames;
    result.mean += value;
    result.fundamental += value * std::polar(1.0, -phase);
    result.second += value * std::polar(1.0, -2 * phase);
  }
  result.mean /= frames;
  result.fundamental *= 2.0 / frames;
  result.second *= 2.0 / frames;
  return result;
}

void testPowerSupplyRipple(bool validate) {
  for (float mains : {50.f, 60.f}) {
    const auto reference = measureSupplyRipple(96000, mains);
    for (float fs : {44100.f, 48000.f}) {
      const auto plain = measureSupplyRipple(fs, mains);
      const auto larger = measureSupplyRipple(fs, mains, 16e-6f);
      std::cout << "supply_ripple," << fs << ',' << mains
                << ",mean_V=" << plain.mean
                << ",at_2x_mains_V=" << std::abs(plain.fundamental) << '\n';
      if (validate) {
        check(std::abs(plain.mean - 325) < 1,
              "supply idle calibration missed the nominal DC rail");
        check(std::abs(plain.fundamental) > .01 &&
                  std::abs(plain.fundamental) > std::abs(plain.second),
              "full-wave supply lacks dominant twice-mains ripple");
        check(std::abs(larger.fundamental) < .8 * std::abs(plain.fundamental),
              "larger filter capacitor failed to reduce ripple");
        check(std::abs(std::abs(plain.fundamental / reference.fundamental) - 1) < .02,
              "supply ripple diverges from higher-rate integration");
      }
    }
  }
}

std::vector<RadioRoomSample> roomImpulse(RadioRoomConfig c, float fs, float radius) {
  RadioRoomRenderer room;
  room.initialize(c, fs, radius);
  std::vector<RadioRoomSample> result(static_cast<size_t>(fs * 2));
  for (size_t i = 0; i < result.size(); ++i)
    result[i] = room.process(i == 0 ? .1f : 0);
  room.reset();
  for (size_t i = 0; i < result.size(); ++i) {
    const auto sample = room.process(i == 0 ? .1f : 0);
    check(sample.left == result[i].left && sample.right == result[i].right,
          "room reset was not deterministic");
  }
  return result;
}

double tailEnergy(const std::vector<RadioRoomSample>& ir, size_t first, size_t last) {
  double result = 0;
  for (size_t i = first; i < last; ++i) {
    check(std::isfinite(ir[i].left) && std::isfinite(ir[i].right), "nonfinite room tail");
    result += ir[i].left * ir[i].left + ir[i].right * ir[i].right;
  }
  return result;
}

std::complex<double> response(const std::vector<RadioRoomSample>& ir, float fs, float hz,
                              bool right = false) {
  std::complex<double> sum{};
  for (size_t i = 0; i < ir.size(); ++i) {
    sum += static_cast<double>(right ? ir[i].right : ir[i].left) *
           std::polar(1.0, -2 * kPi * hz * i / fs);
  }
  return sum;
}

void testRoom(float fs) {
  RadioRoomConfig c;
  RadioRoomRenderer bypass;
  c.enabled = false;
  bypass.initialize(c, fs, .05f);
  for (float x : {-.91f, 0.0f, .004f, .82f}) {
    const auto y = bypass.process(x);
    check(y.left == x && y.right == x, "room disabled is not a bit-exact bypass");
  }
  c.enabled = true;
  const auto defaultIr = roomImpulse(c, fs, .14f);
  check(tailEnergy(defaultIr, static_cast<size_t>(.04 * fs),
                   static_cast<size_t>(.2 * fs)) > 1e-10,
        "room has no late reflections");
  check(tailEnergy(defaultIr, static_cast<size_t>(1.0 * fs), defaultIr.size()) < 1e-12,
        "room tail did not decay");
  double difference = 0;
  for (auto v : defaultIr)
    difference += std::fabs(v.left - v.right);
  check(difference > .001, "asymmetric room collapsed to dual mono");
  c.headphones = false;
  const auto speakers = roomImpulse(c, fs, .14f);
  for (auto v : speakers)
    check(v.left == v.right, "speaker mode retained binaural processing");

  c = RadioRoomConfig{};
  c.reflections = 0;
  auto direct = roomImpulse(c, fs, .14f);
  check(std::fabs(std::abs(response(direct, fs, 4000)) - .1) < 1e-5,
        "on-axis direct sound changed spectral calibration");
  c.speakerYawDegrees = 45;
  const auto small = roomImpulse(c, fs, .05f);
  const auto large = roomImpulse(c, fs, .14f);
  const double smallRatio =
      std::abs(response(small, fs, 3000) / response(small, fs, 300));
  const double largeRatio =
      std::abs(response(large, fs, 3000) / response(large, fs, 300));
  check(largeRatio < .5 * smallRatio, "larger piston did not beam more strongly");

  c.speakerYawDegrees = 0;
  c.headYawDegrees = -35;
  const auto turned = roomImpulse(c, fs, .05f);
  const auto left = response(turned, fs, 3000);
  const auto right = response(turned, fs, 3000, true);
  check(std::fabs(20 * std::log10(std::abs(left / right))) > 2,
        "head rotation has no interaural level difference");
  check(std::fabs(std::arg(response(turned, fs, 500) / response(turned, fs, 500, true))) >
            .1,
        "head rotation has no interaural delay");

  c = RadioRoomConfig{};
  c.reflections = 1;
  c.lateLevel = 1;
  c.decaySeconds = 3;
  RadioRoomRenderer hot;
  hot.initialize(c, fs, .14f);
  for (int i = 0; i < fs; ++i) {
    const auto y = hot.process(i & 8 ? .94f : -.94f);
    check(std::isfinite(y.left) && std::isfinite(y.right) &&
              std::max(std::fabs(y.left), std::fabs(y.right)) <= .940001f,
          "room summation exceeded the linked peak ceiling");
  }
  std::cout << "room," << fs << ",directivity_small_large," << smallRatio << ','
            << largeRatio << '\n';
}

void testSettings() {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("radioify-acoustics-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".toml");
  {
    std::ofstream file(path);
    file << "[audio-filter.test.room]\nenabled = true\nreflections = 0.2\n"
            "headphones = false\ndecaySeconds = 0.4\n"
            "[audio-filter.test.speaker]\nmotorTravelMeters = 0.002\n";
  }
  Radio1938 radio;
  std::string error;
  check(applyRadioSettingsIni(radio, path.string(), "test", &error),
        "room TOML settings failed");
  check(!radio.room.headphones && radio.room.reflections == .2f &&
            radio.speakerStage.speaker.motorTravelMeters == .002f,
        "settings did not reach model");
  {
    std::ofstream file(path);
    file << "[audio-filter.test.room]\nwidthMeters = -1\n";
  }
  check(!applyRadioSettingsIni(radio, path.string(), "test", &error),
        "invalid room accepted");
  check(radio.room.widthMeters == 4, "failed settings left invalid room state");
  std::filesystem::remove(path);
}

void testTriodeJacobian() {
  Radio1938 radio;
  radio.applyReceiverProfile(RadioReceiverProfile::Philco37116);
  radio.init(1, 48000, 5500, 0);
  const auto checkLut = [](const KorenTriodeModel& model, const KorenTriodeLut& lut) {
    const auto view = power_stage_internal::makeTriodeLutView(lut);
    check(view.valid, "missing triode lookup table");
    // A central difference at sub-float voltage spacing independently checks
    // both interpolation continuity and the derivative used by Newton.
    constexpr double dv = 1e-5;
    for (double grid : {-30.125, -15.375, -1.125}) {
      for (double plate :
           {50.123, 150.321, 300.456, static_cast<double>(lut.vpkMax) + 1.0, -1.0}) {
        const auto evaluate = [&](double v) {
          return power_stage_internal::evaluateTriodePlateFast(grid, v, model, lut, view);
        };
        const double numericalSlope =
            (evaluate(plate + dv).currentAmps - evaluate(plate - dv).currentAmps) /
            (2 * dv);
        const double slope = evaluate(plate).conductanceSiemens;
        check(std::fabs(numericalSlope - slope) < 1e-10 + 1e-6 * std::fabs(slope),
              "triode Jacobian differs from the current actually being solved");
      }
    }
  };
  checkLut(radio.power.tubeTriodeModel, radio.power.tubeTriodeLut);
  checkLut(radio.power.outputTubeTriodeModel, radio.power.outputTubeTriodeLut);
}

void testTransformerProjection() {
  for (auto profile :
       {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    Radio1938 radio;
    radio.applyReceiverProfile(profile);
    radio.init(1, 48000, 5500, 0);
    for (bool capacitive : {false, true}) {
      // The coupled output solver expands substeps itself and projects exactly
      // one frozen-history step on each Newton iteration.
      for (int oversampling : {4, 8}) {
        auto transformer = radio.power.outputTransformer;
        transformer.integrationSubsteps = 1;
        transformer.dtSub = 1.0f / (2 * 48000 * oversampling);
        if (capacitive) {
          transformer.primaryShuntCapFarads = 1e-9f;
          transformer.secondaryShuntCapFarads = 1e-6f;
        }
        // Change the Norton load on every step, including its history current.
        // A cached projection for a fixed resistor would fail this comparison.
        for (int i = 0; i < 400; ++i) {
          const SecondaryNortonLoad load{
              static_cast<float>(.05 + .03 * std::sin(.07 * i)),
              static_cast<float>(.1 * std::sin(.13 * i))};
          const float primaryLoad = i % 2 ? 50000.0f : 0.0f;
          const float drive = .02f * std::sin(.19f * i);
          const auto projection = buildAffineProjection(transformer, load, primaryLoad);
          const auto projected = evalAffineProjection(projection, drive);
          const auto direct = transformer.step(drive, load, primaryLoad);
          const auto close = [drive](double actual, double expected, double base,
                                     double slope) {
            // Cancellation can leave a near-zero voltage after adding much
            // larger terms. Bound float roundoff by those operands, not the
            // cancelled result. Both solvers retain float transformer states.
            const double scale = std::max(
                {1.0, std::fabs(base), std::fabs(drive * slope), std::fabs(expected)});
            return std::fabs(actual - expected) <=
                   64 * std::numeric_limits<float>::epsilon() * scale;
          };
          const bool same =
              close(projected.primaryVoltage, direct.primaryVoltage,
                    projection.base.primaryVoltage, projection.slope.primaryVoltage) &&
              close(projected.secondaryVoltage, direct.secondaryVoltage,
                    projection.base.secondaryVoltage,
                    projection.slope.secondaryVoltage) &&
              close(projected.primaryCurrent, direct.primaryCurrent,
                    projection.base.primaryCurrent, projection.slope.primaryCurrent) &&
              close(projected.secondaryCurrent, direct.secondaryCurrent,
                    projection.base.secondaryCurrent, projection.slope.secondaryCurrent);
          if (!same) {
            std::cerr << "projection," << radioReceiverProfileName(profile)
                      << ",caps=" << capacitive << ",oversampling=" << oversampling
                      << ",i=" << i << ",vp=" << projected.primaryVoltage << '/'
                      << direct.primaryVoltage << ",vs=" << projected.secondaryVoltage
                      << '/' << direct.secondaryVoltage
                      << ",ip=" << projected.primaryCurrent << '/'
                      << direct.primaryCurrent << ",is=" << projected.secondaryCurrent
                      << '/' << direct.secondaryCurrent << '\n';
          }
          check(same, "affine transformer state differs from direct integration");
        }
      }
    }
  }
}

void testPlaybackContinuity(RadioReceiverProfile profile) {
  Radio1938 whole, split;
  whole.applyReceiverProfile(profile);
  split.applyReceiverProfile(profile);
  whole.init(1, 48000, 5500, 0);
  split.init(1, 48000, 5500, 0);
  RadioAmIngressConfig ingress;
  RadioPreviewPipeline a, b;
  a.initialize(whole, ingress, RadioPreviewConfig{}, 48000);
  b.initialize(split, ingress, RadioPreviewConfig{}, 48000);
  whole.setCalibrationEnabled(true);
  split.setCalibrationEnabled(true);
  std::vector<float> x(4800 * 2), y;
  for (size_t i = 0; i < x.size() / 2; ++i)
    x[2 * i] = x[2 * i + 1] = .4f * std::sin(.12f * i);
  y = x;
  a.runBlock(whole, x.data(), 4800, 2);
  for (uint32_t i = 0; i < 4800; i += 127)
    b.runBlock(split, y.data() + 2 * i, std::min(127u, 4800 - i), 2);
  // Keep AVC/AFC active: the entire receiver must follow its sample clock,
  // including tuning updates, regardless of the host's callback boundaries.
  float difference = 0;
  for (size_t i = 0; i < x.size(); ++i)
    difference = std::max(difference, std::fabs(x[i] - y[i]));
  check(difference < 1e-5, "playback became dependent on callback block size");
  check(whole.speakerStage.speaker.solverNonconvergenceCount == 0 &&
            split.speakerStage.speaker.solverNonconvergenceCount == 0,
        "coupled amplifier/speaker solve did not converge");
  if (profile == RadioReceiverProfile::Philco37116) {
    check(whole.power.interstageSubstepCount > 0 &&
              whole.power.interstageMaxIterations < 12,
          "Philco interstage stalled at its Newton iteration limit");
  }
}

void benchmarkPlayback(bool profileCosts, const char* capturePath = nullptr) {
  constexpr uint32_t frames = 96000, blockSize = 256;
  std::ofstream capture;
  if (capturePath) {
    capture.open(capturePath, std::ios::binary);
    check(capture.good(), "could not open benchmark capture");
  }
  std::vector<float> waveform(frames * 2);
  for (auto profile :
       {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    for (bool room : {false, true}) {
      Radio1938 radio;
      radio.applyReceiverProfile(profile);
      radio.room.enabled = room;
      radio.init(1, 48000, 5500, .012f);
      RadioAmIngressConfig ingress;
      RadioPreviewPipeline preview;
      preview.initialize(radio, ingress, RadioPreviewConfig{}, 48000);
      preview.reserveBlockFrames(blockSize);
      std::vector<float> block(blockSize * 2, 0.0f);
      preview.runBlock(radio, block.data(), blockSize, 2);  // warmup excluded
      radio.setCalibrationEnabled(profileCosts);
      const auto start = std::chrono::steady_clock::now();
      for (uint32_t offset = 0; offset < frames; offset += blockSize) {
        const auto count = std::min(blockSize, frames - offset);
        for (uint32_t i = 0; i < count; ++i) {
          const double t = static_cast<double>(offset + i) / 48000;
          block[2 * i] = block[2 * i + 1] = static_cast<float>(
              .3 * std::sin(2 * kPi * 160 * t) + .2 * std::sin(2 * kPi * 1000 * t));
        }
        preview.runBlock(radio, block.data(), count, 2);
        std::copy_n(block.data(), count * 2, waveform.data() + offset * 2);
      }
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
      const auto& speaker = radio.speakerStage.speaker;
      std::cout << radioReceiverProfileName(profile) << ",room=" << room
                << ",audio_seconds=2,elapsed_ms=" << ms
                << ",speaker_max_iterations=" << speaker.solverMaxIterations
                << ",nonconvergences=" << speaker.solverNonconvergenceCount << '\n';
      if (profileCosts) {
        const auto& p = radio.power;
        std::cout << "costs_ms,interstage=" << p.interstageSolveTimeNs * 1e-6
                  << ",output=" << p.outputSolveTimeNs * 1e-6
                  << ",detector=" << radio.demod.am.detectorIslandTimeNs * 1e-6
                  << ",driver_evaluations=" << p.interstageDriverEvalCount
                  << ",output_iterations=" << p.outputNewtonIterationCount
                  << ",interstage_iterations=" << p.interstageIterationCount
                  << ",interstage_steps=" << p.interstageSubstepCount << '\n';
      }
      if (capturePath)
        capture.write(reinterpret_cast<const char*>(waveform.data()),
                      waveform.size() * sizeof(float));
      check(speaker.solverNonconvergenceCount == 0, "playback speaker solve failed");
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--receiver-input-report") {
    testReceiverInput(false);
    testDetectorMultirateCoupling(false);
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--ripple-report") {
    testPowerSupplyRipple(false);
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--transformer-test") {
    testTransformerProjection();
    return 0;
  }
  if (argc >= 2 && (std::string_view(argv[1]) == "--benchmark" ||
                    std::string_view(argv[1]) == "--profile")) {
    benchmarkPlayback(std::string_view(argv[1]) == "--profile",
                      argc == 3 ? argv[2] : nullptr);
    return 0;
  }
  for (float fs : {44100.0f, 48000.0f, 96000.0f}) {
    for (auto profile :
         {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116})
      testSpeaker(profile, fs);
    testRoom(fs);
  }
  testThermalMemory();
  testReceiverInput(true);
  testDetectorInputCoupling();
  testDetectorMultirateCoupling(true);
  testPowerSupplyRipple(true);
  testSettings();
  testTriodeJacobian();
  testTransformerProjection();
  for (auto profile :
       {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116})
    testPlaybackContinuity(profile);
  std::cout << "Acoustic model checks passed\n";
}
