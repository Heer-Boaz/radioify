#include "videoprocessor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>

#include "timing_log.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>
#include <cstdio>

#if RADIOIFY_ENABLE_TIMING_LOG
#define RADIOIFY_TIMING_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define RADIOIFY_TIMING_LOG(...) do { } while (0)
#endif

#if RADIOIFY_ENABLE_VIDEO_ERROR_LOG
#define RADIOIFY_VIDEO_ERROR_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define RADIOIFY_VIDEO_ERROR_LOG(...) do { } while (0)
#endif

static inline std::string now_ms() {
    using namespace std::chrono;
    auto t = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    std::ostringstream ss; ss << t; return ss.str();
}
static inline std::string thread_id_str() {
    std::ostringstream ss; ss << std::this_thread::get_id(); return ss.str();
}

VideoViewport calculateViewport(int windowW, int windowH, int videoW, int videoH) {
    if (windowW <= 0 || windowH <= 0 || videoW <= 0 || videoH <= 0) {
        return { 0.0f, 0.0f, static_cast<float>(windowW), static_cast<float>(windowH) };
    }

    float windowAspect = (float)windowW / (float)windowH;
    float videoAspect = (float)videoW / (float)videoH;

    VideoViewport vp;
    if (windowAspect > videoAspect) {
        vp.h = (float)windowH;
        vp.w = vp.h * videoAspect;
        vp.x = (windowW - vp.w) / 2.0f;
        vp.y = 0;
    } else {
        vp.w = (float)windowW;
        vp.h = vp.w / videoAspect;
        vp.x = 0;
        vp.y = (windowH - vp.h) / 2.0f;
    }
    return vp;
}

bool GpuVideoFrameCache::EnsureGpuQueries(ID3D11Device* device) {
    if (!device) return false;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;
    qd.MiscFlags = 0;
    for (int i = 0; i < kFrameBufferCount; ++i) {
        if (!m_gpuDone[i]) {
            if (FAILED(device->CreateQuery(&qd, m_gpuDone[i].GetAddressOf()))) {
                for (int j = 0; j < kFrameBufferCount; ++j) {
                    m_gpuDone[j].Reset();
                }
                return false;
            }
        }
    }
    return true;
}

bool GpuVideoFrameCache::IsBufferReady(ID3D11DeviceContext* context, int index) {
    if (index < 0 || index >= kFrameBufferCount) return false;
    if (!m_gpuInFlight[index]) return true;
    if (!context || !m_gpuDone[index]) return false;
    HRESULT hr = context->GetData(m_gpuDone[index].Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hr == S_OK) {
        m_gpuInFlight[index] = false;
        return true;
    }
    return false;
}

int GpuVideoFrameCache::AcquireWriteIndex(ID3D11DeviceContext* context) {
    if (!context) return -1;
    for (int i = 0; i < kFrameBufferCount; ++i) {
        int index = (m_writeIndex + i) % kFrameBufferCount;
        if (IsBufferReady(context, index)) return index;
    }
    return -1;
}

void GpuVideoFrameCache::MarkFrameInFlight(ID3D11DeviceContext* context) {
    if (!context || m_format == CacheFormat::None) return;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    if (!device) return;
    if (!EnsureGpuQueries(device.Get())) return;
    int index = m_activeIndex;
    if (index < 0 || index >= kFrameBufferCount) return;
    context->End(m_gpuDone[index].Get());
    m_gpuInFlight[index] = true;
}

void GpuVideoFrameCache::InitFrameLatencyFence(ID3D11Device* device) {
    if (!device) return;
    std::lock_guard<std::mutex> lock(m_frameLatencyMutex);
    if (m_frameLatencyFence) return;
    Microsoft::WRL::ComPtr<ID3D11Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5)))) return;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    HRESULT hr = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE,
                                      IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return;
    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!evt) return;
    m_frameLatencyFence = fence;
    m_frameLatencyFenceEvent = evt;
    m_frameLatencyFenceValue = 0;
}

