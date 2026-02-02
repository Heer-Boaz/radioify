#include "gsfaudio.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

#include "miniaudio.h"

extern "C" {
#include <psflib/psflib.h>
}

#if !RADIOIFY_DISABLE_GSF_GPL
#include <zlib.h>
extern "C" {
#include "types.h"
#include "gsf.h"
}
#include "VBA/System.h"
#include "VBA/Sound.h"
#endif

namespace {
constexpr uint32_t kGsfChannels = 2;
constexpr uint32_t kGsfVersion = 0x22;
constexpr uint32_t kDefaultTrackLengthMs = 150000;
constexpr uint32_t kSeekChunkFrames = 4096;

struct GsfMetadata {
  std::string title;
  int lengthMs = -1;
  int fadeMs = 0;
};

std::string toUtf8String(const std::filesystem::path& path) {
#ifdef _WIN32
  auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
#else
  return path.string();
#endif
}

uint64_t msToFrames(int64_t ms, uint32_t sampleRate) {
  if (ms <= 0 || sampleRate == 0) return 0;
  double frames = static_cast<double>(ms) * static_cast<double>(sampleRate) /
                  1000.0;
  return static_cast<uint64_t>(std::llround(frames));
}

std::string trimAscii(const std::string& value) {
  size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start]))) {
    ++start;
  }
  size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(start, end - start);
}

bool asciiEqualNoCase(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    unsigned char ca = static_cast<unsigned char>(*a++);
    unsigned char cb = static_cast<unsigned char>(*b++);
    if (std::tolower(ca) != std::tolower(cb)) return false;
  }
  return *a == '\0' && *b == '\0';
}

bool parseInt(const std::string& text, int* out) {
  if (!out) return false;
  char* end = nullptr;
  long value = std::strtol(text.c_str(), &end, 10);
  if (!end || *end != '\0' || value < 0) return false;
  *out = static_cast<int>(value);
  return true;
}

bool parseDouble(const std::string& text, double* out) {
  if (!out) return false;
  char* end = nullptr;
  double value = std::strtod(text.c_str(), &end);
  if (!end || *end != '\0' || value < 0.0) return false;
  *out = value;
  return true;
}

bool parseTimeMs(const std::string& text, int* outMs) {
  if (!outMs) return false;
  std::string trimmed = trimAscii(text);
  if (trimmed.empty()) return false;

  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= trimmed.size()) {
    size_t pos = trimmed.find(':', start);
    if (pos == std::string::npos) {
      parts.push_back(trimmed.substr(start));
      break;
    }
    parts.push_back(trimmed.substr(start, pos - start));
    start = pos + 1;
  }
  if (parts.empty() || parts.size() > 3) return false;

  double seconds = 0.0;
  if (parts.size() == 3) {
    int hours = 0;
    int minutes = 0;
    double sec = 0.0;
    if (!parseInt(parts[0], &hours) || !parseInt(parts[1], &minutes) ||
        !parseDouble(parts[2], &sec)) {
      return false;
    }
    seconds = static_cast<double>(hours * 3600 + minutes * 60) + sec;
  } else if (parts.size() == 2) {
    int minutes = 0;
    double sec = 0.0;
    if (!parseInt(parts[0], &minutes) || !parseDouble(parts[1], &sec)) {
      return false;
    }
    seconds = static_cast<double>(minutes * 60) + sec;
  } else {
    double sec = 0.0;
    if (!parseDouble(parts[0], &sec)) return false;
    seconds = sec;
  }

  if (seconds < 0.0) return false;
  *outMs = static_cast<int>(std::llround(seconds * 1000.0));
  return true;
}

int gsfInfoCallback(void* context, const char* name, const char* value) {
  if (!context || !name || !value) return 0;
  auto* meta = static_cast<GsfMetadata*>(context);
  if (asciiEqualNoCase(name, "title")) {
    meta->title = trimAscii(value);
  } else if (asciiEqualNoCase(name, "length")) {
    int ms = 0;
    if (parseTimeMs(value, &ms)) {
      meta->lengthMs = ms;
    }
  } else if (asciiEqualNoCase(name, "fade")) {
    int ms = 0;
    if (parseTimeMs(value, &ms)) {
      meta->fadeMs = ms;
    }
  }
  return 0;
}

