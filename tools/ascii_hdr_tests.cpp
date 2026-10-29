#include "asciiart_gpu.h"
#include "playback/framebuffer/mini_player_tui.h"
#include "playback/video/color_conversion.h"

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {

#include "window_render_vs.h"
#include "window_gpu_text_grid_ps.h"

using Microsoft::WRL::ComPtr;

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void checkHr(HRESULT hr, const char* message) {
  check(SUCCEEDED(hr), message);
}

void expectNear(float actual, float expected, const char* message) {
  if (std::abs(actual - expected) > std::max(0.2f, expected * 0.02f)) {
    std::cerr << message << ": expected " << expected << ", got " << actual
              << '\n';
    throw std::runtime_error(message);
  }
}

float pqCode(float nits) {
  const float p = std::pow(nits / 10000.0f, 2610.0f / 16384.0f);
  return std::pow((3424.0f / 4096.0f + 2413.0f / 128.0f * p) /
                      (1.0f + 2392.0f / 128.0f * p),
                  2523.0f / 32.0f);
}

float channelNits(uint32_t color, unsigned channel, float hlgPeak = 1000.0f) {
  const float code = float((color >> (channel * 10)) & 1023u) / 1023.0f;
  return (color >> 30) == 1u
      ? playback_video_color::detail::pqEotf(code) * 10000.0f
      : playback_video_color::detail::hlgEotf(code) * hlgPeak;
}

void waitForGpu(ID3D11Device* device, ID3D11DeviceContext* context) {
  D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
  ComPtr<ID3D11Query> done;
  checkHr(device->CreateQuery(&desc, &done), "GPU event");
  context->End(done.Get());
  context->Flush();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  HRESULT hr = S_FALSE;
  while ((hr = context->GetData(done.Get(), nullptr, 0, 0)) == S_FALSE) {
    check(std::chrono::steady_clock::now() < deadline, "GPU readback timed out");
    SwitchToThread();
  }
  checkHr(hr, "GPU event readback");
}

AsciiArt renderCache(ID3D11Device* device, ID3D11DeviceContext* context,
                     GpuVideoFrameCache& cache) {
  std::recursive_mutex mutex;
  GpuAsciiRenderer renderer(mutex);
  std::string error;
  check(renderer.InitializeWithDevice(device, context, &error), error.c_str());
  AsciiArt art;
  art.width = 2;
  art.height = 2;
  for (int attempt = 0; attempt < 6; ++attempt) {
    if (renderer.RenderFromCache(cache, art, &error)) return art;
    check(error.empty(), error.c_str());
    waitForGpu(device, context);
  }
  throw std::runtime_error("ASCII staging ring produced no frame");
}

AsciiArt p010Fixture(ID3D11Device* device, ID3D11DeviceContext* context,
                     float nits, bool alternateBlack = false,
                     bool splitDots = false) {
  constexpr int width = 8;
  constexpr int height = 8;
  std::vector<uint16_t> pixels(width * height * 3 / 2, 512u << 6);
  const uint16_t y = uint16_t(64 + std::lround(pqCode(nits) * 876.0f));
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      const bool black = (alternateBlack && col % 2 == 0) ||
                         (splitDots && col % 4 >= 2);
      pixels[row * width + col] = (black ? 64u : y) << 6;
    }
  }
  GpuVideoFrameCache cache;
  check(cache.UpdateNV12(device, context,
                          reinterpret_cast<const uint8_t*>(pixels.data()),
                          width * 2, height, width, height, false,
                          YuvMatrix::Bt2020, YuvTransfer::Pq, 10),
        "P010 fixture upload");
  return renderCache(device, context, cache);
}

AsciiArt rgbaFixture(ID3D11Device* device, ID3D11DeviceContext* context,
                     std::array<float, 3> code, YuvTransfer transfer) {
  uint32_t packed = 3u << 30;
  for (unsigned i = 0; i < 3; ++i) {
    packed |= uint32_t(std::lround(code[i] * 1023.0f)) << (i * 10);
  }
  std::array<uint32_t, 64> pixels;
  pixels.fill(packed);
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = 8;
  desc.Height = 8;
  desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data{pixels.data(), 8 * sizeof(uint32_t), 0};
  ComPtr<ID3D11Texture2D> texture;
  checkHr(device->CreateTexture2D(&desc, &data, &texture), "RGB10 fixture");
  GpuVideoFrameCache cache;
  check(cache.Update(device, context, texture.Get(), 0, 8, 8, true,
                     YuvMatrix::Bt2020, transfer, 10), "RGB10 cache upload");
  return renderCache(device, context, cache);
}

