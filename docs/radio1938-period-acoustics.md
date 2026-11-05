# Period acoustic evidence and the Radio1938 fit

2026-11-05. This revision supersedes the acoustic estimates and acoustic
acceptance bands in the [July realism audit](radio1938-realism-audit.md).
Both receivers are source-anchored reconstructions, not measurements of the
Philco BO-1 or Type-W. No exact-part acoustic, impedance, phase, or polar data
has been established. `typical-1930s` does not mean a population average.

## Evidence retained

The [wideband CSV](data/radio1938-period-wideband.csv) preserves the **83 supplied
wideband observations** and nine explicit `NA` entries from the user's report.
It is a transcription of that report, not a new digitization or the complete
199-observation ZIP. `NA` is missing evidence, never silence. Frequencies are
samples of printed curves, not third-octave energy averages. Each trace uses
its own 1 kHz reference; absolute sensitivities cannot be compared.

| ID | Source and role | Established conditions and limits |
| --- | --- | --- |
| K5 | Knowles 1933, Fig. 7B; principal small-cabinet example | Nominal 5-inch speaker in an unnamed personal-set cabinet; alternative large-baffle LF branch is the same driver. Distance, drive and this figure's field excitation unstated. |
| Q5 | Quam 1938 advertisement; comparison only | Dotted 5-inch dynamic comparator, not the solid Permanic curve. Mounting, model, drive and field/PM status unknown. Its bass is not transferred to the small cabinet. |
| K12 | Knowles 1933, Fig. 7A; principal large-speaker shape example | Nominal 12-inch, mounting/drive/distance unstated. Adjacent-size evidence for a 14-inch console, not a Type-W response. |
| D33 | NPL/Magnavox Duode 1936, Fig. 1; premium comparison only | 48 x 53-inch baffle, forward axis at 6 ft, outdoors with reflection-diverting reflectors; 2 V RMS across voice coil plus 1 ohm. Field 10 W, 170 V, 59 mA. 12-inch identification is secondary; special composite coil. Not an anechoic or ordinary Type-W measurement. |

Original 1 kHz ordinates: K5 -23.5 dB, Q5 -2 dB, K12 -17 dB, D33 +19 dB.
Only D33's pressure reference is established here (1 dyne/cm2). The report's
approximate *per-ordinate scan-reading* uncertainties are 1.5 dB for Knowles,
2 dB for Quam, and 1.5 dB for Duode (2.5 dB on steep features). Normalizing adds
shared reference uncertainty. These are not measurement or production errors.
Knowles's irregular 4-10 kHz axis makes the readings above 4 kHz tentative;
they must not determine a precise cutoff. Repeated mountings, fields or angles
are not counted as independent speakers. No statistical min/max envelope is used.

Primary source references supplied with the report:

