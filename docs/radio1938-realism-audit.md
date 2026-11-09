# Radio1938 realism audit

This document describes the model as it stands. The dated measurements and the
reasoning behind each change, newest first, are in the
[audit log](radio1938-audit-log.md); the speaker and cabinet evidence is in
[period acoustics](radio1938-period-acoustics.md), the detector and supply in
[detector and supply](radio1938-detector-supply.md), and the per-node physics
in [node physics](radio_node_physics.md).

## Current state, 2026-11-09

### Signal chain

Playback and export are stereo. The unfiltered path passes the source
unchanged. With a receiver selected, the source's mono sum (what the
transmitter carried) runs through:

- **Broadcast source.** A peak-envelope compressor after the Western Electric
  110A (20 ms attack, 250 ms recovery, 2.5:1), a transmitter cubic term and
  noise inside the period 62-66 dB range.
- **Reception.** A groundwave path and a slowly rotating, delayed skywave path
  (selective fading), on the IRE standard antenna (4 m effective height).
  Man-made noise after ITU-R P.372; atmospheric noise
  (sferics, P.372 section 7) in `night-skywave`. Profiles: `everyday-1938`
  (default), `strong-local`, `night-skywave`.
- **RF, converter and IF.** Absolute gain from tube transconductance and
  transformer transfer impedance; remote-cutoff gm curves under AVC; coupled
  IF transformers; IF plate-swing limits. IRE sensitivity: 67 uV (38-12) and
  0.66 uV (Philco).
- **Detector and AVC.** A solved thermionic diode detector. The 38-12 has
  simple AVC (2 M / 0.03 uF, 60 ms; it holds about 24 dB over 80 dB of
  carrier); the Philco a delayed, amplified AVC (28 V delay, 0.22 s bus from
  Bulletin 258; within 3.5 dB over 80 dB).
- **Audio.** The volume network (the Philco's tapped control with its loudness
  branch), the first audio triode, the Philco's 6F6 driver and interstage
  transformer, the output stage (one 41 single-ended; two 6B4G push-pull
  class A), the output transformer, a nonlinear electrodynamic speaker (Bl(x),
  progressive suspension, coil heating) and the cabinet (the Philco's 95 Hz
  resonance with three clarifiers; the 38-12's small-cabinet mounting). The
  loaded rectifier supply carries ripple, sag and modulation hum into every
  stage.
- **Listening scene.** A room renderer turns the one speaker into left and
  right (a headphone scene by default).
- **Playback level.** Digital makeup and a look-ahead true-peak limiter at 0.97
  of full scale; neither is part of the receiver.

### Listening level

Both receivers run at the same living-room power, 0.18 W into the voice coil
on the 1 kHz, 85% reference tone (38-12 knob 0.032, Philco 0.0146, below its
loudness tap). The makeup matches programme loudness to the unfiltered stereo
path, so one system volume serves both: over ten dynamic tracks and two 1930s
broadcasts the median EBU R128 difference is 0.0 dB (38-12) and -0.3 dB
(Philco). The limiter acts noticeably only on very loud masters.

### The whole output

Measured on the same tracks and broadcasts at `everyday-1938` (mono sums; the
per-band ratio is the share of the output that is a linear image of the
source, against distortion, noise and time-varying gain):

| | 38-12 | Philco 37-116 |
| --- | ---: | ---: |
| Response, -6 dB | ~200 Hz - 3.4 kHz | ~55 Hz - 5.3 kHz |
| Colour | +3 dB near 2.5 kHz | +8..10 dB at 80-100 Hz, +6 dB at 2.5-3 kHz |
| Linear-to-residual, whole band | 15 dB | 11 dB |
| Linear-to-residual, 400-1000 Hz | 16-21 dB | 19-22 dB |
| Output power below 150 Hz (source 24%) | 0% | 50% |
| Programme to silence | 29 dB | 29 dB |
| Speech transmission index, 1936 broadcast | 0.83 | 0.73 |

### What guards it

The stage tests (`radio_circuit_model_tests`, `radio_node_physics`,
`radio_am_ingress_tests`, the acoustic tests) check each part.
`radio_measurements --section reference` holds per-receiver engineering bands,
including the AVC time constant and the second harmonic of an 80 Hz tone at
the terminals. `radio_measurements --section programme` runs the complete
stereo playback path on a deterministic synthetic programme whose spectrum
matches real music, and checks loudness against the source, the
linear-to-residual ratio in three bands, peaks, limiter load and clip flags.
Most bounds are regression bounds, about 5 dB from the present values; the
log records why each moved.

### Open limits

- **Sub-bass.** 30-50 Hz content in modern recordings drives the Philco's cone
  and transformers into harmonics at 63-100 Hz; those bands stay only 5-7 dB
  linear. 1930s programme carried little such content.
- **Philco bass.** Half of its output lies below 150 Hz: its loudness tap, the
  95 Hz cabinet resonance, the speaker's 80 Hz resonance (Qts about 1.7, a
  prior) and the tone-switch position (undocumented) all add there.
