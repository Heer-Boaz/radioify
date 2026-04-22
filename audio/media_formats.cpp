#include "media_formats.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "runtime_helpers.h"

namespace {

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

}

bool isSupportedAudioExt(const std::filesystem::path& path) {
  const std::string ext = toLower(toUtf8String(path.extension()));
  return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".m4a" ||
         ext == ".webm" || ext == ".mp4" || ext == ".mov" || ext == ".ogg" ||
         ext == ".kss" || ext == ".nsf" ||
#if !RADIOIFY_DISABLE_GSF_GPL
         ext == ".gsf" || ext == ".minigsf" ||
#endif
         ext == ".mid" || ext == ".midi" ||
         ext == ".vgm" || ext == ".vgz" ||
         ext == ".psf" || ext == ".minipsf" || ext == ".psf2" || ext == ".minipsf2";
}

bool isSupportedVideoExt(const std::filesystem::path& path) {
  const std::string ext = toLower(toUtf8String(path.extension()));
  return ext == ".mp4" || ext == ".m4v" || ext == ".webm" ||
         ext == ".mov" || ext == ".qt" || ext == ".mkv" ||
         ext == ".avi" || ext == ".wmv" || ext == ".asf" ||
         ext == ".flv" || ext == ".mpg" || ext == ".mpeg" ||
         ext == ".mpe" || ext == ".mpv" || ext == ".m2v" ||
         ext == ".ts" || ext == ".m2ts" || ext == ".mts" ||
         ext == ".3gp" || ext == ".3g2" || ext == ".ogv" ||
         ext == ".vob";
}

bool isSupportedImageExt(const std::filesystem::path& path) {
  const std::string ext = toLower(toUtf8String(path.extension()));
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp";
}

bool isSupportedMediaExt(const std::filesystem::path& path) {
  return isSupportedAudioExt(path) || isSupportedVideoExt(path) ||
         isSupportedImageExt(path);
}

bool isMiniaudioExt(const std::filesystem::path& path) {
  const std::string ext = toLower(toUtf8String(path.extension()));
  return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

bool isFlacExt(const std::filesystem::path& path) {
  return toLower(toUtf8String(path.extension())) == ".flac";
}

bool isGmeExt(const std::filesystem::path& path) {
  return toLower(toUtf8String(path.extension())) == ".nsf";
}

bool isMidiExt(const std::filesystem::path& path) {
  const std::string ext = toLower(toUtf8String(path.extension()));
  return ext == ".mid" || ext == ".midi";
}

bool isGsfExt(const std::filesystem::path& path) {
#if !RADIOIFY_DISABLE_GSF_GPL
  const std::string ext = toLower(toUtf8String(path.extension()));
  return ext == ".gsf" || ext == ".minigsf";
#else
  (void)path;
  return false;
#endif
}

bool isVgmExt(const std::filesystem::path& path) {
  const std::string ext = toLower(toUtf8String(path.extension()));
  return ext == ".vgm" || ext == ".vgz";
}

bool isM4aExt(const std::filesystem::path& path) {
  const std::string ext = toLower(toUtf8String(path.extension()));
  return ext == ".m4a" || ext == ".mp4" || ext == ".webm" || ext == ".mov";
}

bool isKssExt(const std::filesystem::path& path) {
  return toLower(toUtf8String(path.extension())) == ".kss";
}

bool isPsfExt(const std::filesystem::path& path) {
  const std::string ext = toLower(toUtf8String(path.extension()));
  return ext == ".psf" || ext == ".minipsf" || ext == ".psf2" || ext == ".minipsf2";
}

bool isOggExt(const std::filesystem::path& path) {
  return toLower(toUtf8String(path.extension())) == ".ogg";
}

std::string supportedAudioExtensionsText() {
  std::string list = ".wav, .mp3, .flac, .m4a, .webm, .mp4, .mov, .ogg, .kss, .nsf, ";
#if !RADIOIFY_DISABLE_GSF_GPL
  list += ".gsf, .minigsf, ";
#endif
  list += ".mid, .midi, .vgm, .vgz, .psf, .minipsf, .psf2, .minipsf2";
  return list;
}