struct PsfFileContext {
  std::vector<std::filesystem::path> dirStack;
  std::vector<std::filesystem::path> roots;
};

thread_local PsfFileContext g_psfFileContext;

class ScopedPsfFileContext {
 public:
  explicit ScopedPsfFileContext(const std::filesystem::path& baseFile) {
    savedStack_ = std::move(g_psfFileContext.dirStack);
    savedRoots_ = std::move(g_psfFileContext.roots);
    g_psfFileContext.dirStack.clear();
    g_psfFileContext.roots.clear();
    if (!baseFile.empty()) {
      std::filesystem::path root = baseFile.parent_path();
      if (!root.empty()) {
        g_psfFileContext.roots.push_back(root);
      }
    }
  }

  ~ScopedPsfFileContext() {
    g_psfFileContext.dirStack = std::move(savedStack_);
    g_psfFileContext.roots = std::move(savedRoots_);
  }

 private:
  std::vector<std::filesystem::path> savedStack_;
  std::vector<std::filesystem::path> savedRoots_;
};

struct PsfFileHandle {
  std::FILE* fp = nullptr;
  std::filesystem::path path;
  bool pushed = false;
};

bool isAbsolutePath(const std::filesystem::path& path) {
  return path.is_absolute() || path.has_root_name();
}

PsfFileHandle* tryOpenPsfFile(const std::filesystem::path& path) {
  if (path.empty()) return nullptr;
  std::string pathUtf8 = toUtf8String(path);
  std::FILE* fp = std::fopen(pathUtf8.c_str(), "rb");
  if (!fp) return nullptr;
  auto* handle = new PsfFileHandle();
  handle->fp = fp;
  handle->path = path;
  std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    g_psfFileContext.dirStack.push_back(parent);
    handle->pushed = true;
  }
  return handle;
}

void* psf_file_fopen(const char* uri) {
  if (!uri) return nullptr;
  std::filesystem::path path(uri);

  if (isAbsolutePath(path)) {
    return tryOpenPsfFile(path);
  }

  for (auto it = g_psfFileContext.dirStack.rbegin();
       it != g_psfFileContext.dirStack.rend(); ++it) {
    if (PsfFileHandle* handle = tryOpenPsfFile((*it) / path)) {
      return handle;
    }
  }

  for (auto it = g_psfFileContext.roots.rbegin();
       it != g_psfFileContext.roots.rend(); ++it) {
    if (PsfFileHandle* handle = tryOpenPsfFile((*it) / path)) {
      return handle;
    }
  }

  return tryOpenPsfFile(path);
}

size_t psf_file_fread(void* buffer, size_t size, size_t count, void* handle) {
  if (!handle) return 0;
  return std::fread(buffer, size, count,
                    static_cast<PsfFileHandle*>(handle)->fp);
}

int psf_file_fseek(void* handle, int64_t offset, int whence) {
  if (!handle) return -1;
  return std::fseek(static_cast<PsfFileHandle*>(handle)->fp,
                    static_cast<long>(offset), whence);
}

int psf_file_fclose(void* handle) {
  if (!handle) return 0;
  auto* h = static_cast<PsfFileHandle*>(handle);
  int result = std::fclose(h->fp);
  if (h->pushed && !g_psfFileContext.dirStack.empty()) {
    g_psfFileContext.dirStack.pop_back();
  }
  delete h;
  return result;
}

long psf_file_ftell(void* handle) {
  if (!handle) return -1;
  return std::ftell(static_cast<PsfFileHandle*>(handle)->fp);
}

const psf_file_callbacks psf_file_system = {
    "\\/|:", psf_file_fopen, psf_file_fread,
    psf_file_fseek, psf_file_fclose, psf_file_ftell};

bool collectGsfMetadata(const std::filesystem::path& path, GsfMetadata* meta,
                        std::string* error) {
  if (!meta) return false;
  *meta = GsfMetadata{};
  ScopedPsfFileContext scope(path);
  std::string pathUtf8 = toUtf8String(path);
  int version = psf_load(pathUtf8.c_str(), &psf_file_system, kGsfVersion,
                         nullptr, nullptr, gsfInfoCallback, meta, 0);
  if (version < 0) {
    if (error) *error = "Failed to read GSF metadata.";
    return false;
  }
  if (version != static_cast<int>(kGsfVersion)) {
    if (error) *error = "Unsupported GSF version.";
    return false;
  }
  return true;
}

