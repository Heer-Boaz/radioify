#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <array>
#include <string>

#include "audiofilter/models/remote_cutoff_pentode.h"
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
  radio.ifStrip.stagePlateSwingLimitVolts = 0;  // linear selectivity check
  radio.init(1, 48000, 5500, 0);
  radio.mixer.conversionGain = 1;
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
            p.secondaryLoadResistanceOhms, coupling, 1.0));
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

// Independent reference: full impedance of a parallel tuned circuit at the
// dial frequency plus offset. The coil loss is a series resistance fixed by
// its unloaded Q at the dial; the gang capacitor is aligned to resonance.
Complex tankReference(double offset, double dialHz, double l, double coilQ, double shunt) {
  const double w0 = 2 * pi * dialHz;
  const auto impedance = [&](double omega) {
    const Complex coil{w0 * l / coilQ, omega * l};
    return 1. / (1. / coil + Complex{1 / shunt, omega / (w0 * w0 * l)});
  };
  return impedance(2 * pi * (dialHz + offset)) / impedance(w0);
}

Complex measureTank(double fs, double dialHz, double offset, double l, double coilQ,
                    double shunt) {
  TunedRfCircuit tank;
  tank.configure(fs, dialHz, l, coilQ, shunt);
  Complex result{};
  const int count = static_cast<int>(fs / 10);
  for (int n = -count; n < count; ++n) {
    const auto input = std::polar(1.0, 2 * pi * offset * n / fs);
    const auto out =
        tank.process(static_cast<float>(input.real()), static_cast<float>(input.imag()));
    if (n >= 0) result += Complex{out[0], out[1]} * std::conj(input) / double(count);
  }
  return result;
}

// Steady-state envelope gain of the complete front-end node relative to its
// declared resonance gain, for a source-frame tone at the given offset.
double measureFrontEnd(RadioReceiverProfile profile, float tuneOffset, float offset,
                       float stationHz = 1000000) {
  Radio1938 radio;
  radio.applyReceiverProfile(profile);
  radio.identity.driftDepth = 0;
  radio.tuning.magneticTuningEnabled = false;
  radio.tuning.tuneOffsetHz = tuneOffset;
  radio.tuning.stationFrequencyHz = stationHz;
  radio.init(1, 48000, 5500, 0);
  radio.controlBus.controlVoltage = 0;
  Complex result{};
  for (int n = -4800; n < 4800; ++n) {
    const auto in = std::polar(1., 2 * pi * offset * n / 48000);
    RadioSampleContext ctx{};
    ctx.signal.setComplexEnvelope(static_cast<float>(in.real()),
                                  static_cast<float>(in.imag()));
    RadioFrontEndNode::run(radio, 0, ctx);
    if (n >= 0)
      result += Complex{ctx.signal.i, ctx.signal.q} * std::conj(in) / 4800.;
  }
  return std::abs(result) / (radio.frontEnd.antennaVoltageGain * radio.frontEnd.avcGain);
}

