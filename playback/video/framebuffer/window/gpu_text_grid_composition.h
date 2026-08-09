#pragma once

#include <d3d11.h>

enum class GpuTextGridComposition {
    Opaque,
    AlphaOverlay,
};

// A text-grid pass may be either a primary opaque surface or an overlay.
// Bind that contract at the pass boundary so it never depends on state left by
// a preceding video, subtitle, or thumbnail draw.
inline bool bindGpuTextGridComposition(
    ID3D11DeviceContext* context, ID3D11BlendState* alphaBlendState,
    GpuTextGridComposition composition) {
    if (!context ||
        (composition == GpuTextGridComposition::AlphaOverlay &&
         !alphaBlendState)) {
        return false;
    }
    if (composition == GpuTextGridComposition::AlphaOverlay) {
        const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context->OMSetBlendState(alphaBlendState, blendFactor, 0xffffffffu);
    } else {
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    }
    return true;
}
