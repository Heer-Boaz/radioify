// Shared tuning constants for the CPU and GPU ASCII renderers.
//
// Scale legend:
// - BOOL: compile-time feature switch.
// - FLOAT: unitless floating-point parameter.
// - COUNT: integer count, usually braille dots or samples.
// - LUMA_U8: 0..255 luminance/RGB byte scale.
// - SIGNAL_U8: 0..255 CPU signal scale; HLSL exposes it as 0..1.
// - SCALE_256: fixed-point scale where 256 means 1.0; HLSL exposes it as
//   0..1 except for values intentionally above 1.0.
//
// Only put values here when both renderers share the same meaning. Renderer-
// local constants belong next to the code that owns the local algorithm.

// Coverage: converts luma difference to braille dot coverage.
RADIOIFY_ASCII_BOOL(kInkUseBright, true)       // true: brighter signal -> more dots
RADIOIFY_ASCII_FLOAT(kInkGamma, 0.65f)         // exponent over normalized luma
RADIOIFY_ASCII_FLOAT(kCoverageGain, 1.30f)     // post-gamma multiplier
RADIOIFY_ASCII_FLOAT(kCoverageBias, 0.0f)      // post-gamma additive bias
RADIOIFY_ASCII_FLOAT(kCoverageZeroCutoff, 0.03f)  // normalized coverage floor

// Dynamic luma range selection and temporal smoothing.
RADIOIFY_ASCII_FLOAT(kLumLowPercent, 0.01f)   // histogram low percentile
RADIOIFY_ASCII_FLOAT(kLumHighPercent, 0.99f)  // histogram high percentile
RADIOIFY_ASCII_SIGNAL_U8(kLumSmoothAlpha, 40) // previous/current blend alpha
RADIOIFY_ASCII_LUMA_U8(kLumResetDelta, 28)    // snap threshold for luma history
RADIOIFY_ASCII_FLOAT(kHdrReferenceNits, 100.0f)  // SDR target for HDR tonemap
RADIOIFY_ASCII_FLOAT(kHdrScale, 10000.0f / kHdrReferenceNits)  // PQ/HLG nit scale

// Pen color correction.
RADIOIFY_ASCII_LUMA_U8(kColorLift, 0)         // minimum RGB channel value
RADIOIFY_ASCII_LUMA_U8(kInkMinLuma, 40)       // minimum foreground luma
RADIOIFY_ASCII_LUMA_U8(kBgMinLuma, 10)        // minimum background luma
RADIOIFY_ASCII_COUNT(kInkMaxScale, 1280)      // fixed 8.8 maximum ink lift
RADIOIFY_ASCII_SIGNAL_U8(kSignalStrengthFloor, 51)  // minimum FG/BG contrast mix
RADIOIFY_ASCII_COUNT(kColorSaturation, 340)   // saturation scale, 256 == 1.0
RADIOIFY_ASCII_LUMA_U8(kTemporalResetDelta, 48)  // RGB delta sum snap threshold
RADIOIFY_ASCII_COUNT(kBgDominanceMaxDots, 4)  // only mixed cells where BG covers at least half the dot slots
RADIOIFY_ASCII_SIGNAL_U8(kBgDominanceMinSignal, 128)  // minimum edge/luma signal before compensating BG dominance
RADIOIFY_ASCII_LUMA_U8(kBgDominanceMaxGroupRange, 24)  // max luma range inside ink/BG groups for coherent edge cells
RADIOIFY_ASCII_LUMA_U8(kBgDominanceMinInkLuma, 20)  // darkest allowed ink after BG-dominance contrast
RADIOIFY_ASCII_LUMA_U8(kBgDominanceMinBgLuma, 44)  // final BG luma needed before it can visually overpower ink
RADIOIFY_ASCII_LUMA_U8(kBgDominanceMinContrast, 32)  // minimum perceived final ink/BG contrast
RADIOIFY_ASCII_LUMA_U8(kBgDominanceCoverageBonus, 40)  // extra contrast for sparse ink against dominant BG
RADIOIFY_ASCII_SCALE_256(kBgDominanceBrightLeadScale, 48)  // extra contrast from BG luma lead
RADIOIFY_ASCII_COUNT(kBgDominanceChromaDivisor, 6)  // RGB delta divisor added to perceived contrast
RADIOIFY_ASCII_LUMA_U8(kBgDominanceMaxContrast, 80)  // cap for required perceived final contrast
RADIOIFY_ASCII_LUMA_U8(kBgDominanceMaxInkDarken, 40)  // max final per-cell ink darkening
RADIOIFY_ASCII_LUMA_U8(kBgDominanceMaxBgDim, 12)  // max final BG dimming when BG still overpowers ink

// Edge/detail classification.
RADIOIFY_ASCII_LUMA_U8(kDitherMaxEdge, 28)       // max Sobel magnitude for dither
RADIOIFY_ASCII_LUMA_U8(kEdgeThresholdFloor, 6)   // minimum per-dot luma threshold
RADIOIFY_ASCII_LUMA_U8(kBrightBgSwapDelta, 12)   // BG luma lead needed to swap
RADIOIFY_ASCII_COUNT(kBrightBgSwapMaxDots, 4)    // bright-feature dot count limit
RADIOIFY_ASCII_SIGNAL_U8(kBrightBgSwapMinSignal, 128)  // signal needed to swap

// Shadow chroma protection.
RADIOIFY_ASCII_LUMA_U8(kShadowSatStartLuma, 16)
RADIOIFY_ASCII_LUMA_U8(kShadowSatFullLuma, 96)
RADIOIFY_ASCII_SCALE_256(kShadowMinSaturation, 24)
RADIOIFY_ASCII_LUMA_U8(kShadowBlueGuardStartLuma, 44)
RADIOIFY_ASCII_LUMA_U8(kShadowBlueGuardRange, 36)
RADIOIFY_ASCII_SIGNAL_U8(kShadowBlueDominanceStart, 15)
RADIOIFY_ASCII_SIGNAL_U8(kShadowBlueDominanceFull, 51)
RADIOIFY_ASCII_SCALE_256(kShadowBlueGuardKeep, 115)
RADIOIFY_ASCII_SIGNAL_U8(kSourceBlueSignalStart, 8)
RADIOIFY_ASCII_SIGNAL_U8(kSourceBlueSignalFull, 46)
RADIOIFY_ASCII_SIGNAL_U8(kSourceChromaSignalStart, 5)
RADIOIFY_ASCII_SIGNAL_U8(kSourceChromaSignalFull, 36)
RADIOIFY_ASCII_SCALE_256(kSourceBlueGuardRelax, 77)
RADIOIFY_ASCII_SCALE_256(kSourceBlueInkBoost, 26)
