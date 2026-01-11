#include "videowindow.h"
#include "gpu_shared.h"
#include <iostream>
#include <vector>
#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {
    struct ShaderConstants {
        uint32_t isFullRange;
        uint32_t yuvMatrix;
        uint32_t yuvTransfer;
        uint32_t bitDepth;
        // UI
        float progress;
        float overlayAlpha;
        uint32_t isPaused;
        uint32_t padding;
    };

    const char* g_shaderSource = R"(
        struct PS_INPUT {
            float4 pos : SV_POSITION;
            float2 tex : TEXCOORD;
        };

        cbuffer Constants : register(b0) {
            uint isFullRange;
            uint yuvMatrix;
            uint yuvTransfer;
            uint bitDepth;
            float uiProgress;
            float uiAlpha;
            uint uiPaused;
        };

        PS_INPUT VS(uint vid : SV_VertexID) {
            PS_INPUT output;
            output.tex = float2(vid & 1, vid >> 1);
            output.pos = float4(output.tex * float2(2, -2) + float2(-1, 1), 0, 1);
            return output;
        }

        Texture2D texY : register(t0);
        Texture2D texUV : register(t1);
        SamplerState sam : register(s0);

        float ExpandYNorm(float yNorm) {
            float maxCode = (float)((1u << bitDepth) - 1u);
            float yCode = yNorm * maxCode;
            uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
            float yMin = (isFullRange != 0) ? 0.0f : (float)(16u << shift);
            float yMax = (isFullRange != 0) ? maxCode : (float)(235u << shift);
            return saturate((yCode - yMin) / max(yMax - yMin, 1.0f));
        }

        float2 ExpandUV(float2 uvNorm) {
            float maxCode = (float)((1u << bitDepth) - 1u);
            float2 uvCode = uvNorm * maxCode;
            uint shift = (bitDepth > 8u) ? (bitDepth - 8u) : 0u;
            float cMid = (float)(128u << shift);
            float cMin = (isFullRange != 0) ? 0.0f : (float)(16u << shift);
            float cMax = (isFullRange != 0) ? maxCode : (float)(240u << shift);
            return (uvCode - cMid) / max(cMax - cMin, 1.0f);
        }

        float PQEotf(float v) {
            float m1 = 2610.0 / 16384.0;
            float m2 = 2523.0 / 32.0;
            float c1 = 3424.0 / 4096.0;
            float c2 = 2413.0 / 128.0;
            float c3 = 2392.0 / 128.0;
            float vp = pow(max(v, 0.0), 1.0 / m2);
            return pow(max(vp - c1, 0.0) / (c2 - c3 * vp), 1.0 / m1);
        }

        float HlgEotf(float v) {
            const float a = 0.17883277;
            const float b = 1.0 - 4.0 * a;
            const float c = 0.5 - a * log(4.0 * a);
            if (v <= 0.5) return (v * v) / 3.0;
            return (exp((v - c) / a) + b) / 12.0;
        }

        float ToneMapFilmic(float x) {
            const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
            return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
        }

        float LinearToSrgb(float v) {
            v = max(v, 0.0);
            return (v <= 0.0031308) ? (v * 12.92) : (1.055 * pow(v, 1.0 / 2.4) - 0.055);
        }

        float3 ApplyHdrToSdr(float3 v) {
            v = saturate(v);
            float3 linearRgb;
            if (yuvTransfer == 1) linearRgb = float3(PQEotf(v.r), PQEotf(v.g), PQEotf(v.b));
            else if (yuvTransfer == 2) linearRgb = float3(HlgEotf(v.r), HlgEotf(v.g), HlgEotf(v.b));
            else return v;
            
            float3 mapped = float3(ToneMapFilmic(linearRgb.r * 100.0), ToneMapFilmic(linearRgb.g * 100.0), ToneMapFilmic(linearRgb.b * 100.0));
            return float3(LinearToSrgb(mapped.r), LinearToSrgb(mapped.g), LinearToSrgb(mapped.b));
        }

        float4 PS(PS_INPUT input) : SV_Target {
            float y = ExpandYNorm(texY.Sample(sam, input.tex).r);
            float2 uv = ExpandUV(texUV.Sample(sam, input.tex).rg);
            
            float r, g, b;
            if (yuvMatrix == 2) { r = y + 1.4746 * uv.y; g = y - 0.16455 * uv.x - 0.57135 * uv.y; b = y + 1.8814 * uv.x; }
            else if (yuvMatrix == 1) { r = y + 1.4020 * uv.y; g = y - 0.3441 * uv.x - 0.7141 * uv.y; b = y + 1.7720 * uv.x; }
            else { r = y + 1.5748 * uv.y; g = y - 0.1873 * uv.x - 0.4681 * uv.y; b = y + 1.8556 * uv.x; }
            
            float3 rgb = float3(r, g, b);
            if (yuvTransfer != 0) rgb = ApplyHdrToSdr(rgb);
            return float4(saturate(rgb), 1.0);
        }

        float4 PS_UI(PS_INPUT input) : SV_Target {
            if (uiAlpha <= 0.01) discard;
            float2 uv = input.tex;
            float4 color = float4(0, 0, 0, 0);

            // Progress bar at bottom
            if (uv.y > 0.95 && uv.y < 0.98 && uv.x > 0.05 && uv.x < 0.95) {
                float barX = (uv.x - 0.05) / 0.9;
                if (barX < uiProgress) color = float4(1, 0.8, 0.2, 0.8);
                else color = float4(0.3, 0.3, 0.3, 0.6);
            }
            // Pause icon
            if (uiPaused != 0) {
                float2 c = float2(0.5, 0.5);
                float2 d = abs(uv - c);
                if (d.x < 0.02 && d.y < 0.05 && (abs(d.x) > 0.005)) color = float4(1, 1, 1, 0.8);
            }

            return color * uiAlpha;
        }
    )";
}

