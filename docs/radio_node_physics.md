# Radio Node Physics Anchors

This repository now has a throwaway validation harness in [radio_node_physics.cpp](/mnt/b/radioify/radio_node_physics.cpp). It does not score "sound quality". It checks whether each runtime node still behaves like the reduced-order analogue block it claims to represent.

The historical anchors for the node semantics are:

- Philco Service Bulletin 258, Model 37-116 Codes 121-122:
  - IF center `470 K.C.`
  - undistorted output `15 Watts`
  - actual detector / AVC / first-audio topology:
    - `2nd DET. 6J5G`
    - separate `A.V.C. 6J5G`
    - separate magnetic-tuning discriminator `6H6G`
    - `2.0 meg total` volume control tapped at `1.0 meg`
    - `0.01 mfd` first-audio coupling capacitor
    - `110 mmfd` loudness / compensation branch in the Philco family notes
      should be treated as provisional until it is pinned to the exact 37-116
      branch in the chassis schematic
    `https://philcoradio.com/library/download/service%20info/service%20bulletins/Philco%20Service%20Bulletin%20258.pdf`
- RCA Receiving Tube Manual, 1942:
  - detection / AVC / duplex-diode triode / tapped volume control:
    `https://www.njarc.org/manuals/RCA%20Tube%20Manuals/RCA-RCVG_MANUAL_1942.pdf`
  - frequency conversion / superheterodyne IF chain:
    `https://www.njarc.org/manuals/RCA%20Tube%20Manuals/RCA-RCVG_MANUAL_1942.pdf`
  - output-coupling devices / transformer-coupled output stage / loudspeaker load:
    `https://www.njarc.org/manuals/RCA%20Tube%20Manuals/RCA-RCVG_MANUAL_1942.pdf`
- Philco Library, Notes on Model 116:
  - detector / AVC split
  - tapped volume-control behaviour
  - first-audio coupling path
    `https://philcoradio.com/library/index.php/shop-talk/model-notes/notes-on-model-116/`

## Node Map

`Tuning`
- Publishes a bounded tuning offset and an audio-sideband bandwidth target.
- Physics check: carrier frequency stays finite and published bandwidth stays inside the allowed service window.

`Input`
- Represents the source impedance, receiver input impedance, and optional coupling capacitor.
- Physics check: resistor divider matches theory; if a coupling capacitor exists it rejects DC, otherwise the path remains direct-coupled.

`AVC`
- Represents the normalized control voltage derived from the detector AVC branch.
- Physics check: control voltage clamps to the modeled 0..1.25 range.

`AFC`
- Represents magnetic tuning correction that opposes tuning error only when capture and signal conditions allow it.
- Physics check: correction sign opposes detector error.

`ControlBus`
- Represents normalized supply sag / control state shared by downstream nodes.
- Physics check: sag mapping stays bounded 0..1.

`InterferenceDerived`
- Represents pre-detector and post-detector noise/crackle exposure as a function of gain and mistune.
- Physics check: zero configured noise produces zero derived interference.

`FrontEnd`
- Converts real virtual-carrier RF to the source envelope, then applies the ganged antenna (and, for the Philco, RF-stage) tuned circuits at the mechanical dial frequency (AFC pull moves only the oscillator), followed by the declared resonance gain and AVC. Each circuit is a magnitude-matched minimum-phase biquad on the narrowband envelope response.
- Physics check: off-resonance response matches the circuits' loaded-Q magnitude; AVC reduces gain.

`Mixer`
- Represents frequency conversion gain from the tube operating point, not a sound-design saturator.
- Physics check: conversion gain is finite; AVC reduces effective conversion gain.

`IFStrip`
- Represents a reduced-order loaded IF can acting on the complex envelope.
- Physics check: tuned detector feed exceeds a strongly detuned detector feed.

`AMDetector`
- Represents the reduced-order detector / AVC core, including the physical detector storage node that feeds the receiver load and AFC sense probes.
- Physics check: detector node never goes negative; stronger carrier charges more AVC.

`DetectorAudio`
- Exposes the physical detector-audio node to the explicit receiver input network. It must not become a second hidden RC or cleanup stage behind the detector.
- Physics check: exposed detector audio stays non-negative and follows the detector-node physics rather than inventing a separate post-detector low-pass.

`ReceiverInputNetwork`
- Represents the explicit volume control, loudness tap, coupling capacitor, and first-audio grid leak.
- Physics check: detector load conductance is finite positive; DC is blocked from the grid; tap voltage remains inside the detector-to-ground range.

`ReceiverCircuit`
- Represents the first-audio triode voltage amplifier.
- Physics check: a more positive grid lowers plate voltage.

`Tone`
- Represents passive post-first-audio tone shaping.
- Physics check: zero input stays zero.

`Power`
- Represents the driver triode, interstage transformer, push-pull output pair, and output transformer.
- Physics check: derived nominal output reference is finite positive; zero drive produces near-zero secondary; output grids remain anti-phase.

`Noise`
- Represents additive hiss/crackle/hum injection only when the derived noise inputs request it.
- Physics check: zero derived noise is exact passthrough.

`Speaker`
- Represents the bounded reduced-order loudspeaker model.
- Physics check: zero input stays zero.

`Cabinet`
- Represents passive cabinet coloration and rear-path cancellation.
- Physics check: zero input stays zero.

`OutputScale`
- Represents physical speaker-volts to digital scaling.
- Physics check: the modeled reference speaker voltage maps to the expected digital headroom value.

`FinalLimiter`
- Represents post-physical digital safety limiting only.
- Physics check: large digital input causes gain reduction instead of physical-model changes.

`OutputClip`
- Represents the final digital clip safety boundary only.
- Physics check: samples above threshold are marked and clamped.
