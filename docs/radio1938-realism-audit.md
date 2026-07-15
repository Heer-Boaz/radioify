# Radio1938 realism audit

Date: 2026-07-15

## Verdict

The Radio1938 filter is not a simple equalizer-and-noise effect. It contains a
reduced-order AM receiver, detector, control loops, tube stages, transformers,
an electrodynamic speaker load, and a cabinet model. After the remediation in
this audit, the documented electrical anchors and explicit validation bands of
both receiver profiles pass. It is a technically credible reduced-order
period-radio simulation.

Neither profile is an exact, measurement-identical reproduction. No calibrated
response or Thiele/Small measurement was located for either original speaker.
For the 37-116 there is nevertheless strong acoustic evidence: Philco's own
clarifier patent contains a measured whole-cabinet response with and without
the absorbers. The high-end cabinet boom and clarifier response are constrained
by that trace. The smaller receiver's speaker and cabinet curves remain clearly
labelled reduced-order inferences from its documented construction and size.
The defensible claim is "historically anchored and physically modelled," not
"measurement-identical."

Reception is now modelled separately from the receiver and is deliberately
described as a representative condition, not an attribute every radio had in
1938. The default `everyday-1938` profile adds conservative medium-wave
propagation and rare co-channel interference. `strong-local` retains the
former ideal received carrier and is also historically plausible when a strong
local groundwave station dominates. Neither profile turns one recording into a
unique reconstruction of one place, station, antenna, weather condition, and
time of day.

## Live radio is not an archival recording

The familiar cultural shorthand for "old radio" often combines three separate
signal histories: the original live broadcast/receiver chain, later damage or
generation loss in a disc, optical soundtrack, tape, or transfer, and
deliberate film/game exaggeration that makes the period instantly legible. Crackle,
extreme bandwidth loss, wow, and heavy distortion in a surviving recording are
therefore not automatically properties of the radio that originally played the
broadcast.

Radioify models the live chain. It does not add a half-degraded archive medium
after the cabinet, and it does not intentionally caricature age. A good 1938
console receiving a strong local station may consequently sound much better
than a period clip copied through several later media. The subtle
`everyday-1938` reception profile is consistent with that distinction.

## Physical receiver profiles

Playback starts with the radio filter off. The `R` control cycles through three
states in a fixed order: unfiltered audio, `typical-1930s`,
`philco-37-116`, then unfiltered audio again. Receiver choice remains separate
from the default `everyday-1938` reception environment.

`typical-1930s` is not claimed to be a statistical average of every radio sold
during the decade. It is a representative mass-market reconstruction anchored
to the Philco 38-12C, a modest second-or-third household set. The period service
and parts data establish:

- five tubes and a 470 kHz IF, without a separate RF-amplifier tube;
- one type 41 output pentode rather than a push-pull pair;
- 2 W rated audio output;
- a BO-1 five-inch field-coil speaker with a 3.5-ohm voice-coil impedance;
- 450 ohms DC resistance in the output-transformer primary;
- a 500 kohm volume control, 4 Mohm first-audio grid leak, 190 kohm plate load,
  and 0.01 uF audio coupling capacitors.

The RCA 1937 tube manual brackets that receiver rating with published
single-ended type 41 operating points: 1.5 W into 9 kohm at 180 V and 3.4 W
into 7.6 kohm at 250 V, both specified at 10% total harmonic distortion. The
model therefore derives approximately 2 W maximum output but calibrates normal
programme listening well below that distortion-rated limit. Post-reference
digital makeup restores listening level without pretending the physical output
stage is continuously delivering its maximum power.

The concrete receiver dimensions, five-inch cone, electrical load, circuit
values, and output topology are documented. The 115 Hz speaker suspension
proxy, cone breakup, broad 155 Hz cabinet panel rise, and short open-back path
are explicit reduced-order estimates because no surviving anechoic or
whole-cabinet curve was found for the BO-1/38-12C combination.

## Philco 37-116 historical anchors

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

