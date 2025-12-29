#include "m4adecoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <cstring>

namespace {
bool g_mfStarted = false;
bool g_comStarted = false;

void setError(std::string* error, const char* message) {
  if (error) *error = message;
}

bool ensureMediaFoundation(std::string* error) {
  if (g_mfStarted) return true;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr)) {
    g_comStarted = true;
  } else if (hr != RPC_E_CHANGED_MODE) {
    setError(error, "Failed to initialize COM for m4a decoding.");
    return false;
  }

  hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
  if (FAILED(hr)) {
    if (g_comStarted) {
      CoUninitialize();
      g_comStarted = false;
    }
    setError(error, "Failed to start Media Foundation for m4a decoding.");
    return false;
  }

  g_mfStarted = true;
  return true;
}

template <typename T>
void safeRelease(T*& ptr) {
  if (ptr) {
    ptr->Release();
    ptr = nullptr;
  }
}
} // namespace

bool M4aDecoder::init(const std::filesystem::path& path, uint32_t channels, uint32_t sampleRate, std::string* error) {
  uninit();

  if (!ensureMediaFoundation(error)) return false;

  IMFSourceReader* reader = nullptr;
  std::wstring wpath = path.wstring();
  HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &reader);
  if (FAILED(hr)) {
    setError(error, "Failed to open m4a input for decoding.");
    return false;
  }

  IMFMediaType* type = nullptr;
  hr = MFCreateMediaType(&type);
  if (FAILED(hr)) {
    safeRelease(reader);
    setError(error, "Failed to configure m4a decoding.");
    return false;
  }

  hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * sizeof(float));
  if (SUCCEEDED(hr)) {
    hr = type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * channels * sizeof(float));
  }
  if (SUCCEEDED(hr)) {
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type);
  }
  safeRelease(type);
  if (FAILED(hr)) {
    safeRelease(reader);
    setError(error, "Failed to configure m4a output format.");
    return false;
  }

  IMFMediaType* actualType = nullptr;
  hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actualType);
  if (FAILED(hr)) {
    safeRelease(reader);
    setError(error, "Failed to query m4a output format.");
    return false;
  }

  UINT32 actualChannels = 0;
  UINT32 actualRate = 0;
  actualType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &actualChannels);
  actualType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &actualRate);
  safeRelease(actualType);

  if (actualChannels != channels || actualRate != sampleRate) {
    safeRelease(reader);
    setError(error, "Unsupported m4a output format.");
    return false;
  }

  totalFrames_ = 0;
  PROPVARIANT duration{};
  PropVariantInit(&duration);
  hr = reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration);
  if (SUCCEEDED(hr) && duration.vt == VT_UI8 && sampleRate > 0) {
    totalFrames_ = static_cast<uint64_t>((duration.uhVal.QuadPart * sampleRate) / 10000000ULL);
  }
  PropVariantClear(&duration);

  reader_ = reader;
  channels_ = channels;
  sampleRate_ = sampleRate;
  atEnd_ = false;
  cache_.clear();
  cachePos_ = 0;
  return true;
}

void M4aDecoder::uninit() {
  safeRelease(reader_);
  channels_ = 0;
  sampleRate_ = 0;
  totalFrames_ = 0;
  atEnd_ = false;
  cache_.clear();
  cachePos_ = 0;
}

bool M4aDecoder::getTotalFrames(uint64_t* outFrames) const {
  if (!outFrames) return false;
  *outFrames = totalFrames_;
  return totalFrames_ > 0;
}

bool M4aDecoder::readFrames(float* out, uint32_t frameCount, uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  if (!reader_ || !out) return false;
  if (frameCount == 0) return true;

  uint64_t totalRead = 0;
  while (totalRead < frameCount) {
    size_t availableSamples = (cache_.size() > cachePos_) ? (cache_.size() - cachePos_) : 0;
    if (availableSamples > 0) {
      size_t availableFrames = availableSamples / channels_;
      size_t framesToCopy = std::min<size_t>(frameCount - totalRead, availableFrames);
      size_t samplesToCopy = framesToCopy * channels_;
      std::memcpy(out + totalRead * channels_, cache_.data() + cachePos_, samplesToCopy * sizeof(float));
      cachePos_ += samplesToCopy;
      totalRead += framesToCopy;
      if (cachePos_ >= cache_.size()) {
        cache_.clear();
        cachePos_ = 0;
      }
      if (totalRead >= frameCount) break;
    }

    if (atEnd_) break;

    DWORD flags = 0;
    IMFSample* sample = nullptr;
    HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample);
    if (FAILED(hr)) {
      atEnd_ = true;
      break;
    }
    bool endOfStream = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
    if (!sample) {
      if (endOfStream) atEnd_ = true;
      continue;
    }

    IMFMediaBuffer* buffer = nullptr;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    if (SUCCEEDED(hr) && buffer) {
      BYTE* data = nullptr;
      DWORD maxLen = 0;
      DWORD curLen = 0;
      hr = buffer->Lock(&data, &maxLen, &curLen);
      if (SUCCEEDED(hr)) {
        if (data && curLen > 0) {
          size_t floatCount = curLen / sizeof(float);
          cache_.resize(floatCount);
          std::memcpy(cache_.data(), data, floatCount * sizeof(float));
          cachePos_ = 0;
        }
        buffer->Unlock();
      }
    }
    safeRelease(buffer);
    safeRelease(sample);
    if (endOfStream) {
      atEnd_ = true;
    }
  }

  if (framesRead) *framesRead = totalRead;
  return true;
}

bool M4aDecoder::seekToFrame(uint64_t frame) {
  if (!reader_ || sampleRate_ == 0) return false;

  LONGLONG pos = static_cast<LONGLONG>((frame * 10000000ULL) / sampleRate_);
  PROPVARIANT var{};
  PropVariantInit(&var);
  var.vt = VT_I8;
  var.hVal.QuadPart = pos;
  HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, var);
  PropVariantClear(&var);
  if (FAILED(hr)) return false;

  atEnd_ = false;
  cache_.clear();
  cachePos_ = 0;
  return true;
}
