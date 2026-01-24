#include "psfaudio.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "miniaudio.h"

extern "C" {
#include <psflib/psf2fs.h>
#include <psflib/psflib.h>
#include <Highly_Experimental/Core/bios.h>
#include <Highly_Experimental/Core/iop.h>
#include <Highly_Experimental/Core/mkhebios.h>
#include <Highly_Experimental/Core/psx.h>
#include <Highly_Experimental/Core/r3000.h>
#include <Highly_Experimental/Core/spu.h>
#include <psx/driver.h>
}

namespace {
constexpr uint32_t kPsfSampleRate = 44100;
constexpr uint32_t kPsfChannels = 2;
constexpr uint32_t kPsfChunkFrames = 1024;
constexpr int kDefaultTrackLengthMs = 150000;
constexpr size_t kSexyMaxBufferedFrames =
    static_cast<size_t>(kPsfSampleRate) * 2;  // Throttle sexypsf buffering.

enum class PsfBackend {
  HighlyExperimental,
  SexyPsf
};

struct PsfMetadata {
  std::string title;
  int lengthMs = -1;
  int fadeMs = 0;
  int version = 0;
};

struct Psf1LoadState {
  void* emu = nullptr;
  bool first = true;
  unsigned refresh = 0;
};

struct SexyPsfState {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<int16_t> buffer;
  size_t readPos = 0;
  bool inputEnded = false;
  bool stopRequested = false;
};

std::atomic<SexyPsfState*> g_sexyState{nullptr};

typedef struct {
  uint32_t pc0;
  uint32_t gp0;
  uint32_t t_addr;
  uint32_t t_size;
  uint32_t d_addr;
  uint32_t d_size;
  uint32_t b_addr;
  uint32_t b_size;
  uint32_t s_ptr;
  uint32_t s_size;
  uint32_t sp, fp, gp, ret, base;
} exec_header_t;

typedef struct {
  char key[8];
  uint32_t text;
  uint32_t data;
  exec_header_t exec;
  char title[60];
} psxexe_hdr_t;

struct FreeDeleter {
  void operator()(uint8_t* ptr) const {
    std::free(ptr);
  }
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

bool startsWithNoCase(const char* text, const char* prefix) {
  if (!text || !prefix) return false;
  while (*prefix) {
    unsigned char a = static_cast<unsigned char>(*text++);
    unsigned char b = static_cast<unsigned char>(*prefix++);
    if (a == '\0') return false;
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

int psfInfoCallback(void* context, const char* name, const char* value) {
  if (!context || !name || !value) return 0;
  auto* meta = static_cast<PsfMetadata*>(context);
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

void* psf_file_fopen(const char* uri) {
  return std::fopen(uri, "rb");
}

size_t psf_file_fread(void* buffer, size_t size, size_t count, void* handle) {
  return std::fread(buffer, size, count, static_cast<FILE*>(handle));
}

int psf_file_fseek(void* handle, int64_t offset, int whence) {
  return std::fseek(static_cast<FILE*>(handle), static_cast<long>(offset),
                    whence);
}

int psf_file_fclose(void* handle) {
  return std::fclose(static_cast<FILE*>(handle));
}

long psf_file_ftell(void* handle) {
  return std::ftell(static_cast<FILE*>(handle));
}

const psf_file_callbacks psf_file_system = {
    "\\/|:", psf_file_fopen, psf_file_fread,
    psf_file_fseek, psf_file_fclose, psf_file_ftell};

int psf1_load(void* context, const uint8_t* exe, size_t exe_size,
              const uint8_t*, size_t) {
  auto* state = static_cast<Psf1LoadState*>(context);
  if (!state || !exe || exe_size < 0x800) return -1;

  const psxexe_hdr_t* psx = reinterpret_cast<const psxexe_hdr_t*>(exe);
  uint32_t addr = psx->exec.t_addr;
  uint32_t size = static_cast<uint32_t>(exe_size - 0x800);

  addr &= 0x1fffff;
  if (addr < 0x10000 || size > 0x1f0000 || addr + size > 0x200000) {
    return -1;
  }

  void* pIOP = psx_get_iop_state(state->emu);
  iop_upload_to_ram(pIOP, addr, exe + 0x800, size);

  if (!state->refresh) {
    const char* region = reinterpret_cast<const char*>(exe + 113);
    if (startsWithNoCase(region, "Japan")) {
      state->refresh = 60;
    } else if (startsWithNoCase(region, "Europe")) {
      state->refresh = 50;
    } else if (startsWithNoCase(region, "North America")) {
      state->refresh = 60;
    }
  }

  if (state->first) {
    void* pR3000 = iop_get_r3000_state(pIOP);
    r3000_setreg(pR3000, R3000_REG_PC, psx->exec.pc0);
    r3000_setreg(pR3000, R3000_REG_GEN + 29, psx->exec.s_ptr);
    state->first = false;
  }
  return 0;
}

int EMU_CALL virtual_readfile(void* context, const char* path, int offset,
                              char* buffer, int length) {
  return psf2fs_virtual_readfile(context, path, offset, buffer, length);
}

bool collectPsfMetadata(const std::filesystem::path& path, PsfMetadata* meta,
                        std::string* error) {
  if (!meta) return false;
  *meta = PsfMetadata{};
  std::string pathUtf8 = toUtf8String(path);
  int version = psf_load(pathUtf8.c_str(), &psf_file_system, 0, nullptr,
                         nullptr, psfInfoCallback, meta, 0);
  if (version < 0) {
    if (error) *error = "Failed to read PSF metadata.";
    return false;
  }
  if (version != 1 && version != 2) {
    if (error) *error = "Unsupported PSF version.";
    return false;
  }
  meta->version = version;
  return true;
}

int computeTrackLengthMs(const PsfMetadata& meta) {
  int lengthMs = meta.lengthMs > 0 ? meta.lengthMs : kDefaultTrackLengthMs;
  int fadeMs = meta.fadeMs > 0 ? meta.fadeMs : 0;
  return lengthMs + fadeMs;
}

std::filesystem::path findBiosPath(const std::filesystem::path& psfPath) {
  const char* env = std::getenv("RADIOIFY_PSF_BIOS");
  if (env && env[0] != '\0') {
    return std::filesystem::path(env);
  }
  if (!psfPath.empty()) {
    std::filesystem::path local = psfPath.parent_path() / "hebios.bin";
    if (std::filesystem::exists(local)) return local;
  }
  std::filesystem::path cwd = std::filesystem::current_path() / "hebios.bin";
  if (std::filesystem::exists(cwd)) return cwd;
  return {};
}

bool ensureBiosLoaded(const std::filesystem::path& psfPath,
                      std::string* error) {
  static bool loaded = false;
  static std::vector<uint8_t> biosStorage;
  static std::unique_ptr<uint8_t, FreeDeleter> biosOwned;

  if (loaded) return true;

  std::filesystem::path biosPath = findBiosPath(psfPath);
  if (biosPath.empty() || !std::filesystem::exists(biosPath)) {
    if (error) {
      *error =
          "Missing hebios.bin (set RADIOIFY_PSF_BIOS or place it next to the file).";
    }
    return false;
  }

  std::string biosUtf8 = toUtf8String(biosPath);
  std::FILE* fp = std::fopen(biosUtf8.c_str(), "rb");
  if (!fp) {
    if (error) *error = "Failed to open hebios.bin.";
    return false;
  }

  uintmax_t size = 0;
  try {
    size = std::filesystem::file_size(biosPath);
  } catch (...) {
    std::fclose(fp);
    if (error) *error = "Failed to read hebios.bin size.";
    return false;
  }
  if (size == 0) {
    std::fclose(fp);
    if (error) *error = "hebios.bin is empty.";
    return false;
  }

  std::vector<uint8_t> data(size);
  size_t read = std::fread(data.data(), 1, data.size(), fp);
  std::fclose(fp);
  if (read != data.size()) {
    if (error) *error = "Failed to read hebios.bin.";
    return false;
  }

  int biosSize = static_cast<int>(data.size());
  uint8_t* biosData = data.data();
  std::unique_ptr<uint8_t, FreeDeleter> owned;
  if (biosSize >= 0x400000) {
    void* bios = mkhebios_create(data.data(), &biosSize);
    if (!bios) {
      if (error) *error = "Failed to prepare hebios.bin.";
      return false;
    }
    owned.reset(static_cast<uint8_t*>(bios));
    biosData = owned.get();
    data.clear();
  }

  bios_set_image(biosData, static_cast<uint32>(biosSize));
  int initResult = psx_init();
  if (initResult != 0) {
    if (error) *error = "Failed to initialize PSF core.";
    return false;
  }

  loaded = true;
  if (owned) {
    biosOwned = std::move(owned);
    biosStorage.clear();
  } else {
    biosStorage = std::move(data);
  }
  return true;
}

bool initEmulatorState(std::vector<uint8_t>* psxState,
                       void** psf2fs,
                       const std::filesystem::path& path,
                       int version,
                       std::string* error) {
  if (!psxState || !psf2fs) return false;
  *psf2fs = nullptr;

  uint32_t stateSize = psx_get_state_size(static_cast<uint8>(version));
  psxState->assign(stateSize, 0);
  psx_clear_state(psxState->data(), static_cast<uint8>(version));

  std::string pathUtf8 = toUtf8String(path);
  if (version == 1) {
    Psf1LoadState state;
    state.emu = psxState->data();
    state.first = true;
    state.refresh = 0;
    if (psf_load(pathUtf8.c_str(), &psf_file_system, 1, psf1_load, &state,
                 nullptr, nullptr, 0) < 0) {
      if (error) *error = "Invalid PSF file.";
      return false;
    }
    if (state.refresh) {
      psx_set_refresh(psxState->data(), state.refresh);
    }
  } else if (version == 2) {
    void* fs = psf2fs_create();
    if (!fs) {
      if (error) *error = "Failed to initialize PSF2 filesystem.";
      return false;
    }
    Psf1LoadState state;
    state.refresh = 0;
    if (psf_load(pathUtf8.c_str(), &psf_file_system, 2,
                 psf2fs_load_callback, fs, nullptr, &state, 0) < 0) {
      psf2fs_delete(fs);
      if (error) *error = "Invalid PSF2 file.";
      return false;
    }
    if (state.refresh) {
      psx_set_refresh(psxState->data(), state.refresh);
    }
    psx_set_readfile(psxState->data(), virtual_readfile, fs);
    *psf2fs = fs;
  } else {
    if (error) *error = "Unsupported PSF version.";
    return false;
  }

  void* iop = psx_get_iop_state(psxState->data());
  iop_set_compat(iop, IOP_COMPAT_FRIENDLY);
  spu_enable_reverb(iop_get_spu_state(iop), 1);
  return true;
}

size_t inputAvailableFrames(const std::vector<int16_t>& buffer,
                            size_t readPosSamples) {
  if (readPosSamples >= buffer.size()) return 0;
  return (buffer.size() - readPosSamples) / kPsfChannels;
}

void consumeInputFrames(std::vector<int16_t>* buffer, size_t* readPosSamples,
                        size_t frames) {
  if (!buffer || !readPosSamples) return;
  size_t samples = frames * kPsfChannels;
  if (*readPosSamples + samples > buffer->size()) {
    *readPosSamples = buffer->size();
  } else {
    *readPosSamples += samples;
  }
  if (*readPosSamples == 0) return;
  if (*readPosSamples >= buffer->size() / 2 ||
      *readPosSamples >= static_cast<size_t>(8192 * kPsfChannels)) {
    buffer->erase(buffer->begin(), buffer->begin() + *readPosSamples);
    *readPosSamples = 0;
  }
}
}  // namespace

extern "C" void sexyd_update(unsigned char* buffer, long count) {
  SexyPsfState* state = g_sexyState.load(std::memory_order_acquire);
  if (!state) return;
  if (!buffer || count <= 0) {
    state->cv.notify_all();
    return;
  }

  size_t samples = static_cast<size_t>(count) / sizeof(int16_t);
  size_t frames = samples / kPsfChannels;
  size_t sampleCount = frames * kPsfChannels;
  if (sampleCount == 0) {
    state->cv.notify_all();
    return;
  }

  std::unique_lock<std::mutex> lock(state->mutex);
  state->cv.wait(lock, [&]() {
    return state->stopRequested ||
           inputAvailableFrames(state->buffer, state->readPos) <
               kSexyMaxBufferedFrames;
  });
  if (state->stopRequested) return;

  size_t oldSize = state->buffer.size();
  state->buffer.resize(oldSize + sampleCount);
  std::memcpy(state->buffer.data() + oldSize, buffer,
              sampleCount * sizeof(int16_t));
  lock.unlock();
  state->cv.notify_all();
}

struct PsfAudioDecoder::Impl {
  std::filesystem::path path;
  PsfBackend backend = PsfBackend::HighlyExperimental;
  int version = 0;
  uint32_t channels = 0;
  uint32_t sampleRate = 0;
  uint64_t totalFrames = 0;
  uint64_t framePos = 0;
  bool atEnd = false;

  std::vector<uint8_t> psxState;
  void* psf2fs = nullptr;

  ma_data_converter converter{};
  bool converterInit = false;

  std::vector<int16_t> inputBuffer;
  size_t inputReadPos = 0;
  std::vector<int16_t> inputScratch;
  bool inputEnded = false;

  SexyPsfState sexy;
  std::thread sexyThread;

  void stopSexyPlayback();
  bool startSexyPlayback(int seekMs, std::string* error);
};

namespace {
void resetSexyState(SexyPsfState* state) {
  if (!state) return;
  std::lock_guard<std::mutex> lock(state->mutex);
  state->buffer.clear();
  state->readPos = 0;
  state->inputEnded = false;
  state->stopRequested = false;
}
}  // namespace

void PsfAudioDecoder::Impl::stopSexyPlayback() {
  if (!sexyThread.joinable()) {
    g_sexyState.store(nullptr, std::memory_order_release);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(sexy.mutex);
    sexy.stopRequested = true;
  }
  sexy.cv.notify_all();
  sexy_stop();
  sexyThread.join();
  g_sexyState.store(nullptr, std::memory_order_release);
  resetSexyState(&sexy);
}

bool PsfAudioDecoder::Impl::startSexyPlayback(int seekMs,
                                              std::string* error) {
  std::string pathUtf8 = toUtf8String(path);
  PSFINFO* info = sexy_load(pathUtf8.data());
  if (!info) {
    if (error) *error = "Failed to load PSF1.";
    return false;
  }
  sexy_freepsfinfo(info);

  resetSexyState(&sexy);
  g_sexyState.store(&sexy, std::memory_order_release);
  sexyThread = std::thread([this, seekMs]() {
    if (seekMs > 0) {
      sexy_seek(static_cast<u32>(seekMs));
    }
    sexy_execute();
    {
      std::lock_guard<std::mutex> lock(sexy.mutex);
      sexy.inputEnded = true;
    }
    sexy.cv.notify_all();
  });
  return true;
}

PsfAudioDecoder::PsfAudioDecoder() = default;
PsfAudioDecoder::~PsfAudioDecoder() { uninit(); }

bool PsfAudioDecoder::init(const std::filesystem::path& path,
                           uint32_t channels, uint32_t sampleRate,
                           std::string* error, int) {
  uninit();

  if (channels != 1 && channels != 2) {
    if (error) *error = "Unsupported channel count for PSF.";
    return false;
  }

  PsfMetadata meta;
  if (!collectPsfMetadata(path, &meta, error)) {
    return false;
  }

  std::unique_ptr<Impl> impl = std::make_unique<Impl>();
  impl->path = path;
  impl->backend = (meta.version == 1) ? PsfBackend::SexyPsf
                                      : PsfBackend::HighlyExperimental;
  impl->version = meta.version;
  impl->channels = channels;
  impl->sampleRate = sampleRate;

  if (impl->backend == PsfBackend::HighlyExperimental) {
    if (!ensureBiosLoaded(path, error)) {
      return false;
    }
    if (!initEmulatorState(&impl->psxState, &impl->psf2fs, path, impl->version,
                           error)) {
      return false;
    }
  }

  ma_data_converter_config config = ma_data_converter_config_init(
      ma_format_s16, ma_format_f32, kPsfChannels, channels, kPsfSampleRate,
      sampleRate);
  if (ma_data_converter_init(&config, nullptr, &impl->converter) != MA_SUCCESS) {
    if (error) *error = "Failed to initialize PSF resampler.";
    return false;
  }
  impl->converterInit = true;

  if (impl->backend == PsfBackend::SexyPsf) {
    if (!impl->startSexyPlayback(0, error)) {
      ma_data_converter_uninit(&impl->converter, nullptr);
      return false;
    }
  }

  int lengthMs = computeTrackLengthMs(meta);
  impl->totalFrames = msToFrames(lengthMs, sampleRate);
  impl->framePos = 0;
  impl->atEnd = false;
  impl->inputEnded = false;
  impl->inputBuffer.clear();
  impl->inputReadPos = 0;
  impl->inputScratch.clear();

  impl_ = impl.release();
  return true;
}

void PsfAudioDecoder::uninit() {
  if (!impl_) return;
  if (impl_->backend == PsfBackend::SexyPsf) {
    impl_->stopSexyPlayback();
  }
  if (impl_->converterInit) {
    ma_data_converter_uninit(&impl_->converter, nullptr);
  }
  if (impl_->psf2fs) {
    psf2fs_delete(impl_->psf2fs);
  }
  delete impl_;
  impl_ = nullptr;
}

bool PsfAudioDecoder::readFrames(float* out, uint32_t frameCount,
                                 uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  if (!impl_ || !out || frameCount == 0) return false;
  if (impl_->atEnd) return true;

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

  if (impl_->backend == PsfBackend::SexyPsf) {
    uint64_t produced = 0;
    while (produced < toRead) {
      uint64_t remainingOut = toRead - produced;
      ma_uint64 requiredIn = 0;
      ma_data_converter_get_required_input_frame_count(
          &impl_->converter, static_cast<ma_uint64>(remainingOut), &requiredIn);
      if (requiredIn == 0) break;

      ma_uint64 inFrames = 0;
      ma_uint64 outFrames = static_cast<ma_uint64>(remainingOut);
      bool drained = false;
      {
        std::unique_lock<std::mutex> lock(impl_->sexy.mutex);
        impl_->sexy.cv.wait(lock, [&]() {
          return impl_->sexy.stopRequested || impl_->sexy.inputEnded ||
                 inputAvailableFrames(impl_->sexy.buffer,
                                      impl_->sexy.readPos) >= requiredIn;
        });
        if (impl_->sexy.stopRequested) return false;

        size_t available =
            inputAvailableFrames(impl_->sexy.buffer, impl_->sexy.readPos);
        if (available == 0 && impl_->sexy.inputEnded) {
          impl_->atEnd = true;
          break;
        }

        inFrames = static_cast<ma_uint64>(available);
        const int16_t* inPtr =
            impl_->sexy.buffer.data() + impl_->sexy.readPos;
        if (ma_data_converter_process_pcm_frames(
                &impl_->converter, inPtr, &inFrames,
                out + produced * impl_->channels, &outFrames) != MA_SUCCESS) {
          return false;
        }
        consumeInputFrames(&impl_->sexy.buffer, &impl_->sexy.readPos,
                           static_cast<size_t>(inFrames));
        drained = (outFrames == 0 && impl_->sexy.inputEnded &&
                   inputAvailableFrames(impl_->sexy.buffer,
                                        impl_->sexy.readPos) == 0);
      }
      impl_->sexy.cv.notify_all();
      produced += static_cast<uint64_t>(outFrames);
      if (drained) {
        impl_->atEnd = true;
        break;
      }
    }

    impl_->framePos += produced;
    if (impl_->totalFrames > 0 && impl_->framePos >= impl_->totalFrames) {
      impl_->atEnd = true;
    }
    if (framesRead) *framesRead = produced;
    return true;
  }

  uint64_t produced = 0;
  while (produced < toRead) {
    uint64_t remainingOut = toRead - produced;
    ma_uint64 requiredIn = 0;
    ma_data_converter_get_required_input_frame_count(
        &impl_->converter, static_cast<ma_uint64>(remainingOut), &requiredIn);
    if (requiredIn == 0) break;

    while (!impl_->inputEnded &&
           inputAvailableFrames(impl_->inputBuffer, impl_->inputReadPos) <
               requiredIn) {
      uint32_t request = kPsfChunkFrames;
      uint64_t missing =
          requiredIn -
          inputAvailableFrames(impl_->inputBuffer, impl_->inputReadPos);
      if (missing > request) request = static_cast<uint32_t>(missing);

      impl_->inputScratch.resize(static_cast<size_t>(request) * kPsfChannels);
      uint32_t framesOut = request;
      int err =
          psx_execute(impl_->psxState.data(), 0x7FFFFFFF,
                      impl_->inputScratch.data(), &framesOut, 0);
      if (err <= -2) {
        return false;
      }
      if (framesOut == 0) {
        if (err < 0) impl_->inputEnded = true;
        break;
      }
      size_t oldSize = impl_->inputBuffer.size();
      impl_->inputBuffer.resize(oldSize +
                                static_cast<size_t>(framesOut) * kPsfChannels);
      std::memcpy(impl_->inputBuffer.data() + oldSize,
                  impl_->inputScratch.data(),
                  static_cast<size_t>(framesOut) * kPsfChannels *
                      sizeof(int16_t));
      if (err < 0) {
        impl_->inputEnded = true;
      }
    }

    size_t available =
        inputAvailableFrames(impl_->inputBuffer, impl_->inputReadPos);
    if (available == 0) {
      impl_->atEnd = true;
      break;
    }

    ma_uint64 inFrames = static_cast<ma_uint64>(available);
    ma_uint64 outFrames = static_cast<ma_uint64>(remainingOut);
    const int16_t* inPtr =
        impl_->inputBuffer.data() + impl_->inputReadPos;
    if (ma_data_converter_process_pcm_frames(
            &impl_->converter, inPtr, &inFrames,
            out + produced * impl_->channels, &outFrames) != MA_SUCCESS) {
      return false;
    }

    consumeInputFrames(&impl_->inputBuffer, &impl_->inputReadPos,
                       static_cast<size_t>(inFrames));
    produced += static_cast<uint64_t>(outFrames);
    if (outFrames == 0 && impl_->inputEnded &&
        inputAvailableFrames(impl_->inputBuffer, impl_->inputReadPos) == 0) {
      impl_->atEnd = true;
      break;
    }
  }

  impl_->framePos += produced;
  if (impl_->totalFrames > 0 && impl_->framePos >= impl_->totalFrames) {
    impl_->atEnd = true;
  }
  if (framesRead) *framesRead = produced;
  return true;
}

bool PsfAudioDecoder::seekToFrame(uint64_t frame) {
  if (!impl_) return false;
  if (impl_->totalFrames > 0 && frame > impl_->totalFrames) {
    frame = impl_->totalFrames;
  }

  if (impl_->backend == PsfBackend::SexyPsf) {
    if (impl_->converterInit) {
      ma_data_converter_reset(&impl_->converter);
    }
    impl_->stopSexyPlayback();
    impl_->framePos = 0;
    impl_->atEnd = false;

    uint64_t seekMs64 = 0;
    if (impl_->sampleRate > 0) {
      seekMs64 = (frame * 1000) / impl_->sampleRate;
    }
    int seekMs = seekMs64 > static_cast<uint64_t>(std::numeric_limits<int>::max())
                     ? std::numeric_limits<int>::max()
                     : static_cast<int>(seekMs64);
    std::string error;
    if (!impl_->startSexyPlayback(seekMs, &error)) {
      return false;
    }
    impl_->framePos = frame;
    return true;
  }

  std::string error;
  if (impl_->psf2fs) {
    psf2fs_delete(impl_->psf2fs);
    impl_->psf2fs = nullptr;
  }
  if (!initEmulatorState(&impl_->psxState, &impl_->psf2fs, impl_->path,
                         impl_->version, &error)) {
    return false;
  }
  if (impl_->converterInit) {
    ma_data_converter_reset(&impl_->converter);
  }

  impl_->framePos = 0;
  impl_->atEnd = false;
  impl_->inputEnded = false;
  impl_->inputBuffer.clear();
  impl_->inputReadPos = 0;

  if (frame == 0) return true;

  constexpr uint32_t kSkipChunk = 4096;
  std::vector<float> scratch(static_cast<size_t>(kSkipChunk) *
                             impl_->channels);
  uint64_t remaining = frame;
  while (remaining > 0) {
    uint32_t chunk =
        remaining > kSkipChunk ? kSkipChunk : static_cast<uint32_t>(remaining);
    uint64_t read = 0;
    if (!readFrames(scratch.data(), chunk, &read)) {
      return false;
    }
    if (read == 0) break;
    remaining -= read;
  }
  return true;
}

bool PsfAudioDecoder::getTotalFrames(uint64_t* outFrames) const {
  if (!impl_ || !outFrames) return false;
  *outFrames = impl_->totalFrames;
  return true;
}

bool psfListTracks(const std::filesystem::path& path,
                   std::vector<TrackEntry>* out,
                   std::string* error) {
  if (!out) return false;
  out->clear();

  PsfMetadata meta;
  if (!collectPsfMetadata(path, &meta, error)) {
    return false;
  }

  TrackEntry entry{};
  entry.index = 0;
  entry.title = meta.title;
  entry.lengthMs = computeTrackLengthMs(meta);
  out->push_back(std::move(entry));
  return true;
}
