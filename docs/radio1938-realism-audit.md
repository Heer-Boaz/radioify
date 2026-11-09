# Radio1938 realism audit

Initial audit: 2026-07-15. Latest circuit revision: 2026-11-06.

Acoustic update, 2026-11-05: the [period evidence and acoustic fit](radio1938-period-acoustics.md)
supersede the speaker/cabinet estimates, acoustic response targets and output
measurements below. The original electrical anchors and the distinction between
exact-part data, family examples and the Bobb clarifier proxy remain recorded.
The model review below qualifies their interpretation: passing engineering
guards is not evidence that every circuit or fitted tube characteristic is
historically correct.

Detector/supply follow-up, 2026-11-06: [thermionic detector and loaded supply](radio1938-detector-supply.md)
supersedes the effective charge-knee, synthesized supply-ripple, IF voltage-scale,
and AVC normalization descriptions and measurements below. Its circuit data,
remaining approximations and changed acceptance assumptions are recorded separately.

## Verdict

### Absolute gain, impulsive noise and modulation hum, 2026-11-09

**The receivers have absolute gain.** The RF and IF chain was a calibration:
`rfGain` (0.237 / 0.509), an IF `stageGain` (46 / 20) with a
`referenceTransimpedanceOhms`, and the mixer's `rfGridDriveVolts` and
`acLoadResistanceOhms`, chosen to keep the detector at 2.4 V on a 0.12 V
"nominal" carrier. Measured as the IRE/RMA sensitivity (the carrier, 30%
modulated at 400 Hz, that gives the 50 mW standard output into the rated
voice-coil load with the volume control full up; `radio_measurements
--section sensitivity`), the 38-12 needed 12,300 uV and the Philco 5,500 uV.
Period sets needed microvolts: an eight-tube 1937 Wells-Gardner A1 is rated
8 uV on the broadcast band ([Service, December 1937, p. 729](https://www.worldradiohistory.com/Archive-Service-Magazine/30s/Service-1937-12.pdf)),
a ten-tube 1936 Montgomery Ward 7 uV ([Radio-Craft, April 1936](https://www.rfcafe.com/references/radio-craft/ward-10-tube-radio-april-1936-radio-craft.htm)),
and the IRE's "distant-signal" test level is 50 uV
([Standards on Radio Receivers, 2.02](https://www.worldradiohistory.com/Archive-Early-Radio-Assorted/IEE-Staandards-on-Broadcastand-Television-Receivers-1961.pdf)).
Fades were therefore compensated far less than in a real set, whose AVC is
deep in regulation on any usable station.

Every stage now carries its physical gain and no calibration remains:

- Antenna: the EMF is the field times 4 m, the effective height of the IRE
  standard antenna (an open single-wire antenna with its lead-in, which the
  standard dummy antenna represents; same standard, 2.12). `everyday-1938`
  gives 8.7 mV at unit path gain, `strong-local` 40 mV, `night-skywave` 3.3
  mV; the old 0.12 V is the IRE "local-signal" level. The antenna coil's
  step-up to the first grid is a prior, 6 for both sets.
- Philco RF stage: the 6K7's gm (RCA curve at its bias, AVC and screen
  voltage) times the transfer impedance of its RF transformer, 10 kohms
  (prior; a stage gain near 13).
- Converter: its plate current, from the conversion transconductance of the
  tube model (6A7 242 umho at the 38-12's -6 V bias; 6L7 391 umho, RCA rates
  about 350), drives the first IF transformer.
- IF: each later transformer is driven by the plate current of a 78/6K7
  whose gm follows the RCA curve: 1,576 umho for the 38-12's 78 at -2.3 V /
  90 V and 1,351 umho for the Philco's 6K7s at -3 V / 90 V. The cans'
  transfer impedance follows from their existing L, loss and coupling
  priors. Gain now also changes with the bandwidth control, as the cans' Q
  does.

| | 38-12 | Philco 37-116 |
| --- | ---: | ---: |
| IRE sensitivity, before | 12,300 uV | 5,500 uV |
| IRE sensitivity, now | 67 uV | 0.66 uV |

The 38-12, five tubes without an RF stage, sits near the IRE distant-signal
level; the Philco, with an RF stage and two IF stages, is a premium set's
figure, ten times more sensitive than the rated mid-range sets above. Both
depend on the antenna and RF-transformer priors.

**The Philco's AVC delay is the documented one.** With absolute IF volts
the A.V.C. 6J5G's +25 V cathode against the -3 V return (Bulletin 258,
Fig. 2) is used directly: its grid conducts once the last IF plate swings
28 V peak. The plate envelope is the detector input times the last can's
centre-frequency primary/secondary ratio (0.71). The detector now works near
30 V instead of 2.4 V. Audio at the speaker terminals against the antenna
carrier, relative to the everyday-1938 carrier (`--section avc`):

| Antenna carrier | 38-12 | Philco |
| --- | ---: | ---: |
| 0.3 mV | -14.4 dB | -1.7 dB |
| 3 mV | -3.2 dB | -0.5 dB |
| 30 mV | +2.8 dB | +0.5 dB |
| 0.3 V | +6.6 dB | +1.2 dB |
| 3 V | +9.3 dB | +1.8 dB |

Fades are now held as a real set holds them: the 38-12's simple AVC within
about 24 dB over 80 dB of carrier, the Philco's delayed AVC within 3.5 dB.

**Listening setting.** The volume control is a listener setting, kept at the
former room level (0.29 and 0.53 of the rated reference volts): 0.032 on the
38-12 (was 0.085) and 0.084 on the Philco (was 0.54). With the detector near
30 V the Philco's control now sits below its 1 Mohm tap at room level, where
the tap network adds the bass compensation it was designed for. The
reference sweep therefore removes the loudness branch at the same 1 kHz grid
level rather than the same knob position; at the same position the plain
control would overdrive the audio chain.

Two Philco guards changed. Terminal IMD is -37.7 dB (guard -38 to -35):
at a 30 V detector the single 75 ms AVC filter prior leaves a little audio
ripple on the controlled grids (-36.8 dB with ten times the capacitance),
and the loudness tap lifts 400 Hz into the output stage at room level, so
acoustic IMD is -30.5 dB (guard -35 to -28). The guards were engineering
bounds, not Philco data.

Each IF tube's plate is limited to the swing its supply allows (200 V for
the Philco, 80 V for the 38-12's roughly 100 V IF supply; priors), with a
rounded clip that leaves normal full-modulation reception unchanged (checked
within 0.1 dB). It matters only for impulses far above the carrier.

Playback against the unfiltered source, the same eleven clips as before:
the 38-12 is 14-17.5 dB quieter (unchanged within 1 dB), the Philco 5.5-8 dB
(was 7.6-9.9 dB), music peaks at most -8.7 and -3.9 dBFS. Programme loudness
to the noise of a silent carrier: 38-12 32.7 / 44.2 / 26.5 dB and Philco
31.7 / 43.8 / 27.4 dB for everyday / strong-local / night-skywave.

**Crackle is now reception noise.** The former crackle was a programme-
independent stream of RF impulses of fixed size in volts at the receiver
input, scaled by the `noise` weight. With a physical antenna EMF it would
have been 23 dB stronger against the carrier, and it was anchored to
nothing. It is removed (`crackleAmpScale`, `crackleRateScale`, `noiseref`
and `noiseWeightScaleMax` are gone). In its place, atmospheric noise from
lightning (ITU-R P.372, section 7) can be added at the antenna with the
man-made noise:

- Level: the P.372 charts give Fam at 1 MHz per season, time block and
  place. For 2000-2400 local time over the north-eastern United States
  they read about 65-70 dB above kT0b in winter (Fig. 20a), 80-85 dB in
  spring (Fig. 26a) and 80-90 dB in summer (Fig. 32a), against 72.5 dB for
  residential and 67.2 dB for rural man-made noise. The default is the
  spring value, 80 dB, falling 27 dB per decade (the 80 dB grade over
  0.5-2 MHz, Fig. 26b). By day it is far below man-made noise at medium
  frequencies, so only the night profile would carry it.
- Character: a Poisson stream of sferics with log-normal amplitudes, plus a
  Gaussian share of the power for unresolved distant strokes. 3,000 sferics
  per second, a log-amplitude spread of 2 (about 17 dB) and a 10% Gaussian
  share reproduce P.372's median voltage deviation for the same chart, Vdm
  6.2 dB in 200 Hz (Fig. 26c), at 6.3 dB, and 9.2 dB in 10 kHz against the
  9.4 dB of its bandwidth translation (Fig. 40). The receiver's own
  selectivity then sets the impulsiveness it hears.

Tests check the C/N0, the power split between the Gaussian and impulsive
parts, and the sferic rate and amplitude statistics at 44.1 and 96 kHz.
`night-skywave` carries it. Programme loudness to the noise of a silent
carrier there falls from 26.5 to 13.5 dB (38-12) and from 27.4 to 17.5 dB
(Philco), as the 13 dB lower C/N0 predicts; the largest crashes in a minute
peak at -0.5 and -1.7 dBFS. Daytime profiles carry none.

**Overload recovery of the push-pull output.** The first crashes drove the
Philco's output-transformer primary to about 7,700 V and held it there:
the output triodes were evaluated from a table clamped at about 490 V of
plate voltage, so beyond it they became current sources with no plate
resistance. With nothing to damp the transformer, their current drained
the main B+ to -230 V and the stage stayed in a ~40 Hz oscillation. Outside
its table range a triode now uses the Koren equation itself, which holds at
any voltage; the same crash peaks at 433 V on the primary, the B+ dips to
321 V and recovers within milliseconds. Tests check that a triode beyond
its table keeps rising current and conductance, and that both receivers
keep their B+ through eight seconds of night-time crashes (the Philco's
lowest point in a minute is 277 V).

**Modulation hum.** The RF and IF screens take their voltage from the B+,
so its ripple modulates their gm and puts hum on any carrier, even an
unmodulated one. Each stage's gm now follows the supply's main node, scaled
to its screen voltage, through the network that feeds it. In the 38-12 the
6A7 and 78 screens and the IF plates take the B+ line after the field coil
directly (Bulletin 284, schematic); the Philco feeds its RF and IF stages
through the 4000-ohm half of resistor 128 with 2 uF, and its screen bus near
the 6A8G has 20,000 ohms (61) and 8 uF (62) (Bulletin 258; that every RF/IF
screen shares that bus is read from a dense schematic). On an unmodulated
carrier the 120 Hz component at the detector is -52 dB in the 38-12 and
negligible in the Philco (-139 dB). In the 38-12 it partly cancels the
output stage's own ripple hum at the speaker terminals. The converter's own
screen dependence is not modelled.

### AVC and selective fading, 2026-11-06

**The AVC barely regulated.** The AVC line was charged by a separate diode
through 1 Mohm into 500 kohm and a 0.54 V delay. That divider let the line reach
only about 0.3 V at the nominal 0.12 V RMS carrier while the detector stood
at 2.4 V, and the RF and IF stages then reduced their gain linearly with a
hard floor (`max(0.35, 1 - 0.18 x)` and `max(0.20, 1 - 0.18 x)`). From 30 mV
to 3 V at the antenna the audio rose by about 32 dB. Both schematics show
something else:

- 38-12 ([Bulletin 284](https://philcoradio.com/library/download/service%20info/service%20bulletins/Philco%20Service%20Bulletin%20284.pdf),
  parts 9-14): simple, undelayed AVC. The 6A7 and 78 grid returns take the
  DC at the top of the 500 k volume control through 2 Mohm and 0.03 uF (60 ms).
- 37-116 ([Bulletin 258](https://philcoradio.com/library/download/service%20info/service%20bulletins/Philco%20Service%20Bulletin%20258.pdf),
  parts 80, 90-92, 95): the "A.V.C. 6J5G" has its plate grounded, so only its
  grid-cathode diode works. It takes the last IF plate through 110 pF, returns
  through 20 k / 1 M with .01 uF, and its cathode sits at +25 V (Fig. 2): a
  delayed AVC rectifier on the IF, separate from the audio detector. It feeds
  the RF, mixer and both IF grids.

The AVC line is now an RC filter whose source is the detector-load DC
(38-12) or a delayed rectifier on the IF envelope (37-116). The diode,
charge and discharge settings are removed; `avcFromIfEnvelope`,
`avcFilterResistanceOhms`, `avcFilterCapFarads` and `avcDelayBiasVolts`
describe the network.

**Controlled stages follow their tube.** Each AVC-controlled RF or IF stage
scales with its own transconductance, gm(bias - AVC) / gm(bias). The 78 and
6K7 share RCA's "super-control" characteristic
([RCA-78, graph 92S-5248, April 5, 1937](https://frank.pocnet.net/sheets/049/7/78.pdf)):
the -3 V point is RCA's 1450 umho rating, -4 to -22 V a two-exponential fit to
graphical readings within 10%, and the tail below -22 V, where the curve
merges into the axis, is extrapolated to its visible cutoff near -40 V. The
graph's 75/100/125 V screen curves coincide after scaling grid volts by
100/Ec2, so the operating screen voltage scales the grid axis. Operating
points are from the socket-voltage figures: Philco RF 6K7G -3 V / 84 V
screen, both IF 6K7G -3 V / 90 V; 38-12 IF 78 -2.3 V / 90 V. The 38-12 has
no RF amplifier, so its antenna circuit is no longer AVC-controlled. The
converters keep their existing pentode model with the full AVC line.

**Calibration choices.** The model's IF scale is calibrated, not absolute,
so the Philco's documented threshold (+25 V cathode against a -3 V AVC
return, about 28 V of IF plate signal) cannot be transferred. Its delay is
set to 4 V, which keeps the detector at its former 2.4 V working point at
the nominal carrier; that working point sets the diode distortion. The 75 ms
Philco filter remains a prior, because the AVC bus beyond 90-92 could not be
traced unambiguously. The Philco's tapped volume control stays at 25%, since
turning it changes the bass compensation; its nominal playback falls from
0.62 to 0.53 of the rated reference volts (-1.4 dB), and the reference band
is now 0.5-0.8. The 38-12's untapped control moves from 5% to 8.5% so the
app's default playback level stays where it was: a listener setting, not
circuit data.

Audio at the speaker terminals relative to the nominal carrier (1 kHz, 30%):

| Antenna carrier | 38-12 before | 38-12 after | Philco before | Philco after |
| --- | ---: | ---: | ---: | ---: |
| 10 mV | -22.7 dB | -18.7 dB | -22.6 dB | -22.5 dB |
| 30 mV | -11.9 dB | -8.9 dB | -11.6 dB | -11.3 dB |
| 0.3 V | +6.7 dB | +4.2 dB | +6.2 dB | +2.3 dB |
| 1 V | +14.1 dB | +8.3 dB | +20.1 dB | +4.0 dB |

(`radio_measurements --section avc`.) Strong stations are now held, the
Philco more firmly than the table set. Below the Philco's delay the curve is
unchanged, as expected of delayed AVC. At 3 V the Philco output jumps by
about 20 dB both before and after this change; that overload is outside this
revision.

**Simple AVC loads the 38-12 detector.** The 38-12's 2 Mohm AVC resistor
hangs on the top of the volume control. Its 0.03 uF capacitor blocks DC but
is a short at audio frequencies, so the detector sees 500 k for DC and about
400 k in parallel with the first-audio branch for audio. When the modulation
is deeper than that AC/DC ratio allows, the troughs are clipped. The AVC
branch is now part of the detector's affine load (the warm start solves the
DC working point without it). The Philco's separate 6J5G rectifier does not
load its detector, and a delay bias is now accepted only with
`avcFromIfEnvelope`, since simple AVC follows the detector load directly.

Whole-chain 1 kHz SINAD of the 38-12 at the nominal carrier
(`radio_measurements --section sinad --mod-index <m>`):

| Modulation | 30% | 60% | 80% | 90% | 95% |
| --- | ---: | ---: | ---: | ---: | ---: |
| Without AVC load | 27.5 dB | 20.6 dB | 17.8 dB | 16.2 dB | 15.3 dB |
| With AVC load | 27.4 dB | 20.4 dB | 16.6 dB | 14.6 dB | 13.8 dB |

Playback is 0.5 dB quieter. The volume setting is unchanged, because that
loss belongs to the circuit. The Philco results are identical.

**Two documented 38-12 capacitors were missing.** Rider's Volume VIII,
Philco pp. 8-69/8-70 (Model 38-12, Code 121, June 1937) shows part 21, a
0.01 uF capacitor across the output-transformer primary (41 plate to B+), and
part 17, 250 mmf from the 75 plate to ground. Neither was modelled. Part 21
is the fixed top cut of a pentode output stage: against the roughly 9 k
reflected load it rolls the terminal response off from about 2 kHz. The output
transformer now accepts a capacitor on one winding only (that node was
previously required to have one on both), and the first-audio plate has a
`tubePlateShuntCapFarads` setting, solved with the interstage coupling
network. The same page gives the detector filter as 51 k, not 50 k. It also
names the speaker: Model BO-1, part 36-1366, cone and voice coil 36-3981.

Whole-chain acoustic sweep of the 38-12, change from the former model
(`radio_measurements --section sweep`):

| Hz | 1,000 | 2,000 | 2,500 | 3,150 | 4,000 | 5,000 | 6,000 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Change | +0.4 dB | -0.3 dB | -2.2 dB | -5.3 dB | -9.5 dB | -13.7 dB | -17.3 dB |

The terminal -3 dB point moves from 2.5 to 2.0 kHz. Terminal SINAD rises from
18.7 to 19.5 dB and acoustic SINAD from 15.6 to 16.4 dB, because harmonics
above 2 kHz are attenuated. The speaker's acoustic fit to Knowles K5 drives the
voice coil directly, so this electrical cut is not counted twice. The 9 k
primary load remains derived from RCA's 180 V operating point; a 44:1 ratio
quoted from a later replacement listing (about 6.8 k) could not be traced to
its source.

**The Philco's loudness network was wrong.** Bulletin 258 (schematic p. 3,
parts list p. 4) was read at full resolution. The 1 M tap of the 2 M volume
control (102) returns through 70 k (103) to .008 uF (104) and .006 uF (105)
in series; the tone switch grounds the 70 k end, the capacitor junction, or
neither. The model used 490 k in parallel with 110 pF instead, and weakened
that branch with an invented factor that depended on the wiper position
(`blend`). The branch is now a series R-C that is always connected, so its
effect follows from the pot geometry alone. The switch position is
undocumented: the drawn position (70 k + 3.43 nF) is the chosen listening
setting, not a known factory default.

The same page corrects the driver input. The first-audio plate has 110 pF
(97X) to ground. It feeds the 6F6G driver through .05 uF (96) with a 240 k
(108) grid return; the model had 0.02 uF and 100 k without a source, which
cut the bass from about 75 Hz. The grid sits on the wiper of the 500 k
expander potentiometer (106), which is ganged with the I.F. expander and has
110 pF (107) at its far end. Its mechanical law is undocumented, so the
wiper is set at the coupling end (full audio fidelity); only the network's
load remains. There is no capacitor across the output transformer. The
Philco's generic tone EQ (+0.3 dB at 2.1 kHz, 0.9 dB tilt) has no source and
is removed.

The loudness network makes the volume setting part of the tone. At the
former 25% setting playback fell by 12 dB. The listening setting is now 54%,
just above the tap, where the nominal carrier gives the former level
(speaker reference ratio 0.532); the tap network still shapes the response
there. Acoustic sweep relative to 1 kHz (`radio_measurements --section
sweep`):

| Hz | 100 | 200 | 400 | 1,000 | 2,500 | 4,000 | 6,000 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Former model | -6.5 dB | -5.2 dB | -4.1 dB | 0 | +7.6 dB | +2.7 dB | -10.2 dB |
| Without loudness branch | -1.8 dB | -3.9 dB | -3.3 dB | 0 | +8.2 dB | +4.6 dB | -7.1 dB |
| New, 54% | +5.7 dB | 0.0 dB | -1.7 dB | 0 | +7.4 dB | +3.0 dB | -9.0 dB |

The middle row isolates the driver coupling and tone-EQ corrections; the
loudness branch adds the rest. Terminal SINAD is 34.6 dB and acoustic SINAD
30.1 dB (34.6 and 29.5 before). The real compensation depends on where the
knob sits for a given loudness, which depends on the absolute sensitivity
noted below.

The reference bandwidth rows now measure the receiver without the loudness
branch, since they describe its bandwidth rather than a volume-dependent
tone. A new row, `loudness_branch_error_db`, checks the branch in the full
chain against an independent nodal impedance calculation of the tapped pot
(ideal source) at 10% modulation, from 100 Hz to 4 kHz: 0.06 dB, limit 0.3
dB. At 85% modulation the difference reaches 1.1 dB because the detector is
no longer linear, so the check is made at low modulation.

**Remaining limit: sensitivity.** Resolved on 2026-11-09; see "Absolute
gain, impulsive noise and modulation hum" above. The table and the 4 V
delay in this section describe the calibrated gain chain of that date.

**Listening level against the unfiltered source.** Rendered with
`render-radio` (`everyday-1938`, room on), ten loudly mastered music tracks
(-10 to -24 LUFS) came out 15-19 dB (38-12) and 10-13 dB (Philco) quieter in
integrated loudness (EBU R128) than the unfiltered source. The radio output
has a higher peak-to-loudness ratio (15-16 dB against 10-13 dB): the source
masters are limited, the radio band-limits the treble that R128 weights, and
the Philco's loudness network boosts bass that costs headroom but adds
little loudness. Without limiting, the digital makeup gain can only use the
free headroom. It now stops at the first of two limits: the steady 1 kHz
full-modulation reference tone may not exceed 0.95 of full scale
(`max_digital_output`), and no music peak in any of the three reception
profiles may reach full scale. The 38-12 is bounded by the reference tone
(0.60 to 0.73, +1.7 dB; tone 0.94, music peaks at most -7 dBFS); the Philco
by music peaks (0.25 to 0.35, +2.9 dB; music peaks at most -1.2 dBFS, tone
0.49). The remaining loudness gap is about 14.5 dB (38-12) and 8.5 dB
(Philco); closing it would need limiting, which stays off.

**Noise comes from the antenna, set by the reception profile.** The hiss
was a fixed receiver-internal IF noise (`noise` weight x `ifNoiseMix`),
about 26 dB below programme loudness in both receivers and identical for
`strong-local` and `everyday-1938`. At medium frequencies a receiver is
limited by the noise its antenna picks up with the signal, not by its own
tubes, so that noise is now white RF noise added with the carrier at the
antenna. Its density follows from two period-independent ratios, so the
receivers' undocumented absolute sensitivity drops out:

- Carrier field: FCC, *Standards of Good Engineering Practice Concerning
  Standard Broadcast Stations* (1939), Table I: the groundwave needed to
  overcome the usual noise is 10-50 mV/m in city business areas, 2-10 mV/m
  in city residential areas and 0.1-0.5 mV/m in rural areas (winter); towns
  up to 2,500 need 0.5 mV/m (Table II). Site rule: at least 5-10 mV/m over
  the most distant residential section of the station's city.
- Noise field: ITU-R P.372, Table 1, median man-made noise Fam = c - d
  log f(MHz) dB above kT0b (city 76.8, residential 72.5, rural 67.2, quiet
  rural 53.6; d = 27.7, 28.6 for quiet rural), converted to field strength
  with eq. (7), En = Fa + 20 log f(MHz) + 10 log b - 95.5 dB(uV/m).

| Profile | Groundwave | Noise | C/N0 at 1 MHz |
| --- | ---: | --- | ---: |
| `strong-local` | 10 mV/m | residential | 103.0 dB-Hz |
| `everyday-1938` | 2 mV/m (residential limit) | residential | 89.7 dB-Hz |
| `night-skywave` | 0.5 mV/m | rural | 86.7 dB-Hz |

(C/N0 refers to the carrier at unit path gain, i.e. the groundwave field
divided by its path gain.) Programme loudness to the noise of a silent
carrier, R128 integrated, became:

| Receiver | Before | `everyday-1938` | `strong-local` | `night-skywave` |
| --- | ---: | ---: | ---: | ---: |
| 38-12 | ~27 dB | 34 dB | 44 dB | 29 dB |
| Philco | ~26 dB | 31 dB | 44 dB | 26.5 dB |

The Philco hisses more than the 38-12 at the same antenna ratio because its
IF and acoustic response pass more treble. FCC footnote 5 defines noise as
objectionable when it is as disturbing as a co-channel programme 20:1
(26 dB) down; at the residential primary-service limit both receivers stay
above that, as they should. Caveats: the ITU coefficients are 1970s
medians and 1938 man-made noise was probably higher; night-time
atmospheric noise is included since 2026-11-09 (see above). The receiver-internal IF noise is set to zero.
Tests check the generated density against the formula at 44.1, 48 and 96
kHz, and that the audio noise follows the field ratio between
`everyday-1938` and `strong-local` through the whole receiver (within 1.5
dB).

**Warm start.** Every playback start, seek and render began with a thump
(0.38 of full scale in the 38-12, decaying over about 20 ms). The carrier
warmup settled the receiver and then cleared its AVC, AFC and supply loops,
so AVC recharged from full gain; it also warmed on a bare unit-gain carrier
while reception delivers a groundwave plus a skywave phasor, whose delayed
path started from an empty envelope history. The warmup now keeps the
settled loops (it only clears its own observations), runs on the actual
initial complex path gain, and fills the delay history with the carrier.
The first 50 ms are now no louder than the steady noise floor in every
profile.

**Selective fading.** A skywave arrives later than the groundwave. The
reception sample now carries it as a second, delayed copy of the transmitted
waveform with its own complex gain, instead of a multiplier on the whole
waveform. Its RF phase is in the gain; the delay applies to the modulation,
interpolated with a four-point Lagrange filter. One hop reflected near 100 km
at 100-300 km ground range adds 60-125 km of path, 0.2-0.4 ms, drawn per seed
from its own stream so the fade realisation of a seed is unchanged. Sidebands
1/delay apart then fade in turn and independently of the carrier.

- `everyday-1938` keeps its weak skywave (0.10 against 0.92); the delay adds
  at most about 0.9 dB of slow ripple across the audio band.
- New `night-skywave`: the fringe of the groundwave area after dark, both
  paths at 0.60, the skywave amplitude varying about 3 dB (ITU-R P.1321) and
  rotating at the same 10.8-28.8 fades per hour.

Snapshots with equal paths and a 0.3 ms skywave (`--section fading`, 1 kHz):

| Skywave phase | Carrier | 38-12 audio | 38-12 THD | Philco audio | Philco THD |
| --- | ---: | ---: | ---: | ---: | ---: |
| 0 deg | +1.6 dB | 0 dB | -36 dB | 0 dB | -46 dB |
| 90 deg | -1.4 dB | -2.0 dB | -23 dB | -1.5 dB | -21 dB |
| 150 deg | -10.2 dB | -5.9 dB | +1 dB | -7.8 dB | +1 dB |
| 180 deg | null | -1.9 dB | 2nd harmonic only | -4.2 dB | 2nd harmonic only |

As the carrier fades below its sidebands the envelope detector rectifies
them and distortion rises past the fundamental: the impairment the ITU
envelope-detector study cited below describes. `radio_am_ingress_tests`
checks that an antiphase, equal-strength skywave removes the carrier while
sidebands at 2*pi*f*delay = pi add.

### Physical RF front end, 2026-11-06

The former front end placed series-RLC "tanks" and helper bandpasses on the
virtual carrier at a quarter of the sample rate. Their bandwidth was derived
from the audio bandwidth control (about 58 kHz around a 12 kHz carrier, a Q
near 0.2), so they were practically transparent and their L/R values had no
physical meaning. A real filter cannot represent the actual problem: a
medium-wave tuned circuit is roughly 15-27 kHz wide, comparable with the
virtual carrier itself, so its negative-frequency image would distort the
sidebands asymmetrically.

The front end now owns the representation conversion (quadrature downmix and
image-rejection FIR, moved unchanged from the IF strip) and applies the tuned
circuits to the complex envelope. Each is a parallel tuned circuit ganged to
the dial: trimmers align it to `stationFrequencyHz + dialAppliedHz`, the
mechanical knob position. Philco Magnetic Tuning "changes or shifts the
oscillator frequency" ([Rider 1937 via Philco service tip 33](https://philcoradio.com/repairbench/tips/svctip33.htm)),
so AFC pull moves the local oscillator and IF, not the antenna and RF
circuits. Its loaded Q is

`1/QL = 1/Qcoil + w*L/Rshunt`

with envelope half-bandwidth `w/(2*QL)`. `Rshunt` is the reflected antenna,
RF-plate and grid damping, not a discrete resistor. A fixed coil Q therefore
gives a wider passband higher on the dial, as in a real receiver. Specimen
drift changes coil loss, not alignment. The resonance gain `rfGain` is a
declared antenna-to-mixer-grid gain. It keeps the former effective level at
the default 5.5 kHz control position (0.237 / 0.509, measured from the former
discretized tanks). The former coupling of RF gain to the bandwidth control
is removed.

| Prior at 1 MHz (`stationFrequencyHz` default) | Typical set | Philco |
| --- | ---: | ---: |
| Tuned circuits before the converter | antenna | antenna + RF stage |
| Inductance | 230 uH | 230 / 230 uH |
| Unloaded coil Q | 80 | 100 / 100 |
| Reflected shunt load | 100 kohms | 150 / 250 kohms |
| Loaded Q | 37 | 51 / 63 |
| Front-end response at +/-5 kHz | -0.56 dB | -2.47 dB |

These coil and loading values are **priors** for period broadcast coils, not
measured 38-12 or 37-116 windings. The two-circuit console is more selective
in RF than the single-circuit table set; its expanded IF partly offsets that.
The dial position is limited to 530-1710 kHz.

A first-order digital filter cannot follow this response once the half-
bandwidth is a sizeable fraction of the envelope rate: the first version, a
bilinear one-pole prewarped at 5 kHz, was accurate within +/-6 kHz but lost
about 1 dB too much through both Philco circuits at the 8.8 kHz source edge
of 44.1 kHz playback. Each circuit is now a minimum-phase biquad per
quadrature whose squared magnitude is matched to `1/(1 + (f/B)^2)` at DC,
.1, .2, .3 fs and Nyquist (a 5x5 solve and spectral factorisation at
configuration time). Its phase differs from the circuit by an almost
constant sub-sample advance, which delays all sidebands alike.

`radio_circuit_model_tests` sweeps 530/1000/1710 kHz, two inductances, two
coil Qs and two loads at 44.1/48/96 kHz over the widest source band plus
10 kHz of detuning (capped at .3 fs; 13.2, 14.4 and 20 kHz). Against the
narrowband envelope it implements, the maximum error is **0.004 dB**, and
**0.33 dB** up to .45 fs. Against the full parallel RLC impedance (series coil
loss, aligned capacitor, shunt load) it is **0.17 dB**; that remainder is the
real circuit's asymmetry about resonance at 530 kHz and 20 kHz offset, which
the symmetric envelope omits. After removing the common delay the phase
residual is below 0.9 degrees. Both Philco circuits together at the 44.1 kHz
source edge are within 0.075 dB of the full circuit, again the circuit's own
asymmetry. The test also checks that heavier loading and a higher dial
frequency broaden the response, that the circuits follow a 3 kHz detuning,
that centred sidebands stay symmetric, that the console is more selective
than the table set, that AFC pull moves the oscillator but not the RF
circuits, and that a bypassed front end still delivers real RF and baseband
I/Q to the rest of the receiver.

Removed settings: `frontend.inputHpHz`, `selectivityPeakHz/Q/GainDb`,
`antennaCapacitanceFarads`, `antennaSeriesResistanceOhms`,
`antennaLoadResistanceOhms`, the corresponding `rf*` fields and
`tuning.preBwScale/postBwScale` are rejected as unknown settings. New:
`frontend.antennaCoilQ`, `antennaShuntLoadOhms`, `rfCoilQ`, `rfShuntLoadOhms`
and `tuning.stationFrequencyHz`. `processIqBaseband` now enters at the front
end, so baseband input also passes the tuned circuits. When a signal's entry
pass is disabled, the pipeline now starts at the next enabled pass in the
full dependency order (solved over enabled and disabled passes, so disabling a
pass never reorders the rest); before, a disabled entry pass skipped the
entire receiver and returned the raw input. `audio_graph_tests` covers this.

Nominal reference harness (48 kHz, 5 kHz control, .12 V RMS carrier, .012 noise),
before -> after:

| Metric | Typical set | Philco |
| --- | ---: | ---: |
| Terminal SINAD, dB | 20.32 -> 20.34 | 34.82 -> 35.05 |
| Acoustic SINAD, dB | 17.30 -> 17.33 | 29.46 -> 30.18 |
| Speaker voltage / rated reference | .278 -> .273 | .659 -> .623 |

The harness runs at 5 kHz, where the former tanks had slightly more gain
than at the 5.5 kHz playback default; that explains the small level changes.
All 106 CTests pass. The RF section still has no other stations, so image and
adjacent-channel rejection are not exercised beyond impulses and the
co-channel carrier.

### Circuit corrections and remaining limits, 2026-11-06

This section supersedes the November 5 findings below for IF selectivity,
first-audio characteristics, AFC, interference routing and supplemental hum.
The earlier findings and measurements remain as a dated record. The acoustic
fits have not been retuned to conceal the electrical changes.

**The receiver now has coupled IF resonators.** Each transformer contains
primary and secondary envelope-voltage states for both quadratures, with
separate winding losses, inductances, secondary loading and mutual coupling.
For resonant angular frequency `w`, the aligned tank capacitances are
`Cp = 1/(w*w*Lp)` and `Cs = 1/(w*w*Ls)`. In the narrowband approximation the
decay rates are `Rp/(2*Lp)` and `Rs/(2*Ls) + 1/(2*Cs*Rload)`, and coupling rate
is `w*k/2`, with the corresponding inductance-ratio factors between states.
The code integrates this linear system by the trapezoidal rule at four times
the envelope sample rate. Fixed-size double-precision histories avoid both
heap allocation and a nonlinear solver in the IF sample loop.

Changing coupling or loading changes the transfer, including insertion loss.
`referenceTransimpedanceOhms` is a declared input-unit/gain calibration, not a
physical resistor or a normalization recomputed to erase those changes.
Tuning rotates the signal relative to the receiver resonators, while the
transmitter carrier and programme filter remain fixed. The existing bandwidth
control scales the equivalent damping/coupling together relative to its
5.5 kHz position; it is not a measured -3 dB bandwidth shared by both sets.

| Equivalent IF prior at the 5.5 kHz control position | Typical set | Philco |
| --- | ---: | ---: |
| Double-tuned transformers | 2 | 3 |
| Primary / secondary inductance | 220 / 250 uH | 220 / 250 uH |
| Primary / secondary series loss | 5 / 5 ohms | 15 / 15 ohms |
| Secondary shunt load | 200 kohms | 200 kohms |
| Interstage / output coupling | .012 / .012 | .032 / .032 |
| Reference transimpedance | 40 kohms | 17 kohms |

The [Philco 37-116 service material](https://philcoradio.com/library/download/service%20info/service%20bulletins/Philco%20Service%20Bulletin%20258.pdf),
also reproduced in [Rider 1937](https://www.worldradiohistory.com/Archive-Rider/37/Rider-1937-PQR.pdf),
Philco 7-35, names three double-tuned transformers and expanded alignment
peaks at 465 and 475 kHz. The component losses, coupling and insertion gain
above are **equivalent-circuit priors**, not measured Philco winding values
or a recovered factory selectivity curve. The console represents the expanded
setting. Its wider response is therefore deliberately less affected by a
4.5 kHz offset than the small set. The gain/AVC law and IF amplification
remain reduced-order models; this change does not turn them into a full
antenna-to-detector circuit solution. The RF front end is described in the
following section.

`radio_circuit_model_tests` compares the envelope transformer against an
independent complex impedance solution of the full RF L/C/mutual-inductance
circuit, with varied inductance, coupling, load, positive/negative frequency
offsets and sample rates of 44.1, 48 and 96 kHz. Maximum magnitude error is
**0.296 dB** in that grid. Zero coupling transfers no signal; heavier loading
changes insertion loss. At 48 kHz and the 5.5 kHz control position, detuning
4.5 kHz makes the two 4 kHz sideband magnitudes differ by **31.23 dB** for
the typical set and **3.31 dB** for the expanded console. These are numerical
circuit checks, not historical measurements.

**AFC now senses a real received carrier.** Its two discriminator slopes
include the carrier and follow tuning in the source-envelope frame. The
34 Hz setting smooths the error voltage; it no longer incorrectly makes the
discriminator probes 34 Hz wide. Signal gating uses the detector level rather
than delayed AVC, which can be zero on a usable carrier. Discriminator gain
(`afcLoopGain`, default 8) and the 5 kHz pull limit are separate, explicit
control-model priors. The continuous deadband avoids an abrupt target jump.
Tuning advances on a fixed 64-sample clock, so host callback sizes cannot
change acquisition or audio output. Actual RF-carrier tests reduce +/-4 kHz
to about +/-216 Hz after two seconds and +/-500 Hz to about +/-52 Hz. This
proportional loop retains a residual error; it is not a measured Philco AFC
transfer. Normal playback still starts at station centre, without fabricated
random detuning.

**Transmitter and receiver bandwidths are separate.** The programme lowpass
is 10 kHz, limited to .20 times the sample rate when necessary for the virtual
carrier representation. Source anti-image filtering prevents high-frequency
programme content folding into the radio band (for example 20 kHz becoming
4 kHz at 48 kHz sampling). Downmix image rejection remains a separate numerical
filter. Its coefficients are designed at initialization, outside ongoing AFC
retuning. The source filter has 34 samples of delay, about 0.71 ms at 48 kHz;
it does not add a long audio-buffer latency. Source-noise calibration includes
the filter's actual noise bandwidth. A duplicate RF-gain application in the
real-RF path has also been removed.

**First-audio tube shape is independent of the circuit operating point.**
The Koren parameters now come from approximate graphical readings of RCA
characteristic families, retained in the
[transcription](data/radio1938-triode-characteristics.csv) and reproducible
with `python tools/fit_radio_triodes.py` (NumPy required). The script fits
characteristics, then checks separate tabulated points; it does not fit a
fictitious tube to the receiver's desired plate voltage.

- [6J5 RCA sheet](https://frank.pocnet.net/sheets/049/6/6J5.pdf): family graph
  92CM-4771R1, April 21, 1943; separate typical-condition table dated 1950.
- [75 RCA sheet](https://frank.pocnet.net/sheets/049/7/75.pdf), September 2,
  1941, explicitly refers to the
  [6SQ7 characteristics](https://frank.pocnet.net/sheets/049/6/6SQ7.pdf),
  graph 92C-6310, August 13, 1941. The 250 V / -2 V / .8 mA point is also in
  the [75 manual entry](https://frank.pocnet.net/sheets/021/7/75.pdf).

These later manufacturer curves are type-family evidence, not measurements
of a 1938 specimen. Held-out current errors are below 20%; for example the
6J5 90 V / zero-bias current differs by about 15%. Approximate scan readings
and the compact Koren equation do not justify precision tube tolerances.
The circuit then solves its own load line. The inferred bias priors are
-3.7 V for the 6J5 and -.5 V for the 75, giving approximately 89 V / 1.07 mA
and 89 V / .584 mA respectively. They are not directly measured grid biases.
The Philco's documented 51 kohm plate resistor and bypassed 99 kohm feed
resistor are now distinct AC and DC loads. Its printed 90 V service reading
used a 1000 ohms/volt meter and is not an exact unloaded-voltage target.
The feed's DC drop is solved; a full dynamic B+ distribution network remains
outside this revision.

**Interference is applied at its source.** Atmospheric impulses enter ahead
of the RF front end and IF. Their event rate and source amplitude no longer
depend on programme level, AVC, or a synthetic detector-exposure formula.
The receiver determines their filtering and detection. Their absolute rate,
amplitude distribution and unresolved pulse shape remain scenario priors.
Supplemental hiss and hum no longer have programme ducking. The hum contains
120/240/360 Hz rectifier-related terms plus a separate small 60 Hz pickup
term at the default US mains setting. It still passes through the acoustic
transfer; real bass attenuation is not undone to make hum louder. The
existing supply-ripple path already operated at twice mains frequency and
has not been doubled again.

The everyday reception profile now draws carrier-only co-channel offsets
between 0 and 100 Hz, a scenario bounded by two compliant US +/-50 Hz
transmitters. The [FRC annual report](https://docs.fcc.gov/public/attachments/DOC-338477A1.pdf)
records the June 22, 1932 tolerance change. A uniform draw is not an observed
1938 distribution, and foreign or out-of-tolerance stations can differ more.
There is still no second programme source: this remains carrier interference,
not simulated crosstalk from another broadcast.

**Settings and compatibility:** new IF, DC-feed and AFC controls are registered
in the settings schema and validated before changes reach the live receiver.
Invalid settings preserve its complete DSP history. Removed noise settings
`scAttackMs`, `scReleaseMs`, `sidechainMaskRef`, `hissMaskDepth`, `burstMaskDepth`
and `crackleDecayMs` now produce an explicit unknown-setting error. Source
impulses use `crackleAmpScale` and `crackleRateScale`; hum uses `humHzDefault`,
`humAmpScale`, `humMainsPickupMix`, `humSecondHarmonicMix` and
`humThirdHarmonicMix`. The last two are relative to the rectified fundamental
(120 Hz on 60 Hz mains). First-audio `tubeScreenVolts`, `tubePlateCurrentAmps`,
`tubeMutualConductanceSiemens`, `tubePlateKneeVolts` and `tubeGridSoftnessVolts`
are removed rather than accepted as ineffective controls on the new fitted
characteristic. These removals do not apply to similarly named power-stage
settings. Existing UI controls and the headphone room default are preserved.

**Limits that remain:** the acoustic EQ is still constrained by generic
period examples, not a measured BO-1 or Type-W response. The supplemental
hum/noise path is an acoustic proxy, not heater leakage and ripple solved
through every tube and supply node. The detector still has an effective
diode/source-resistance charging law, rather than a validated thermionic
space-charge/contact-potential characteristic. Its `junctionSlopeVolts`
settings are charge-knee rounding parameters, not cathode `kT/e`. Substituting
85 mV into that law was evaluated and rejected: at nominal Philco modulation
it changed acoustic SINAD from about 32.44 to 26.30 dB without establishing
a valid vacuum-diode model. The 28 dB regression guard was retained. Exact
diode I/V evidence, a dynamic B+ network, a second broadcast source and
calibrated receiver/room recordings are still needed for those refinements.

The [before/after sweeps](data/radio1938-circuit-sweeps.csv) and
[IMD results](data/radio1938-circuit-imd.csv) use 48 kHz, the 5 kHz bandwidth
control, .12 V RMS carrier, .85 modulation-index setting and disabled AFC
to isolate detuning. Results are for the acoustic output, not speaker-terminal
voltage. At a 4.5 kHz offset the typical set's 1 kHz gain now falls 10.27 dB;
its largest response-shape change relative to 1 kHz is 26.74 dB over 50--6000 Hz,
versus 1.91 dB before. The console's corresponding shape change is 5.12 dB,
versus 1.97 dB before. Full-chain IMD need not increase monotonically with
detuning: attenuation also reduces downstream tube drive. Use the independently
checked IF sideband asymmetry to assess selectivity rather than treating a
single final-output IMD number as an IF measurement.

The [nominal results](data/radio1938-circuit-nominal.csv) retain the existing
receiver-specific engineering guards. At .12 V RMS carrier, .85 nominal
modulation, 48 kHz, the 5 kHz control position and .012 noise weight, terminal /
acoustic SINAD is **20.16 / 17.12 dB** for the typical set and **37.77 / 32.44 dB**
for the Philco. No nominal power, speaker, output or limiter clipping is
reported. These are synthetic bench results, not measured receiver fidelity.

The triode oversampling comparison now uses the same characteristic, bias
fraction and effective plate supply in the legacy and current implementations.
Its regression comparison covers 0--8 kHz, the radio band: comparing the full
Nyquist band alone incorrectly rewards the legacy resampler for rolling off
the programme itself. Both full-band and radio-band alias figures remain
reported in the [alias results](data/radio1938-circuit-alias.csv). The 32x
reference, default-2x regression margin and 8x improvement guards remain;
the source-image test also retains its -100 dBc requirement.

For performance, three alternating before/after runs use two seconds of stereo
playback at 48 kHz in 256-frame callbacks, with initialization/warmup excluded.
The [raw timings](data/radio1938-circuit-performance.csv) are wall-clock render
costs, **not output latency**. The baseline binaries were saved before this
circuit revision, after the earlier oversampling work. Median milliseconds:

| Receiver | Room | Before | After |
| --- | --- | ---: | ---: |
| Typical | off | 216.328 | 217.743 |
| Typical | on | 225.520 | 225.322 |
| Philco | off | 404.098 | 403.553 |
| Philco | on | 415.843 | 416.886 |

All four differences are below 1% in this run; this is approximate cost parity,
not a guaranteed speedup. The speaker solver records no nonconvergences.
`build.ps1 -Static -Tests` passes **106 CTests plus 15 and 10 Python tests**,
including real-carrier AFC acquisition, reset, callback-size independence,
independent IF impedance checks, source bandwidth/image rejection, characteristic
holdouts, noise-source independence and transactional settings validation.
The Windows build is published to `dist/radioify.exe`.

### Model review and measurement-tool correction, 2026-11-05

A code review identified material limitations not covered by the earlier
passing regression tests. The following distinctions now govern further work:

- **IF selectivity is heuristic.** The two coupling settings, inductances and
  load are collapsed into one second-order complex lowpass with clamped
  bandwidth/Q. There are no independently coupled resonator states. Detuning
  does rotate the envelope through that filter, but the response is too broad
  to stand in for the intended IF transformers. A fresh typical-set measurement
  at 48 kHz, 5 kHz bandwidth and 4.5 kHz detuning changes the 50--6000 Hz response
  by at most 1.45 dB absolute, or 1.91 dB after normalization to each trace's
  1 kHz level. Thus "no effect" is too strong; the inadequate selectivity is a
  valid finding. The source-image FIR introduced in the numerical audit must
  be kept distinct from receiver selectivity when this is corrected. Transmitter
  programme bandwidth must also be separated from receiver bandwidth.
- **First-audio tube parameters mix operating points.** The 6J5 target combines
  90 V plate voltage with -8 V grid bias, 9 mA and 2.6 mA/V. The
  [RCA 6J5 data sheet](https://frank.pocnet.net/sheets/049/6/6J5.pdf), page 1
  (September 1950), assigns the latter three values to 250 V, and gives a
  different 90 V operating point. Fitting a Koren equation to the mixed targets
  does not repair their provenance. The 75 target also needs a full-curve and
  circuit-bias check. A cutoff estimate of -Va/mu alone is not a replacement
  for those curves. These presets have not been retuned by this review.
- **Noise texture and hum are still behavioural.** `hissMask` explicitly
  attenuates both with programme level; impulses use detector-side exposure
  gating. That is not an RF impulse passing through the complete receiver.
  The supplemental tone has mains and second-harmonic terms. Separately, the
  supply-ripple model already generates full-wave ripple at twice mains
  frequency. A hum revision must distinguish heater/pickup hum from rectifier
  ripple instead of simply doubling every existing oscillator.
- **The supply is incomplete, but not merely an output gain multiplier.**
  `processResistorLoadedTriodeStage` scales the load-line supply voltage, and
  the output solver receives a changing plate operating voltage. The waveform
  and sag envelope driving those voltages are behavioural; a coupled rectifier,
  reservoir and distribution network is not present. This does not establish
  quantitatively correct supply-hum rejection in either output topology.
- **Co-channel offsets need a geographical/historical qualification.** The
  [Federal Radio Commission annual report](https://docs.fcc.gov/public/attachments/DOC-338477A1.pdf)
  records a reduction from +/-500 to +/-50 Hz effective June 22, 1932. Two
  compliant US transmitters on one assigned channel can therefore differ by
  at most 100 Hz. That bound does not establish a distribution of typical
  offsets or perfect compliance. The current 320--900 Hz unmodulated interferer
  is not established as representative US reception in 1938; it also omits the
  interfering programme.
- **Acoustic fits remain examples, not validation against the named radios.**
  A large residual EQ coefficient is not itself the complete acoustic response,
  since the motor, output stage and cabinet transfers also contribute. However,
  the current typical-set whole-chain sweep does put 100 Hz 44.56 dB below its
  maximum. No calibrated recording of the named receiver validates that
  extrapolation. Ordinary restoration videos would additionally include source,
  microphone, room and recording processing.

The measurement tool had a separate, reproducible identity error. With
`--radio-settings`, `--preset philco-37-116` selected a TOML overlay but skipped
applying the Philco receiver profile. A minimal overlay containing only the
existing oversampling factor changed speaker-terminal RMS from 6.72715 V to
0.854946 V and removed the second output-tube grid signal: it was the small set.

**Corrected:** built-in preset names now select their receiver before applying
an overlay. `--receiver-profile` explicitly selects the base receiver for an
arbitrarily named TOML preset. Every report prints the resolved receiver name,
first-audio mu, oversampling factor and nominal output power. For example:

```powershell
.\build-ninja-clangcl-static\radio_measurements.exe `
  --receiver-profile philco-37-116 --preset custom `
  --radio-settings tools/testdata/radio_measurements_profiles.toml --section config
```

The minimal-overlay signal measurement now matches the original Philco result
again, including its second output grid. Three CLI regression cases cover
both built-in identities and an explicitly selected Philco with a custom
overlay. `build.ps1 -Static -Tests` passes 105 CTests plus 15 and 10 Python tests.
Playback DSP is unchanged by this tool correction. Earlier Philco measurements
made with partial TOML overrides require rechecking; ordinary `--preset`
measurements without a settings file were not affected by this bug.

The next audio revision should first establish a real coupled-resonator IF
transfer, receiver-specific selectivity and independently checked tube curves/
bias points. RF impulses, physical hum routing and source/reception separation
then have a meaningful transfer to pass through. Calibrated acoustic reference
measurements remain a separate requirement, not something another EQ fit can
manufacture.

### Earlier audit context

The Radio1938 filter is not a simple equalizer-and-noise effect. It contains a
reduced-order AM receiver, detector, control loops, tube stages, transformers,
an electrodynamic speaker load, and a cabinet model. After the remediation in
this audit, the documented engineering validation bands of both receiver
profiles pass. It is a hybrid of circuit simulation, fitted acoustic response
and behavioural approximations. Its numerical verification is stronger than
its verification against actual receiver measurements. The IF approximation
and first-audio tube parameter inconsistencies below prevent a general claim
of high physical fidelity for the complete chain.

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
  selective (see "AVC and selective fading, 2026-11-06");
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
