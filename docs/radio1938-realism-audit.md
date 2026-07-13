# Radio1938 realism audit

Date: 2026-07-13

## Verdict

The Radio1938 filter is not a simple equalizer-and-noise effect. It contains a
reduced-order AM receiver, detector, control loops, tube stages, transformers,
an electrodynamic speaker load, and a cabinet model. After the remediation in
this audit, its documented electrical anchors and all explicit validation bands
pass. It is a technically credible reduced-order period-radio simulation.

It is still not an exact historical reproduction of one measured Philco
37-116. No calibrated response or Thiele/Small measurement of an original
36-1219 Type-W speaker was located. There is, however, much stronger acoustic
evidence than the initial audit assumed: Philco's own clarifier patent contains
a measured whole-cabinet response with and without the absorbers. The cabinet
boom and clarifier response are now constrained by that trace, while the
speaker-only response remains a clearly labelled reconstruction. The defensible
claim is therefore "historically anchored and physically modelled," not
"measurement-identical."

## Historical anchors

The following values are traceable to period documentation:

- 470 kHz intermediate frequency;
- approximately 15 W undistorted output;
- push-pull class-A output stage;
- Type-W high-fidelity electrodynamic speaker, part 36-1219, with a 14-inch
  cone and 3.9-ohm nominal voice-coil impedance;
- three identical Type-K acoustic clarifiers, part 36-1155;
- expanded IF alignment with peaks around 465 and 475 kHz;
- magnetic tuning intended to capture a station within approximately 5 kHz.

Philco's acoustic-development evidence additionally establishes that:

- the clarifiers are damped tuned absorbers, not sound-producing passive
  radiators or upper-midrange enhancers;
- the representative open-back cabinet resonance occupies approximately
  70-150 Hz and peaks near 95 Hz;
- a damped six-inch clarifier is centered near 108 Hz;
- the measured clarifier system reduces the cabinet peak by about 10 dB and is
  effectively inert outside the resonance band.

Period broadcast-chain evidence establishes a separate source-side envelope:

- Western Electric's 630A moving-coil microphone covered 40-10,000 Hz;
- the Western Electric 110A program amplifier was specified flat within 1 dB
  from 30-10,000 Hz with less than 1% distortion;
- its compressor inserted in 20 ms, recovered in 250 ms, and converted an
  input 5 dB above threshold into only 2 dB above threshold, a 2.5:1 ratio;
- an RCA 5-D transmitter installation at WBNS reported output noise 62-66 dB
  below full modulation and approximately 0.65-1.9% audio distortion across
  its tabulated frequency and modulation points.

Sources:

