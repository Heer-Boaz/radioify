#include "videowindow.h"

#include "gpu_shared.h"
#include "text_grid_bitmap_renderer.h"
#include "videowindow_internal.h"

#include <vector>

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
    if (!renderScreenGridToBitmap(cells.data(), cols, rows, m_width, m_height,
                                  &rgbaPixels)) {
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