int computeTrackLengthMs(const GsfMetadata& meta) {
  int lengthMs = meta.lengthMs > 0 ? meta.lengthMs : kDefaultTrackLengthMs;
  int fadeMs = meta.fadeMs > 0 ? meta.fadeMs : 0;
  return lengthMs + fadeMs;
}

size_t inputAvailableFrames(const std::vector<int16_t>& buffer,
                            size_t readPosSamples) {
  if (readPosSamples >= buffer.size()) return 0;
  return (buffer.size() - readPosSamples) / kGsfChannels;
}

void consumeInputFrames(std::vector<int16_t>* buffer, size_t* readPosSamples,
                        size_t frames) {
  if (!buffer || !readPosSamples) return;
  size_t samples = frames * kGsfChannels;
  if (*readPosSamples + samples > buffer->size()) {
    *readPosSamples = buffer->size();
  } else {
    *readPosSamples += samples;
  }
  if (*readPosSamples == 0) return;
  if (*readPosSamples >= buffer->size() / 2 ||
      *readPosSamples >= static_cast<size_t>(8192 * kGsfChannels)) {
    buffer->erase(buffer->begin(), buffer->begin() + *readPosSamples);
    *readPosSamples = 0;
  }
}
}  // namespace

#if !RADIOIFY_DISABLE_GSF_GPL

extern "C" {
int defvolume = 1000;
int relvolume = 1000;
int TrackLength = 0;
int FadeLength = 0;
int IgnoreTrackLength = 0;
int DefaultLength = static_cast<int>(kDefaultTrackLengthMs);
int playforever = 0;
int TrailingSilence = 1000;
int DetectSilence = 0;
int silencedetected = 0;
int silencelength = 5;
int cpupercent = 0;
int sndSamplesPerSec = 0;
int sndNumChannels = 0;
int sndBitsPerSample = 16;
int deflen = static_cast<int>(kDefaultTrackLengthMs / 1000);
int deffade = 0;
double decode_pos_ms = 0.0;
int seek_needed = -1;
}

namespace {
struct GsfImpl {
  std::filesystem::path path;
  uint32_t channels = 0;
  uint32_t sampleRate = 0;
  uint32_t inputSampleRate = 0;
  uint64_t totalFrames = 0;
  uint64_t framePos = 0;
  bool atEnd = false;

  std::vector<int16_t> inputBuffer;
  size_t inputReadPos = 0;

  ma_data_converter converter{};
  bool converterInit = false;

  void resetBuffer() {
    inputBuffer.clear();
    inputReadPos = 0;
  }

  void pushSamples(const int16_t* samples, size_t sampleCount) {
    if (!samples || sampleCount == 0) return;
    inputBuffer.insert(inputBuffer.end(), samples, samples + sampleCount);
  }

  size_t availableFrames() const {
    return inputAvailableFrames(inputBuffer, inputReadPos);
  }
};

static std::mutex g_activeMutex;
static GsfImpl* g_active = nullptr;

}  // namespace

extern "C" void writeSound(void) {
  std::lock_guard<std::mutex> lock(g_activeMutex);
  if (!g_active) return;
  if (soundBufferLen <= 0) return;
  size_t sampleCount = static_cast<size_t>(soundBufferLen) / sizeof(int16_t);
  const int16_t* samples = reinterpret_cast<const int16_t*>(soundFinalWave);
  g_active->pushSamples(samples, sampleCount);
}

extern "C" void end_of_track(void) {
  std::lock_guard<std::mutex> lock(g_activeMutex);
  if (g_active) {
    g_active->atEnd = true;
  }
}

struct GsfAudioDecoder::Impl : public GsfImpl {
  bool initCore(std::string* error) {
    if (!path.empty()) {
      int ok = GSFRun(const_cast<char*>(path.string().c_str()));
      if (!ok) {
        if (error) *error = "Failed to load GSF file.";
        return false;
      }
    }

    if (!path.empty()) {
      inputSampleRate = static_cast<uint32_t>(sndSamplesPerSec);
      if (inputSampleRate == 0) {
        if (error) *error = "Invalid GSF sample rate.";
        return false;
      }
      return true;
    }
    if (error) *error = "Invalid GSF path.";
    return false;
  }

