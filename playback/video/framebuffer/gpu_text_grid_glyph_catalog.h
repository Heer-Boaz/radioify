#pragma once

#include <cstdint>

namespace playback_gpu_text_grid {

inline constexpr std::uint32_t kAtlasColumns = 16;
inline constexpr std::uint32_t kAtlasRows = 24;
inline constexpr std::uint32_t kAtlasGlyphCount =
    kAtlasColumns * kAtlasRows;
inline constexpr std::uint32_t kBlockGlyphAtlasStart = 111;
inline constexpr std::uint32_t kBrailleGlyphAtlasStart = 119;
inline constexpr std::uint32_t kUiGlyphAtlasStart = 375;
inline constexpr std::uint32_t kMissingGlyphAtlasIndex = '?' - ' ';

// Resolve Unicode once at the CPU/GPU boundary. GPU cells contain atlas
// identities rather than code points, so the shader and atlas generator
// cannot silently acquire different glyph whitelists.
constexpr std::uint32_t glyphIndex(std::uint32_t codepoint) {
  if (codepoint >= 32u && codepoint <= 126u)
    return codepoint - 32u;
  switch (codepoint) {
  case 0x25B6u: return 95u;
  case 0x23F8u: return 96u;
  case 0x25A0u: return 97u;
  case 0x2022u: return 98u;
  case 0x2500u:
  case 0x2501u: return 99u;
  case 0x2502u:
  case 0x2503u: return 100u;
  case 0x250Cu: return 101u;
  case 0x2510u: return 102u;
  case 0x2514u: return 103u;
  case 0x2518u: return 104u;
  case 0x251Cu: return 105u;
  case 0x2524u: return 106u;
  case 0x252Cu: return 107u;
  case 0x2534u: return 108u;
  case 0x253Cu: return 109u;
  case 0x25CBu: return 110u;
  case 0x2504u: return kUiGlyphAtlasStart + 0u;
  case 0x250Au: return kUiGlyphAtlasStart + 1u;
  case 0x256Bu: return kUiGlyphAtlasStart + 2u;
  case 0x2550u: return kUiGlyphAtlasStart + 3u;
  case 0x2591u: return kUiGlyphAtlasStart + 4u;
  case 0x2191u: return kUiGlyphAtlasStart + 5u;
  case 0x2193u: return kUiGlyphAtlasStart + 6u;
  case 0x00B7u: return kUiGlyphAtlasStart + 7u;
  case 0x2026u: return kUiGlyphAtlasStart + 8u;
  case 0x2013u:
  case 0x2014u:
  case 0x2212u: return '-' - ' ';
  default: break;
  }
  if (codepoint >= 0x2588u && codepoint <= 0x258Fu)
    return kBlockGlyphAtlasStart + (codepoint - 0x2588u);
  if (codepoint >= 0x2800u && codepoint <= 0x28FFu)
    return kBrailleGlyphAtlasStart + (codepoint - 0x2800u);
  return kMissingGlyphAtlasIndex;
}

constexpr std::uint32_t codepoint(std::uint32_t atlasIndex) {
  if (atlasIndex <= 94u)
    return 32u + atlasIndex;
  switch (atlasIndex) {
  case 95u: return 0x25B6u;
  case 96u: return 0x23F8u;
  case 97u: return 0x25A0u;
  case 98u: return 0x2022u;
  case 99u: return 0x2500u;
  case 100u: return 0x2502u;
  case 101u: return 0x250Cu;
  case 102u: return 0x2510u;
  case 103u: return 0x2514u;
  case 104u: return 0x2518u;
  case 105u: return 0x251Cu;
  case 106u: return 0x2524u;
  case 107u: return 0x252Cu;
  case 108u: return 0x2534u;
  case 109u: return 0x253Cu;
  case 110u: return 0x25CBu;
  default: break;
  }
  if (atlasIndex >= kBlockGlyphAtlasStart &&
      atlasIndex < kBlockGlyphAtlasStart + 8u)
    return 0x2588u + (atlasIndex - kBlockGlyphAtlasStart);
  if (atlasIndex >= kBrailleGlyphAtlasStart &&
      atlasIndex < kBrailleGlyphAtlasStart + 256u)
    return 0x2800u + (atlasIndex - kBrailleGlyphAtlasStart);
  if (atlasIndex >= kUiGlyphAtlasStart &&
      atlasIndex < kUiGlyphAtlasStart + 9u) {
    constexpr std::uint32_t uiCodepoints[] = {
        0x2504u, 0x250Au, 0x256Bu, 0x2550u, 0x2591u,
        0x2191u, 0x2193u, 0x00B7u, 0x2026u};
    return uiCodepoints[atlasIndex - kUiGlyphAtlasStart];
  }
  return '?';
}

static_assert(kUiGlyphAtlasStart + 9u == kAtlasGlyphCount);
static_assert(codepoint(glyphIndex(0x250Au)) == 0x250Au);
static_assert(codepoint(glyphIndex(0x256Bu)) == 0x256Bu);

} // namespace playback_gpu_text_grid
