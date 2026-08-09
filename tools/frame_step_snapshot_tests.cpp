#include "playback/video/frame_step_snapshot.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace {

constexpr UINT kWidth = 8;
constexpr UINT kHeight = 6;
constexpr size_t kLogicalFrameCount = 12;

bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "frame_step_snapshot_tests: " << message << '\n';
    return false;
  }
  return true;
}

std::array<uint8_t, 4> colorFor(size_t frameIndex) {
  return {static_cast<uint8_t>(17 + frameIndex * 11),
          static_cast<uint8_t>(231 - frameIndex * 7),
          static_cast<uint8_t>(43 + frameIndex * 13), 255};
}

std::vector<uint8_t> solidPixels(const std::array<uint8_t, 4> &color) {
  std::vector<uint8_t> pixels(kWidth * kHeight * 4);
  for (size_t offset = 0; offset < pixels.size(); offset += 4) {
    std::copy(color.begin(), color.end(), pixels.begin() + offset);
  }
  return pixels;
}

bool textureHasColor(ID3D11Device *device, ID3D11DeviceContext *context,
                     ID3D11Texture2D *texture,
                     const std::array<uint8_t, 4> &expected) {
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
    return false;
  }
  context->CopyResource(staging.Get(), texture);

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return false;
  }
  bool matches = true;
  for (UINT y = 0; y < kHeight && matches; ++y) {
    const auto *row = static_cast<const uint8_t *>(mapped.pData) +
                      static_cast<size_t>(y) * mapped.RowPitch;
    for (UINT x = 0; x < kWidth; ++x) {
      const auto *pixel = row + static_cast<size_t>(x) * 4;
      if (!std::equal(expected.begin(), expected.end(), pixel)) {
        matches = false;
        break;
      }
    }
  }
  context->Unmap(staging.Get(), 0);
  return matches;
}

} // namespace

int main() {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL featureLevel{};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                               nullptr, 0, D3D11_SDK_VERSION,
                               device.GetAddressOf(), &featureLevel,
                               context.GetAddressOf()))) {
    return expect(false, "a headless WARP device must be available") ? 0 : 1;
  }

  D3D11_TEXTURE2D_DESC sourceDesc{};
  sourceDesc.Width = kWidth;
  sourceDesc.Height = kHeight;
  sourceDesc.MipLevels = 1;
  sourceDesc.ArraySize = 2;
  sourceDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sourceDesc.SampleDesc.Count = 1;
  sourceDesc.Usage = D3D11_USAGE_DEFAULT;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> decoderPool;
  if (FAILED(device->CreateTexture2D(&sourceDesc, nullptr, &decoderPool))) {
    return expect(false, "the synthetic decoder pool must be creatable") ? 0
                                                                         : 1;
  }

  std::recursive_mutex contextMutex;
  std::vector<VideoFrame> snapshots;
  snapshots.reserve(kLogicalFrameCount);
  for (size_t frameIndex = 0; frameIndex < kLogicalFrameCount; ++frameIndex) {
    const auto pixels = solidPixels(colorFor(frameIndex));
    const UINT poolSlice = static_cast<UINT>(frameIndex % sourceDesc.ArraySize);
    context->UpdateSubresource(
        decoderPool.Get(),
        D3D11CalcSubresource(0, poolSlice, sourceDesc.MipLevels), nullptr,
        pixels.data(), kWidth * 4, 0);

    VideoFrame decoded;
    decoded.width = static_cast<int>(kWidth);
    decoded.height = static_cast<int>(kHeight);
    decoded.format = VideoPixelFormat::HWTexture;
    decoded.hwTexture = decoderPool;
    decoded.hwTextureArrayIndex = static_cast<int>(poolSlice);
    decoded.storageBytes = pixels.size();
    const std::shared_ptr<int> decoderOwner = std::make_shared<int>(1);
    decoded.hwFrameRef = std::shared_ptr<AVFrame>(
        decoderOwner, reinterpret_cast<AVFrame *>(decoderOwner.get()));

    VideoFrame snapshot;
    if (!playback_video_frame_step_snapshot::materializeHardwareFrame(
            decoded, device.Get(), context.Get(), &contextMutex, &snapshot)) {
      return expect(false, "every decoder surface must materialize") ? 0 : 1;
    }
    if (!expect(snapshot.hwTexture.Get() != decoderPool.Get(),
                "a snapshot must not alias the decoder texture array") ||
        !expect(snapshot.hwTextureArrayIndex == 0,
                "a snapshot must own one canonical array slice") ||
        !expect(!snapshot.hwFrameRef,
                "a materialized snapshot must release decoder ownership")) {
      return 1;
    }
    snapshots.push_back(std::move(snapshot));
  }

  // Recycle both synthetic decoder slots once more. Every logical frame must
  // retain the pixels that existed when it was materialized.
  const auto overwritten = solidPixels({1, 2, 3, 4});
  for (UINT poolSlice = 0; poolSlice < sourceDesc.ArraySize; ++poolSlice) {
    context->UpdateSubresource(
        decoderPool.Get(),
        D3D11CalcSubresource(0, poolSlice, sourceDesc.MipLevels), nullptr,
        overwritten.data(), kWidth * 4, 0);
  }

  for (size_t frameIndex = 0; frameIndex < snapshots.size(); ++frameIndex) {
    if (!expect(textureHasColor(device.Get(), context.Get(),
                                snapshots[frameIndex].hwTexture.Get(),
                                colorFor(frameIndex)),
                "reusing a decoder-pool slot changed a cached frame")) {
      std::cerr << "frame_step_snapshot_tests: logical_frame=" << frameIndex
                << '\n';
      return 1;
    }
  }
  return 0;
}
