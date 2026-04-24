#include "bitmap_renderer.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

#if RADIOIFY_HAS_LIBASS
extern "C" {
#include <ass/ass.h>
}
#endif

namespace {

std::string nowMsString() {
  using namespace std::chrono;
  std::ostringstream out;
  out << duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
             .count();
  return out.str();
}

std::string threadIdString() {
  std::ostringstream out;
  out << std::this_thread::get_id();
  return out.str();
}

uint64_t assScriptFingerprint(const std::string& script) {
  constexpr uint64_t kFNVOffset = 1469598103934665603ull;
  constexpr uint64_t kFNVPrime = 1099511628211ull;
  uint64_t hash = kFNVOffset;
  for (unsigned char ch : script) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= kFNVPrime;
  }
  return hash;
}

#if RADIOIFY_HAS_LIBASS
class LibassBitmapRenderer {
 public:
  LibassBitmapRenderer() = default;

  ~LibassBitmapRenderer() { resetAssState(); }

  SubtitleAssRenderResult render(
      const std::shared_ptr<const std::string>& assScript,
      const std::shared_ptr<const SubtitleFontAttachmentList>& assFonts,
      int64_t clockUs, int canvasW, int canvasH,
      std::vector<uint8_t>* outCanvas) {
    if (!outCanvas || canvasW <= 0 || canvasH <= 0) {
      return {SubtitleAssRenderStatus::Error, "Invalid ASS canvas target."};
    }

    const size_t pixelBytes = static_cast<size_t>(canvasW) *
                              static_cast<size_t>(canvasH) * 4u;
    if (outCanvas->size() != pixelBytes) {
      outCanvas->assign(pixelBytes, static_cast<uint8_t>(0));
    } else {
      std::fill(outCanvas->begin(), outCanvas->end(), static_cast<uint8_t>(0));
    }

    if (!assScript || assScript->empty()) {
      return {SubtitleAssRenderStatus::NoGlyph, {}};
    }

    const uint64_t scriptHash = assScriptFingerprint(*assScript);
    std::string parseError;
    if (!ensureTrack(assScript, assFonts, &parseError)) {
      if (parseError.empty()) {
        parseError = "Failed to parse ASS script.";
      }
      logErrorOnce(scriptHash, parseError);
      return {SubtitleAssRenderStatus::Error, parseError};
    }

    ass_set_frame_size(renderer_, canvasW, canvasH);
    ass_set_storage_size(renderer_, canvasW, canvasH);

    lastErrorMessage_.clear();
    int detectChange = 0;
    ASS_Image* img = ass_render_frame(
        renderer_, track_,
        static_cast<long long>(std::max<int64_t>(0, clockUs / 1000)),
        &detectChange);
    (void)detectChange;

    if (!lastErrorMessage_.empty()) {
      std::string renderError = "libass render error: " + lastErrorMessage_;
      logErrorOnce(scriptHash, renderError);
      return {SubtitleAssRenderStatus::Error, renderError};
    }

    bool drewAny = false;
    for (ASS_Image* cur = img; cur != nullptr; cur = cur->next) {
      if (!cur->bitmap || cur->w <= 0 || cur->h <= 0) continue;
      const uint8_t r = static_cast<uint8_t>((cur->color >> 24) & 0xFFu);
      const uint8_t g = static_cast<uint8_t>((cur->color >> 16) & 0xFFu);
      const uint8_t b = static_cast<uint8_t>((cur->color >> 8) & 0xFFu);
      const float colorAlpha =
          static_cast<float>(255u - (cur->color & 0xFFu)) / 255.0f;
      if (colorAlpha <= 0.0f) continue;
      drewAny = true;

      for (int y = 0; y < cur->h; ++y) {
        const int dstY = cur->dst_y + y;
        if (dstY < 0 || dstY >= canvasH) continue;
        const uint8_t* srcRow =
            cur->bitmap + static_cast<std::ptrdiff_t>(y) * cur->stride;
        for (int x = 0; x < cur->w; ++x) {
          const int dstX = cur->dst_x + x;
          if (dstX < 0 || dstX >= canvasW) continue;
          const float cov = static_cast<float>(srcRow[x]) / 255.0f;
          if (cov <= 0.0f) continue;

          const float srcA = std::clamp(cov * colorAlpha, 0.0f, 1.0f);
          const size_t dstIdx =
              (static_cast<size_t>(dstY) * static_cast<size_t>(canvasW) +
               static_cast<size_t>(dstX)) *
              4u;
          const float dstA =
              static_cast<float>((*outCanvas)[dstIdx + 3]) / 255.0f;
          const float outA = srcA + dstA * (1.0f - srcA);
          if (outA <= 0.0001f) continue;

          const float srcRgb[3] = {static_cast<float>(b) / 255.0f,
                                   static_cast<float>(g) / 255.0f,
                                   static_cast<float>(r) / 255.0f};
          for (int c = 0; c < 3; ++c) {
            const float dstC =
                static_cast<float>((*outCanvas)[dstIdx + static_cast<size_t>(c)]) /
                255.0f;
            const float outC =
                (srcRgb[c] * srcA + dstC * dstA * (1.0f - srcA)) / outA;
            (*outCanvas)[dstIdx + static_cast<size_t>(c)] =
                static_cast<uint8_t>(
                    std::lround(255.0f * std::clamp(outC, 0.0f, 1.0f)));
          }
          (*outCanvas)[dstIdx + 3] = static_cast<uint8_t>(
              std::lround(255.0f * std::clamp(outA, 0.0f, 1.0f)));
        }
      }
    }

    return {drewAny ? SubtitleAssRenderStatus::WithGlyph
                    : SubtitleAssRenderStatus::NoGlyph,
            {}};
  }