void GpuVideoFrameCache::ResetFrameLatencyFence() {
    std::lock_guard<std::mutex> lock(m_frameLatencyMutex);
    if (m_frameLatencyFenceEvent) {
        CloseHandle(reinterpret_cast<HANDLE>(m_frameLatencyFenceEvent));
        m_frameLatencyFenceEvent = nullptr;
    }
    m_frameLatencyFence.Reset();
    m_frameLatencyFenceValue = 0;
}

void GpuVideoFrameCache::WaitForFrameLatency(uint32_t timeoutMs,
                                             FrameLatencyWaitHandle waitableHandle) {
    HANDLE waitHandle = reinterpret_cast<HANDLE>(waitableHandle);
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    HANDLE fenceEvent = nullptr;
    uint64_t fenceValue = 0;
    {
        std::lock_guard<std::mutex> lock(m_frameLatencyMutex);
        fence = m_frameLatencyFence;
        fenceEvent = reinterpret_cast<HANDLE>(m_frameLatencyFenceEvent);
        fenceValue = m_frameLatencyFenceValue;
    }
    if (waitHandle) {
        WaitForSingleObject(waitHandle, timeoutMs);
        return;
    }
    if (!fence || !fenceEvent || fenceValue == 0) return;
    if (fence->GetCompletedValue() < fenceValue) {
        fence->SetEventOnCompletion(fenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, timeoutMs);
    }
}

void GpuVideoFrameCache::SignalFrameLatencyFence(ID3D11DeviceContext* context) {
    if (!context) return;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    InitFrameLatencyFence(device.Get());

    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context4)))) return;

    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    uint64_t signalValue = 0;
    {
        std::lock_guard<std::mutex> lock(m_frameLatencyMutex);
        if (!m_frameLatencyFence) return;
        signalValue = ++m_frameLatencyFenceValue;
        fence = m_frameLatencyFence;
    }
    context4->Signal(fence.Get(), signalValue);
}

