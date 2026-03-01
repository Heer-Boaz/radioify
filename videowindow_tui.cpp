#include "videowindow.h"

#include "gpu_shared.h"
#include "videowindow_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
bool renderScreenGridToBitmap(const std::vector<ScreenCell>& cells, int cols,
                              int rows, int width, int height,
                              std::vector<uint8_t>& outPixels) {
    outPixels.clear();
    if (cols <= 0 || rows <= 0 || width <= 0 || height <= 0) return false;
    size_t needCells = static_cast<size_t>(cols) * static_cast<size_t>(rows);
    if (cells.size() < needCells) return false;
    size_t pixelBytes =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (pixelBytes == 0) return false;
    outPixels.resize(pixelBytes);

    // Fill per-cell background first.
    for (int py = 0; py < height; ++py) {
        int cellY =
            static_cast<int>((static_cast<int64_t>(py) * rows) / std::max(1, height));
        cellY = std::clamp(cellY, 0, rows - 1);
        uint8_t* dstRow = outPixels.data() +
                          static_cast<size_t>(py) * static_cast<size_t>(width) * 4u;
        size_t rowBase = static_cast<size_t>(cellY) * static_cast<size_t>(cols);
        for (int px = 0; px < width; ++px) {
            int cellX = static_cast<int>(
                (static_cast<int64_t>(px) * cols) / std::max(1, width));
            cellX = std::clamp(cellX, 0, cols - 1);
            const auto& cell = cells[rowBase + static_cast<size_t>(cellX)];
            uint8_t* p = dstRow + static_cast<size_t>(px) * 4u;
            p[0] = cell.bg.b;
            p[1] = cell.bg.g;
            p[2] = cell.bg.r;
            p[3] = 255;
        }
    }

    HDC memDC = CreateCompatibleDC(nullptr);
    if (!memDC) return true;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HBITMAP dib =
        CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!dib || !dibBits) {
        if (dib) DeleteObject(dib);
        DeleteDC(memDC);
        return true;
    }

    HGDIOBJ oldBmp = SelectObject(memDC, dib);
    std::memcpy(dibBits, outPixels.data(), pixelBytes);
    SetBkMode(memDC, TRANSPARENT);

    int cellH = std::max(1, height / rows);
    int fontH = std::max(8, static_cast<int>(std::round(cellH * 0.95f)));
    HFONT font = CreateFontW(
        -fontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        FIXED_PITCH | FF_DONTCARE, L"Consolas");
    bool ownsFont = (font != nullptr);
    if (!font) {
        font = static_cast<HFONT>(GetStockObject(SYSTEM_FIXED_FONT));
    }
    HGDIOBJ oldFont = SelectObject(memDC, font);

    COLORREF lastFg = RGB(255, 255, 255);
    bool hasFg = false;
    for (int r = 0; r < rows; ++r) {
        int y0 = static_cast<int>((static_cast<int64_t>(r) * height) / rows);
        int y1 = static_cast<int>((static_cast<int64_t>(r + 1) * height) / rows);
        if (y1 <= y0) y1 = y0 + 1;
        for (int c = 0; c < cols; ++c) {
            size_t idx =
                static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c);
            const auto& cell = cells[idx];
            wchar_t ch = cell.ch ? cell.ch : L' ';
            if (ch == L' ') continue;

            int x0 = static_cast<int>((static_cast<int64_t>(c) * width) / cols);
            int x1 = static_cast<int>((static_cast<int64_t>(c + 1) * width) / cols);
            if (x1 <= x0) x1 = x0 + 1;

            COLORREF fg = RGB(cell.fg.r, cell.fg.g, cell.fg.b);
            if (!hasFg || fg != lastFg) {
                SetTextColor(memDC, fg);
                lastFg = fg;
                hasFg = true;
            }

            RECT rc{x0, y0, x1, y1};
            DrawTextW(memDC, &ch, 1, &rc,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }
    }

    std::memcpy(outPixels.data(), dibBits, pixelBytes);

    if (oldFont) SelectObject(memDC, oldFont);
    if (ownsFont && font) DeleteObject(font);
    if (oldBmp) SelectObject(memDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDC);
    return true;
}
}  // namespace

void VideoWindow::PresentTextGrid(const std::vector<ScreenCell>& cells, int cols,
                                  int rows, bool nonBlocking) {
    std::unique_lock<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd || !m_swapChain || !IsWindowVisible(m_hWnd)) return;
    if (cols <= 0 || rows <= 0 || cells.empty()) return;

    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain = m_swapChain;
    UINT presentInterval = m_presentInterval.load(std::memory_order_relaxed);

    RECT rect;
    if (GetClientRect(m_hWnd, &rect)) {
        m_width = rect.right - rect.left;
        m_height = rect.bottom - rect.top;
    }
    if (m_width <= 0 || m_height <= 0) return;

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context || !m_renderTargetView || !m_constantBuffer) return;

    std::vector<uint8_t> rgbaPixels;
    if (!renderScreenGridToBitmap(cells, cols, rows, m_width, m_height,
                                  rgbaPixels)) {
        return;
    }

    if (!m_tuiTexture || !m_tuiSrv || m_tuiTexWidth != m_width ||
        m_tuiTexHeight != m_height) {
        m_tuiTexture.Reset();
        m_tuiSrv.Reset();
        m_tuiTexWidth = 0;
        m_tuiTexHeight = 0;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(m_width);
        td.Height = static_cast<UINT>(m_height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device->CreateTexture2D(&td, nullptr, &m_tuiTexture)) ||
            !m_tuiTexture) {
            return;
        }
        if (FAILED(device->CreateShaderResourceView(m_tuiTexture.Get(), nullptr,
                                                    &m_tuiSrv)) ||
            !m_tuiSrv) {
            m_tuiTexture.Reset();
            return;
        }
        m_tuiTexWidth = m_width;
        m_tuiTexHeight = m_height;
    }

    D3D11_BOX box{0, 0, 0, static_cast<UINT>(m_width),
                  static_cast<UINT>(m_height), 1};
    context->UpdateSubresource(m_tuiTexture.Get(), 0, &box, rgbaPixels.data(),
                               static_cast<UINT>(m_width * 4), 0);

    float clearColor[4] = {0, 0, 0, 1};
    context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
    context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), NULL);

    D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(m_width),
                               static_cast<float>(m_height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);

    context->IASetInputLayout(NULL);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(m_vertexShader.Get(), NULL, 0);
    context->PSSetShader(m_pixelShader.Get(), NULL, 0);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0,
                                   D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            ShaderConstants sc{};
            sc.hasRGBA = 1u;
            sc.rotationQuarterTurns = 0u;
            std::memcpy(mapped.pData, &sc, sizeof(ShaderConstants));
            context->Unmap(m_constantBuffer.Get(), 0);
        }
    }

    ID3D11ShaderResourceView* srvs[3] = {nullptr, nullptr, m_tuiSrv.Get()};
    context->PSSetShaderResources(0, 3, srvs);
    context->Draw(4, 0);
    ID3D11ShaderResourceView* nullSrvs[3] = {nullptr, nullptr, nullptr};
    context->PSSetShaderResources(0, 3, nullSrvs);

    lock.unlock();
    if (!swapChain) return;
    UINT flags = nonBlocking ? DXGI_PRESENT_DO_NOT_WAIT : 0u;
    (void)swapChain->Present(presentInterval, flags);
}