void testFrontEndTunedCircuits() {
  // Cover the widest transmitted band plus 10 kHz of mechanical detuning, up
  // to .3 fs; beyond that only a bounded magnitude error is promised. The
  // discretisation is judged against the symmetric narrowband envelope it
  // implements; the full impedance adds the circuit's own small asymmetry.
  double discreteError = 0, wideError = 0, maxError = 0, maxPhaseResidual = 0;
  for (double fs : {44100., 48000., 96000.}) {
    const double sourceBw = std::min(10000., .2 * fs);
    const double covered = std::min(sourceBw + 10000, .3 * fs);
    for (double dial : {530000., 1000000., 1710000.})
      for (double l : {200e-6, 260e-6})
        for (double coilQ : {60., 120.})
          for (double shunt : {50000., 300000.}) {
            TunedRfCircuit design;
            design.configure(fs, dial, l, coilQ, shunt);
            const auto narrowband = [&](double offset) {
              return std::abs(1. / Complex{1, offset / design.halfBandwidthHz()});
            };
            std::vector<double> offsets, phaseErrors;
            for (int step = -12; step <= 12; ++step) {
              const double offset = covered * step / 12;
              const Complex measured = measureTank(fs, dial, offset, l, coilQ, shunt);
              const Complex reference = tankReference(offset, dial, l, coilQ, shunt);
              discreteError = std::max(
                  discreteError,
                  std::abs(20 * std::log10(std::abs(measured) / narrowband(offset))));
              maxError = std::max(
                  maxError, std::abs(20 * std::log10(std::abs(measured / reference))));
              offsets.push_back(offset);
              phaseErrors.push_back(std::arg(measured / reference));
            }
            // A common sub-sample delay moves every sideband alike; only the
            // phase left after removing it can distort the envelope.
            double num = 0, den = 0;
            for (size_t i = 0; i < offsets.size(); ++i) {
              num += offsets[i] * phaseErrors[i];
              den += offsets[i] * offsets[i];
            }
            for (size_t i = 0; i < offsets.size(); ++i)
              maxPhaseResidual = std::max(
                  maxPhaseResidual,
                  std::abs(phaseErrors[i] - num / den * offsets[i]) * 180 / pi);
            for (double fraction : {-.45, -.4, .4, .45}) {
              const double offset = fraction * fs;
              wideError = std::max(
                  wideError,
                  std::abs(20 * std::log10(
                               std::abs(measureTank(fs, dial, offset, l, coilQ, shunt)) /
                               narrowband(offset))));
            }
          }
  }
  std::cout << "RF tank discretisation max error dB: " << discreteError
            << " (to .45 fs: " << wideError
            << "); full impedance reference max error dB: " << maxError
            << ", phase residual deg: " << maxPhaseResidual << '\n';
  check(discreteError < .01, "tuned RF envelope discretisation warps the sidebands");
  check(wideError < .4, "tuned RF envelope fails near the envelope Nyquist limit");
  // .17 dB is the real circuit's asymmetry at 530 kHz and 20 kHz offset.
  check(maxError < .2, "tuned RF envelope diverges from the full RLC circuit");
  check(maxPhaseResidual < 1.5, "tuned RF envelope phase distorts the sidebands");
  // The reviewer's case: both Philco circuits at the 44.1 kHz source edge.
  const double edge = .2 * 44100;
  const Complex philco = measureTank(44100, 1e6, edge, 230e-6, 100, 150000) *
                         measureTank(44100, 1e6, edge, 230e-6, 100, 250000);
  const Complex philcoReference = tankReference(edge, 1e6, 230e-6, 100, 150000) *
                                  tankReference(edge, 1e6, 230e-6, 100, 250000);
  const double philcoError = 20 * std::log10(std::abs(philco / philcoReference));
  std::cout << "Philco RF circuits at 44.1 kHz source edge, error dB: " << philcoError
            << '\n';
  // The remaining .075 dB is the real pair's asymmetry about 1 MHz.
  check(std::abs(philcoError) < .1,
        "ganged Philco circuits miss the reference at the wide source edge");
  check(std::abs(measureTank(48000, 1e6, 5000, 230e-6, 100, 50000)) >
            std::abs(measureTank(48000, 1e6, 5000, 230e-6, 100, 300000)),
        "heavier antenna loading must broaden the tuned circuit");
  check(std::abs(measureTank(48000, 1.5e6, 5000, 230e-6, 100, 150000)) >
            std::abs(measureTank(48000, .6e6, 5000, 230e-6, 100, 150000)),
        "fixed coil Q must give a wider passband higher on the dial");

  for (auto profile : {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    const double centred = measureFrontEnd(profile, 0, 0);
    check(std::abs(centred - 1) < 1e-3, "front end changed its declared resonance gain");
    check(std::abs(measureFrontEnd(profile, 3000, 3000) / centred - 1) < 1e-3,
          "RF tuned circuits do not follow the dial");
    const double upper = measureFrontEnd(profile, 0, 5000);
    const double lower = measureFrontEnd(profile, 0, -5000);
    check(std::abs(20 * std::log10(upper / lower)) < .01,
          "centred RF response must keep symmetric AM sidebands");
  }
  const double small = measureFrontEnd(RadioReceiverProfile::Typical1930s, 0, 5000);
  const double console = measureFrontEnd(RadioReceiverProfile::Philco37116, 0, 5000);
  std::cout << "front end 5 kHz sideband dB: typical=" << 20 * std::log10(small)
            << ", philco=" << 20 * std::log10(console) << '\n';
  check(console < small, "two ganged RF circuits must be more selective than one");

  // A bypassed front end still delivers real RF to the IF through the same
  // representation conversion, without inventing RF gain or selectivity.
  Radio1938 bypassed;
  bypassed.graph.setEnabled(PassId::FrontEnd, false);
  bypassed.init(1, 48000, 5500, 0);
  std::vector<float> program(4800), output(4800);
  for (int n = 0; n < 4800; ++n)
    program[n] = .5f * static_cast<float>(std::sin(2 * pi * 1000 * n / 48000));
  bypassed.processAmAudio(program.data(), output.data(), 4800, .12f, .85f);
  double energy = 0;
  for (int n = 2400; n < 4800; ++n) {
    check(std::isfinite(output[n]), "bypassed front end produced a non-finite sample");
    energy += output[n] * double(output[n]);
  }
  check(energy > 0, "bypassed front end stopped real RF reaching the IF strip");

  // Baseband I/Q enters at the front end; bypassing it must not bypass the
  // mixer, IF and detector behind it.
  Radio1938 iqBypassed;
  iqBypassed.graph.setEnabled(PassId::FrontEnd, false);
  iqBypassed.init(1, 48000, 5500, 0);
  std::vector<float> iq(2 * 4800);
  for (int n = 0; n < 4800; ++n)
    iq[2 * n] = .1f + .02f * static_cast<float>(std::sin(2 * pi * 1000 * n / 48000));
  iqBypassed.processIqBaseband(iq.data(), output.data(), 4800);
  double rawDifference = 0;
  for (int n = 2400; n < 4800; ++n)
    rawDifference = std::max(rawDifference, std::abs(double(output[n]) - iq[2 * n]));
  check(iqBypassed.mixer.inputDriveEnv > 0 && rawDifference > .01,
        "bypassed front end skipped the I/Q receiver chain");

  // Magnetic Tuning shifts the oscillator only (Rider, 1937); the antenna
  // and RF circuits stay where the knob put them.
  Radio1938 pulled;
  pulled.applyReceiverProfile(RadioReceiverProfile::Philco37116);
  pulled.identity.driftDepth = 0;
  pulled.tuning.magneticTuningEnabled = true;
  pulled.tuning.tuneOffsetHz = 2000;
  pulled.init(1, 48000, 5500, 0);
  pulled.tuning.afcCorrectionHz = -1500;
  RadioSampleContext idle{};
  for (int n = 0; n < 96000; ++n) RadioTuningNode::run(pulled, idle);
  RadioFrontEndNode::run(pulled, 0, idle);
  check(std::abs(pulled.tuning.tuneAppliedHz - 500) < 5 &&
            std::abs(pulled.tuning.dialAppliedHz - 2000) < 5 &&
            std::abs(pulled.frontEnd.tunedHz - (pulled.tuning.stationFrequencyHz + 2000)) < 5,
        "AFC pull must move the oscillator, not the ganged RF circuits");
}

// A capacitor across only the primary (the 38-12's 0.01 uF on the 41 plate)
// must leave the secondary node algebraic. Reference: the capacitor in
// parallel with leakage + (magnetizing || reflected load).
void testPrimaryOnlyShuntCapacitor() {
  const double fs = 96000, ll = 1e-3, lm = 100, load = 3.5, cap = .01e-6;
  const double n = std::sqrt(9000 / load);
  for (double f : {500.0, 2000.0, 4000.0}) {
    CurrentDrivenTransformer transformer;
    transformer.setModel(float(fs), float(ll), float(lm), float(n), 0, 0, float(cap), 0, 0,
                         0, 4);
    Complex response{};
    const int settle = int(fs / 5), count = int(fs / 5);
    for (int i = 0; i < settle + count; ++i) {
      const double drive = 1e-3 * std::sin(2 * pi * f * i / fs);
      const auto sample = transformer.step(float(drive), float(load));
      if (i >= settle)
        response += double(sample.primaryVoltage) * std::polar(2.0 / count, -2 * pi * f * i / fs);
    }
    const Complex jw{0, 2 * pi * f};
    const Complex winding = jw * ll + 1.0 / (1.0 / (jw * lm) + 1.0 / (n * n * load));
    const Complex z = 1.0 / (jw * cap + 1.0 / winding);
    const double errorDb = 20 * std::log10(std::abs(response) / (1e-3 * std::abs(z)));
    check(std::abs(errorDb) < .1, "primary-only shunt capacitor response is wrong");
  }
}

void testRemoteCutoffAvc() {
  // RCA-78/6K7 rating: 1450 umho at Eb 250, Ec2 100, Ec1 -3 V.
  check(std::abs(remoteCutoffTransconductance(-3) - 1450e-6) < 1e-9,
        "remote-cutoff curve misses the RCA rating");
  for (double v = -2; v > -50; v -= .25)
    check(remoteCutoffTransconductance(v - .25) < remoteCutoffTransconductance(v),
          "remote-cutoff transconductance must fall with bias");
  check(remoteCutoffRelativeGain(-3, 0, 90) == 1, "zero AVC must leave the gain alone");
  check(std::abs(remoteCutoffRelativeGain(-2.52, 8.4, 84) -
                 remoteCutoffRelativeGain(-3, 10, 100)) < 1e-12,
        "screen voltage must scale the grid axis");
  // The tapered grid gives tens of dB of control without cutoff.
  const double at20 = remoteCutoffRelativeGain(-3, 20, 100);
  check(at20 > 1e-3 && at20 < .05, "remote-cutoff control range is implausible");

  for (auto profile : {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    Radio1938 radio;
    radio.applyReceiverProfile(profile);
    radio.init(1, 48000, 5500, 0);
    radio.controlBus.controlVoltage = 5 / radio.demod.am.controlVoltageRef;
    check(std::abs(radioAvcLineVolts(radio) - 5) < 1e-5, "AVC line is not in volts");
    RadioSampleContext ctx{};
    ctx.signal.setComplexEnvelope(.1f, 0);
    RadioFrontEndNode::run(radio, 0, ctx);
    ctx.signal.setComplexEnvelope(.1f, 0);
    RadioIFStripNode::run(radio, 0, ctx);
    const auto& f = radio.frontEnd;
    const auto& s = radio.ifStrip;
    // The RF stage gain is its gm at the AVC bias times its transformer load;
    // the screens follow the present main B+.
    const double screen = radioScreenSupplyScale(radio);
    const double expectedRf =
        f.secondTunedCircuitEnabled
            ? remoteCutoffStageTransconductance(f.rfStageBiasVolts, 5,
                                                f.rfStageScreenVolts * screen) *
                  f.rfStageLoadOhms
            : 1.0;
    const double expectedIf = std::pow(
        remoteCutoffStageTransconductance(s.stageBiasVolts, 5, s.stageScreenVolts * screen) /
            s.stageTransconductanceSiemens,
        s.avcStageCount);
    check(std::abs(f.avcGain / expectedRf - 1) < 1e-5 &&
              std::abs(s.avcGain / expectedIf - 1) < 1e-5,
          "AVC does not act through each controlled stage's transconductance");
  }
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
    loud.controlBus.controlVoltage = 1;
    RadioInterferenceDerivedNode::run(quiet, a);
    RadioInterferenceDerivedNode::run(loud, b);
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
  check(std::abs(hum120) > std::abs(hum60), "full-wave hum must dominate mains pickup");
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
// A triode far outside its table (a transformer flyback above B+, or a
// grid far past its bias) must keep its plate resistance, not freeze into a
// current source.
void testTriodeBeyondTable() {
  Radio1938 radio;
  radio.applyReceiverProfile(RadioReceiverProfile::Philco37116);
  radio.init(1, 48000, 5000, 0);
  const auto& p = radio.power;
  check(p.outputTubeTriodeLut.valid(), "output triode table missing");
  const double top = p.outputTubeTriodeLut.vpkMax;
  for (double vgk : {double(p.outputTubeBiasVolts), 0.5 * p.outputTubeBiasVolts}) {
    const auto edge = evaluateKorenTriodePlateRuntime(vgk, top, p.outputTubeTriodeModel,
                                                      p.outputTubeTriodeLut);
    const auto beyond = evaluateKorenTriodePlateRuntime(vgk, 3 * top, p.outputTubeTriodeModel,
                                                        p.outputTubeTriodeLut);
    // Space-charge current grows about as the 1.5 power of plate voltage.
    check(beyond.currentAmps > 2 * edge.currentAmps &&
              beyond.conductanceSiemens > edge.conductanceSiemens,
          "triode beyond its table lost its plate resistance");
  }
  const auto negative = evaluateKorenTriodePlateRuntime(
      0, -50, p.outputTubeTriodeModel, p.outputTubeTriodeLut);
  check(negative.currentAmps == 0, "a triode conducts with a negative plate");
}

// Modulation hum: the RF/IF screens follow the rippling B+, so an
// unmodulated carrier picks up 120 Hz at the detector. The 38-12 feeds its
// screens straight from the B+ after the field coil (Bulletin 284); the
// Philco's pass two RC sections (Bulletin 258), which must leave far less.
double detectorHumRatioDb(RadioReceiverProfile profile, bool screenRipple) {
  Radio1938 radio;
  radio.applyReceiverProfile(profile);
  radio.init(1, 48000, 5000, 0);
  if (!screenRipple) radio.powerSupply.mainReferenceVolts = 0;  // scale stays 1
  constexpr int count = 48000;
  double dc = 0;
  Complex hum{};
  for (int n = 0; n < 2 * count; ++n) {
    float input = 0, output;
    radio.processAmAudio(&input, &output, 1, .02f, .85f);
    if (n >= count) {
      dc += radio.demod.am.detectorNode / count;
      hum += double(radio.demod.am.detectorNode) * std::polar(1., -2 * pi * 120 * n / count) /
             double(count);
    }
  }
  return 20 * std::log10(std::sqrt(2.) * std::abs(hum) / dc);
}

// A lightning crash far beyond any carrier must saturate the IF tubes at
// their plate swing instead of passing through a linear strip, while the
// normal working level stays linear.
void testIfPlateSwingLimit() {
  for (auto profile : {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    double peaks[2] = {};
    for (int conduction = 0; conduction < 2; ++conduction) {
      Radio1938 radio;
      radio.applyReceiverProfile(profile);
      if (conduction == 0) radio.ifStrip.stagePlateSwingLimitVolts = 0;
      radio.init(1, 48000, 5500, 0);
      radio.controlBus.controlVoltage = 0;
      radio.mixer.conversionGain = 1e-3f;
      double peak = 0;
      for (int n = 0; n < 4800; ++n) {
        RadioSampleContext ctx{};
        ctx.signal.setComplexEnvelope(n == 100 ? 1000.0f : 0.0f, 0);
        RadioIFStripNode::run(radio, 0, ctx);
        peak = std::max(peak, std::hypot(double(ctx.signal.detectorInputI),
                                         double(ctx.signal.detectorInputQ)));
      }
      peaks[conduction] = peak;
    }
    check(peaks[1] < .2 * peaks[0], "an IF crash is not limited by the plate swing");
  }
  for (auto profile : {RadioReceiverProfile::Typical1930s, RadioReceiverProfile::Philco37116}) {
    double levels[2] = {};
    for (int conduction = 0; conduction < 2; ++conduction) {
      Radio1938 radio;
      radio.applyReceiverProfile(profile);
      if (conduction == 0) radio.ifStrip.stagePlateSwingLimitVolts = 0;
      radio.init(1, 48000, 5000, 0);
      std::vector<float> input(24000), output(24000);
      for (size_t n = 0; n < input.size(); ++n)
        input[n] = static_cast<float>(std::sin(2 * pi * 1000 * n / 48000.0));
      radio.processAmAudio(input.data(), output.data(), 24000, .0087f, .85f);
      double sum = 0;
      for (size_t n = 12000; n < output.size(); ++n) sum += double(output[n]) * output[n];
      levels[conduction] = std::sqrt(sum / 12000);
    }
    check(std::abs(20 * std::log10(levels[1] / levels[0])) < .1,
          "the IF plate-swing limit compresses normal full-modulation reception");
  }
}

void testModulationHum() {
  const double typical = detectorHumRatioDb(RadioReceiverProfile::Typical1930s, true);
  const double typicalQuiet = detectorHumRatioDb(RadioReceiverProfile::Typical1930s, false);
  const double philco = detectorHumRatioDb(RadioReceiverProfile::Philco37116, true);
  std::cout << "detector 120 Hz modulation hum dB: typical=" << typical
            << " (no screen ripple " << typicalQuiet << "), philco=" << philco << '\n';
  check(typical > typicalQuiet + 20 && typical > -65 && typical < -35,
        "B+ ripple on the 38-12 screens must modulate the carrier");
  check(philco < typical - 20, "the Philco's screen filtering must suppress modulation hum");
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
  std::cout << "avc," << radio.demod.am.avcEnv << ",IF gm,"
            << radio.ifStrip.stageTransconductanceSiemens << '\n';
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
  testTriodeBeyondTable();
  testModulationHum();
  testIfPlateSwingLimit();
  testCircuitReference();
  testReceiverTuning();
  testFrontEndTunedCircuits();
  testRemoteCutoffAvc();
  testPrimaryOnlyShuntCapacitor();
  testIndependentTubeCharacteristics();
  testPhysicalNoiseSources();
  testAfcWithRealCarrier();
}