- [H. S. Knowles, "Loudspeaker cost vs. quality," Electronics, September 1933](https://www.worldradiohistory.com/Archive-Electronics/30s/Electronics-1933-09.pdf), pp. 240-242, 256, 260; Fig. 7 p. 242, Fig. 1 p. 240. The separate three-foot-baffle distortion experiment does not specify Fig. 7's setup.
- [Quam-Nichols advertisement, Electronics, August 1938](https://www.worldradiohistory.com/Archive-Electronics/30s/Electronics-1938-08.pdf), p. 37, dotted dynamic comparator.
- [Burgess Dempster, "The Magnavox 'Duode' Speaker," Wireless World, 6 March 1936](https://www.worldradiohistory.com/UK/Wireless-World/30s/Wireless-World-1936-03.pdf), pp. 241-243, Fig. 1 p. 241.
- [J. D. Seabert, "Electrodynamic Speaker Design Considerations," Proceedings IRE, June 1934](https://www.worldradiohistory.com/Archive-IRE/30s/IRE-1934-06.pdf), pp. 738-750, DOI 10.1109/JRPROC.1934.226380. Fig. 1 is unsized, measured on a 48-inch square baffle at 12 inches, at three angles. Fig. 2 is a 12-inch cone-area experiment with unusually soft suspension; Fig. 1's distance must not be assigned to it.
- [H. F. Olson, Elements of Acoustical Engineering, 1940](https://www.introni.it/pdf/Olson%20-%20Elements%20of%20Acoustical%20Engineering%201940.pdf), Figs. 7.8-7.9, pp. 128-130. The finite-baffle mass-controlled example supports cancellation, not a universal total low-bass slope; the console example has no established diameter.

The Seabert cone-area experiment, Knowles's two field settings on one driver,
and Olson's unsized console provide mechanism context, not extra wideband
speakers to average into K5/K12. Duode's output at 8 kHz is a premium extension
example. Jensen's contemporary A-12 standard/HF claims (about 5/7.5 kHz) have
no specified dB criterion, so neither is treated as a -3 dB calibration.
Uncalibrated or inconsistent-axis plots from the separate Duode review,
Sherman, Rola and Swift are not converted into invented dB targets.

## Signal model

The former acoustic node treated `I * Re` from the motor solver as pressure.
That quantity follows force, not radiated pressure. It stayed almost flat at
50 Hz even for the five-inch speaker. The new `radiation` biquad converts motor
force to normalized acceleration using the **same effective mechanical mass,
compliance and damping** as the electrical solve:

```
M x'' + R x' + x/C = Bl I
pressure / motor ~ s^2 / (s^2 + (R/M)s + 1/(MC))
```

Back EMF and coil inductance remain in the electrical solver. The radiation
section uses mechanical Q, not electrically loaded total Q, so electrical
damping is counted once. This is a piston-region approximation with a
normalized scale, not a calibration of absolute SPL, phase or room radiation.
The residual wideband sections approximate the complete response; their gains
must not be read as separately measured physical breakup-mode amplitudes.

The small model uses a **200 Hz natural-frequency prior**, within Knowles's
180-250 Hz statement for the small speaker under discussion. This is not a
measured free-air BO-1 resonance. The console uses **80 Hz**, within the
explicitly unsized 70-120 Hz historical prior, not 14-inch-specific T/S data.
Other unmeasured motor parameters remain estimates.

For the small cabinet, one empirical mounting section approximates K5's
cabinet-minus-large-baffle comparison:

| Hz | Supplied difference, dB | Implemented difference, dB at 48 kHz |
| ---: | ---: | ---: |
| 125 | -10.5 | -11.3 |
| 160 | -9.5 | -7.5 |
| 200 | -4.5 | -4.6 |
| 250 | -3.0 | -2.5 |
| 315 | -1.5 | -1.2 |

Its 235 Hz second-order high-pass is a smooth **total mounting-change fit**,
not an isolated dipole law or a measured 38-12C baffle corner. Its deep-bass
asymptote is extrapolation outside the data. For the console, the 120 Hz
mounting section is a residual fit to K12 with the motor and existing
cabinet/clarifier proxy included; the actual K12 mounting is unknown.
The split between driver and cabinet is therefore not uniquely identifiable.
The complete chain, rather than each invented physical subdivision, is checked
against the labelled examples.

The separate Bobb US2059929 Fig. 10 proxy remains: 95 Hz untreated resonance,
three 108 Hz damping sections, about 9 dB maximum attenuation. It is not new
Type-W evidence. Its tests isolate the resonance/absorber comparison; the
wideband tests include it together with the mounting fit. The old ungrounded
155 Hz small-cabinet boost, cloth low-pass and listener-dependent rear-delay
mix are disabled in the presets. No extra cabinet transfer is cascaded onto
the completed in-cabinet fit. Optional cabinet settings remain available.

## Geometry is metadata, not filter calibration

Period catalog exterior W x H x D:

- [38-12C / Philco 12C, 1938 catalog, PDF p. 75/98](https://philcoradio.com/library/wp-content/uploads/2019/12/Philco-Sales-Catalog-for-1938-June-1937-300dpi.pdf): **11 5/8 x 8 x 6 1/8 inches**.
- [37-116X, 1937 catalog, printed p. 10 / PDF p. 11/48](https://philcoradio.com/library/wp-content/uploads/2024/04/RR0689-1937-Philco-Sales-Catalog.pdf): **26 7/8 x 42 5/8 x 17 1/4 inches**. The earlier 116X's 12-inch depth is not used.

Modern specimen dimensions disagree with the console catalog, particularly in
depth; the catalog is retained as nominal without inventing a reason for the
discrepancy. Interior dimensions, actual baffle outline, speaker height, board
inclination, diffuser dimensions, openings and original cloth transmission
remain unknown. Reported grille-cloth rectangles are not baffle dimensions.
Exterior half-wavelength scales (581/844 Hz and 251/158 Hz) are neither baffle
corners nor transition bands. Neither presets nor tests derive a delay or
cancellation corner from those numbers. No floor/wall or off-axis response is
claimed.

## Validation and numerical comparison

`radio_node_physics --section acoustics --preset <profile>` drives the modeled
voice-coil terminals with a 0.1 V peak sine, including back EMF and inductance,
then runs the production speaker and cabinet nodes. RF, IF, detector, tube
coloration, digital makeup and room placement are excluded from this comparison.
Each trace is normalized to its own 1 kHz result. All 16 K5/K12 points from
125 Hz to 4 kHz are checked. Smooth-model tolerances are 4 dB normally, 5 dB
at 160/200 Hz and 6 dB at 125 Hz. These tolerances are engineering choices,
not the publication's scan uncertainty or a population envelope. In particular,
the narrow K5 200 Hz feature is smoothed, not precisely reconstructed.

| Hz | Small before | Small after | K5 | Console before | Console after | K12 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 125 | -4.5 | -24.1 | -25.0 | -1.3 | -6.5 | -6.5 |
| 200 | +1.2 | -9.2 | -4.5 | +1.2 | -4.1 | -4.5 |
| 500 | +0.8 | -4.4 | -5.5 | +0.1 | -2.2 | -2.5 |
| 1,000 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| 2,000 | -0.4 | +3.1 | +5.5 | -0.8 | +5.5 | +7.0 |
| 2,500 | -1.4 | +5.8 | +5.0 | -1.5 | +8.0 | +8.0 |
| 3,150 | -2.3 | +4.3 | +4.5 | -2.6 | +8.4 | +7.0 |
| 4,000 | -4.2 | -2.5 | -2.5 | -5.1 | +4.4 | +6.0 |

All entries are dB relative to the same trace at 1 kHz; simulation at 48 kHz.
The [comparison CSV](data/radio1938-acoustic-comparison.csv) preserves the
unrounded results at the sampled frequencies.

![Measured simulation before and after, against the labelled Knowles examples](data/radio1938-acoustic-comparison.svg)

The 50 Hz rejection and broad 8 kHz roll-off are engineering guards only.
CTest checks both profiles at 44.1, 48 and 96 kHz. Playback tests exercise
full-scale tones from 125 Hz through 4 kHz, a mixed programme, reset, bypass
and receiver transitions. Makeup gain reserves headroom for the actual
upper-midrange peaks; it does not change the physical 2 W/15 W power references.

`radio_measurements --section reference` reports the receiver before acoustic
weighting and the full acoustic output separately. The former flat-response
low -3 dB edge is unsuitable for a speaker with a strong 2-3 kHz rise: relative
to the global maximum it describes the *top of that rise*, not bass extension.
The terminal voltage is observed while the entire production graph still runs;
nodes are not disabled or reordered to obtain it. The original SINAD thresholds
of 35/20 dB were measured *after the former acoustic filters*, which attenuated
noise and harmonics. They cannot be transferred to either an unweighted
electrical output or the new rising acoustic response.

A controlled comparison during the initial acoustic fit (before the ingress
review below) changed only the motor's natural-frequency prior back
to 65 Hz (console) or 115 Hz (small), using
`radio_measurements --section reference --preset <profile> --speaker-natural-hz <hz>`.
It confirms that the receiver itself has not acquired the apparent acoustic
distortion increase:

| Metric | Small, prior 115 Hz | Small, new 200 Hz | Console, prior 65 Hz | Console, new 80 Hz |
| --- | ---: | ---: | ---: | ---: |
| Terminal SINAD, dB | 19.11 | 19.19 | 32.22 | 32.24 |
| Terminal IMD, dB | -23.96 | -24.52 | -42.35 | -42.36 |
| Terminal low/high -3 dB edges, Hz | 150 / 5,000 | 200 / 4,000 | 75 / 4,000 | 75 / 4,000 |

The reference tests explicitly guard terminal SINAD at 18/30 dB minimum
(small/console), and terminal IMD at -23/-38 dB maximum. The acoustic values are
17.80/29.66 dB SINAD and -20.75/-39.04 dB IMD after the ingress review, with guards of 15/28 dB and
-18/-35 dB respectively. These are model regression limits, not historical
distortion measurements. The lower acoustic SINAD is reported, not hidden by
the electrical check. Physical power, detector/AVC timing and nominal clipping
checks remain. Final terminal SINAD is 19.22/31.76 dB. Full-scale playback tone
peaks are 0.846/0.753; the mixed programme peaks are 0.329/0.293, below the 0.95
digital headroom limit. The reference guard bands were not relaxed in the review.

Validation: `powershell -NoProfile -File .\build.ps1 -Static -Tests` builds the
application and runs all 99 CTests, including the new acoustic cases, followed
by the speech bootstrap's Python regression checks. The built application is
`dist/radioify.exe`.

## Implementation review and playback noise

The follow-up review retained the acoustic fit and corrected signal ownership
and measurement issues. The radiation high-pass and residual EQ use standard
bilinear biquads ([W3C Audio EQ Cookbook](https://www.w3.org/TR/audio-eq-cookbook/)).
The radiation section is a reduced mechanical observer driven by the solved
coil current; it is not another electrical load or another copy of the cabinet.
Its state is now refreshed with the motor when receiver identity changes.
Only the power node owns the physical terminal-voltage state.

Terminal measurements now use an optional caller-owned observation buffer in
the production AM entrypoint. The previous copied ingress implementation was
removed; the envelope trace also uses production ingress. A stereo regression
checks that observing the voltage leaves audio samples identical, including
split blocks. With tuning held fixed, partitioned and single-block results
agree. This test also exposed a separate ingress issue: the carrier oscillator
was reconstructed from a separately accumulated float phase at every block.
Keeping the oscillator state across blocks and carrier warmup removes that
discontinuity. Warmup scratch storage now accounts for the channel count.
The observation and oscillator changes add no callback allocations.

The user's observation that the console sounded much cleaner was reproduced
with the actual playback preview path: 48 kHz, 5.5 kHz bandwidth, default
`everyday-1938` reception, `noise=0.012`, 0.12 V RMS received carrier and 0.85
modulation index. A silent programme is followed by a 0.5-peak 1 kHz tone.
Each lasts one second; the final half-second of each is measured. Noise is
DC-removed, unweighted RMS; signal is the 1 kHz fundamental. These ratios are
not full-modulation SNR, A-weighted figures, or historical receiver measurements.

| Model | Before review, S/N dB | After review, S/N dB | After review, quiet-output RMS |
| --- | ---: | ---: | ---: |
| Small table set | 34.79 | 34.35 | 0.003020 |
| Console | 44.50 | 31.34 | 0.002939 |

Source-isolation measurements showed that IF hiss dominates this contrast;
the extra post-power hiss and hum are much smaller. Previously IF noise was
added at the detector with AVC factors but without the nominal IF gain/filter.
The console's 4.6x IF signal gain therefore gave it an artificial S/N advantage.
Noise is now added at the IF input, after mixer conversion and before IF gain
and loaded-can filtering. This follows input-referred noise accounting
([Analog Devices](https://www.analog.com/en/resources/technical-articles/low-noise-instrumentation-amplifiers.html));
it does not establish the noise figure of either historical receiver.
The preset noise-control values and digital makeup are unchanged by this review.
Consequently the console's hiss increases by about 13 dB; the small set changes
by less than 0.5 dB. The remaining signal-relative contrast reflects the
different upstream receivers and acoustic weighting, not a requirement that
both receivers have identical S/N.

`ifInputNoiseAmp` retains the existing amplitude convention at 48 kHz. With two
independent uniform quadratures scaled by `1/sqrt(2)`, each quadrature's one-sided
input density is `amplitude/sqrt(3*48000)` V/sqrt(Hz). Scaling the sample amplitude
by `sqrt(fs/48000)` preserves this density at other rates; the IF filter sets
the integrated bandwidth. Tests cover gain invariance, bandwidth reduction,
and 44.1/48/96 kHz with a fixed physical passband. The latter uses a 2 kHz service
bandwidth to stay below the IF model's existing sample-rate cutoff cap.

`radio_receiver_profile_tests --noise-report` reproduces the individual-source
measurements in [the noise comparison CSV](data/radio1938-playback-noise.csv).
The default playback regression checks signal-relative noise and peak headroom.
Its 28-45 dB S/N guard and maximum 6 dB inter-profile difference are engineering
limits for this stimulus and noise control, not historical tolerances.
Impulsive-interference exposure, the quiet post-power texture/hum, cone
nonlinearity and the cabinet residual remain explicitly approximate components.
The source evidence does not justify treating them as calibrated component data.

The reconstructed responses have been measured in software, not compared by
ear with original receivers. Original phase, polar response, absolute SPL,
manufacturing variation, listening position and a 14-inch-specific wideband
measurement remain open evidence gaps.