  void shutdownCore() {
    GSFClose();
  }

  bool pumpAudio(size_t minFrames) {
    size_t idleLoops = 0;
    size_t prevAvailable = availableFrames();
    while (!atEnd && availableFrames() < minFrames) {
      if (!EmulationLoop()) {
        if (availableFrames() == prevAvailable) {
          ++idleLoops;
          if (idleLoops > 50) break;
        }
      }
      size_t nowAvailable = availableFrames();
      if (nowAvailable == prevAvailable) {
        ++idleLoops;
        if (idleLoops > 200) break;
      } else {
        idleLoops = 0;
      }
      prevAvailable = nowAvailable;
    }
    return true;
  }
};

GsfAudioDecoder::GsfAudioDecoder() = default;
GsfAudioDecoder::~GsfAudioDecoder() { uninit(); }

bool GsfAudioDecoder::init(const std::filesystem::path& path,
                           uint32_t channels, uint32_t sampleRate,
                           std::string* error, int) {
  uninit();

  if (channels != 1 && channels != 2) {
    if (error) *error = "Unsupported channel count for GSF.";
    return false;
  }

  GsfMetadata meta;
  collectGsfMetadata(path, &meta, nullptr);
  int trackMs = computeTrackLengthMs(meta);
  FadeLength = meta.fadeMs > 0 ? meta.fadeMs : 0;
  TrackLength = trackMs;
  IgnoreTrackLength = 0;
  DefaultLength = static_cast<int>(kDefaultTrackLengthMs);
  playforever = 0;
  DetectSilence = 0;
  silencedetected = 0;
  silencelength = 5;
  TrailingSilence = 1000;
  deflen = static_cast<int>(kDefaultTrackLengthMs / 1000);
  deffade = 0;
  relvolume = defvolume;

  decode_pos_ms = 0.0;
  seek_needed = -1;

  soundQuality = 1;
  soundLowPass = 0;
  soundEcho = 0;

  std::unique_ptr<Impl> impl = std::make_unique<Impl>();
  impl->path = path;
  impl->channels = channels;
  impl->sampleRate = sampleRate;
  impl->framePos = 0;
  impl->atEnd = false;
  impl->resetBuffer();

  {
    std::lock_guard<std::mutex> lock(g_activeMutex);
    g_active = impl.get();
  }

  if (!impl->initCore(error)) {
    std::lock_guard<std::mutex> lock(g_activeMutex);
    g_active = nullptr;
    return false;
  }

  ma_data_converter_config config = ma_data_converter_config_init(
      ma_format_s16, ma_format_f32, kGsfChannels, channels,
      impl->inputSampleRate, sampleRate);
  if (ma_data_converter_init(&config, nullptr, &impl->converter) != MA_SUCCESS) {
    if (error) *error = "Failed to initialize GSF resampler.";
    impl->shutdownCore();
    std::lock_guard<std::mutex> lock(g_activeMutex);
    g_active = nullptr;
    return false;
  }
  impl->converterInit = true;

  impl->totalFrames = msToFrames(trackMs, sampleRate);
  impl_ = impl.release();
  return true;
}

void GsfAudioDecoder::uninit() {
  if (!impl_) return;
  if (impl_->converterInit) {
    ma_data_converter_uninit(&impl_->converter, nullptr);
  }
  impl_->shutdownCore();
  {
    std::lock_guard<std::mutex> lock(g_activeMutex);
    if (g_active == impl_) g_active = nullptr;
  }
  delete impl_;
  impl_ = nullptr;
}

