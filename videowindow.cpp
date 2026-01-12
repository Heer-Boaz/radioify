#include "videowindow.h"
#include "gpu_shared.h"
#include <iostream>
#include <vector>
#include <mutex>
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
        float progress;
        float overlayAlpha;
        uint32_t isPaused;
        uint32_t hasRGBA;
    };

    static_assert((sizeof(ShaderConstants) % 16) == 0, "ShaderConstants size must be 16-byte aligned");

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
            uint uiHasRGBA;
        };

        PS_INPUT VS(uint vid : SV_VertexID) {
            PS_INPUT output;
            output.tex = float2(vid & 1, vid >> 1);
            output.pos = float4(output.tex * float2(2, -2) + float2(-1, 1), 0, 1);
            return output;
        }

        Texture2D texY : register(t0);
        Texture2D texUV : register(t1);
        Texture2D texRGBA : register(t2);
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
            if (uiHasRGBA != 0) {
                float4 c = texRGBA.Sample(sam, input.tex);
                return float4(saturate(c.rgb), 1.0);
            }

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
            bool hit = false;

            // Progress bar at bottom (only UI element from console)
            if (uv.y > 0.95 && uv.y < 0.99 && uv.x > 0.02 && uv.x < 0.98) {
                float barX = (uv.x - 0.02) / 0.96;
                if (barX < uiProgress) color = float4(1, 0.8, 0.2, 0.9);
                else color = float4(0.3, 0.3, 0.3, 0.7);
                hit = true;
            }

            // Pause icon (center)
            if (uiPaused != 0) {
                float2 c = float2(0.5, 0.5);
                float2 d = abs(uv - c);
                if (d.y < 0.06 && ((d.x > 0.015 && d.x < 0.025) || (d.x > 0.035 && d.x < 0.045))) {
                    color = float4(1, 1, 1, 0.9);
                    hit = true;
                }
            }

            if (!hit) discard;
            return color * uiAlpha;
        }
    )";
}

VideoWindow::VideoWindow() {}

VideoWindow::~VideoWindow() {
    Close();
}

void VideoWindow::Cleanup() {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) return;

    // Unbind shader resources / UAVs / RTVs and clear state to avoid driver pinning
    ID3D11ShaderResourceView* nullSRVs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    context->PSSetShaderResources(0, 5, nullSRVs);
    context->CSSetShaderResources(0, 5, nullSRVs);

    ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
    context->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);

    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(0, &nullRTV, nullptr);

    context->VSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);

    // Force clear/flush to ensure driver releases any references
    context->ClearState();
    context->Flush();
    context->Release();

    // Reset COM objects we hold
    m_renderTargetView.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    m_uiShader.Reset();
    m_sampler.Reset();
    m_constantBuffer.Reset();
    m_copySrvY.Reset();
    m_copySrvUV.Reset();
    m_copyTexture.Reset();
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
            ::ShowWindow(pThis->m_hWnd, SW_HIDE);
            return 0;
        }
        if (uMsg == WM_DESTROY) {
            pThis->m_hWnd = nullptr;
            return 0;
        }

        if (uMsg == WM_KEYDOWN) {
            InputEvent ev;
            ev.type = InputEvent::Type::Key;
            ev.key.vk = (WORD)wParam;
            
            // Map VK to ASCII for common keys if possible
            if (wParam >= 'A' && wParam <= 'Z') ev.key.ch = (char)wParam;
            else if (wParam == VK_SPACE) ev.key.ch = ' ';
            else if (wParam == VK_ESCAPE) ev.key.ch = 27;
            else if (wParam == VK_OEM_4) ev.key.ch = '[';
            else if (wParam == VK_OEM_6) ev.key.ch = ']';

            if (GetKeyState(VK_CONTROL) & 0x8000) ev.key.control |= LEFT_CTRL_PRESSED;
            if (GetKeyState(VK_SHIFT) & 0x8000) ev.key.control |= SHIFT_PRESSED;
            if (GetKeyState(VK_MENU) & 0x8000) ev.key.control |= LEFT_ALT_PRESSED;

            std::lock_guard<std::mutex> lock(pThis->m_inputMutex);
            pThis->m_inputQueue.push_back(ev);
            return 0;
        }

        if (uMsg == WM_LBUTTONDOWN || uMsg == WM_MOUSEMOVE) {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            bool leftPressed = (wParam & MK_LBUTTON) != 0;
            
            // Mouse seeking in bottom area (like terminal)
            if (leftPressed && pThis->m_height > 0 && y > pThis->m_height * 0.90) {
                InputEvent ev;
                ev.type = InputEvent::Type::Mouse;
                ev.mouse.pos.X = (SHORT)x;
                ev.mouse.pos.Y = (SHORT)y;
                ev.mouse.buttonState = FROM_LEFT_1ST_BUTTON_PRESSED;
                ev.mouse.eventFlags = (uMsg == WM_MOUSEMOVE) ? MOUSE_MOVED : 0;
                ev.mouse.control = 0x80000000; // Custom flag for window-originated event
                
                std::lock_guard<std::mutex> lock(pThis->m_inputMutex);
                pThis->m_inputQueue.push_back(ev);
            }
            return 0;
        }
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