void checkArt(const AsciiArt& art, std::array<float, 3> expected,
               YuvTransfer transfer) {
  check(art.cells.size() == 4, "HDR fixture dimensions");
  for (const auto& cell : art.cells) {
    for (const auto& color : {cell.fg, cell.bg}) {
      check(color.hdr >> 30 == uint32_t(transfer), "source HDR transfer retained");
      for (unsigned channel = 0; channel < 3; ++channel) {
        expectNear(channelNits(color.hdr, channel), expected[channel],
             "ASCII source luminance retained");
      }
    }
  }
}

std::array<float, 4> renderCell(ID3D11Device* device,
                                ID3D11DeviceContext* context,
                                const Color& fg, const Color& bg,
                                float coverage, uint32_t encoding) {
  // Exercise the production ScreenCell -> GPU text-grid colour handoff too.
  ScreenCell screenCell;
  screenCell.ch = L' ';
  screenCell.fg = fg;
  screenCell.bg = bg;
  GpuTextGridFrame grid;
  playback_framebuffer_presenter::buildGpuTextGridFrameFromScreenCells(
      {screenCell}, 1, 1, grid);
  check(grid.cells[0].fg == (fg.hdr ? fg.hdr : gpuTextGridRgb(fg.r, fg.g, fg.b)),
        "foreground HDR survived screen-cell conversion");
  grid.cells[0].glyphIndex = 0;

  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  checkHr(device->CreateVertexShader(kWindowVs, kWindowVs_Size, nullptr, &vs),
          "production vertex shader");
  checkHr(device->CreatePixelShader(kWindowPsGpuTextGrid,
                                    kWindowPsGpuTextGrid_Size, nullptr, &ps),
          "production text-grid pixel shader");
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = desc.Height = desc.MipLevels = desc.ArraySize =
      desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA cellData{grid.cells.data(), sizeof(GpuTextGridCell), 0};
  ComPtr<ID3D11Texture2D> cells;
  ComPtr<ID3D11ShaderResourceView> cellSrv;
  checkHr(device->CreateTexture2D(&desc, &cellData, &cells), "cell texture");
  checkHr(device->CreateShaderResourceView(cells.Get(), nullptr, &cellSrv), "cells SRV");
  desc.Format = DXGI_FORMAT_R32_FLOAT;
  D3D11_SUBRESOURCE_DATA atlasData{&coverage, sizeof(float), 0};
  ComPtr<ID3D11Texture2D> atlas;
  ComPtr<ID3D11ShaderResourceView> atlasSrv;
  checkHr(device->CreateTexture2D(&desc, &atlasData, &atlas), "glyph atlas");
  checkHr(device->CreateShaderResourceView(atlas.Get(), nullptr, &atlasSrv), "atlas SRV");
  desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> target;
  ComPtr<ID3D11RenderTargetView> rtv;
  checkHr(device->CreateTexture2D(&desc, nullptr, &target), "float output");
  checkHr(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "output RTV");
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  checkHr(device->CreateTexture2D(&desc, nullptr, &staging), "output readback");
  struct Constants {
    std::array<uint32_t, 4> layout;
    std::array<float, 4> display;
  } constants{{1, 1, 1, encoding}, {240, 1000, 400, 0}};
  D3D11_BUFFER_DESC cbDesc{};
  cbDesc.ByteWidth = sizeof(constants);
  cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA cbData{&constants, 0, 0};
  ComPtr<ID3D11Buffer> cb;
  checkHr(device->CreateBuffer(&cbDesc, &cbData, &cb), "output constants");
  D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
  context->RSSetViewports(1, &viewport);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->PSSetShader(ps.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srvs[] = {cellSrv.Get(), atlasSrv.Get()};
  context->PSSetShaderResources(0, 2, srvs);
  context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
  context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  context->Draw(4, 0);
  context->OMSetRenderTargets(0, nullptr, nullptr);
  context->CopyResource(staging.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  checkHr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "output pixels");
  const auto pixel = *static_cast<const std::array<float, 4>*>(mapped.pData);
  context->Unmap(staging.Get(), 0);
  return pixel;
}

}  // namespace

