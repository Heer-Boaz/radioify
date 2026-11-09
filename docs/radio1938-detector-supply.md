# Thermionic detector and loaded power supply

Revision: 2026-11-06, after the IF/circuit revision in `radio1938-realism-audit.md`.

This replaces the rounded-resistance detector and the programme-envelope supply
modulation. It does not establish an exact measured Philco response. The UI and
headphone room defaults are unchanged; the acoustic speaker/cabinet fits were
not altered to compensate for circuit changes.

## Detector

`ThermionicDiode` uses the space-charge relation `I = K * V^(3/2)` above
`V = 1.5 * kT/e`, joined continuously in current and slope to an exponential
retarding-field tail. Voltage includes a separately declared contact potential.
This is a compact finite-temperature approximation, **not** a semiconductor
Shockley junction or a complete Langmuir-Fry electron-transport solution.

A monotonic scalar solve includes source resistance and the implicit capacitor
companions. Both audio and AVC branches use the diode current law; AVC delay is
a bias voltage across the branch, not subtraction from an ideal rectified audio
envelope. At zero RF, thermal contact current can establish a small DC bias.
That bias is blocked by the audio coupling network and is not a noise source.

The RF detector filter now retains both capacitor voltages. The volume/grid
network remains a loaded port, with its previously validated block-averaged
coupling; it is not applied as an extra EQ after detection.

| Parameter | Typical 1930s set | Philco 37-116 |
| --- | ---: | ---: |
| Detector construction | 75 diode section | 6J5 grid used as diode anode; plate grounded |
| First / second RF filter capacitor | 100 / 100 pF | 110 / 110 pF |
| Filter series resistance | 50 kohms | 51 kohms |
| Separate audio discharge resistor | none; volume network provides load | 160 kohms |
| Diode perveance prior, A/V^(3/2) | .000085 | .00045 |
| Thermal voltage / contact potential prior | .073 / 0 V | .073 / 0 V |
| IF source resistance prior | 5.1 kohms | 5.1 kohms |

The small-set filter is from Bulletin 284, components 12B/12C/13; the Philco
filter is from Bulletin 258 / Rider 7-31/32, components 72/72A/73. The latter's
51 kohm value must not be misread as 700 ohms from a reduced scan.
The actual Philco audio detector is **not** the 6H6 AFC discriminator.

The current-scale assumptions remain family priors: the independent 6H6
observations in TM 11-662 are 10 mA at 8 V, 40 mA at 20 V and 66.4 mA at 28 V.
A .000445 perveance reproduces these within 4%. Tuttle's measured 6AV6/6AT6 diode
sections illustrate the smaller .000085 scale. Neither establishes the exact
low-current curve of the installed 75 or grid-connected 6J5. The .073 V thermal
voltage corresponds to approximately 850 K; no contact-potential measurement
for these specimens was found.

The previous sub-volt IF calibration depended on the nearly ideal diode.
Nominal IF gain is now 20 / 46, with physical volume positions .05 / .25.
These remain gain-staging priors. The normalized AVC reference is
`mixer.avcGridDriveVolts / 1.25`, so one measured AVC volt imposes one volt of
additional mixer-grid bias. Previously the normalization multiplied it by
roughly six. Downstream Philco digital makeup (now 2.41, see the realism audit) sets listening level;
this does not increase electrical tube drive.

## Supply

`RectifierSupply` contains full-wave rectification, transformer source resistance,
reservoir charge, filter choke current, the transformer centre-tap return, an
auxiliary supply branch and the bypassed first-audio feed. A fixed-size nodal
matrix is inverted at initialization. Runtime uses its affine Thevenin port and
one nonlinear diode solve, with no allocations. Zero-resistance wires merge
nodes exactly; no small artificial resistor is inserted for a missing branch.

| Parameter | Typical 1930s set | Philco 37-116 |
| --- | ---: | ---: |
| Rectifier in the service schematic | 84 | 5U4G |
| Reservoir capacitor | 12 uF | 16 uF (8 + 8) |
| Main filter capacitor | 4 uF to chassis | 8 uF to transformer centre tap |
| Main choke / field winding | 1500 ohms; 10 H prior | 80 ohms; 4.75 H |
| Centre-tap return | 250 + 70 ohms | 360 + 18 ohms; 5 H field prior |
| Return bypass | none | 10 uF |
| Auxiliary audio feed | direct wire | 1000 + 400 ohms; 8 H choke prior; 3 uF |
| First-audio decoupling | direct wire | 99 kohms / .05 uF |
| Source resistance prior, per conducting half-cycle | 400 ohms | 100 ohms |
| Rectifier perveance prior, A/V^(3/2) | .00058 | .00075 |
| Other main / auxiliary load priors | 20 / 0 mA | 10 / 30 mA |

Philco parts catalogue 1938 identifies choke 32-7056 as 4.75 H at 150 mA,
80 ohms. The other inductances and rectifier current scales above are declared
priors, not measured winding or specimen data. The 1937 audio feed uses the
3 uF branch after resistor 124 (1000 ohms) and choke 120 (400 ohms); the separate
4000-ohm / 2 uF branch must not be substituted for it. Other RF branches are
represented by standing loads, not individually solved.

The unknown mains-transformer ratio is calibrated once at initialization against
the existing idle B+ priors (190 / 325 V), using the nominal tube currents, and
then held fixed. The circuit starts from a settled, energized state. Playback
load changes are not normalized away. These voltage priors are not a claim to
have recovered exact transformer ratios or all service voltmeter readings.