bool VideoWindow::Open(int width, int height, const std::string& title) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
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
    if (!device) {
        std::fprintf(stderr, "VideoWindow: no device in Open()\n");
        return false;
    }

    // Ensure multithread protection if available (ascii renderer enables this)
    {
        Microsoft::WRL::ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
            mt->SetMultithreadProtected(TRUE);
        }
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, uiBlob, errorBlob;
    HRESULT hr = D3DCompile(g_shaderSource, strlen(g_shaderSource), NULL, NULL, NULL, "VS", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            std::fprintf(stderr, "VideoWindow: VS compile error: %s\n", (const char*)errorBlob->GetBufferPointer());
        }
        return false;
    }
    hr = D3DCompile(g_shaderSource, strlen(g_shaderSource), NULL, NULL, NULL, "PS", "ps_5_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) std::fprintf(stderr, "VideoWindow: PS compile error: %s\n", (const char*)errorBlob->GetBufferPointer());
        return false;
    }
    hr = D3DCompile(g_shaderSource, strlen(g_shaderSource), NULL, NULL, NULL, "PS_UI", "ps_5_0", 0, 0, &uiBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) std::fprintf(stderr, "VideoWindow: PS_UI compile error: %s\n", (const char*)errorBlob->GetBufferPointer());
        return false;
    }

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &m_vertexShader);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreateVertexShader failed (0x%08X)\n", static_cast<unsigned int>(hr)); return false; }
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &m_pixelShader);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreatePixelShader(PS) failed (0x%08X)\n", static_cast<unsigned int>(hr)); return false; }
    hr = device->CreatePixelShader(uiBlob->GetBufferPointer(), uiBlob->GetBufferSize(), NULL, &m_uiShader);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreatePixelShader(UI) failed (0x%08X)\n", static_cast<unsigned int>(hr)); return false; }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(ShaderConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbDesc, NULL, &m_constantBuffer);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreateBuffer(constant) failed (0x%08X)\n", static_cast<unsigned int>(hr)); Cleanup(); return false; }

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampDesc, &m_sampler);
    if (FAILED(hr)) { std::fprintf(stderr, "VideoWindow: CreateSamplerState failed (0x%08X)\n", static_cast<unsigned int>(hr)); Cleanup(); return false; }

    ::ShowWindow(m_hWnd, SW_SHOW);
    m_width = width;
    m_height = height;

    return true;
}

bool VideoWindow::CreateSwapChain(int width, int height) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    ID3D11Device* device = getSharedGpuDevice();
    if (!device) {
        std::fprintf(stderr, "VideoWindow: no shared GPU device available\n");
        return false;
    }
    if (width <= 0 || height <= 0) {
        std::fprintf(stderr, "VideoWindow: invalid swapchain dimensions %d x %d\n", width, height);
        return false;
    }
    
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
    if (FAILED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory))) || !dxgiFactory) {
        std::fprintf(stderr, "VideoWindow: failed to get DXGI factory\n");
        return false;
    }

    if (FAILED(dxgiFactory->CreateSwapChain(device, &scd, &m_swapChain))) {
        std::fprintf(stderr, "VideoWindow: CreateSwapChain failed\n");
        return false;
    }

    Resize(width, height);
    return true;
}

