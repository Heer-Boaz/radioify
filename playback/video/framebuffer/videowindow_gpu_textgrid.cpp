#include "videowindow.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gpu_shared.h"
#include "terminal_font.h"

namespace {
constexpr int kGlyphAtlasCols = 16;
constexpr int kGlyphAtlasRows = 24;
constexpr int kBlockGlyphAtlasStart = 111;
constexpr int kBrailleGlyphAtlasStart = 119;

struct GpuTextGridConstants {
    uint32_t glyphCellWidth = 1;
    uint32_t glyphCellHeight = 1;
    uint32_t glyphAtlasCols = kGlyphAtlasCols;
    uint32_t pad = 0;
};

static_assert(sizeof(GpuTextGridConstants) == 16,
              "GPU text grid constants must be 16-byte aligned");

RadioifyTerminalFontMetrics measureTerminalFontMetrics(UINT dpi) {
    HDC memDC = CreateCompatibleDC(nullptr);
    if (!memDC) {
        RadioifyTerminalFontMetrics metrics{};
        metrics.dpi = std::max<UINT>(1, dpi);
        metrics.fontPixelHeight =
            radioifyTerminalFontPixelHeightForDpi(metrics.dpi);
        metrics.fontWeight = radioifyTerminalFontWeight();
        metrics.cellWidth = kGpuTextGridFallbackCellPixelWidth;
        metrics.cellHeight = kGpuTextGridFallbackCellPixelHeight;
        return metrics;
    }

    RadioifyTerminalFontMetrics metrics =
        measureRadioifyTerminalFontMetrics(memDC, dpi);
    DeleteDC(memDC);
    metrics.cellWidth = std::max(1, metrics.cellWidth);
    metrics.cellHeight = std::max(1, metrics.cellHeight);
    metrics.fontWeight = std::clamp(metrics.fontWeight, 1, 1000);
    return metrics;
}

wchar_t glyphForAtlasIndex(int index) {
    if (index >= 0 && index <= 94) {
        return static_cast<wchar_t>(32 + index);
    }
    switch (index) {
        case 95:
            return L'\u25B6';
        case 96:
            return L'\u23F8';
        case 97:
            return L'\u25A0';
        case 98:
            return L'\u2022';
        case 99:
            return L'\u2500';
        case 100:
            return L'\u2502';
        case 101:
            return L'\u250C';
        case 102:
            return L'\u2510';
        case 103:
            return L'\u2514';
        case 104:
            return L'\u2518';
        case 105:
            return L'\u251C';
        case 106:
            return L'\u2524';
        case 107:
            return L'\u252C';
        case 108:
            return L'\u2534';
        case 109:
            return L'\u253C';
        case 110:
            return L'\u25CB';
        default:
            if (index >= kBlockGlyphAtlasStart &&
                index < kBlockGlyphAtlasStart + 8) {
                return static_cast<wchar_t>(
                    L'\u2588' + (index - kBlockGlyphAtlasStart));
            }
            if (index >= kBrailleGlyphAtlasStart &&
                index < kBrailleGlyphAtlasStart + 256) {
                return static_cast<wchar_t>(
                    L'\u2800' + (index - kBrailleGlyphAtlasStart));
            }
            return L'?';
    }
}

bool renderGlyphAtlas(std::vector<uint8_t>& outAlpha, int cellWidth,
                      int cellHeight, UINT dpi, int fontWeight) {
    cellWidth = std::max(1, cellWidth);
    cellHeight = std::max(1, cellHeight);
    const int atlasWidth = kGlyphAtlasCols * cellWidth;
    const int atlasHeight = kGlyphAtlasRows * cellHeight;
    const size_t alphaBytes =
        static_cast<size_t>(atlasWidth) * static_cast<size_t>(atlasHeight);
    outAlpha.assign(alphaBytes, static_cast<uint8_t>(0));

    HDC memDC = CreateCompatibleDC(nullptr);
    if (!memDC) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = atlasWidth;
    bmi.bmiHeader.biHeight = -atlasHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HBITMAP dib =
        CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!dib || !dibBits) {
        if (dib) DeleteObject(dib);
        DeleteDC(memDC);
        return false;
    }

