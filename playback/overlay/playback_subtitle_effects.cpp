#include "playback_subtitle_effects.h"

#include <algorithm>
#include <cmath>

namespace playback_overlay {
namespace {

float assAlphaToOpacity(int alpha) {
  return std::clamp(static_cast<float>(255 - std::clamp(alpha, 0, 255)) /
                        255.0f,
                    0.0f, 1.0f);
}

void applySubtitleOpacity(WindowUiState::SubtitleCue* cue, float opacity) {
  const float a = std::clamp(opacity, 0.0f, 1.0f);
  cue->primaryAlpha *= a;
  cue->backAlpha *= a;
  for (auto& run : cue->textRuns) {
    run.primaryAlpha *= a;
    run.backAlpha *= a;
  }
}

float transformProgress(const SubtitleTransform& transform, const SubtitleCue& cue,
                        int64_t clockUs) {
  const double elapsedMs =
      static_cast<double>(std::max<int64_t>(0, clockUs - cue.startUs)) / 1000.0;
  const int startMs = std::max(0, transform.startMs);
  int endMs = std::max(startMs, transform.endMs);
  if (endMs == startMs && cue.endUs > cue.startUs) {
    endMs = std::max(startMs + 1,
                     static_cast<int>((cue.endUs - cue.startUs) / 1000));
  }

  double progress = elapsedMs >= static_cast<double>(startMs) ? 1.0 : 0.0;
  if (endMs > startMs) {
    progress = (elapsedMs - static_cast<double>(startMs)) /
               static_cast<double>(endMs - startMs);
  }
  progress = std::clamp(progress, 0.0, 1.0);
  const float accel = std::max(0.01f, transform.accel);
  if (std::abs(accel - 1.0f) > 0.001f) {
    progress = std::pow(progress, static_cast<double>(accel));
  }
  return static_cast<float>(progress);
}

uint8_t lerpByte(uint8_t from, uint8_t to, float t) {
  return static_cast<uint8_t>(
      std::lround(static_cast<float>(from) +
                  (static_cast<float>(to) - static_cast<float>(from)) *
                      std::clamp(t, 0.0f, 1.0f)));
}

float lerpFloat(float from, float to, float t) {
  return from + (to - from) * std::clamp(t, 0.0f, 1.0f);
}

Color lerpSubtitleColor(Color from, Color to, float t) {
  return Color{lerpByte(from.r, to.r, t), lerpByte(from.g, to.g, t),
               lerpByte(from.b, to.b, t)};
}

void applySubtitleTransform(WindowUiState::SubtitleCue* item,
                            const SubtitleTransform& transform, float progress) {
  if (transform.hasSizeScale) {
    item->sizeScale = lerpFloat(item->sizeScale, transform.sizeScale, progress);
  }
  if (transform.hasScaleX) {
    item->scaleX = lerpFloat(item->scaleX, transform.scaleX, progress);
  }
  if (transform.hasScaleY) {
    item->scaleY = lerpFloat(item->scaleY, transform.scaleY, progress);
  }

  const Color itemPrimaryStart =
      item->hasPrimaryColor ? item->primaryColor : Color{255, 255, 255};
  if (transform.hasPrimaryColor) {
    item->hasPrimaryColor = true;
    item->primaryColor =
        lerpSubtitleColor(itemPrimaryStart,
                          Color{transform.primaryR, transform.primaryG,
                                transform.primaryB},
                          progress);
  }
  if (transform.hasPrimaryAlpha) {
    item->primaryAlpha =
        lerpFloat(item->primaryAlpha, transform.primaryAlpha, progress);
  }

  const Color itemBackStart =
      item->hasBackColor ? item->backColor : Color{0, 0, 0};
  if (transform.hasBackColor) {
    item->hasBackColor = true;
    item->backColor =
        lerpSubtitleColor(itemBackStart,
                          Color{transform.backR, transform.backG,
                                transform.backB},
                          progress);
  }
  if (transform.hasBackAlpha) {
    item->backAlpha = lerpFloat(item->backAlpha, transform.backAlpha, progress);
  }

  for (auto& run : item->textRuns) {
    const Color runPrimaryStart =
        run.hasPrimaryColor ? run.primaryColor : itemPrimaryStart;
    if (transform.hasPrimaryColor) {
      run.hasPrimaryColor = true;
      run.primaryColor =
          lerpSubtitleColor(runPrimaryStart,
                            Color{transform.primaryR, transform.primaryG,
                                  transform.primaryB},
                            progress);
    }
    if (transform.hasPrimaryAlpha) {
      run.primaryAlpha =
          lerpFloat(run.primaryAlpha, transform.primaryAlpha, progress);
    }

    const Color runBackStart = run.hasBackColor ? run.backColor : itemBackStart;
    if (transform.hasBackColor) {
      run.hasBackColor = true;
      run.backColor =
          lerpSubtitleColor(runBackStart,
                            Color{transform.backR, transform.backG,
                                  transform.backB},
                            progress);
    }
    if (transform.hasBackAlpha) {
      run.backAlpha = lerpFloat(run.backAlpha, transform.backAlpha, progress);
    }
  }
}

void applySubtitleTransforms(WindowUiState::SubtitleCue* item,
                             const SubtitleCue& cue, int64_t clockUs) {
  if (cue.transforms.empty()) return;
  item->hasTransform = true;
  for (const SubtitleTransform& transform : cue.transforms) {
    applySubtitleTransform(item, transform,
                           transformProgress(transform, cue, clockUs));
  }
  item->sizeScale = std::clamp(item->sizeScale, 0.35f, 3.5f);
  item->scaleX = std::clamp(item->scaleX, 0.40f, 3.5f);
  item->scaleY = std::clamp(item->scaleY, 0.40f, 3.5f);
}

}  // namespace

float subtitleFadeOpacity(const SubtitleCue& cue, int64_t clockUs) {
  const double elapsedMs =
      static_cast<double>(std::max<int64_t>(0, clockUs - cue.startUs)) /
      1000.0;
  if (cue.hasComplexFade) {
    const int t1 = std::max(0, cue.fadeT1Ms);
    const int t2 = std::max(t1, cue.fadeT2Ms);
    const int t3 = std::max(t2, cue.fadeT3Ms);
    const int t4 = std::max(t3, cue.fadeT4Ms);
    auto lerpAlpha = [](int a, int b, double t) {
      return static_cast<float>(static_cast<double>(a) +
                                (static_cast<double>(b - a) *
                                 std::clamp(t, 0.0, 1.0)));
    };

    float alpha = static_cast<float>(cue.fadeAlpha3);
    if (elapsedMs < t1) {
      alpha = static_cast<float>(cue.fadeAlpha1);
    } else if (elapsedMs < t2 && t2 > t1) {
      alpha = lerpAlpha(cue.fadeAlpha1, cue.fadeAlpha2,
                        (elapsedMs - t1) / static_cast<double>(t2 - t1));
    } else if (elapsedMs < t3) {
      alpha = static_cast<float>(cue.fadeAlpha2);
    } else if (elapsedMs < t4 && t4 > t3) {
      alpha = lerpAlpha(cue.fadeAlpha2, cue.fadeAlpha3,
                        (elapsedMs - t3) / static_cast<double>(t4 - t3));
    }
    return assAlphaToOpacity(static_cast<int>(std::lround(alpha)));
  }

  float opacity = 1.0f;
  if (cue.hasSimpleFade && cue.fadeInMs > 0 && elapsedMs < cue.fadeInMs) {
    opacity = std::min(opacity,
                       static_cast<float>(elapsedMs /
                                          static_cast<double>(cue.fadeInMs)));
  }
  if (cue.hasSimpleFade && cue.fadeOutMs > 0 && cue.endUs > cue.startUs) {
    const double durationMs =
        static_cast<double>(cue.endUs - cue.startUs) / 1000.0;
    const double remainingMs = std::max(0.0, durationMs - elapsedMs);
    if (remainingMs < cue.fadeOutMs) {
      opacity = std::min(opacity,
                         static_cast<float>(remainingMs /
                                            static_cast<double>(cue.fadeOutMs)));
    }
  }
  return std::clamp(opacity, 0.0f, 1.0f);
}

void applySubtitleTimelineEffects(WindowUiState::SubtitleCue* item,
                                  const SubtitleCue& cue, int64_t clockUs,
                                  float fadeOpacity) {
  if (!item) return;
  applySubtitleTransforms(item, cue, clockUs);
  applySubtitleOpacity(item, fadeOpacity);
}

}  // namespace playback_overlay