At the power-stage oversampling rate, the next supply step uses actual solved
output plate currents, driver current and averaged first-audio current. The
single-ended screen-current load is an explicit .15 plate-current ratio prior;
its current model follows the instantaneous screen supply. Plate voltages use
the resulting rail voltages. The driver transformer keeps a fixed DC reference,
so supply ripple is not accidentally cancelled by recentering its current.
The supply/audio coupling is staggered by one internal sample, not a single
monolithic nonlinear solve of the entire receiver.

Hum therefore follows mains frequency, capacitance, inductance and loading.
Push-pull output devices see the same B+ disturbance. The former artificial
ripple waveform, gain-sag envelope and branch-depth controls are removed.
Supplemental synthesized hum is disabled in both presets to avoid counting
rectifier hum twice; it remains available as an explicitly optional noise
experiment. A zero mains-frequency setting selects a DC bench supply, preserving
load regulation while isolating ripple from aliasing tests.

## Verification and limits

The circuit tests check independent published diode-current points, derivative
continuity, Thevenin-branch residuals, every supply node's charge/current balance,
choke flux, the first-audio DC resistor drop, load-step droop, 50/60 Hz spectra,
capacitance sensitivity, integration-rate convergence and deterministic reset.
Detector/volume coupling is compared with a fully coupled 16x reference.
The normal audio, oversampling, clipping, IF, settings-transaction and playback
tests remain active.

Three old acceptance assumptions are explicitly superseded:

- A shared 35–55 us detector time constant described the old single capacitor.
  The two-capacitor dominant release poles now have separate 30–45 us (Philco)
  and 90–120 us (typical) guards.
- The small receiver's complete electrical bandwidth includes its real detector
  filter. Its coarse-grid high -3 dB guard now starts at 1.8 kHz, rather than
  demanding at least 2.5 kHz regardless of the detector load.
- Nominal Philco playback uses a lower volume setting and approximately
  36–64% of the rated electrical power. The voltage-reference-ratio guard is
  .60–.80. The independently derived 12–18 W capacity, SINAD, IMD and clipping
  limits remain unchanged. Output-driver capability and normal listening drive
  are distinct tests.

Remaining limitations include unmeasured signal-diode characteristics, finite-
temperature approximation, no heater warm-up or rectifier emission saturation,
constant equivalent loads for RF stages, linear supply inductors, fixed tube
biases rather than a complete back-bias/heater network, and a simplified
single-ended screen-current law. Some detector/AVC coupling components are still
represented by equivalents. The speaker/cabinet response remains based on
source-labelled family examples, not exact-driver acoustic measurements.

## Measured result of this revision

`build.ps1 -Static -Tests` passed all **106 CTests and 25 Python tests**. The
published `dist/radioify.exe` matches the tested build binary by SHA-256.

| Observation, nominal harness | Typical set | Philco |
| --- | ---: | ---: |
| Electrical SINAD | 20.32 dB | 34.82 dB |
| Acoustic SINAD | 17.30 dB | 29.46 dB |
| Speaker voltage / rated reference | .278 | .659 |
| Upper electrical -3 dB point, coarse frequency grid | 2.0 kHz | 4.5 kHz |
| 2 s stereo playback, room on, previous median | 219.31 ms | 406.31 ms |
| 2 s stereo playback, room on, new median | 244.32 ms | 414.45 ms |

The playback timings are the medians of five alternating before/after runs of
the same 48 kHz, 256-frame-block workload. They are **rendering cost, not added
audio latency**; initialization and instrumented calibration are excluded.
The increase is approximately 11.4% / 2.0%. No speaker solver nonconvergence
occurred. The four-versus-eight-samples-per-IF-cycle comparison changes nominal
SINAD by .04 / .17 dB and speaker-reference level by about .04 / .06 dB.

Reproducible data:

- [Nominal electrical/acoustic guards](data/radio1938-detector-supply-nominal.csv).
- [IF waveform integration convergence](data/radio1938-detector-supply-convergence.csv).
- [All paired playback timings](data/radio1938-detector-supply-performance.csv).
- [Settled per-stage DC, signal, SINAD and 120 Hz hum](data/radio1938-detector-supply-stages.csv),
  measured over the final second of a three-second 1 kHz programme.

The old `audioDiodeDrop` / `audioJunctionSlopeVolts` family of settings and the
synthetic `power.rippleDepth`, sag-envelope and branch-depth settings have no
one-to-one physical equivalents. They are rejected by the settings parser,
rather than silently ignored. The new diode parameters are under `demod`, and
supply components under `powersupply`; the canonical first-audio DC feed resistor
remains `receivercircuit.tubeDcFeedResistanceOhms`.

## Sources

- [Philco Service Bulletin 284, 38-12; Rider 8-69/70](https://www.nostalgiaair.org/pagesbymodel/236/M0013236.pdf).
- [Philco Service Bulletin 258, 37-116](https://philcoradio.com/library/download/service%20info/service%20bulletins/Philco%20Service%20Bulletin%20258.pdf),
  [Rider 1937, Philco 7-31/32 and 7-36](https://www.worldradiohistory.com/Archive-Rider/37/Rider-1937-PQR.pdf).
- [Philco Parts Catalog 1938](https://philcoradio.com/library/download/parts/catalogs/1938/Philco%20Parts%20Catalog%201938.pdf), choke 32-7056.
- [US Army TM 11-662, Basic Theory and Application of Electron Tubes](https://www.schematicsforfree.com/files/Theory%2C%20Education%20%26%20Reference/Components/Basic%20Theory%20and%20Application%20of%20Vacuum%20Tubes.pdf), printed pp. 31–33, Fig. 33.
- [J. B. Tuttle, Electronics 27: Vacuum Tubes](https://mysite.du.edu/~etuttle/electron/elect27.htm), measured diode characteristics and cathode-temperature discussion.