VideoWindow::VideoWindow() {}

VideoWindow::~VideoWindow() {
    Close();
}

LRESULT CALLBACK VideoWindow::WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    VideoWindow* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
        pThis = (VideoWindow*)pCreate->lpCreateParams;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)pThis);
    } else {
        pThis = (VideoWindow*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    }

    if (pThis) {
        if (uMsg == WM_SIZE) {
            pThis->Resize(LOWORD(lParam), HIWORD(lParam));
            return 0;
        }
        if (uMsg == WM_CLOSE) {
            pThis->ShowWindow(false);
            return 0;
        }
        if (uMsg == WM_DESTROY) {
            pThis->m_hWnd = nullptr;
            return 0;
        }
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

bool VideoWindow::Open(int width, int height, const std::string& title) {
    HINSTANCE hInstance = GetModuleHandle(NULL);
    const wchar_t* className = L"RadioifyVideoWindow";

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    if (!GetClassInfoExW(hInstance, className, &wc)) {
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassExW(&wc);
    }

    RECT wr = { 0, 0, width, height };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    m_hWnd = CreateWindowExW(
        0, className, std::wstring(title.begin(), title.end()).c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, hInstance, this);

    if (!m_hWnd) return false;

    if (!CreateSwapChain(width, height)) return false;

    ID3D11Device* device = getSharedGpuDevice();
    
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, uiBlob, errorBlob;
    if (FAILED(D3DCompile(g_shaderSource, strlen(g_shaderSource), NULL, NULL, NULL, "VS", "vs_5_0", 0, 0, &vsBlob, &errorBlob))) {
        return false;
    }
    if (FAILED(D3DCompile(g_shaderSource, strlen(g_shaderSource), NULL, NULL, NULL, "PS", "ps_5_0", 0, 0, &psBlob, &errorBlob))) {
        return false;
    }
    if (FAILED(D3DCompile(g_shaderSource, strlen(g_shaderSource), NULL, NULL, NULL, "PS_UI", "ps_5_0", 0, 0, &uiBlob, &errorBlob))) {
        return false;
    }

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &m_vertexShader);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &m_pixelShader);
    device->CreatePixelShader(uiBlob->GetBufferPointer(), uiBlob->GetBufferSize(), NULL, &m_uiShader);

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(ShaderConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cbDesc, NULL, &m_constantBuffer);

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sampDesc, &m_sampler);

    ::ShowWindow(m_hWnd, SW_SHOW);
    m_width = width;
    m_height = height;

    return true;
}

bool VideoWindow::CreateSwapChain(int width, int height) {
    ID3D11Device* device = getSharedGpuDevice();
    
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = m_hWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    
    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    dxgiDevice->GetAdapter(&dxgiAdapter);
    
    Microsoft::WRL::ComPtr<IDXGIFactory> dxgiFactory;
    dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));

    if (FAILED(dxgiFactory->CreateSwapChain(device, &scd, &m_swapChain))) {
        return false;
    }

    Resize(width, height);
    return true;
}

void VideoWindow::Resize(int width, int height) {
    if (!m_swapChain) return;

    m_renderTargetView.Reset();
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    
    ID3D11Device* device = getSharedGpuDevice();
    device->CreateRenderTargetView(backBuffer.Get(), NULL, &m_renderTargetView);
    
    m_width = width;
    m_height = height;
}

void VideoWindow::Close() {
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
    m_swapChain.Reset();
    m_renderTargetView.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    m_sampler.Reset();
    m_constantBuffer.Reset();
    m_copyTexture.Reset();
    m_copySrvY.Reset();
    m_copySrvUV.Reset();
}

