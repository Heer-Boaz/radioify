#include "playback/video/framebuffer/window/gpu_text_grid_composition.h"
#include "playback/video/framebuffer/gpu_text_grid_glyph_catalog.h"

#include <d3d11.h>
#include <iostream>
#include <wrl/client.h>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "gpu_text_grid_composition_tests: " << message << '\n';
    return false;
  }
  return true;
}

Microsoft::WRL::ComPtr<ID3D11BlendState> currentBlendState(
    ID3D11DeviceContext* context) {
  Microsoft::WRL::ComPtr<ID3D11BlendState> state;
  float factor[4]{};
  UINT sampleMask = 0;
  context->OMGetBlendState(state.GetAddressOf(), factor, &sampleMask);
  return state;
}

}  // namespace

int main() {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL featureLevel{};
  if (FAILED(D3D11CreateDevice(
          nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
          D3D11_SDK_VERSION, device.GetAddressOf(), &featureLevel,
          context.GetAddressOf()))) {
    return expect(false, "a headless WARP device must be available") ? 0 : 1;
  }

  D3D11_BLEND_DESC descriptor{};
  descriptor.RenderTarget[0].BlendEnable = TRUE;
  descriptor.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  descriptor.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  descriptor.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  descriptor.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  descriptor.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  descriptor.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  descriptor.RenderTarget[0].RenderTargetWriteMask =
      D3D11_COLOR_WRITE_ENABLE_ALL;
  Microsoft::WRL::ComPtr<ID3D11BlendState> alphaBlend;
  if (FAILED(device->CreateBlendState(&descriptor,
                                      alphaBlend.GetAddressOf()))) {
    return expect(false, "the overlay blend state must be creatable") ? 0 : 1;
  }

  bool ok = true;
  context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
  ok &= expect(bindGpuTextGridComposition(
                   context.Get(), alphaBlend.Get(),
                   GpuTextGridComposition::AlphaOverlay),
               "an overlay pass must accept its alpha blend state");
  ok &= expect(currentBlendState(context.Get()).Get() == alphaBlend.Get(),
               "an overlay pass must replace inherited opaque composition");

  ok &= expect(bindGpuTextGridComposition(
                   context.Get(), alphaBlend.Get(),
                   GpuTextGridComposition::Opaque),
               "a primary text-grid pass must accept opaque composition");
  ok &= expect(!currentBlendState(context.Get()),
               "an opaque pass must bind the D3D11 no-blend state");

  ok &= expect(!bindGpuTextGridComposition(
                   context.Get(), nullptr,
                   GpuTextGridComposition::AlphaOverlay),
               "an overlay pass must reject a missing blend state");
  ok &= expect(!currentBlendState(context.Get()),
               "a rejected composition must not mutate inherited state");
  for (const std::uint32_t codepoint :
       {0x2504u, 0x250Au, 0x256Bu, 0x2550u, 0x2591u, 0x2191u, 0x2193u,
        0x00B7u, 0x2026u}) {
    const std::uint32_t atlasIndex =
        playback_gpu_text_grid::glyphIndex(codepoint);
    ok &= expect(
        atlasIndex != playback_gpu_text_grid::kMissingGlyphAtlasIndex &&
            playback_gpu_text_grid::codepoint(atlasIndex) == codepoint,
        "every emitted timeline/panel UI glyph must have one catalogued "
        "framebuffer atlas identity");
  }
  return ok ? 0 : 1;
}
