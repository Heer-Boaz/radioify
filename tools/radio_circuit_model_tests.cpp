#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <array>
#include <string>

#include "radio.h"

namespace {
constexpr double pi = 3.14159265358979323846;
using Complex = std::complex<double>;
void check(bool pass, const char* message) {
  if (!pass) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

// Independent reference: solve the two-port RF admittance matrix at 470 kHz
// plus the requested offset. This retains the full L/C impedances and mutual
// inductance; it does not use the production envelope differential equation.
Complex rfTransfer(double offset, double lp, double ls, double rp, double rs, double load,
                   double k, double reference) {
  const double w0 = 2 * pi * 470000;
  const double w = 2 * pi * (470000 + offset);
  const Complex zp{rp, w * lp}, zs{rs, w * ls};
  const Complex mutual{0, w * k * std::sqrt(lp * ls)};
  const Complex det = zp * zs - mutual * mutual;
  const Complex y11 = zs / det + Complex{0, w / (w0 * w0 * lp)};
  const Complex y22 = zp / det + Complex{1 / load, w / (w0 * w0 * ls)};
  const Complex y12 = -mutual / det;
  return -y12 / (y11 * y22 - y12 * y12) / reference;
}

double measureCan(double fs, double frequency, double k, double load, double lp = .00022,
                  double rp = 7.5) {
  CoupledIfTransformer can;
  can.configure(fs, 470000, lp, .00025, rp, 7.5, load, k, 27000);
  Complex result{};
  const int count = static_cast<int>(fs / 10);
  for (int n = -count; n < count; ++n) {
    const auto input = std::polar(1.0, 2 * pi * frequency * n / fs);
    const auto out =
        can.process(static_cast<float>(input.real()), static_cast<float>(input.imag()));
    if (n >= 0) result += Complex{out[0], out[1]} * std::conj(input) / double(count);
  }
  return std::abs(result);
}

void testCircuitReference() {
  double maxError = 0;
  for (double fs : {44100., 48000., 96000.})
    for (double k : {.006, .012, .024, .032})
      for (double load : {50000., 200000.})
        for (double lp : {.00018, .00022, .00028})
          for (double offset : {-8000., -5000., -2500., 0., 2500., 5000., 8000.}) {
            const double measured = measureCan(4 * fs, offset, k, load, lp);
            const double reference =
                std::abs(rfTransfer(offset, lp, .00025, 7.5, 7.5, load, k, 27000));
            const double error = std::abs(20 * std::log10(measured / reference));
            maxError = std::max(maxError, error);
          }
  std::cout << "IF full RF impedance reference max error dB: " << maxError << '\n';
  check(maxError < .35, "coupled envelope diverges from full RF circuit");
  check(measureCan(192000, 0, 0, 200000) == 0, "zero coupling must transmit nothing");
  const double broad = measureCan(192000, 5000, .024, 200000);
  const double narrow = measureCan(192000, 5000, .006, 200000);
  check(broad > 3 * narrow, "coupling does not determine selectivity");
  check(measureCan(192000, 0, .012, 50000) < .75 * measureCan(192000, 0, .012, 200000),
        "secondary loading must change insertion loss");
}

double measureStrip(RadioReceiverProfile profile, float offset, float frequency) {
  Radio1938 radio;
  radio.applyReceiverProfile(profile);
  radio.identity.driftDepth = 0;
  radio.tuning.magneticTuningEnabled = false;
  radio.tuning.tuneOffsetHz = offset;
  radio.init(1, 48000, 5500, 0);
  radio.mixer.conversionGain = 1;
  radio.ifStrip.stageGain = 1;
  double energy = 0;
  for (int n = -4800; n < 4800; ++n) {
    const auto in = std::polar(1., 2 * pi * frequency * n / 48000);
    RadioSampleContext ctx{};
    ctx.signal.setComplexEnvelope(static_cast<float>(in.real()),
                                  static_cast<float>(in.imag()));
    RadioIFStripNode::run(radio, 0, ctx);
    if (n >= 0)
      energy += ctx.signal.detectorInputI * double(ctx.signal.detectorInputI) +
                ctx.signal.detectorInputQ * double(ctx.signal.detectorInputQ);
  }
  return std::sqrt(energy / 4800);
}

void testReceiverTuning() {
  for (auto profile : {kDefaultRadioReceiverProfile, RadioReceiverProfile::Philco37116}) {
    const double centred = measureStrip(profile, 0, 0);
    const double shifted = measureStrip(profile, 4500, 4500);
    check(std::abs(shifted / centred - 1) < .001,
          "receiver selectivity does not follow the local oscillator");
    const double upper = measureStrip(profile, 4500, 4000);
    const double lower = measureStrip(profile, 4500, -4000);
    const double asymmetry = 20 * std::log10(upper / lower);
    std::cout << radioReceiverProfileName(profile)
              << ", detuned sideband asymmetry dB=" << asymmetry << '\n';
    // Expanded Philco is much broader than the table set. Compare with its
    // actual RF circuit, not a shared arbitrary attenuation target.
    Radio1938 preset;
    preset.applyReceiverProfile(profile);
    const auto& p = preset.ifStrip;
    double expectedDb = 0;
    for (int can = 0; can < p.transformerCount; ++can) {
      const double coupling = can + 1 == p.transformerCount ? p.outputCouplingCoeff
                                                            : p.interstageCouplingCoeff;
      const auto reference = [&](double f) {
        return std::abs(rfTransfer(
            f, p.primaryInductanceHenries, p.secondaryInductanceHenries,
            p.primarySeriesResistanceOhms, p.secondarySeriesResistanceOhms,
            p.secondaryLoadResistanceOhms, coupling, p.referenceTransimpedanceOhms));
      };
      expectedDb += 20 * std::log10(reference(-500) / reference(-8500));
    }
    const auto interpolationGain = [](double hz) {
      const double omega = pi * hz / 48000;
      const double ratio = std::sin(omega) / (4 * std::sin(omega / 4));
      return ratio * ratio;
    };
    expectedDb += 20 * std::log10(interpolationGain(500) / interpolationGain(8500));
    check(asymmetry > 1.5 && std::abs(asymmetry - expectedDb) < .7,
          "detuned sidebands disagree with the independent RF circuit");
  }
  const double small = measureStrip(kDefaultRadioReceiverProfile, 0, 6000) /
                       measureStrip(kDefaultRadioReceiverProfile, 0, 0);
  const double console = measureStrip(RadioReceiverProfile::Philco37116, 0, 6000) /
                         measureStrip(RadioReceiverProfile::Philco37116, 0, 0);
  check(console > 2 * small, "receivers need distinct IF selectivity");
}

void testIndependentTubeCharacteristics() {
  const KorenTriodeLut noTable;
  const auto sixJ5 = rca6J5Characteristic(), seventyFive = rca75Characteristic();
  const auto ma = [&](double plate, double grid, const KorenTriodeModel& model) {
    return 1000 *
           evaluateKorenTriodePlateRuntime(grid, plate, model, noTable).currentAmps;
  };
  // Manufacturer tabulated points were withheld from the graphical fits.
  check(std::abs(ma(250, -8, sixJ5) / 9 - 1) < .20, "6J5 misses RCA 250 V point");
  check(std::abs(ma(90, 0, sixJ5) / 10 - 1) < .20, "6J5 misses RCA 90 V point");
  check(std::abs(ma(250, -2, seventyFive) / .8 - 1) < .20, "75 misses RCA 250 V point");
  check(ma(90, -8, sixJ5) < .01 && ma(90, -2, seventyFive) < .01,
        "impossible mixed operating points are still being fitted");
  for (auto profile : {kDefaultRadioReceiverProfile, RadioReceiverProfile::Philco37116}) {
    Radio1938 radio;
    radio.applyReceiverProfile(profile);
    radio.init(1, 48000, 5500, 0);
    const auto& r = radio.receiverCircuit;
    const double current = (r.tubePlateSupplyVolts - r.tubeQuiescentPlateVolts) /
                           (r.tubeLoadResistanceOhms + r.tubeDcFeedResistanceOhms);
    check(std::abs(current / r.tubeQuiescentPlateCurrentAmps - 1) < .001,
          "DC load line does not conserve current");
    check(std::abs((r.tubeEffectiveSupplyVolts - r.tubeQuiescentPlateVolts) /
                       r.tubeLoadResistanceOhms / current -
                   1) < .001,
          "bypassed DC feed incorrectly became the AC plate load");
  }
}

void testPhysicalNoiseSources() {
  Radio1938 quiet, loud;
  quiet.noiseConfig.enableHumTone = true;  // optional pickup source, independent of B+
  loud.noiseConfig.enableHumTone = true;
  quiet.init(1, 48000, 5500, .15f);
  loud.init(1, 48000, 5500, .15f);
  double hum60 = 0, hum120 = 0;
  for (int n = 0; n < 48000; ++n) {
    RadioSampleContext a{}, b{};
    a.signal.setRealRf(0);
    b.signal.setRealRf(1);
    // A strong carrier/programme must not suppress atmospheric events at source.
    loud.controlBus.controlVoltage = 1;
    RadioInterferenceDerivedNode::run(quiet, a);
    RadioInterferenceDerivedNode::run(loud, b);
    a.derived.rfImpulseRate = b.derived.rfImpulseRate = 100;
    const double q = RadioInputNode::run(quiet, 0, a);
    const double l = RadioInputNode::run(loud, 1, b);
    check(std::abs(l - q - 1) < 2e-7,
          "RF impulses depend on programme or carrier strength");
    a.derived.noiseAmp = b.derived.noiseAmp = 0;
    a.derived.humAmp = b.derived.humAmp = .1f;
    loud.speakerStage.speaker.electricalCurrentAmps = 10;
    const double hq = RadioNoiseNode::run(quiet, 0, a);
    const double hl = RadioNoiseNode::run(loud, 0, b);
    check(std::abs(hq - hl) < 1e-7, "hum is still being ducked by programme level");
    if (n >= 24000) {
      hum60 += hq * std::sin(2 * pi * 60 * n / 48000);
      hum120 += hq * std::sin(2 * pi * 120 * n / 48000);
    }
  }
  check(quiet.input.impulseEventCount > 50 &&
            quiet.input.impulseEventCount == loud.input.impulseEventCount,
        "RF impulse event process stopped or depends on reception");
  check(std::abs(hum120) > std::abs(hum60), "full-wave hum must dominate mains pickup");
  quiet.reset();
  check(quiet.input.impulseEventCount == 0, "reset left RF impulse diagnostics behind");
}

void testThermionicDiode() {
  // TM 11-662 Fig. 33: independent type-6H6 positive-voltage observations.
  ThermionicDiode diode{.000445f, .073f, 0};
  for (const auto point : {std::pair{8., .010}, {20., .040}, {28., .0664}})
    check(std::abs(diode.evaluate(point.first).current / point.second - 1) < .04,
          "space-charge characteristic misses published 6H6 current");
  for (float k : {.000085f, .000445f, .00075f}) {
    diode.perveance = k;
    for (double resistance : {100., 5100., 1000000.})
      for (double source : {-5., -.5, 0., .05, .2, 1., 20., 600.}) {
        const double current = diode.current(source, resistance);
        const double voltage = source - resistance * current;
        const double residual = current - diode.evaluate(voltage).current;
        check(std::isfinite(current) && current >= 0 &&
                  std::abs(residual) < 1e-12 + 1e-7 * current,
              "thermionic branch violates Kirchhoff's voltage/current laws");
      }
  }
  const double join = 1.5 * diode.thermalVolts;
  const double h = 1e-6;
  const double numeric =
      (diode.evaluate(join + h).current - diode.evaluate(join - h).current) / (2 * h);
  check(std::abs(numeric / diode.evaluate(join).conductance - 1) < 1e-5,
        "thermal/space-charge join has a discontinuous derivative");
  check(diode.evaluate(-.15).current > 0 &&
            diode.evaluate(-.3).current < diode.evaluate(-.15).current / 5,
        "hot cathode lacks the retarding-field current tail");
}

void testSupplyConservation() {
  for (const auto profile :
       {kDefaultRadioReceiverProfile, RadioReceiverProfile::Philco37116}) {
    Radio1938 radio;
    radio.applyReceiverProfile(profile);
    const auto c = radio.powerSupply.config;
    RectifierSupply supply;
    constexpr double fs = 96000;
    const RectifierSupply::Load idle{.08, .018, .001};
    supply.configure(c, fs, 60);
    supply.start(325, idle);
    double maxResidual = 0, rectifierSum = 0, audioFeedDropSum = 0;
    const double oldMain = supply.volts(RectifierSupply::Main);
    for (int n = 0; n < int(fs); ++n) {
      const auto before = supply.state();
      const RectifierSupply::Load load{idle.mainAmps + (n >= 48000 ? .02 : 0),
                                       idle.auxiliaryAmps, idle.audioAmps};
      supply.step(load);
      const auto after = supply.state();
      const auto& v = after.volts;
      const auto& u = before.volts;
      const double cr = c.reservoirFarads * fs * ((v[0] - v[2]) - (u[0] - u[2]));
      const double cm = c.filterFarads * fs *
                        ((v[1] - u[1]) - (c.filterReturnsToCentreTap ? v[2] - u[2] : 0));
      const double ct = c.returnFarads * fs * (v[2] - u[2]);
      const double ca = c.auxiliaryFarads * fs * (v[3] - u[3]);
      const double cf = c.audioFarads * fs * (v[4] - u[4]);
      const double ra = after.auxiliaryAmps;
      const double rf = after.audioFeedAmps;
      for (double residual :
           {supply.rectifierCurrent() - after.filterAmps - cr,
            after.filterAmps - load.mainAmps - ra - cm,
            cr + (c.filterReturnsToCentreTap ? cm : 0) - supply.rectifierCurrent() -
                after.returnAmps - ct,
            ra - load.auxiliaryAmps - rf - ca, rf - load.audioAmps - cf,
            v[3] - v[4] - rf * c.audioResistanceOhms,
            v[1] - v[3] - c.auxiliaryResistanceOhms * after.auxiliaryAmps -
                c.auxiliaryHenries * fs * (after.auxiliaryAmps - before.auxiliaryAmps),
            v[0] - v[1] - c.filterResistanceOhms * after.filterAmps -
                c.filterHenries * fs * (after.filterAmps - before.filterAmps)})
        maxResidual = std::max(maxResidual, std::abs(residual));
      if (n < 48000) {
        rectifierSum += supply.rectifierCurrent();
        audioFeedDropSum += v[3] - v[4];
      }
    }
    std::cout << "supply KCL/KVL max residual=" << maxResidual
              << ", loaded rail=" << supply.volts(RectifierSupply::Main) << '\n';
    check(maxResidual < 2e-6, "supply network lost charge or choke flux");
    check(std::abs(rectifierSum / 48000 - .099) < .0001,
          "average rectifier current does not supply the idle load");
    check(std::abs(audioFeedDropSum / 48000 - .001 * c.audioResistanceOhms) < .05,
          "first-audio current did not develop the physical DC feed drop");
    check(supply.volts(RectifierSupply::Main) < oldMain - 2,
          "physical load step did not lower B+");
    supply.reset();
    check(supply.volts(RectifierSupply::Main) == oldMain,
          "supply reset does not restore deterministic periodic initial state");
  }
}

void testAfcWithRealCarrier() {
  for (float offset : {-4000.f, -500.f, 500.f, 4000.f}) {
    Radio1938 radio;
    radio.applyReceiverProfile(RadioReceiverProfile::Philco37116);
    radio.tuning.tuneOffsetHz = offset;
    radio.init(1, 48000, 5500, 0);
    std::vector<float> silence(256), output(256);
    for (int block = 0; block < 375; ++block)
      radio.processAmAudio(silence.data(), output.data(), 256, .12f, .85f);
    std::cout << "AFC real carrier offset=" << offset
              << ", residual=" << radio.tuning.tuneAppliedHz << '\n';
    check(std::abs(radio.tuning.tuneAppliedHz) < 350,
          "real carrier cannot drive magnetic tuning toward station centre");
    radio.reset();
    check(radio.tuning.tuneAppliedHz == offset && radio.tuning.afcCorrectionHz == 0,
          "reset retained the previous station's AFC pull");
  }
}
void reportDetectorSupply(const char* profile, const char* settings) {
  Radio1938 radio;
  check(radio.applyReceiverProfile(profile), "unknown profile");
  if (settings) {
    std::string error;
    check(applyRadioSettingsIni(radio, settings, "default", &error), error.c_str());
  }
  radio.init(1, 48000, 5000, .012f);
  constexpr int count = 48000;
  std::array<double, 7> sum{}, energy{};
  std::array<Complex, 7> fundamental{}, hum{};
  for (int n = 0; n < 3 * count; ++n) {
    float input = static_cast<float>(std::sin(2 * pi * 1000 * n / count));
    float output;
    radio.processAmAudio(&input, &output, 1, .12f, .85f);
    if (n >= 2 * count) {
      const std::array<double, 7> sample{
          radio.demod.am.detectorNode,
          radio.receiverCircuit.gridVoltage,
          radio.receiverCircuit.tubePlateVoltage,
          radio.power.outputTransformer.secondaryVoltage,
          output,
          radio.powerSupply.circuit.volts(RectifierSupply::Auxiliary),
          radio.receiverCircuit.tubeSupplyVolts};
      for (int k = 0; k < 7; ++k) {
        sum[k] += sample[k] / count;
        energy[k] += sample[k] * sample[k] / count;
        fundamental[k] +=
            sample[k] * std::polar(1., -2 * pi * 1000 * n / count) / double(count);
        hum[k] += sample[k] * std::polar(1., -2 * pi * 120 * n / count) / double(count);
      }
    }
  }
  for (int k = 0; k < 7; ++k) {
    const double tonePower = 2 * std::norm(fundamental[k]);
    const double residual = energy[k] - sum[k] * sum[k] - tonePower;
    std::cout << "stage," << k << ",dc," << sum[k] << ",tone_rms," << std::sqrt(tonePower)
              << ",sinad," << 10 * std::log10(tonePower / std::max(residual, 1e-30))
              << ",hum120_rms," << std::sqrt(2.) * std::abs(hum[k]) << '\n';
  }
  std::cout << "avc," << radio.demod.am.avcEnv << ",IF gain," << radio.ifStrip.stageGain
            << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "--supply-report") {
    reportDetectorSupply(argv[2], argc >= 4 ? argv[3] : nullptr);
    return 0;
  }
  testThermionicDiode();
  for (auto profile :
       {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    Radio1938 radio;
    radio.applyReceiverProfile(profile);
    check(std::abs(radio.mixer.avcGridDriveVolts /
                       (1.25 * radio.demod.am.controlVoltageRef) -
                   1) < 1e-6,
          "physical AVC volts are being multiplied at the mixer grid");
  }
  testSupplyConservation();
  testCircuitReference();
  testReceiverTuning();
  testIndependentTubeCharacteristics();
  testPhysicalNoiseSources();
  testAfcWithRealCarrier();
}