void VideoWindow::Resize(int width, int height) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_swapChain) return;
    m_renderTargetView.Reset();
    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) return;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) return;

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;
    hr = device->CreateRenderTargetView(backBuffer.Get(), NULL, &m_renderTargetView);
    if (FAILED(hr)) {
        m_renderTargetView.Reset();
        return;
    }

    m_width = width;
    m_height = height;
}

void VideoWindow::Close() {
    // Hide the window first to release focus/ownership of the monitor
    if (m_hWnd) {
        ::ShowWindow(m_hWnd, SW_HIDE);
    }
    // Perform centralized cleanup (unbind, ClearState, flush, reset local resources)
    Cleanup();

    // If swapchain is in full-screen exclusive, revert to windowed first
    if (m_swapChain) {
        // Best-effort: leave fullscreen and present once so the driver releases surfaces
        (void)m_swapChain->SetFullscreenState(FALSE, NULL);
        IDXGISwapChain* raw = m_swapChain.Get();
        if (raw) {
            raw->Present(0, 0);
        }
    }

    // Release swapchain last
    m_swapChain.Reset();

    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
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

bool VideoWindow::PollInput(InputEvent& ev) {
    std::lock_guard<std::mutex> lock(m_inputMutex);
    if (m_inputQueue.empty()) return false;
    ev = m_inputQueue.front();
    m_inputQueue.erase(m_inputQueue.begin());
    return true;
}

void VideoWindow::Present(ID3D11Texture2D* texture, int arrayIndex, int width, int height,
                           bool fullRange, YuvMatrix matrix, YuvTransfer transfer, int bitDepth,
                           const WindowUiState& ui) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    if (!m_hWnd || !m_swapChain || !IsWindowVisible(m_hWnd)) return;

    ID3D11Device* device = getSharedGpuDevice();
    if (!device) return;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context || !m_constantBuffer) return;
    if (!m_renderTargetView) return;
    
    // Update of constant buffer moved below (after determining texture format such as RGBA)

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

    if (!texture) return;
    D3D11_TEXTURE2D_DESC srcDesc;
    texture->GetDesc(&srcDesc);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvY, srvUV, srvRGBA;
    bool is10Bit = (srcDesc.Format == DXGI_FORMAT_P010);
    bool isRGBA = (srcDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || srcDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || srcDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

    // Update constants (now that we know if RGBA path is requested)
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
            sc->hasRGBA = isRGBA ? 1u : 0u;
            context->Unmap(m_constantBuffer.Get(), 0);
        }
    }

    if (isRGBA) {
        if (srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = srcDesc.Format;
            if (srcDesc.ArraySize > 1) {
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                srvDesc.Texture2DArray.FirstArraySlice = arrayIndex;
                srvDesc.Texture2DArray.ArraySize = 1;
                srvDesc.Texture2DArray.MipLevels = 1;
            } else {
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;
            }
            HRESULT hrRGBA = device->CreateShaderResourceView(texture, &srvDesc, &srvRGBA);
            if (FAILED(hrRGBA) || !srvRGBA) {
                std::fprintf(stderr, "VideoWindow: CreateShaderResourceView(RGBA) failed (0x%08X), falling back to copy path\n", static_cast<unsigned int>(hrRGBA));
                srvRGBA.Reset();
            }
        }
    }

    if (!isRGBA && (srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
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
        HRESULT hrY = device->CreateShaderResourceView(texture, &srvDescY, &srvY);
        if (FAILED(hrY) || !srvY) {
            // Fallback to copy path if SRV creation fails
            std::fprintf(stderr, "VideoWindow: CreateShaderResourceView(Y) failed (0x%08X), falling back to copy path\n", static_cast<unsigned int>(hrY));
            srvY.Reset();
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = srvDescY;
        srvDescUV.Format = is10Bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        HRESULT hrUV = device->CreateShaderResourceView(texture, &srvDescUV, &srvUV);
        if (FAILED(hrUV) || !srvUV) {
            std::fprintf(stderr, "VideoWindow: CreateShaderResourceView(UV) failed (0x%08X), falling back to copy path\n", static_cast<unsigned int>(hrUV));
            srvUV.Reset();
        }

        if (!srvY || !srvUV) {
            // Force path through copy texture below
            srvY.Reset(); srvUV.Reset();
        }
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
            // Validate dimensions to avoid enormous allocations / driver issues
            const uint32_t MAX_COPY_DIM = 8192u; // conservative limit
            if (srcDesc.Width == 0 || srcDesc.Height == 0 || srcDesc.Width > MAX_COPY_DIM || srcDesc.Height > MAX_COPY_DIM) {
                std::fprintf(stderr, "VideoWindow: refusing to create copy texture with dimensions %u x %u\n", srcDesc.Width, srcDesc.Height);
                return;
            }

            // Unbind any SRVs that might reference the old texture before resetting
            ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
            context->PSSetShaderResources(0, 3, nullSRVs);
            context->Flush();

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
            HRESULT hr = device->CreateTexture2D(&cDesc, NULL, &m_copyTexture);
            if (FAILED(hr) || !m_copyTexture) {
                std::fprintf(stderr, "VideoWindow: CreateTexture2D(copy) failed (0x%08X)\n", static_cast<unsigned int>(hr));
                return;
            }

            if (isRGBA) {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDescRGBA = {};
                srvDescRGBA.Format = srcDesc.Format;
                srvDescRGBA.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDescRGBA.Texture2D.MipLevels = 1;
                hr = device->CreateShaderResourceView(m_copyTexture.Get(), &srvDescRGBA, &m_copySrvRGBA);
                if (FAILED(hr)) {
                    std::fprintf(stderr, "VideoWindow: CreateShaderResourceView(RGBA copy) failed (0x%08X)\n", static_cast<unsigned int>(hr));
                    m_copyTexture.Reset();
                    return;
                }
            } else {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
                srvDescY.Format = is10Bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
                srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDescY.Texture2D.MipLevels = 1;
                hr = device->CreateShaderResourceView(m_copyTexture.Get(), &srvDescY, &m_copySrvY);
                if (FAILED(hr)) {
                    std::fprintf(stderr, "VideoWindow: CreateShaderResourceView(Y) failed (0x%08X)\n", static_cast<unsigned int>(hr));
                    m_copyTexture.Reset();
                    return;
                }

                D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = srvDescY;
                srvDescUV.Format = is10Bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
                hr = device->CreateShaderResourceView(m_copyTexture.Get(), &srvDescUV, &m_copySrvUV);
                if (FAILED(hr)) {
                    std::fprintf(stderr, "VideoWindow: CreateShaderResourceView(UV) failed (0x%08X)\n", static_cast<unsigned int>(hr));
                    m_copySrvY.Reset();
                    m_copyTexture.Reset();
                    return;
                }
            }
        }

        if (srcDesc.ArraySize > 1) {
            context->CopySubresourceRegion(m_copyTexture.Get(), 0, 0, 0, 0, texture, D3D11CalcSubresource(0, arrayIndex, 1), NULL);
        } else {
            context->CopyResource(m_copyTexture.Get(), texture);
        }
        // Ensure copy completes before sampling; flush to the driver so subsequent Draw samples updated data
        context->Flush();
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

    HRESULT hr = m_swapChain->Present(1, 0);

    // Unbind SRVs, samplers and constant buffers to avoid holding references across frames
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    context->PSSetShaderResources(0, 2, nullSRVs);
    context->CSSetShaderResources(0, 2, nullSRVs);
    ID3D11SamplerState* nullSamplers[1] = { nullptr };
    context->PSSetSamplers(0, 1, nullSamplers);
    ID3D11Buffer* nullCB[1] = { nullptr };
    context->PSSetConstantBuffers(0, 1, nullCB);

    if (FAILED(hr)) {
        std::fprintf(stderr, "VideoWindow: Present failed (0x%08X)\n", static_cast<unsigned int>(hr));
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            ID3D11Device* device = getSharedGpuDevice();
            if (device) {
                HRESULT reason = device->GetDeviceRemovedReason();
                std::fprintf(stderr, "VideoWindow: device removed/reset reason=0x%08X\n", static_cast<unsigned int>(reason));
            }
            // Try to clean up GPU state and drop the swapchain to avoid hangs
            Cleanup();
            m_swapChain.Reset();
        }
    }
}
