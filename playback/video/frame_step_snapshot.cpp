#include "playback/video/frame_step_snapshot.h"

#include <utility>

#include <wrl/client.h>

namespace playback_video_frame_step_snapshot {

bool materializeHardwareFrame(const VideoFrame &source, ID3D11Device *device,
                              ID3D11DeviceContext *immediateContext,
                              std::recursive_mutex *immediateContextMutex,
                              VideoFrame *snapshot) {
  if (!snapshot || source.format != VideoPixelFormat::HWTexture ||
      !source.hwTexture || !device || !immediateContext ||
      !immediateContextMutex) {
    return false;
  }

  D3D11_TEXTURE2D_DESC sourceDesc{};
  source.hwTexture->GetDesc(&sourceDesc);
  if (sourceDesc.MipLevels != 1 || sourceDesc.SampleDesc.Count != 1 ||
      source.hwTextureArrayIndex < 0 ||
      static_cast<UINT>(source.hwTextureArrayIndex) >= sourceDesc.ArraySize) {
    return false;
  }

  D3D11_TEXTURE2D_DESC snapshotDesc = sourceDesc;
  snapshotDesc.ArraySize = 1;
  snapshotDesc.Usage = D3D11_USAGE_DEFAULT;
  // The snapshot is consumed as a copy or video-processor source. It does not
  // inherit decoder-pool binding flags or lifetime semantics.
  snapshotDesc.BindFlags = 0;
  snapshotDesc.CPUAccessFlags = 0;
  snapshotDesc.MiscFlags = 0;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  // ID3D11Device resource creation is free-threaded. Keep the cross-pipeline
  // lock scoped to the immediate-context command so allocation does not block
  // presentation.
  if (FAILED(device->CreateTexture2D(&snapshotDesc, nullptr, &texture))) {
    return false;
  }

  {
    std::lock_guard<std::recursive_mutex> lock(*immediateContextMutex);
    immediateContext->CopySubresourceRegion(
        texture.Get(), 0, 0, 0, 0, source.hwTexture.Get(),
        D3D11CalcSubresource(0, static_cast<UINT>(source.hwTextureArrayIndex),
                             sourceDesc.MipLevels),
        nullptr);
  }

  VideoFrame materialized = source;
  materialized.hwTexture = std::move(texture);
  materialized.hwTextureArrayIndex = 0;
  materialized.hwFrameRef.reset();
  materialized.cacheLease.reset();
  *snapshot = std::move(materialized);
  return true;
}

} // namespace playback_video_frame_step_snapshot