 private:
  bool ensureRenderer(
      const std::shared_ptr<const SubtitleFontAttachmentList>& assFonts,
      std::string* outError) {
    std::shared_ptr<const SubtitleFontAttachmentList> loadedFonts =
        loadedFonts_.lock();
    if (library_ && renderer_ && loadedFonts.get() == assFonts.get()) {
      return true;
    }

    resetAssState();

    library_ = ass_library_init();
    if (!library_) {
      initError_ = "libass library initialization failed";
      if (outError) *outError = initError_;
      return false;
    }
    ass_set_message_cb(library_, &LibassBitmapRenderer::logCallback, this);

    if (assFonts) {
      for (const SubtitleFontAttachment& attachment : *assFonts) {
        if (attachment.filename.empty() || attachment.data.empty() ||
            attachment.data.size() >
                static_cast<size_t>((std::numeric_limits<int>::max)())) {
          continue;
        }
        ass_add_font(library_, attachment.filename.c_str(),
                     reinterpret_cast<const char*>(attachment.data.data()),
                     static_cast<int>(attachment.data.size()));
      }
    }

    renderer_ = ass_renderer_init(library_);
    if (!renderer_) {
      initError_ = "libass renderer initialization failed";
      if (outError) *outError = initError_;
      return false;
    }
    ass_set_fonts(renderer_, nullptr, nullptr, 1, nullptr, 1);
    loadedFonts_ = assFonts;
    return true;
  }