void VideoWindow::ShowWindow(bool show) {
    if (m_hWnd) {
        ::ShowWindow(m_hWnd, show ? SW_SHOW : SW_HIDE);
    }
}

void VideoWindow::PollEvents() {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

void VideoWindow::Present(ID3D11Texture2D* texture, int arrayIndex, int width, int height,
                           bool fullRange, YuvMatrix matrix, YuvTransfer transfer, int bitDepth,
                           const WindowUiState& ui) {
    if (!m_hWnd || !m_swapChain || !IsWindowVisible(m_hWnd)) return;

    ID3D11Device* device = getSharedGpuDevice();
    ID3D11DeviceContext* context;
    device->GetImmediateContext(&context);
    
    // Update constants
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            ShaderConstants* sc = (ShaderConstants*)mapped.pData;
            sc->isFullRange = fullRange ? 1 : 0;
            sc->yuvMatrix = (uint32_t)matrix;
            sc->yuvTransfer = (uint32_t)transfer;
            sc->bitDepth = (uint32_t)bitDepth;
            sc->progress = ui.progress;
            sc->overlayAlpha = ui.overlayAlpha;
            sc->isPaused = ui.isPaused ? 1 : 0;
            context->Unmap(m_constantBuffer.Get(), 0);
        }
    }

    // Set up rendering
    float clearColor[4] = { 0, 0, 0, 1 };
    context->ClearRenderTargetView(m_renderTargetView.Get(), clearColor);
    context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), NULL);

    D3D11_VIEWPORT viewport = { 0, 0, (float)m_width, (float)m_height, 0, 1 };
    context->RSSetViewports(1, &viewport);

    context->IASetInputLayout(NULL);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(m_vertexShader.Get(), NULL, 0);
    context->PSSetShader(m_pixelShader.Get(), NULL, 0);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

    D3D11_TEXTURE2D_DESC srcDesc;
    texture->GetDesc(&srcDesc);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvY, srvUV;
    bool is10Bit = (srcDesc.Format == DXGI_FORMAT_P010);

    if (srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
        srvDescY.Format = is10Bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        if (srcDesc.ArraySize > 1) {
            srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDescY.Texture2DArray.FirstArraySlice = arrayIndex;
            srvDescY.Texture2DArray.ArraySize = 1;
            srvDescY.Texture2DArray.MipLevels = 1;
        } else {
            srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDescY.Texture2D.MipLevels = 1;
        }
        device->CreateShaderResourceView(texture, &srvDescY, &srvY);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = srvDescY;
        srvDescUV.Format = is10Bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        device->CreateShaderResourceView(texture, &srvDescUV, &srvUV);
    } else {
        // Must copy to an SRV-enabled texture
        bool needNewCopy = !m_copyTexture;
        if (m_copyTexture) {
            D3D11_TEXTURE2D_DESC cDesc;
            m_copyTexture->GetDesc(&cDesc);
            if (cDesc.Width != srcDesc.Width || cDesc.Height != srcDesc.Height || cDesc.Format != srcDesc.Format) {
                needNewCopy = true;
            }
        }

        if (needNewCopy) {
            m_copyTexture.Reset();
            m_copySrvY.Reset();
            m_copySrvUV.Reset();
            D3D11_TEXTURE2D_DESC cDesc = {};
            cDesc.Width = srcDesc.Width;
            cDesc.Height = srcDesc.Height;
            cDesc.MipLevels = 1;
            cDesc.ArraySize = 1;
            cDesc.Format = srcDesc.Format;
            cDesc.SampleDesc.Count = 1;
            cDesc.Usage = D3D11_USAGE_DEFAULT;
            cDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&cDesc, NULL, &m_copyTexture))) return;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
            srvDescY.Format = is10Bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
            srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDescY.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(m_copyTexture.Get(), &srvDescY, &m_copySrvY);

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = srvDescY;
            srvDescUV.Format = is10Bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
            device->CreateShaderResourceView(m_copyTexture.Get(), &srvDescUV, &m_copySrvUV);
        }

        if (srcDesc.ArraySize > 1) {
            context->CopySubresourceRegion(m_copyTexture.Get(), 0, 0, 0, 0, texture, D3D11CalcSubresource(0, arrayIndex, 1), NULL);
        } else {
            context->CopyResource(m_copyTexture.Get(), texture);
        }
        srvY = m_copySrvY;
        srvUV = m_copySrvUV;
    }

    if (srvY && srvUV) {
        ID3D11ShaderResourceView* srvs[] = { srvY.Get(), srvUV.Get() };
        context->PSSetShaderResources(0, 2, srvs);
        context->Draw(4, 0);

        // Draw UI
        if (ui.overlayAlpha > 0.01f) {
            context->PSSetShader(m_uiShader.Get(), NULL, 0);
            context->Draw(4, 0);
        }
    }

    m_swapChain->Present(1, 0);
    context->Release();
}