    HGDIOBJ oldBmp = SelectObject(memDC, dib);
    std::memset(dibBits, 0, alphaBytes * 4u);
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));
    SetTextAlign(memDC, TA_LEFT | TA_TOP | TA_NOUPDATECP);

    HFONT font = createRadioifyTerminalFontForDpi(
        dpi, std::clamp(fontWeight, 1, 1000));
    const bool ownsFont = font != nullptr;
    if (!font) {
        font = static_cast<HFONT>(GetStockObject(SYSTEM_FIXED_FONT));
    }
    HGDIOBJ oldFont = SelectObject(memDC, font);

    for (int index = 0; index < kGlyphAtlasCols * kGlyphAtlasRows; ++index) {
        const wchar_t ch = glyphForAtlasIndex(index);
        const int col = index % kGlyphAtlasCols;
        const int row = index / kGlyphAtlasCols;
        RECT rc{col * cellWidth, row * cellHeight, (col + 1) * cellWidth,
                (row + 1) * cellHeight};
        ExtTextOutW(memDC, rc.left, rc.top, ETO_CLIPPED, &rc, &ch, 1,
                    nullptr);
    }

    const uint8_t* src = static_cast<const uint8_t*>(dibBits);
    for (size_t i = 0; i < alphaBytes; ++i) {
        const uint8_t b = src[i * 4u + 0u];
        const uint8_t g = src[i * 4u + 1u];
        const uint8_t r = src[i * 4u + 2u];
        outAlpha[i] = std::max({r, g, b});
    }

    if (oldFont) SelectObject(memDC, oldFont);
    if (ownsFont && font) DeleteObject(font);
    if (oldBmp) SelectObject(memDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDC);
    return true;
}
}  // namespace

UINT VideoWindow::TextGridDpi() const {
    if (m_hWnd) {
        const UINT dpi = GetDpiForWindow(m_hWnd);
        if (dpi != 0) return dpi;
    }
    return USER_DEFAULT_SCREEN_DPI;
}

SIZE VideoWindow::TextGridCellSize() const {
    const RadioifyTerminalFontMetrics metrics =
        measureTerminalFontMetrics(TextGridDpi());
    return SIZE{metrics.cellWidth, metrics.cellHeight};
}

void VideoWindow::GetPictureInPictureTextCellSize(int& outCellWidth,
                                                  int& outCellHeight) const {
    const SIZE cellSize = TextGridCellSize();
    outCellWidth = std::max(1, static_cast<int>(cellSize.cx));
    outCellHeight = std::max(1, static_cast<int>(cellSize.cy));
}

bool VideoWindow::EnsureGpuTextGridConstants(ID3D11Device* device) {
    if (m_gpuTextGridConstants) return true;
    if (!device) return false;

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(GpuTextGridConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device->CreateBuffer(
        &cbDesc, nullptr, m_gpuTextGridConstants.GetAddressOf()));
}

bool VideoWindow::EnsureGpuTextGlyphAtlas(ID3D11Device* device, int cellWidth,
                                          int cellHeight, UINT dpi,
                                          int fontWeight) {
    cellWidth = std::max(1, cellWidth);
    cellHeight = std::max(1, cellHeight);
    dpi = std::max<UINT>(1, dpi);
    fontWeight = std::clamp(fontWeight, 1, 1000);
    if (m_gpuTextGlyphAtlasSrv &&
        m_gpuTextGlyphAtlasCellWidth == cellWidth &&
        m_gpuTextGlyphAtlasCellHeight == cellHeight &&
        m_gpuTextGlyphAtlasDpi == dpi &&
        m_gpuTextGlyphAtlasWeight == fontWeight) {
        return true;
    }
    if (!device) return false;

    std::vector<uint8_t> alpha;
    if (!renderGlyphAtlas(alpha, cellWidth, cellHeight, dpi, fontWeight)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(kGlyphAtlasCols * cellWidth);
    td.Height = static_cast<UINT>(kGlyphAtlasRows * cellHeight);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = alpha.data();
    init.SysMemPitch = td.Width;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&td, &init, &texture)) || !texture) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv)) ||
        !srv) {
        return false;
    }

    m_gpuTextGlyphAtlasTexture = texture;
    m_gpuTextGlyphAtlasSrv = srv;
    m_gpuTextGlyphAtlasCellWidth = cellWidth;
    m_gpuTextGlyphAtlasCellHeight = cellHeight;
    m_gpuTextGlyphAtlasDpi = dpi;
    m_gpuTextGlyphAtlasWeight = fontWeight;
    return true;
}