bool GsfAudioDecoder::readFrames(float* out, uint32_t frameCount,
                                 uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  if (!impl_ || !out || frameCount == 0) return false;

  uint64_t toRead = frameCount;
  if (impl_->totalFrames > 0) {
    if (impl_->framePos >= impl_->totalFrames) {
      impl_->atEnd = true;
      return true;
    }
    uint64_t remaining = impl_->totalFrames - impl_->framePos;
    toRead = std::min<uint64_t>(toRead, remaining);
  }
  if (toRead == 0) {
    impl_->atEnd = true;
    return true;
  }

  uint64_t produced = 0;
  while (produced < toRead) {
    uint64_t remainingOut = toRead - produced;
    ma_uint64 requiredIn = 0;
    ma_data_converter_get_required_input_frame_count(
        &impl_->converter, static_cast<ma_uint64>(remainingOut), &requiredIn);
    if (requiredIn == 0) break;

    if (!impl_->pumpAudio(static_cast<size_t>(requiredIn))) {
      return false;
    }

    size_t available = impl_->availableFrames();
    if (available == 0) {
      break;
    }

    ma_uint64 inFrames = static_cast<ma_uint64>(available);
    ma_uint64 outFrames = static_cast<ma_uint64>(remainingOut);
    const int16_t* inPtr = impl_->inputBuffer.data() + impl_->inputReadPos;
    if (ma_data_converter_process_pcm_frames(
            &impl_->converter, inPtr, &inFrames,
            out + produced * impl_->channels, &outFrames) != MA_SUCCESS) {
      return false;
    }

    consumeInputFrames(&impl_->inputBuffer, &impl_->inputReadPos,
                       static_cast<size_t>(inFrames));
    produced += static_cast<uint64_t>(outFrames);
    if (outFrames == 0) break;
  }

  impl_->framePos += produced;
  if (impl_->totalFrames > 0 && impl_->framePos >= impl_->totalFrames) {
    impl_->atEnd = true;
  }
  if (framesRead) *framesRead = produced;
  return true;
}

bool GsfAudioDecoder::seekToFrame(uint64_t frame) {
  if (!impl_) return false;
  if (impl_->totalFrames > 0 && frame > impl_->totalFrames) {
    frame = impl_->totalFrames;
  }

  if (impl_->converterInit) {
    ma_data_converter_reset(&impl_->converter);
  }

  impl_->shutdownCore();
  impl_->resetBuffer();
  impl_->framePos = 0;
  impl_->atEnd = false;
  decode_pos_ms = 0.0;
  seek_needed = -1;

  if (!impl_->initCore(nullptr)) {
    return false;
  }

  if (frame == 0) return true;

  std::vector<float> scratch(static_cast<size_t>(kSeekChunkFrames) *
                             impl_->channels);
  uint64_t remaining = frame;
  while (remaining > 0) {
    uint32_t chunk = remaining > kSeekChunkFrames
                         ? kSeekChunkFrames
                         : static_cast<uint32_t>(remaining);
    uint64_t read = 0;
    if (!readFrames(scratch.data(), chunk, &read)) {
      return false;
    }
    if (read == 0) break;
    remaining -= read;
  }
  return true;
}

bool GsfAudioDecoder::getTotalFrames(uint64_t* outFrames) const {
  if (!impl_ || !outFrames) return false;
  *outFrames = impl_->totalFrames;
  return true;
}

#else

GsfAudioDecoder::GsfAudioDecoder() = default;
GsfAudioDecoder::~GsfAudioDecoder() { uninit(); }

bool GsfAudioDecoder::init(const std::filesystem::path&, uint32_t,
                           uint32_t, std::string* error, int) {
  if (error) *error = "GSF support not enabled.";
  return false;
}

void GsfAudioDecoder::uninit() {}

bool GsfAudioDecoder::readFrames(float*, uint32_t, uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  return false;
}

bool GsfAudioDecoder::seekToFrame(uint64_t) {
  return false;
}

bool GsfAudioDecoder::getTotalFrames(uint64_t* outFrames) const {
  if (!outFrames) return false;
  *outFrames = 0;
  return false;
}

#endif

bool gsfListTracks(const std::filesystem::path& path,
                   std::vector<TrackEntry>* out,
                   std::string* error) {
  if (!out) return false;
  out->clear();

#if RADIOIFY_DISABLE_GSF_GPL
  if (error) *error = "GSF support not enabled.";
  return false;
#else
  GsfMetadata meta;
  if (!collectGsfMetadata(path, &meta, error)) {
    return false;
  }

  TrackEntry entry{};
  entry.index = 0;
  entry.title = meta.title;
  entry.lengthMs = computeTrackLengthMs(meta);
  out->push_back(std::move(entry));
  return true;
#endif
}