int main(int argc, char** argv) try {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const bool hardware = argc == 2 && std::string(argv[1]) == "--hardware";
  checkHr(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE
                                             : D3D_DRIVER_TYPE_WARP,
                            nullptr, 0, nullptr,
                            0, D3D11_SDK_VERSION, &device, nullptr, &context),
          "headless WARP device");
  for (float nits : {0.0f, 10.0f, 100.0f, 203.0f, 400.0f, 1000.0f, 4000.0f}) {
    const AsciiArt art = p010Fixture(device.Get(), context.Get(), nits);
    checkArt(art, {nits, nits, nits}, YuvTransfer::Pq);
    for (uint32_t encoding : {1u, 2u}) {
      const auto pixel = renderCell(device.Get(), context.Get(), art.cells[0].fg,
                                     art.cells[0].bg, 0.5f, encoding);
      const float outputNits = encoding == 1u ? pixel[1] * 80.0f
          : playback_video_color::detail::pqEotf(pixel[1]) * 10000.0f;
      expectNear(outputNits, nits, "end-to-end P010 HDR luminance");
    }
  }
  checkArt(p010Fixture(device.Get(), context.Get(), 1000, true),
           {500, 500, 500}, YuvTransfer::Pq);
  // Braille dots 1,2,3,7 form the left column (Unicode mask 0x47).
  // Their light must follow the final glyph mask, including refinement.
  const auto split = p010Fixture(device.Get(), context.Get(), 1000, false, true);
  auto bitCount = [](unsigned value) {
    unsigned count = 0;
    for (; value != 0; value &= value - 1) ++count;
    return count;
  };
  bool sawPartialMask = false;
  for (const auto& cell : split.cells) {
    const unsigned mask = unsigned(cell.ch) - 0x2800u;
    const unsigned inkCount = bitCount(mask);
    const unsigned brightInk = bitCount(mask & 0x47u);
    sawPartialMask |= inkCount != 0 && inkCount != 8;
    expectNear(channelNits(cell.fg.hdr, 0),
               inkCount ? 1000.0f * brightInk / inkCount : 500.0f,
               "HDR foreground follows the braille mask");
    expectNear(channelNits(cell.bg.hdr, 0),
               inkCount != 8 ? 1000.0f * (4u - brightInk) / (8u - inkCount)
                             : 500.0f,
               "HDR background follows the braille mask");
  }
  check(sawPartialMask, "contrast fixture must exercise partial braille masks");
  const std::array<float, 3> primaryNits{1000, 100, 10};
  const auto wideGamut = rgbaFixture(device.Get(), context.Get(),
      {pqCode(1000), pqCode(100), pqCode(10)}, YuvTransfer::Pq);
  checkArt(wideGamut, primaryNits, YuvTransfer::Pq);
  const auto hdr10 = renderCell(device.Get(), context.Get(), wideGamut.cells[0].fg,
                                 wideGamut.cells[0].bg, 1, 2);
  for (unsigned channel = 0; channel < 3; ++channel) {
    expectNear(playback_video_color::detail::pqEotf(hdr10[channel]) * 10000.0f,
         primaryNits[channel], "BT.2020 HDR primaries retained");
  }
  const auto hlg = rgbaFixture(device.Get(), context.Get(),
                                {0.75f, 0.75f, 0.75f}, YuvTransfer::Hlg);
  checkArt(hlg, {264.963f, 264.963f, 264.963f}, YuvTransfer::Hlg);
  for (uint32_t encoding : {1u, 2u}) {
    const auto pixel = renderCell(device.Get(), context.Get(), hlg.cells[0].fg,
                                   hlg.cells[0].bg, 0.5f, encoding);
    const float nits = encoding == 1u ? pixel[1] * 80.0f
        : playback_video_color::detail::pqEotf(pixel[1]) * 10000.0f;
    expectNear(nits, 264.963f, "HLG survives HDR presentation");
  }
  for (float coverage : {0.0f, 0.5f, 1.0f}) {
    for (uint32_t encoding : {0u, 1u, 2u}) {
      const auto white = renderCell(device.Get(), context.Get(),
                                     Color{255, 255, 255}, Color{255, 255, 255},
                                     coverage, encoding);
      const float value = encoding == 0u ? white[1] : encoding == 1u
          ? white[1] * 80.0f
          : playback_video_color::detail::pqEotf(white[1]) * 10000.0f;
      expectNear(value, encoding == 0u ? 1.0f : 240.0f,
           "SDR white must not acquire artificial HDR highlights");
    }
  }
  const auto sdr = renderCell(device.Get(), context.Get(), wideGamut.cells[0].fg,
                               wideGamut.cells[0].bg, 1, 0);
  for (float channel : sdr) {
    check(std::isfinite(channel) && channel >= 0 && channel <= 1,
          "HDR tone mapping is bounded for SDR output");
  }
  const auto sdrArt = rgbaFixture(device.Get(), context.Get(), {0.5f, 0.5f, 0.5f},
                                   YuvTransfer::Sdr);
  for (const auto& cell : sdrArt.cells) {
    check(cell.fg.hdr == 0 && cell.bg.hdr == 0, "SDR cells must not retain HDR tags");
  }
  std::cout << (hardware ? "Hardware" : "WARP")
            << " ascii_hdr_tests: PQ/HLG, linear averaging, braille masks, "
               "BT.2020, HDR10/scRGB, "
               "SDR output and SDR white passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "ascii_hdr_tests: " << error.what() << '\n';
  return 1;
}