bool VideoWindow::DrawGpuTextGridFrame(ID3D11Device* device,
                                       ID3D11DeviceContext* context,
                                       const GpuTextGridFrame& frame,
                                       const D3D11_VIEWPORT& viewport) {
    if (!device || !context || frame.cols <= 0 || frame.rows <= 0) {
        return false;
    }
    const size_t cellCount =
        static_cast<size_t>(frame.cols) * static_cast<size_t>(frame.rows);
    if (frame.cells.size() < cellCount) return false;

    const UINT dpi = TextGridDpi();
    const RadioifyTerminalFontMetrics terminalFont =
        measureTerminalFontMetrics(dpi);
    const int cellWidth = terminalFont.cellWidth;
    const int cellHeight = terminalFont.cellHeight;
    const int fontWeight = terminalFont.fontWeight;

    if (!EnsureGpuTextGlyphAtlas(device, cellWidth, cellHeight, dpi,
                                 fontWeight)) {
        return false;
    }
    if (!EnsureGpuTextGridConstants(device)) return false;
    if (!m_renderTargetView || !m_vertexShader || !m_gpuTextGridShader) {
        return false;
    }

    if (!m_gpuTextGridTexture || !m_gpuTextGridSrv ||
        m_gpuTextGridCols != frame.cols || m_gpuTextGridRows != frame.rows) {
        m_gpuTextGridTexture.Reset();
        m_gpuTextGridSrv.Reset();
        m_gpuTextGridCols = 0;
        m_gpuTextGridRows = 0;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(frame.cols);
        td.Height = static_cast<UINT>(frame.rows);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device->CreateTexture2D(&td, nullptr,
                                           &m_gpuTextGridTexture)) ||
            !m_gpuTextGridTexture) {
            return false;
        }
        if (FAILED(device->CreateShaderResourceView(
                m_gpuTextGridTexture.Get(), nullptr, &m_gpuTextGridSrv)) ||
            !m_gpuTextGridSrv) {
            m_gpuTextGridTexture.Reset();
            return false;
        }
        m_gpuTextGridCols = frame.cols;
        m_gpuTextGridRows = frame.rows;
    }

    D3D11_BOX box{0, 0, 0, static_cast<UINT>(frame.cols),
                  static_cast<UINT>(frame.rows), 1};
    context->UpdateSubresource(
        m_gpuTextGridTexture.Get(), 0, &box, frame.cells.data(),
        static_cast<UINT>(frame.cols * sizeof(GpuTextGridCell)), 0);

    D3D11_MAPPED_SUBRESOURCE mappedConstants{};
    if (SUCCEEDED(context->Map(m_gpuTextGridConstants.Get(), 0,
                               D3D11_MAP_WRITE_DISCARD, 0,
                               &mappedConstants))) {
        GpuTextGridConstants constants{};
        constants.glyphCellWidth = static_cast<uint32_t>(cellWidth);
        constants.glyphCellHeight = static_cast<uint32_t>(cellHeight);
        constants.glyphAtlasCols = kGlyphAtlasCols;
        std::memcpy(mappedConstants.pData, &constants, sizeof(constants));
        context->Unmap(m_gpuTextGridConstants.Get(), 0);
    }

    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->PSSetShader(m_gpuTextGridShader.Get(), nullptr, 0);
    ID3D11Buffer* constantBuffers[1] = {m_gpuTextGridConstants.Get()};
    context->PSSetConstantBuffers(0, 1, constantBuffers);
    ID3D11ShaderResourceView* srvs[2] = {m_gpuTextGridSrv.Get(),
                                         m_gpuTextGlyphAtlasSrv.Get()};
    context->PSSetShaderResources(0, 2, srvs);
    context->Draw(4, 0);

    ID3D11ShaderResourceView* nullSrvs[2] = {nullptr, nullptr};
    context->PSSetShaderResources(0, 2, nullSrvs);
    ID3D11Buffer* nullBuffers[1] = {nullptr};
    context->PSSetConstantBuffers(0, 1, nullBuffers);
    return true;
}

void VideoWindow::PresentGpuTextGrid(const GpuTextGridFrame& frame,
                                     bool nonBlocking) {
    std::unique_lock<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd || !m_swapChain || !IsWindowVisible(m_hWnd)) return;
    if (frame.cols <= 0 || frame.rows <= 0) return;
    m_pictureInPictureGridCols.store(frame.cols, std::memory_order_relaxed);
    m_pictureInPictureGridRows.store(frame.rows, std::memory_order_relaxed);
    const size_t cellCount =
        static_cast<size_t>(frame.cols) * static_cast<size_t>(frame.rows);
    if (frame.cells.size() < cellCount) return;

    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain = m_swapChain;
    UINT presentInterval = m_presentInterval.load(std::memory_order_relaxed);

    RECT rect;
    if (GetClientRect(m_hWnd, &rect)) {
        m_width = rect.right - rect.left;
        m_height = rect.bottom - rect.top;
    }
    if (m_width <= 0 || m_height <= 0) return;

    const SIZE cellSize = TextGridCellSize();
    const int cellWidth = std::max(1, static_cast<int>(cellSize.cx));
    const int cellHeight = std::max(1, static_cast<int>(cellSize.cy));

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context || !m_renderTargetView) return;

    const float clearColor[4] = {0.018f, 0.022f, 0.026f, 1.0f};
    context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
    context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);

    const int gridPixelWidth =
        std::min(m_width, frame.cols * cellWidth);
    const int gridPixelHeight =
        std::min(m_height, frame.rows * cellHeight);
    D3D11_VIEWPORT viewport = {
        0.0f, 0.0f, static_cast<float>(gridPixelWidth),
        static_cast<float>(gridPixelHeight), 0.0f, 1.0f};
    if (!DrawGpuTextGridFrame(device, context.Get(), frame, viewport)) return;
    DrawPictureInPictureBorder(context.Get());

    lock.unlock();
    if (!swapChain) return;
    UINT flags = nonBlocking ? DXGI_PRESENT_DO_NOT_WAIT : 0u;
    (void)swapChain->Present(presentInterval, flags);
}