- [Philco Service Bulletin 258](https://philcoradio.com/library/download/service%20info/service%20bulletins/Philco%20Service%20Bulletin%20258.pdf)
- [Philco 1937 Parts Catalog](https://philcoradio.com/library/download/parts/catalogs/1937/Philco%20Parts%20Catalog%201937.pdf)
- [Philco acoustic-clarifier patent US2059929A](https://patents.google.com/patent/US2059929A/en)
- [Philco Model 116 instructions](https://philcoradio.com/library/wp-content/uploads/2019/12/Philco-116X-Instructions.pdf)
- [Radio-Craft, January 1937 service data](https://www.rsp-italy.it/Electronics/Magazines/Radio-Craft/_contents/Radio-Craft%201937%2001.pdf)
- [Western Electric 630A microphone bulletin](https://www.worldradiohistory.com/Archive-Catalogs/Western-Electric/WE-630A_Mic_promo.pdf)
- [Western Electric 110A program-amplifier bulletin](https://www.worldradiohistory.com/Archive-Catalogs/Western-Electric/Western-Electric-110A-Program-Amp-1937.pdf)
- [RCA Broadcast News, July 1938, WBNS 5-D installation](https://www.worldradiohistory.com/Archive-All-BC-Engineering/RCA-Broadcast-News/RCA-28.pdf)

## Model classification

### Documented

- receiver topology and tube complement;
- IF center frequency;
- nominal undistorted output power;
- selected resistors, capacitors, controls, and transformer/load anchors;
- speaker type, diameter, part number, and nominal voice-coil load;
- number, type, and part number of the acoustic clarifiers.

### Philco-lineage measurement proxy

Figure 10 of US2059929A is a real Philco measurement, but not a measurement of
the production 37-116. Its test cabinet used an 8.5-inch driven speaker, one
large absorber, and two six-inch absorbers. The 37-116 instead used a 14-inch
Type-W speaker and three identical Type-K absorbers. The patent trace is
therefore used only for the shared acoustic mechanism and its bounded targets:
the 70-150 Hz cabinet-resonance band, the 95 Hz untreated peak, approximately
108 Hz for the damped small absorber, and roughly 10 dB maximum reduction.
It is not copied as an exact full-range 37-116 curve.

### Reduced-order physics

- the 470 kHz IF is represented as a complex envelope instead of being sampled
  literally at audio sample rates;
- the live detector uses a bounded real-time solve budget;
- tube and transformer stages use compact numerical models suitable for the
  playback hot path;
- the broadcast program limiter and transmitter transfer use a peak-envelope
  compressor and calibrated cubic term instead of a component-level studio and
  transmitter circuit simulation.

### Fitted sound design

- speaker Thiele/Small parameters that are absent from the Philco service data;
- cone breakup, dip, top roll-off, and compliance asymmetry;
- grille and listener-dependent open-back rear-radiation parameters;
- seeded component drift;
- procedural hiss, crackle, and hum levels;
- deterministic broadband transmitter noise inside the documented period
  output-noise range.

Fitted values are acceptable when they are labelled as estimates and validated
against an explicit response target. They are not evidence of historical
accuracy by themselves.

## Pre-fix baseline measurements

Measured before remediation at 48 kHz, 5.5 kHz requested audio bandwidth, and
the normal 0.012 noise setting unless noted otherwise.

| Metric | Measured | Required | Baseline |
| --- | ---: | ---: | --- |
| Low -3 dB edge | 75 Hz | 60-200 Hz | pass |
| High -3 dB edge | 600 Hz | 2.5-5.5 kHz | fail |
| Detector time constant | 45.16 us | 35-55 us | pass |
| AVC time constant | 75.0 ms | 60-90 ms | pass |
| Nominal SINAD | 35.99 dB | at least 35 dB | pass |
| Speaker reference ratio | 0.663 | 0.85-1.10 | fail |
| IF center | 470 kHz | 469-471 kHz | pass |
| Nominal output power | 14.52 W | 12-18 W | pass |

Stage isolation located the high-frequency loss as follows:

| Last enabled stage | High -3 dB edge |
| --- | ---: |
| IF strip | 4.5 kHz |
| Detector | 4.5 kHz |
| Receiver circuit | 4.0 kHz |
| Tone | 4.0 kHz |
| Power stage | 4.0 kHz |
| Speaker | 600 Hz |
| Speaker and cabinet | 600 Hz |

The end-to-end live render, relative to 150 Hz, measured approximately -2.9 dB
at 600 Hz, -8.1 dB at 1 kHz, -21.5 dB at 2.5 kHz, and -34.1 dB at 4 kHz.

## Defects found

### 1. Speaker response is over-damped at high frequencies

The power stage remained useful above the final 600 Hz edge, but the speaker
stage reduced the total high -3 dB edge to 600 Hz. The speaker path combined
the electromechanical load response with additional fitted cone and low-pass
filters. A separate solver defect also changed the output-transformer
integration count from eight substeps to one after the first sample, slowing
the simulated electrical time by a factor of eight.

### 2. Live preview narrows the bandwidth twice

The receiver treats `bwHz` as audio sideband bandwidth and derives a physical
AM channel twice that width. Playback and export multiplied the same audio
bandwidth by `0.48` before two additional program low-pass filters. At the
default 5.5 kHz setting this created two prefilters at 2.64 kHz before the IF
model applied its own bandwidth.

### 3. Magnetic-tuning range is too small

The preset used a 420 Hz capture range and limited correction to 110 Hz. This
was not compatible with the period description of capture within approximately
5 kHz. Its former linear capture multiplier also reduced correction authority
to zero at the stated capture boundary.

### 4. Validation does not reliably fail

- `radio_measurements` prints failed reference rows but returns success;
- `radio_node_physics` overflows the Windows process stack in several sections;
- the Tone bypass test disables presence while leaving the tilt stage enabled;
- some transient metrics are printed without proving a meaningful response.

### 5. Cabinet and clarifier tuning is not historically credible

The preset placed its three clarifiers at 165, 205, and 255 Hz with only tiny
couplings. Philco's service data says the 37-116 used three identical Type-K
units, while its clarifier patent places the damped small unit near 108 Hz and
shows absorption concentrated in the 70-150 Hz cabinet-boom band. The old
values could neither represent the installed parts nor reproduce the measured
clarifier effect. The separate 180, 650, and 900 Hz cabinet sections were also
an unconstrained generic fit rather than a reconstruction of the published
response.

### 6. The transmitted program is unrealistically ideal

The live preview previously converted the modern input directly into ideal AM
after mono conversion and the channel high/low-pass. It had no program limiting,
transmitter transfer nonlinearity, or source-side noise. That made the receiver
model do all of the ageing while the station feeding it remained mathematically
perfect.

This omission was separate from the former false-whistle bug. That whistle was
a real-RF downmix image leaking into the source envelope of an unmodulated
carrier. It is already rejected in the IF-strip owner and locked by pure-carrier
quiet tests. A natural heterodyne whistle requires a second RF carrier; it must
not be manufactured as a permanent audio oscillator in the program source.

## Acceptance criteria

The remediation is complete only when all of the following hold:

1. The live and calibration paths share one explicit audio-bandwidth contract.
2. The full-chain high -3 dB edge is between 2.5 and 5.5 kHz in the wide/default
   fidelity setting.
3. Speaker reference RMS ratio is between 0.85 and 1.10 without digital,
   speaker, or power clipping at nominal input.
4. IF center, detector time constant, AVC time constant, nominal SINAD, and
   nominal power remain within their existing reference bands.
5. Magnetic tuning measurably reduces offsets across its documented capture
   range and never reinforces the error.
6. Every requested node-physics section runs without stack overflow.
7. Any failed reference row produces a non-zero process exit code.
8. `radio_am_ingress_tests`, `radio_node_physics`, `radio_measurements`, the
   project test suite, and the static Windows build all pass.
9. The untreated cabinet has a 95 Hz resonance and the Type-K clarifiers reduce
   the 95-108 Hz region by approximately 6-10 dB while leaving 500 Hz within
   0.6 dB.
10. The live broadcast source preserves the 110A timing and 2.5:1 compression
    slope, stays near 1% full-level THD, produces approximately 64 dB
    full-modulation signal-to-noise, remains block invariant, and contains no
    coherent whistle.

## Remediation record

### Owner fixes

- The output-transformer solver now restores its configured eight integration
  substeps after each sample. A node test locks this invariant.
- The power stage owns configuration and reset of the electromechanical speaker
  load it solves. Its output to the acoustic speaker stage is an explicit
  voice-coil motor-equivalent signal; the speaker node no longer reads hidden
  upstream state.
- The fitted speaker roll-off was recalibrated as one response: 1.5 dB
  suspension gain, 5 kHz top low-pass, and 12% high-frequency loss blended from
  4 kHz. Reduced-order IF gain changed from 3.0 to 4.6 to restore the missing
  plate-voltage swing at the physical speaker reference, not as post-output
  makeup.
- Preview now consumes the receiver's resolved audio bandwidth and applies one
  program low-pass at that edge; playback and export no longer invent a second
  `0.48`-scaled value. A test verifies approximately -3 dB at the resolved edge.
- Magnetic-tuning capture and correction authority are both 5 kHz. A capture
  gate preserves discriminator authority inside that range and rejects offsets
  outside it.
- The measurement executable returns exit code 2 when any reference row fails;
  node fixtures no longer place the large calibration-pass array on the Windows
  stack; the Tone bypass fixture disables both tone branches; and the transient
  carrier burst now lasts long enough to measure the 75 ms AVC network.
- The former generic cabinet fit is replaced by an 8 dB, 95 Hz untreated
  resonance followed by three identical 108 Hz absorbers. Their combined
  3 dB damping sections give 9.0 dB maximum reduction and flatten the
  response across the patent's 70-150 Hz band. Unsupported 650 and 900 Hz
  cabinet sections were removed. The clarifiers are minimum-phase damping
  sections rather than inverted secondary radiators, so they also shorten the
  resonance tail. The mild grille and open-back paths remain explicitly fitted
  because they depend on cloth, wall, and listener geometry.
- A `BroadcastSource` preview pass now owns retained limiter envelope, gain, and
  deterministic noise state before the existing program-band filter. Its
  defaults place the threshold at 80% AM modulation and reproduce the 110A's
  20 ms insertion, 250 ms removal, and 2.5:1 compression slope. A 0.04 cubic
  term gives about 1% full-level third-harmonic distortion. Broadband noise is
  normalized to 64 dB below a full-modulation sine after the channel filter.
  The raw AM entry point remains ideal so receiver calibration and source
  conditioning stay independently testable.

### Post-fix measurements

Measured with the production preset at 48 kHz, 5.5 kHz requested audio
bandwidth, and noise setting 0.012.

| Metric | Measured | Required | Result |
| --- | ---: | ---: | --- |
| Low -3 dB edge | 150 Hz | 60-200 Hz | pass |
| High -3 dB edge | 2.5 kHz | 2.5-5.5 kHz | pass |
| Detector time constant | 45.16 us | 35-55 us | pass |
| AVC time constant | 75.0 ms | 60-90 ms | pass |
| Nominal SINAD | 37.30 dB | at least 35 dB | pass |
| Speaker reference ratio | 0.866 | 0.85-1.10 | pass |
| Maximum digital output | 0.893 | at most 0.95 | pass |
| IF center | 470 kHz | 469-471 kHz | pass |
| Nominal output power | 14.52 W | 12-18 W | pass |
| Speaker load at 1 kHz | 3.27 ohm | 3-5 ohm | pass |
| Residual after synthetic 4 kHz AFC error | 7.78 Hz | at most 100 Hz | pass |

Nominal power, speaker, output-clip, and final-limiter flags all remained zero.
The longer carrier-burst diagnostic measured non-zero AVC edges of 59.64 ms
rise and 120.57 ms fall; the previous all-zero result was a test-fixture error.

The dedicated cabinet comparison, which removes only the clarifier damping
from the untreated fixture, measured:

| Cabinet metric | Measured | Required | Result |
| --- | ---: | ---: | --- |
| Untreated gain at 95 Hz | +7.92 dB | +6.5 to +9.5 dB | pass |
| Clarifier reduction at 95 Hz | 7.83 dB | 6-9 dB | pass |
| Maximum reduction near 108 Hz | 9.00 dB | 8-10.5 dB | pass |
| Clarifier effect at 500 Hz | 0.21 dB | at most 0.6 dB | pass |
| Treated 70-150 Hz response span | 3.90 dB | at most 4.5 dB | pass |
| Reduction of 95 Hz burst hang-over | 9.27 dB | 6-12 dB | pass |

The dedicated broadcast-source regression measured:

| Broadcast-source metric | Measured | Required | Result |
| --- | ---: | ---: | --- |
| Output for an input 5 dB over threshold | +2.01 dB | +2 dB, within 0.27 dB | pass |
| Full-level cubic THD | 1.031% | 0.9-1.2% | pass |
| Noise below full modulation | 63.994 dB | 63-65 dB | pass |
| Strongest scanned coherent tone / noise RMS | 0.0197 | at most 0.03 | pass |
| Disabled-source sample error | 0 | exactly 0 | pass |
| One-block versus split-block error | 0 | at most 0.00001 | pass |

No oscillator was added to the broadcast source. Its only generated signal is
seeded broadband noise, which then passes through the same 45 Hz high-pass and
requested channel low-pass as the program. Existing IF and full-RF regressions
continue to require an unmodulated carrier to remain quiet through detection.

### Acceptance status

All ten acceptance criteria above pass in the implemented validation paths.
The low-frequency cabinet/clarifier behavior is now tested against a Philco
measurement proxy. The exact full-range 37-116 speaker curve remains explicitly
classified as reconstructed until a calibrated original 36-1219 unit is
measured.