Modern ITU medium-wave propagation guidance supplies environmental rather than
receiver-specific anchors:

- groundwave may be treated as non-fading over these short listening periods;
- an individual skywave mode varies, with approximately 3 dB typical
  within-hour standard deviation and 10-30 fades per hour;
- a groundwave-plus-skywave composite is milder than skywave alone: the ITU's
  example with skywave 6 dB below groundwave has approximately 0.72 dB
  composite standard deviation;
- selective fading is a genuine envelope-detector impairment, while an
  audible heterodyne requires another RF carrier rather than a tone inserted
  after detection.

Sources:

- [Philco Service Bulletin 258](https://philcoradio.com/library/download/service%20info/service%20bulletins/Philco%20Service%20Bulletin%20258.pdf)
- [Philco Service Bulletin 284, Model 38-12](https://philcoradio.com/library/download/service%20info/service%20bulletins/Philco%20Service%20Bulletin%20284.pdf)
- [Philco 1937 Parts Catalog](https://philcoradio.com/library/download/parts/catalogs/1937/Philco%20Parts%20Catalog%201937.pdf)
- [Philco speaker table](https://philcoradio.com/library/index.php/parts/speakers/)
- [Philco volume-control table](https://philcoradio.com/library/index.php/parts/volume-controls/)
- [Philco 1938 Radio Gallery](https://philcoradio.com/gallery2/1938a/)
- [RCA Receiving Tube Manual RC-13, 1937](https://www.worldradiohistory.com/Archive-Catalogs/RCA/RCA-Receiving-Tube-Manual-1937.pdf)
- [Philco acoustic-clarifier patent US2059929A](https://patents.google.com/patent/US2059929A/en)
- [Philco Model 116 instructions](https://philcoradio.com/library/wp-content/uploads/2019/12/Philco-116X-Instructions.pdf)
- [Radio-Craft, January 1937 service data](https://www.rsp-italy.it/Electronics/Magazines/Radio-Craft/_contents/Radio-Craft%201937%2001.pdf)
- [Western Electric 630A microphone bulletin](https://www.worldradiohistory.com/Archive-Catalogs/Western-Electric/WE-630A_Mic_promo.pdf)
- [Western Electric 110A program-amplifier bulletin](https://www.worldradiohistory.com/Archive-Catalogs/Western-Electric/Western-Electric-110A-Program-Amp-1937.pdf)
- [RCA Broadcast News, July 1938, WBNS 5-D installation](https://www.worldradiohistory.com/Archive-All-BC-Engineering/RCA-Broadcast-News/RCA-28.pdf)
- [ITU-R P.1321, propagation factors affecting systems using digital modulation below 30 MHz](https://www.itu.int/dms_pubrec/itu-r/rec/p/R-REC-P.1321-0-199708-S%21%21PDF-E.pdf)
- [ITU-R BS.2482, digital sound broadcasting in the LF/MF bands](https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BS.2482-2020-PDF-E.pdf)
- [ITU envelope-detector and selective-fading study](https://search.itu.int/history/HistoryDigitalCollectionDocLibrary/4.279.43.en.1010.pdf)

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
- reception uses one stable groundwave phasor and one slowly rotating,
  stochastic skywave phasor rather than a geographic ionosphere, antenna, and
  multipath field solver; the current weak-skywave profile applies flat
  composite fading and does not claim to reproduce frequency-selective
  sideband fading;
- a rare interferer is synthesized as a second real RF carrier before the
  receiver front end, so any heterodyne is created by the existing physical
  tuning, IF, and envelope-detector path.

### Fitted sound design

- speaker Thiele/Small parameters that are absent from the Philco service data;
- cone breakup, dip, top roll-off, and compliance asymmetry;
- grille and listener-dependent open-back rear-radiation parameters;
- seeded component drift;
- procedural hiss, crackle, and hum levels;
- deterministic broadband transmitter noise inside the documented period
  output-noise range.
- the representative groundwave/skywave ratio and the intermittent
  co-channel carrier's level, offset, and occurrence envelope.

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

### 7. The reception environment is unrealistically perfect

Even after source conditioning, the preview still delivered one perfectly
stable AM carrier directly to the antenna input. That was a credible strong
local-station case, but too clean as the only listening condition. There was no
groundwave/skywave vector combination, slow propagation fading, or real
co-channel carrier capable of producing an occasional detector heterodyne.
Adding those effects after the detector would have repeated the former false
whistle problem, so reception needed its own RF-domain owner before the
receiver pipeline.

## Acceptance criteria

The remediation is complete only when all of the following hold:

1. The live and calibration paths share one explicit audio-bandwidth contract.
2. The 37-116 full-chain high -3 dB edge is between 2.5 and 5.5 kHz.
3. Its speaker reference RMS ratio is between 0.85 and 1.10 without digital,
   speaker, or power clipping at nominal input.
4. IF center, detector time constant, AVC time constant, nominal SINAD, and
   nominal power remain within each receiver's explicit reference bands.
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
11. `everyday-1938` is the default for playback and rendered output, combines
    the desired carrier as coherent groundwave and skywave RF paths, and
    remains independent of audio block boundaries.
12. `strong-local` is exactly transparent to ideal AM ingress, and no profile
    adds a permanent audio or RF whistle; the optional interferer has an
    explicit quiet interval and enters only as a second RF carrier.
13. `typical-1930s` uses a single-ended output topology, no separate RF stage or
    magnetic tuning, approximately 2 W nominal output, and profile-specific
    reference bands rather than inheriting the 37-116's high-fidelity targets.
14. Playback starts unfiltered and cycles unfiltered -> typical -> Philco ->
    unfiltered. Receiver replacement fades to dry, switches the active
    pre-initialized chain at the dry boundary, and fades in so a live model
    change cannot introduce a hard discontinuity.

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
- A following `ReceptionEnvironment` pass now owns propagation state. Its
  `everyday-1938` default combines a 0.92 groundwave phasor with a nominal 0.10
  skywave phasor. The skywave rotates at 0.003-0.008 Hz (10.8-28.8 fades per
  hour), with slow amplitude and Doppler wander. This deliberately produces a
  mild approximately 1.9 dB nominal peak-to-trough carrier span rather than
  theatrical drop-outs. A second -50 to -42 dBc carrier can fade in only after
  a 60-180 second event-free wait, remains for 4-10 seconds, and is 320-900 Hz
  from the desired carrier. Its 750 ms attack and 1.8 s release avoid a switched
  tone. It is injected as real RF before all receiver stages.
- The selectable `strong-local` profile disables that reception sidecar. The
  preview then calls the original ideal AM path directly; this is an explicit
  contract, not a set of neutral multipliers hidden in the hot loop. Playback,
  export, and CLI option ownership all resolve the same profile factory.
- A second physical receiver factory now owns the `typical-1930s` model. It
  removes the 37-116's RF stage, magnetic tuning, interstage transformer,
  push-pull pair, large Type-W speaker, and clarifiers. Its single 41 pentode
  drives a 9 kohm-to-3.5 ohm transformer model with the documented 450 ohm
  primary resistance, followed by a distinct five-inch speaker and compact
  open-back cabinet reconstruction.
- Front-end and IF nodes now configure their tuned networks directly during
  lifecycle initialization. Runtime revision checks remain only for actual
  retuning; profile changes no longer rely on forcing a synthetic cache miss.
- `RadioPlaybackFilter` is the single playback owner for requested mode,
  receiver lifecycle, reset generations, transition state, and scratch memory.
  It initializes the typical and Philco chains once before playback. The
  dedicated radio-DSP worker is the only thread that processes those chains.
  Decoders publish raw frames to an SPSC handoff ring; the worker publishes
  processed frames to a second SPSC ring; and the hardware callback only drains
  that processed output. PTS anchors travel through matching lock-free SPSC
  timelines, so the callback neither runs DSP nor waits on a producer mutex. The
  worker-side state machine fades the active receiver to dry, selects and resets
  the already-initialized destination chain, and fades it in. Seek and frame-step
  serial changes reset both rings and their PTS timelines at that same worker
  boundary before playback is re-primed. There are
  no global receiver templates, shadow active-profile fields, profile copies,
  settings reads, receiver `init()` calls in steady state, or decoder-side DSP
  paths.

### Post-fix measurements

Measured with the Philco 37-116 profile at 48 kHz, 5 kHz requested audio
bandwidth, and noise setting 0.012.

| Metric | Measured | Required | Result |
| --- | ---: | ---: | --- |
| Low -3 dB edge | 150 Hz | 60-200 Hz | pass |
| High -3 dB edge | 2.5 kHz | 2.5-5.5 kHz | pass |
| Detector time constant | 45.16 us | 35-55 us | pass |
| AVC time constant | 75.0 ms | 60-90 ms | pass |
| Nominal SINAD | 37.11 dB | at least 35 dB | pass |
| Speaker reference ratio | 0.889 | 0.85-1.10 | pass |
| Maximum digital output | 0.910 | at most 0.95 | pass |
| IF center | 470 kHz | 469-471 kHz | pass |
| Nominal output power | 14.52 W | 12-18 W | pass |
| Speaker load at 1 kHz | 3.27 ohm | 3-5 ohm | pass |
| Residual after synthetic 4 kHz AFC error | 7.78 Hz | at most 100 Hz | pass |

Nominal power, speaker, output-clip, and final-limiter flags all remained zero.
The longer carrier-burst diagnostic measured non-zero AVC edges of 59.64 ms
rise and 120.57 ms fall; the previous all-zero result was a test-fixture error.

The representative mass-market profile, measured with the same 48 kHz harness
at 5 kHz requested audio bandwidth, produced:

| Metric | Measured | Profile band | Result |
| --- | ---: | ---: | --- |
| Low -3 dB edge | 150 Hz | 100-250 Hz | pass |
| High -3 dB edge | 2.5 kHz | 2.2-3.5 kHz | pass |
| Intermodulation distortion | -25.51 dB | at most -25 dB | pass |
| Detector time constant | 41.18 us | 35-55 us | pass |
| AVC time constant | 75.0 ms | 60-90 ms | pass |
| Nominal SINAD | 21.20 dB | at least 20 dB | pass |
| Speaker reference ratio | 0.315 | 0.18-0.40 | pass |
| Maximum digital output | 0.903 | at most 0.95 | pass |
| IF center | 470 kHz | 469-471 kHz | pass |
| Nominal output power | 2.083 W | 1.5-2.5 W | pass |

Power, speaker, output-clip, and final-limiter flags all remained zero for this
profile as well.

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

The dedicated reception regressions measured:

| Reception metric | Measured | Required | Result |
| --- | ---: | ---: | --- |
| Accelerated coherent two-path minimum | 0.820 | 0.817-0.823 | pass |
| Accelerated coherent two-path maximum | 1.020 | 1.017-1.023 | pass |
| `strong-local` identity error | 0 | exactly 0 | pass |
| One-block versus split-block reception error | 0 | at most 0.00001 | pass |
| Default first-second unwanted carrier samples | 0 | exactly 0 | pass |
| Accelerated intermittent fixture | quiet and active frames | both required | pass |

The 0.5 Hz accelerated fixture tests vector-combination geometry without a
multi-minute test runtime; production keeps the ITU-scale 0.003-0.008 Hz rate.
The interferer is deterministic for repeatable exports, but its presence is
not continuous and it is never represented as a program-band oscillator.

### Acceptance status

All twelve acceptance criteria above pass in the implemented validation paths.
The low-frequency cabinet/clarifier behavior is now tested against a Philco
measurement proxy. The exact full-range 37-116 speaker curve remains explicitly
classified as reconstructed until a calibrated original 36-1219 unit is
measured.