  bool ensureTrack(const std::shared_ptr<const std::string>& assScript,
                   const std::shared_ptr<const SubtitleFontAttachmentList>& assFonts,
                   std::string* outError) {
    if (!assScript) {
      if (outError) *outError = "libass track input is invalid.";
      return false;
    }
    if (!ensureRenderer(assFonts, outError)) {
      return false;
    }
    if (track_) {
      std::shared_ptr<const std::string> loaded = loadedScript_.lock();
      if (loaded && loaded.get() == assScript.get()) return true;
    }

    if (track_) {
      ass_free_track(track_);
      track_ = nullptr;
    }
    loadedScript_.reset();
    scriptCache_.clear();

    scriptCache_ = *assScript;
    if (scriptCache_.empty()) {
      if (outError) *outError = "ASS script is empty.";
      return false;
    }

    lastErrorMessage_.clear();
    track_ = ass_read_memory(library_, scriptCache_.data(),
                             static_cast<int>(scriptCache_.size()), nullptr);
    if (!track_) {
      if (outError) {
        *outError = lastErrorMessage_.empty()
                        ? "libass failed to parse ASS script."
                        : ("libass parse error: " + lastErrorMessage_);
      }
      scriptCache_.clear();
      return false;
    }
    if (track_->n_events <= 0) {
      if (outError) {
        *outError = "libass parsed ASS script without dialogue events.";
      }
      ass_free_track(track_);
      track_ = nullptr;
      scriptCache_.clear();
      return false;
    }
    loadedScript_ = assScript;
    return true;
  }

  void resetAssState() {
    if (track_) {
      ass_free_track(track_);
      track_ = nullptr;
    }
    if (renderer_) {
      ass_renderer_done(renderer_);
      renderer_ = nullptr;
    }
    if (library_) {
      ass_library_done(library_);
      library_ = nullptr;
    }
    loadedScript_.reset();
    loadedFonts_.reset();
    scriptCache_.clear();
    initError_.clear();
    lastErrorMessage_.clear();
  }

  void logErrorOnce(uint64_t scriptHash, const std::string& message) {
    if (message.empty()) return;
    if (loggedErrorHashes_.insert(scriptHash).second) {
      std::fprintf(stderr,
                   "[%s] [tid=%s] ASS renderer error [script=%016llx]: %s\n",
                   nowMsString().c_str(), threadIdString().c_str(),
                   static_cast<unsigned long long>(scriptHash),
                   message.c_str());
    }
  }

  static void logCallback(int level, const char* fmt, va_list va, void* data) {
    if (level > 1 || !fmt || !data) return;
    auto* self = static_cast<LibassBitmapRenderer*>(data);
    char buffer[1024];
    const int written = std::vsnprintf(buffer, sizeof(buffer), fmt, va);
    if (written <= 0) return;
    self->lastErrorMessage_.assign(buffer);
    while (!self->lastErrorMessage_.empty()) {
      char tail = self->lastErrorMessage_.back();
      if (tail == '\n' || tail == '\r' || tail == '\t' || tail == ' ') {
        self->lastErrorMessage_.pop_back();
      } else {
        break;
      }
    }
  }

  ASS_Library* library_ = nullptr;
  ASS_Renderer* renderer_ = nullptr;
  ASS_Track* track_ = nullptr;
  std::weak_ptr<const std::string> loadedScript_;
  std::weak_ptr<const SubtitleFontAttachmentList> loadedFonts_;
  std::string scriptCache_;
  std::string initError_;
  std::string lastErrorMessage_;
  std::unordered_set<uint64_t> loggedErrorHashes_;
};
#endif

}  // namespace

SubtitleAssRenderResult renderAssSubtitlesToBgraCanvas(
    const std::shared_ptr<const std::string>& assScript,
    const std::shared_ptr<const SubtitleFontAttachmentList>& assFonts,
    int64_t clockUs, int canvasW, int canvasH,
    std::vector<uint8_t>* outCanvas) {
#if RADIOIFY_HAS_LIBASS
  static LibassBitmapRenderer renderer;
  return renderer.render(assScript, assFonts, clockUs, canvasW, canvasH,
                         outCanvas);
#else
  (void)assScript;
  (void)assFonts;
  (void)clockUs;
  (void)canvasW;
  (void)canvasH;
  (void)outCanvas;
  return {SubtitleAssRenderStatus::Error,
          "ASS renderer unavailable: build without libass."};
#endif
}
