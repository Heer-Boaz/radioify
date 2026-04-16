#include "videowindow.h"

#include "gpu_shared.h"

void VideoWindow::PresentGpuTextGrid(const GpuTextGridFrame& frame,
                                     bool nonBlocking) {
    std::unique_lock<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd || !m_swapChain || !IsWindowVisible(m_hWnd)) return;
    if (frame.cols <= 0 || frame.rows <= 0) return;
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

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context || !m_renderTargetView || !m_vertexShader ||
        !m_gpuTextGridShader) {
        return;
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
        td.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device->CreateTexture2D(&td, nullptr,
                                           &m_gpuTextGridTexture)) ||
            !m_gpuTextGridTexture) {
            return;
        }
        if (FAILED(device->CreateShaderResourceView(
                m_gpuTextGridTexture.Get(), nullptr, &m_gpuTextGridSrv)) ||
            !m_gpuTextGridSrv) {
            m_gpuTextGridTexture.Reset();
            return;
        }
        m_gpuTextGridCols = frame.cols;
        m_gpuTextGridRows = frame.rows;
    }

    D3D11_BOX box{0, 0, 0, static_cast<UINT>(frame.cols),
                  static_cast<UINT>(frame.rows), 1};
    context->UpdateSubresource(
        m_gpuTextGridTexture.Get(), 0, &box, frame.cells.data(),
        static_cast<UINT>(frame.cols * sizeof(GpuTextGridCell)), 0);

    const float clearColor[4] = {0.018f, 0.022f, 0.026f, 1.0f};
    context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
    context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);

    D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(m_width),
                               static_cast<float>(m_height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    context->PSSetShader(m_gpuTextGridShader.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, m_gpuTextGridSrv.GetAddressOf());
    context->Draw(4, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->PSSetShaderResources(0, 1, &nullSrv);

    lock.unlock();
    if (!swapChain) return;
    UINT flags = nonBlocking ? DXGI_PRESENT_DO_NOT_WAIT : 0u;
    (void)swapChain->Present(presentInterval, flags);
}
