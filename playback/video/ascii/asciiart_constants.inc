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
RADIOIFY_ASCII_LUMA_U8(kLumMinimumRange, 80)  // minimum selected luma span
RADIOIFY_ASCII_SIGNAL_U8(kLumSmoothAlpha, 40) // previous/current blend alpha
RADIOIFY_ASCII_LUMA_U8(kLumResetDelta, 28)    // snap threshold for luma history
RADIOIFY_ASCII_FLOAT(kHdrReferenceNits, 100.0f)  // SDR target for HDR tonemap
RADIOIFY_ASCII_FLOAT(kHdrScale, 10000.0f / kHdrReferenceNits)  // PQ/HLG nit scale

// Pen color and display model.
RADIOIFY_ASCII_LUMA_U8(kColorLift, 0)         // minimum RGB channel value
RADIOIFY_ASCII_LUMA_U8(kTemporalResetDelta, 48)  // RGB delta sum snap threshold
RADIOIFY_ASCII_SCALE_256(kInkVisibleDotCoverage, 96)   // visible dot area inside its 2x4 sample slot
RADIOIFY_ASCII_COUNT(kInkCoverageMaxScale, 512)  // max fixed 8.8 FG/BG delta expansion

// Edge/detail classification.
RADIOIFY_ASCII_LUMA_U8(kDitherMaxEdge, 28)       // max Sobel magnitude for dither
RADIOIFY_ASCII_LUMA_U8(kEdgeThresholdFloor, 6)   // minimum per-dot luma threshold
RADIOIFY_ASCII_LUMA_U8(kEdgeMaskFitMinRange, 18) // min local luma span for cell-level edge mask fitting
RADIOIFY_ASCII_SIGNAL_U8(kEdgeMaskFitMinSignal, 128)  // min edge/luma signal for structural mask fitting
RADIOIFY_ASCII_SCALE_256(kEdgeMaskFitMinGain, 16)  // min relative reconstruction-error drop, 256 == 100%
RADIOIFY_ASCII_LUMA_U8(kNeighborContinuityMinContrast, 18)  // min FG/BG luma contrast before neighbor continuity may add connector dots
RADIOIFY_ASCII_SCALE_256(kNeighborContinuityMinGain, 96)  // min topology-score improvement before changing a cell mask
RADIOIFY_ASCII_SCALE_256(kNeighborContinuityStrength, 0)  // reward/penalty strength for matching neighbor border/corner dots; 0 keeps dot masks unchanged
RADIOIFY_ASCII_LUMA_U8(kNeighborColorAaMinContrast, 14)  // min FG/BG luma contrast before neighbor display colors can soften a partial-cell BG
RADIOIFY_ASCII_SCALE_256(kNeighborColorAaStrength, 96)  // max BG blend toward neighbor display mean on partial edge cells
RADIOIFY_ASCII_SCALE_256(kNeighborFgColorAaStrength, 96)  // max FG blend toward neighboring ink colors on partial edge cells
RADIOIFY_ASCII_SCALE_256(kPerceptualLumaErrorWeight, 256)  // extra luma reconstruction weight, 256 == 1.0
RADIOIFY_ASCII_SCALE_256(kPerceptualBrightBgPenalty, 512)  // low-contrast bright-BG penalty strength, 256 == 1.0
RADIOIFY_ASCII_LUMA_U8(kPerceptualPreferredBrightBgInkContrast, 36)  // preferred visible ink luma delta before bright BG may win an edge cell
RADIOIFY_ASCII_LUMA_U8(kEdgeBackgroundBlendMinDelta, 18)  // start pulling partial-edge BG away from a single source side
RADIOIFY_ASCII_LUMA_U8(kEdgeBackgroundBlendFullDelta, 96)  // RGB-channel delta where partial-edge BG antialiasing reaches full strength
RADIOIFY_ASCII_SCALE_256(kEdgeBackgroundBlendStrength, 224)  // max BG pull toward cell mean on mixed edge cells, 256 == 100%
RADIOIFY_ASCII_LUMA_U8(kColorBoundarySoftMinDelta, 28)  // start treating related-hue FG/BG deltas as tonal shading
RADIOIFY_ASCII_LUMA_U8(kColorBoundarySoftFullDelta, 112)  // RGB-channel delta where tonal-boundary handling reaches full strength
RADIOIFY_ASCII_SCALE_256(kColorBoundarySoftStrength, 128)  // max FG/BG color pull toward the cell mean, 256 == 100%
RADIOIFY_ASCII_SCALE_256(kColorBoundarySignalAttenuation, 224)  // max edge/detail signal reduction for related-hue tonal boundaries
RADIOIFY_ASCII_SCALE_256(kColorBoundaryHueSimilarity, 112)  // related-hue threshold; 256 == identical chroma direction