bool GpuVideoFrameCache::Update(ID3D11Device* device, ID3D11DeviceContext* context,
                                ID3D11Texture2D* texture, int arrayIndex, int width, int height,
                                bool fullRange, YuvMatrix matrix, YuvTransfer transfer, int bitDepth) {
    if (!texture || !device || !context) return false;

    using namespace std::chrono;
    auto t0_total = steady_clock::now();
    RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::Update(texture) w=%d h=%d bd=%d\n", now_ms().c_str(), thread_id_str().c_str(), width, height, bitDepth);

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    if (desc.Format == DXGI_FORMAT_NV12 || desc.Format == DXGI_FORMAT_P010) {
        auto t0_ensure = steady_clock::now();
        if (!EnsureNV12(device, width, height, bitDepth)) return false;
        auto d_ensure = duration_cast<milliseconds>(steady_clock::now() - t0_ensure).count();
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::Update EnsureNV12 took %lld ms\n", now_ms().c_str(), thread_id_str().c_str(), (long long)d_ensure);

        int writeIndex = AcquireWriteIndex(context);
        if (writeIndex < 0) {
            RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::Update -> drop (no free NV12 buffer)\n", now_ms().c_str(), thread_id_str().c_str());
            return false;
        }

        m_width = width;
        m_height = height;
        m_bitDepth = bitDepth;
        m_fullRange = fullRange;
        m_matrix = matrix;
        m_transfer = transfer;

        // Copy the planar texture planes. NV12/P010 have 2 planes (Y and UV).
        // In DX11, these are represented as separate subresources.
        auto t0_copy = steady_clock::now();
    #if defined(RADIOIFY_ENABLE_GPU_TIMING)
        {
            Microsoft::WRL::ComPtr<ID3D11Query> qDisjoint, qStart, qEnd;
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            device->CreateQuery(&qd, qDisjoint.GetAddressOf());
            qd.Query = D3D11_QUERY_TIMESTAMP;
            device->CreateQuery(&qd, qStart.GetAddressOf());
            device->CreateQuery(&qd, qEnd.GetAddressOf());

            context->Begin(qDisjoint.Get());
            context->End(qStart.Get());
            context->CopySubresourceRegion(m_texYuv[writeIndex].Get(), 0, 0, 0, 0, texture, D3D11CalcSubresource(0, arrayIndex, desc.MipLevels), nullptr);
            context->CopySubresourceRegion(m_texYuv[writeIndex].Get(), 1, 0, 0, 0, texture, D3D11CalcSubresource(1, arrayIndex, desc.MipLevels), nullptr);
            context->End(qEnd.Get());
            context->End(qDisjoint.Get());

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            UINT64 t1 = 0;
            UINT64 t2 = 0;
            if (context->GetData(qDisjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                context->GetData(qStart.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                context->GetData(qEnd.Get(), &t2, sizeof(t2), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                !disjoint.Disjoint) {
                double gpu_ms = (double)(t2 - t1) / (double)disjoint.Frequency * 1000.0;
                RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::Update NV12 GPU copy time %.3f ms\n", now_ms().c_str(), thread_id_str().c_str(), gpu_ms);
            }
        }
    #else
        context->CopySubresourceRegion(m_texYuv[writeIndex].Get(), 0, 0, 0, 0, texture, D3D11CalcSubresource(0, arrayIndex, desc.MipLevels), nullptr);
        context->CopySubresourceRegion(m_texYuv[writeIndex].Get(), 1, 0, 0, 0, texture, D3D11CalcSubresource(1, arrayIndex, desc.MipLevels), nullptr);
    #endif
        auto d_copy = duration_cast<milliseconds>(steady_clock::now() - t0_copy).count();

        m_format = CacheFormat::Yuv;
        m_activeIndex = writeIndex;
        m_writeIndex = (writeIndex + 1) % kFrameBufferCount;
        auto d_total = duration_cast<milliseconds>(steady_clock::now() - t0_total).count();
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::Update -> NV12 copy done (copy %lld ms total %lld ms)\n", now_ms().c_str(), thread_id_str().c_str(), (long long)d_copy, (long long)d_total);
    } else {
        // RGBA path
        auto t0_ensure = steady_clock::now();
        if (!EnsureRGBA(device, width, height)) return false;
        auto d_ensure = duration_cast<milliseconds>(steady_clock::now() - t0_ensure).count();

        int writeIndex = AcquireWriteIndex(context);
        if (writeIndex < 0) {
            RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::Update -> drop (no free RGBA buffer)\n", now_ms().c_str(), thread_id_str().c_str());
            return false;
        }

        m_width = width;
        m_height = height;

        D3D11_BOX srcBox;
        srcBox.left = 0; srcBox.top = 0; srcBox.front = 0;
        srcBox.right = width; srcBox.bottom = height; srcBox.back = 1;
        auto t0_copy = steady_clock::now();
    #if defined(RADIOIFY_ENABLE_GPU_TIMING)
        {
            Microsoft::WRL::ComPtr<ID3D11Query> qDisjoint, qStart, qEnd;
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            device->CreateQuery(&qd, qDisjoint.GetAddressOf());
            qd.Query = D3D11_QUERY_TIMESTAMP;
            device->CreateQuery(&qd, qStart.GetAddressOf());
            device->CreateQuery(&qd, qEnd.GetAddressOf());

            context->Begin(qDisjoint.Get());
            context->End(qStart.Get());
            context->CopySubresourceRegion(m_texRGBA[writeIndex].Get(), 0, 0, 0, 0, texture, D3D11CalcSubresource(0, arrayIndex, desc.MipLevels), &srcBox);
            context->End(qEnd.Get());
            context->End(qDisjoint.Get());

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            UINT64 t1 = 0;
            UINT64 t2 = 0;
            if (context->GetData(qDisjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                context->GetData(qStart.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                context->GetData(qEnd.Get(), &t2, sizeof(t2), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                !disjoint.Disjoint) {
                double gpu_ms = (double)(t2 - t1) / (double)disjoint.Frequency * 1000.0;
                RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::Update RGBA GPU copy time %.3f ms\n", now_ms().c_str(), thread_id_str().c_str(), gpu_ms);
            }
        }
    #else
        context->CopySubresourceRegion(m_texRGBA[writeIndex].Get(), 0, 0, 0, 0, texture, D3D11CalcSubresource(0, arrayIndex, desc.MipLevels), &srcBox);
    #endif
        auto d_copy = duration_cast<milliseconds>(steady_clock::now() - t0_copy).count();

        m_format = CacheFormat::Rgba;
        m_activeIndex = writeIndex;
        m_writeIndex = (writeIndex + 1) % kFrameBufferCount;
        auto d_total = duration_cast<milliseconds>(steady_clock::now() - t0_total).count();
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::Update -> RGBA copy done (ensure %lld ms copy %lld ms total %lld ms)\n", now_ms().c_str(), thread_id_str().c_str(), (long long)d_ensure, (long long)d_copy, (long long)d_total);
    }

    return true;
}

bool GpuVideoFrameCache::Update(ID3D11Device* device, ID3D11DeviceContext* context,
                                const uint8_t* rgba, int stride, int width, int height) {
    if (!device || !context || !rgba) return false;
    using namespace std::chrono;
    auto t0_total = steady_clock::now();
    auto t0_ensure = steady_clock::now();
    if (!EnsureRGBA(device, width, height)) return false;
    auto d_ensure = duration_cast<milliseconds>(steady_clock::now() - t0_ensure).count();
    int writeIndex = AcquireWriteIndex(context);
    if (writeIndex < 0) {
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::Update(rgba) -> drop (no free buffer)\n", now_ms().c_str(), thread_id_str().c_str());
        return false;
    }
    auto t0_upd = steady_clock::now();
#if defined(RADIOIFY_ENABLE_STAGING_UPLOAD)
    if (!EnsureStagingRGBA(device, width, height)) {
        RADIOIFY_VIDEO_ERROR_LOG("[%s] [tid=%s] GpuVideoFrameCache::Update(rgba) EnsureStagingRGBA failed\n", now_ms().c_str(), thread_id_str().c_str());
        return false;
    }
    if (!UploadRGBAToDefaultViaStaging(context, writeIndex, rgba, stride, width, height)) {
        RADIOIFY_VIDEO_ERROR_LOG("[%s] [tid=%s] GpuVideoFrameCache::Update(rgba) staging upload failed\n", now_ms().c_str(), thread_id_str().c_str());
        return false;
    }
    auto d_upd = duration_cast<milliseconds>(steady_clock::now() - t0_upd).count();
#else
    context->UpdateSubresource(m_texRGBA[writeIndex].Get(), 0, nullptr, rgba, stride, 0);
    auto d_upd = duration_cast<milliseconds>(steady_clock::now() - t0_upd).count();
#endif
    m_width = width;
    m_height = height;
    m_format = CacheFormat::Rgba;
    m_activeIndex = writeIndex;
    m_writeIndex = (writeIndex + 1) % kFrameBufferCount;
    auto d_total = duration_cast<milliseconds>(steady_clock::now() - t0_total).count();
    RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::Update(rgba) ensure %lld ms update %lld ms total %lld ms\n", now_ms().c_str(), thread_id_str().c_str(), (long long)d_ensure, (long long)d_upd, (long long)d_total);
    return true;
}

bool GpuVideoFrameCache::UpdateNV12(ID3D11Device* device, ID3D11DeviceContext* context,
                                    const uint8_t* yuv, int stride, int planeHeight, int width, int height,
                                    bool fullRange, YuvMatrix matrix, YuvTransfer transfer, int bitDepth) {
    if (!device || !context || !yuv) return false;
    using namespace std::chrono;
    auto t0_total = steady_clock::now();
    RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::UpdateNV12 w=%d h=%d bd=%d\n", now_ms().c_str(), thread_id_str().c_str(), width, height, bitDepth);
    auto t0_ensure = steady_clock::now();
    if (!EnsureNV12(device, width, height, bitDepth)) {
        RADIOIFY_VIDEO_ERROR_LOG("[%s] [tid=%s] GpuVideoFrameCache::UpdateNV12 -> EnsureNV12 failed\n", now_ms().c_str(), thread_id_str().c_str());
        return false;
    }
    auto d_ensure = duration_cast<milliseconds>(steady_clock::now() - t0_ensure).count();

    // Update the planes separately
    int writeIndex = AcquireWriteIndex(context);
    if (writeIndex < 0) {
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::UpdateNV12 -> drop (no free buffer)\n", now_ms().c_str(), thread_id_str().c_str());
        return false;
    }
    auto t0_upd = steady_clock::now();
    using namespace std::chrono;
#if defined(RADIOIFY_ENABLE_STAGING_UPLOAD)
    {
        auto t0_staging = steady_clock::now();
        if (!EnsureStagingNV12(device, width, height, bitDepth)) {
            RADIOIFY_VIDEO_ERROR_LOG("[%s] [tid=%s] GpuVideoFrameCache::UpdateNV12 EnsureStagingNV12 failed\n", now_ms().c_str(), thread_id_str().c_str());
            return false;
        }
        if (!UploadNV12ToDefaultViaStaging(context, writeIndex, yuv, stride, planeHeight, width, height)) {
            RADIOIFY_VIDEO_ERROR_LOG("[%s] [tid=%s] GpuVideoFrameCache::UpdateNV12 staging upload failed\n", now_ms().c_str(), thread_id_str().c_str());
            return false;
        }
        auto d_staging = duration_cast<milliseconds>(steady_clock::now() - t0_staging).count();
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::UpdateNV12 staging upload took %lld ms\n", now_ms().c_str(), thread_id_str().c_str(), (long long)d_staging);
    }
#elif defined(RADIOIFY_ENABLE_GPU_TIMING)
    {
        Microsoft::WRL::ComPtr<ID3D11Query> qDisjoint, qStart, qEnd;
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        device->CreateQuery(&qd, qDisjoint.GetAddressOf());
        qd.Query = D3D11_QUERY_TIMESTAMP;
        device->CreateQuery(&qd, qStart.GetAddressOf());
        device->CreateQuery(&qd, qEnd.GetAddressOf());

        context->Begin(qDisjoint.Get());
        context->End(qStart.Get());
        context->UpdateSubresource(m_texYuv[writeIndex].Get(), 0, nullptr, yuv, stride, 0);
        context->UpdateSubresource(m_texYuv[writeIndex].Get(), 1, nullptr, yuv + (stride * planeHeight), stride, 0);
        context->End(qEnd.Get());
        context->End(qDisjoint.Get());

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 t1 = 0;
        UINT64 t2 = 0;
        if (context->GetData(qDisjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            context->GetData(qStart.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            context->GetData(qEnd.Get(), &t2, sizeof(t2), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            !disjoint.Disjoint) {
            double gpu_ms = (double)(t2 - t1) / (double)disjoint.Frequency * 1000.0;
            RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::UpdateNV12 GPU UpdateSubresource time %.3f ms\n", now_ms().c_str(), thread_id_str().c_str(), gpu_ms);
        }
    }
#else
    context->UpdateSubresource(m_texYuv[writeIndex].Get(), 0, nullptr, yuv, stride, 0);
    context->UpdateSubresource(m_texYuv[writeIndex].Get(), 1, nullptr, yuv + (stride * planeHeight), stride, 0);
#endif
    auto d_upd = duration_cast<milliseconds>(steady_clock::now() - t0_upd).count();
    RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::UpdateNV12 -> UpdateSubresource done (ensure %lld ms update %lld ms)\n", now_ms().c_str(), thread_id_str().c_str(), (long long)d_ensure, (long long)d_upd);

    m_width = width;
    m_height = height;
    m_fullRange = fullRange;
    m_matrix = matrix;
    m_transfer = transfer;
    m_bitDepth = bitDepth;
    m_format = CacheFormat::Yuv;
    m_activeIndex = writeIndex;
    m_writeIndex = (writeIndex + 1) % kFrameBufferCount;
    auto d_total = duration_cast<milliseconds>(steady_clock::now() - t0_total).count();
    RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::UpdateNV12 total %lld ms\n", now_ms().c_str(), thread_id_str().c_str(), (long long)d_total);
    return true;
}

bool GpuVideoFrameCache::EnsureNV12(ID3D11Device* device, int width, int height, int bitDepth) {
    RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureNV12 requested w=%d h=%d bd=%d (cached w=%d h=%d bd=%d)\n", now_ms().c_str(), thread_id_str().c_str(), width, height, bitDepth, m_width, m_height, m_bitDepth);
    bool cacheMatch = (m_width == width && m_height == height && m_bitDepth == bitDepth);
    if (cacheMatch) {
        for (int i = 0; i < kFrameBufferCount; ++i) {
            if (!m_texYuv[i] || !m_srvY[i] || !m_srvUV[i]) {
                cacheMatch = false;
                break;
            }
        }
    }
    if (cacheMatch) {
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureNV12: already matched\n", now_ms().c_str(), thread_id_str().c_str());
        return true;
    }

    for (int i = 0; i < kFrameBufferCount; ++i) {
        m_texYuv[i].Reset();
        m_srvY[i].Reset();
        m_srvUV[i].Reset();
    }
    for (int i = 0; i < kFrameBufferCount; ++i) {
        m_gpuInFlight[i] = false;
    }
    m_activeIndex = 0;
    m_writeIndex = 0;
    
    DXGI_FORMAT yuvFormat = (bitDepth > 8) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = yuvFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    using namespace std::chrono;
    // Create SRV for Y plane
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = (bitDepth > 8) ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvUvDesc = srvDesc;
    srvUvDesc.Format = (bitDepth > 8) ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;

    for (int i = 0; i < kFrameBufferCount; ++i) {
        auto t0_create = steady_clock::now();
        if (FAILED(device->CreateTexture2D(&desc, nullptr, m_texYuv[i].GetAddressOf()))) {
            RADIOIFY_VIDEO_ERROR_LOG("[%s] [tid=%s] EnsureNV12: CreateTexture2D FAILED\n", now_ms().c_str(), thread_id_str().c_str());
            return false;
        }
        auto d_create = duration_cast<milliseconds>(steady_clock::now() - t0_create).count();
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureNV12: CreateTexture2D took %lld ms\n", now_ms().c_str(), thread_id_str().c_str(), (long long)d_create);

        auto t0_srv_y = steady_clock::now();
        if (FAILED(device->CreateShaderResourceView(m_texYuv[i].Get(), &srvDesc, m_srvY[i].GetAddressOf()))) {
            RADIOIFY_VIDEO_ERROR_LOG("[%s] [tid=%s] EnsureNV12: CreateSRV Y FAILED\n", now_ms().c_str(), thread_id_str().c_str());
            return false;
        }
        auto d_srv_y = duration_cast<milliseconds>(steady_clock::now() - t0_srv_y).count();
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureNV12: CreateSRV Y took %lld ms\n", now_ms().c_str(), thread_id_str().c_str(), (long long)d_srv_y);

        auto t0_srv_uv = steady_clock::now();
        if (FAILED(device->CreateShaderResourceView(m_texYuv[i].Get(), &srvUvDesc, m_srvUV[i].GetAddressOf()))) {
            RADIOIFY_VIDEO_ERROR_LOG("[%s] [tid=%s] EnsureNV12: CreateSRV UV FAILED\n", now_ms().c_str(), thread_id_str().c_str());
            return false;
        }
        auto d_srv_uv = duration_cast<milliseconds>(steady_clock::now() - t0_srv_uv).count();
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureNV12: CreateSRV UV took %lld ms\n", now_ms().c_str(), thread_id_str().c_str(), (long long)d_srv_uv);
    }

    RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureNV12: success\n", now_ms().c_str(), thread_id_str().c_str());
    return true;
}

bool GpuVideoFrameCache::EnsureRGBA(ID3D11Device* device, int width, int height) {
    RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureRGBA requested w=%d h=%d (cached w=%d h=%d)\n", now_ms().c_str(), thread_id_str().c_str(), width, height, m_width, m_height);
    bool cacheMatch = (m_width == width && m_height == height);
    if (cacheMatch) {
        for (int i = 0; i < kFrameBufferCount; ++i) {
            if (!m_texRGBA[i] || !m_srvRGBA[i]) {
                cacheMatch = false;
                break;
            }
        }
    }
    if (cacheMatch) {
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureRGBA: already matched\n", now_ms().c_str(), thread_id_str().c_str());
        return true;
    }

    for (int i = 0; i < kFrameBufferCount; ++i) {
        m_texRGBA[i].Reset();
        m_srvRGBA[i].Reset();
    }
    for (int i = 0; i < kFrameBufferCount; ++i) {
        m_gpuInFlight[i] = false;
    }
    m_activeIndex = 0;
    m_writeIndex = 0;
    
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    using namespace std::chrono;
    for (int i = 0; i < kFrameBufferCount; ++i) {
        auto t0_create = steady_clock::now();
        if (FAILED(device->CreateTexture2D(&desc, nullptr, m_texRGBA[i].GetAddressOf()))) {
            RADIOIFY_VIDEO_ERROR_LOG("[%s] [tid=%s] EnsureRGBA: CreateTexture2D FAILED\n", now_ms().c_str(), thread_id_str().c_str());
            return false;
        }
        auto d_create = duration_cast<milliseconds>(steady_clock::now() - t0_create).count();
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureRGBA: CreateTexture2D took %lld ms\n", now_ms().c_str(), thread_id_str().c_str(), (long long)d_create);
        auto t0_srv = steady_clock::now();
        if (FAILED(device->CreateShaderResourceView(m_texRGBA[i].Get(), nullptr, m_srvRGBA[i].GetAddressOf()))) {
            RADIOIFY_VIDEO_ERROR_LOG("[%s] [tid=%s] EnsureRGBA: CreateSRV FAILED\n", now_ms().c_str(), thread_id_str().c_str());
            return false;
        }
        auto d_srv = duration_cast<milliseconds>(steady_clock::now() - t0_srv).count();
        RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureRGBA: CreateSRV took %lld ms\n", now_ms().c_str(), thread_id_str().c_str(), (long long)d_srv);
    }

    RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureRGBA: success\n", now_ms().c_str(), thread_id_str().c_str());
    return true;
}

#if defined(RADIOIFY_ENABLE_STAGING_UPLOAD)

bool GpuVideoFrameCache::EnsureStagingNV12(ID3D11Device* device, int width, int height, int bitDepth) {
    DXGI_FORMAT yuvFormat = (bitDepth > 8) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    if (m_stagingYuv) {
        D3D11_TEXTURE2D_DESC desc;
        m_stagingYuv->GetDesc(&desc);
        if (desc.Width == width && desc.Height == height && desc.Format == yuvFormat) return true;
    }

    m_stagingYuv.Reset();
    D3D11_TEXTURE2D_DESC sdesc{};
    sdesc.Width = width;
    sdesc.Height = height;
    sdesc.MipLevels = 1;
    sdesc.ArraySize = 1;
    sdesc.Format = yuvFormat;
    sdesc.SampleDesc.Count = 1;
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.BindFlags = 0;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device->CreateTexture2D(&sdesc, nullptr, m_stagingYuv.GetAddressOf()))) {
        RADIOIFY_VIDEO_ERROR_LOG("[%s] [tid=%s] EnsureStagingNV12: CreateTexture2D FAILED\n", now_ms().c_str(), thread_id_str().c_str());
        return false;
    }
    RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureStagingNV12: success\n", now_ms().c_str(), thread_id_str().c_str());
    return true;
}

bool GpuVideoFrameCache::EnsureStagingRGBA(ID3D11Device* device, int width, int height) {
    if (m_stagingRGBA) {
        D3D11_TEXTURE2D_DESC desc;
        m_stagingRGBA->GetDesc(&desc);
        if (desc.Width == width && desc.Height == height && desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) return true;
    }

    m_stagingRGBA.Reset();
    D3D11_TEXTURE2D_DESC sdesc{};
    sdesc.Width = width;
    sdesc.Height = height;
    sdesc.MipLevels = 1;
    sdesc.ArraySize = 1;
    sdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sdesc.SampleDesc.Count = 1;
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.BindFlags = 0;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device->CreateTexture2D(&sdesc, nullptr, m_stagingRGBA.GetAddressOf()))) {
        RADIOIFY_VIDEO_ERROR_LOG("[%s] [tid=%s] EnsureStagingRGBA: CreateTexture2D FAILED\n", now_ms().c_str(), thread_id_str().c_str());
        return false;
    }
    RADIOIFY_TIMING_LOG("[%s] [tid=%s] EnsureStagingRGBA: success\n", now_ms().c_str(), thread_id_str().c_str());
    return true;
}

bool GpuVideoFrameCache::UploadNV12ToDefaultViaStaging(ID3D11DeviceContext* context, int dstIndex, const uint8_t* yuv, int stride, int planeHeight, int width, int height) {
    // writes Y plane then UV plane to staging and issues CopySubresourceRegion into default texture
    if (!m_stagingYuv) return false;

    D3D11_MAPPED_SUBRESOURCE mapped;
    // Y plane
    if (FAILED(context->Map(m_stagingYuv.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) return false;
    for (int r = 0; r < height; ++r) {
        memcpy((uint8_t*)mapped.pData + (size_t)r * mapped.RowPitch, yuv + (size_t)r * stride, (size_t)width);
    }
    context->Unmap(m_stagingYuv.Get(), 0);

    // UV plane (subresource 1)
    if (FAILED(context->Map(m_stagingYuv.Get(), 1, D3D11_MAP_WRITE, 0, &mapped))) return false;
    const uint8_t* uvSrc = yuv + (size_t)stride * planeHeight;
    for (int r = 0; r < planeHeight; ++r) {
        memcpy((uint8_t*)mapped.pData + (size_t)r * mapped.RowPitch, uvSrc + (size_t)r * stride, (size_t)stride);
    }
    context->Unmap(m_stagingYuv.Get(), 1);

    // Copy into the default texture planes
    context->CopySubresourceRegion(m_texYuv[dstIndex].Get(), 0, 0, 0, 0, m_stagingYuv.Get(), 0, nullptr);
    context->CopySubresourceRegion(m_texYuv[dstIndex].Get(), 1, 0, 0, 0, m_stagingYuv.Get(), 1, nullptr);
    return true;
}

bool GpuVideoFrameCache::UploadRGBAToDefaultViaStaging(ID3D11DeviceContext* context, int dstIndex, const uint8_t* rgba, int stride, int width, int height) {
    if (!m_stagingRGBA) return false;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(m_stagingRGBA.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) return false;
    for (int r = 0; r < height; ++r) {
        memcpy((uint8_t*)mapped.pData + (size_t)r * mapped.RowPitch, rgba + (size_t)r * stride, (size_t)stride);
    }
    context->Unmap(m_stagingRGBA.Get(), 0);

    context->CopySubresourceRegion(m_texRGBA[dstIndex].Get(), 0, 0, 0, 0, m_stagingRGBA.Get(), 0, nullptr);
    return true;
}

#endif

void GpuVideoFrameCache::Reset() {
    RADIOIFY_TIMING_LOG("[%s] [tid=%s] GpuVideoFrameCache::Reset\n", now_ms().c_str(), thread_id_str().c_str());
    for (int i = 0; i < kFrameBufferCount; ++i) {
        m_texYuv[i].Reset();
        m_texRGBA[i].Reset();
        m_srvY[i].Reset();
        m_srvUV[i].Reset();
        m_srvRGBA[i].Reset();
        m_gpuDone[i].Reset();
        m_gpuInFlight[i] = false;
    }
    m_activeIndex = 0;
    m_writeIndex = 0;
    m_format = CacheFormat::None;
    ResetFrameLatencyFence();
}