- **38-12 bass** falls steeply, -36 dB at 100 Hz against 1 kHz.
- **Speakers.** No Thiele/Small data survives for either speaker; their
  parameters, travel limits and cone-region sections are labelled priors fitted
  to period family curves.
- **AVC.** One symmetric pole; in the Philco the grid leak (95) makes release
  about 1.5 times slower than charge. That every controlled grid return has its
  own .05 uF is read from a dense schematic.
- **Wide stereo mixes** lose up to 7 dB in the mono sum; no broadcast operator
  rides the level before the transmitter.
- **Listener settings** (reception profile, tone switch, room as headphones or
  speakers, the receiver's own volume) are presets or command-line options;
  the app has no settings window yet.

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

`typical-1930s` stands for the compact, inexpensive AC table set on which a
large share of US listeners heard ordinary network programmes in the late
1930s. It is anchored to the Philco 38-12C (1938 line, introduced June 1937;
$22.50; 91,800 made). It is not a statistical average of every radio in use,
and its sound is not the average sound of the decade.

Table sets were the larger part of the market, not a niche. Radio Retailing's
domestic figures count 3,578,612 table sets against 2,636,872 consoles in
1937, and 3,573,400 against 1,107,300 in 1938; radio-phonographs and auto sets
are counted separately. "Table" also covers sets larger than the 38-12C, so
these figures support the category, not a five-inch speaker as the national
norm. Philco sold the 38-12 as a "second or third" set for the bedroom or
kitchen. That is the maker's sales positioning, not evidence of how buyers used
it; in many homes it may have been the only radio. The profile does not
represent older sets still in use or battery sets on farms, and as a specific
model it fits 1937-1940 rather than the earlier 1930s. The period service and
parts data establish:

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
values, and output topology are documented. The speaker and its mounting are
explicit reduced-order estimates, because no BO-1 Thiele/Small data and no
surviving anechoic or whole-cabinet curve was found for the BO-1/38-12C
combination: a 200 Hz suspension resonance within Knowles's 180-250 Hz
natural-frequency range for a small speaker, smooth cone-region sections
fitted to the K5 small-cabinet example, and a 235 Hz mounting high-pass for
K5's total cabinet-minus-baffle difference. No separate panel resonance or
open-back path is added on top of that total.

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
- [Radio Retailing, January 1939, p. 11, business at a glance](https://www.worldradiohistory.com/Archive-Radio-Retailing/30s/Radio-Retailing-1939-01.pdf)
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
- reception uses one stable groundwave path and one slowly rotating,
  stochastic, delayed skywave path rather than a geographic ionosphere,
  antenna, and multipath field solver; the delay makes the fading frequency
  selective (see "AVC and selective fading, 2026-11-06" in the
  [audit log](radio1938-audit-log.md));
- a rare interferer is synthesized as a second real RF carrier before the
  receiver front end, so any heterodyne is created by the existing physical
  tuning, IF, and envelope-detector path.

### Fitted sound design

- speaker Thiele/Small parameters that are absent from the Philco service data;
- cone breakup, dip, top roll-off, and compliance asymmetry;
- grille and listener-dependent open-back rear-radiation parameters;
- seeded component drift;
- procedural post-detector hiss and supplemental hum levels (crackle is
  now antenna noise of the reception profile);
- deterministic broadband transmitter noise inside the documented period
  output-noise range.
- the representative groundwave/skywave ratio and the intermittent
  co-channel carrier's level, offset, and occurrence envelope.

Fitted values are acceptable when they are labelled as estimates and validated
against an explicit response target. They are not evidence of historical
accuracy by themselves.
