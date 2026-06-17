#include "media_formats.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>

#include "runtime_helpers.h"

namespace {

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string lowerExtension(const std::filesystem::path& path) {
  return toLower(toUtf8String(path.extension()));
}

bool hasExtension(std::string_view ext,
                  std::initializer_list<std::string_view> candidates) {
  for (std::string_view candidate : candidates) {
    if (ext == candidate) return true;
  }
  return false;
}

bool isMiniaudioExtValue(std::string_view ext) {
  return hasExtension(ext, {".wav", ".mp3"});
}

bool isFfmpegAudioExtValue(std::string_view ext) {
  return hasExtension(ext, {".flac", ".ogg", ".wma", ".aac", ".ac3",
                            ".eac3", ".aif", ".aiff", ".aifc", ".opus",
                            ".oga", ".mka", ".wv", ".tta", ".caf", ".au",
                            ".mp2", ".ape", ".tak", ".amr", ".ra", ".dts",
                            ".dsf", ".qcp", ".spx", ".mpc", ".xwma",
                            ".w64", ".voc", ".awb", ".gsm", ".oma",
                            ".aa", ".aax", ".mlp", ".truehd", ".ac4",
                            ".loas", ".latm"});
}

bool isM4aExtValue(std::string_view ext) {
  return hasExtension(ext, {".m4a", ".m4b", ".m4r", ".m4p", ".mp4",
                            ".webm", ".mov", ".mkv"});
}

bool isGmeExtValue(std::string_view ext) {
  return ext == ".nsf";
}

bool isMidiExtValue(std::string_view ext) {
  return hasExtension(ext, {".mid", ".midi"});
}

bool isGsfExtValue(std::string_view ext) {
#if !RADIOIFY_DISABLE_GSF_GPL
  return hasExtension(ext, {".gsf", ".minigsf"});
#else
  (void)ext;
  return false;
#endif
}

bool isVgmExtValue(std::string_view ext) {
  return hasExtension(ext, {".vgm", ".vgz"});
}

bool isKssExtValue(std::string_view ext) {
  return ext == ".kss";
}

bool isPsfExtValue(std::string_view ext) {
  return hasExtension(ext, {".psf", ".minipsf", ".psf2", ".minipsf2"});
}

bool isSupportedAudioExtValue(std::string_view ext) {
  return isMiniaudioExtValue(ext) || isFfmpegAudioExtValue(ext) ||
         isM4aExtValue(ext) || isGmeExtValue(ext) || isMidiExtValue(ext) ||
         isGsfExtValue(ext) || isVgmExtValue(ext) || isKssExtValue(ext) ||
         isPsfExtValue(ext);
}

}

bool isSupportedAudioExt(const std::filesystem::path& path) {
  return isSupportedAudioExtValue(lowerExtension(path));
}

bool isSupportedVideoExt(const std::filesystem::path& path) {
  const std::string ext = lowerExtension(path);
  return ext == ".mp4" || ext == ".m4v" || ext == ".webm" ||
         ext == ".mov" || ext == ".qt" || ext == ".mkv" ||
         ext == ".avi" || ext == ".wmv" || ext == ".asf" ||
         ext == ".flv" || ext == ".mpg" || ext == ".mpeg" ||
         ext == ".mpe" || ext == ".mpv" || ext == ".m2v" ||
         ext == ".ts" || ext == ".m2ts" || ext == ".mts" ||
         ext == ".3gp" || ext == ".3g2" || ext == ".ogv" ||
         ext == ".vob" || ext == ".mxf" || ext == ".f4v" ||
         ext == ".dv" || ext == ".ogm" || ext == ".ivf" ||
         ext == ".nut" || ext == ".rm" || ext == ".rmvb" ||
         ext == ".bik" || ext == ".smk" || ext == ".wtv" ||
         ext == ".nsv" || ext == ".pmp" || ext == ".divx" ||
         ext == ".mjpg" || ext == ".mjpeg" || ext == ".mj2" ||
         ext == ".y4m" || ext == ".roq" || ext == ".mod" ||
         ext == ".tod";
}

bool isSupportedImageExt(const std::filesystem::path& path) {
  const std::string ext = lowerExtension(path);
  return ext == ".jpg" || ext == ".jpeg" || ext == ".jpe" ||
         ext == ".jfif" || ext == ".png" || ext == ".bmp" ||
         ext == ".gif" || ext == ".tif" || ext == ".tiff" ||
         ext == ".webp" || ext == ".heic" || ext == ".heif" ||
         ext == ".avif" || ext == ".ico";
}

bool isSupportedMediaExt(const std::filesystem::path& path) {
  return isSupportedAudioExt(path) || isSupportedVideoExt(path) ||
         isSupportedImageExt(path);
}

bool isMiniaudioExt(const std::filesystem::path& path) {
  return isMiniaudioExtValue(lowerExtension(path));
}

bool isFfmpegAudioExt(const std::filesystem::path& path) {
  return isFfmpegAudioExtValue(lowerExtension(path));
}

bool isFlacExt(const std::filesystem::path& path) {
  return lowerExtension(path) == ".flac";
}

bool isGmeExt(const std::filesystem::path& path) {
  return isGmeExtValue(lowerExtension(path));
}

bool isMidiExt(const std::filesystem::path& path) {
  return isMidiExtValue(lowerExtension(path));
}

bool isGsfExt(const std::filesystem::path& path) {
  return isGsfExtValue(lowerExtension(path));
}

bool isVgmExt(const std::filesystem::path& path) {
  return isVgmExtValue(lowerExtension(path));
}

bool isM4aExt(const std::filesystem::path& path) {
  return isM4aExtValue(lowerExtension(path));
}

bool isKssExt(const std::filesystem::path& path) {
  return isKssExtValue(lowerExtension(path));
}

bool isPsfExt(const std::filesystem::path& path) {
  return isPsfExtValue(lowerExtension(path));
}

bool isOggExt(const std::filesystem::path& path) {
  return lowerExtension(path) == ".ogg";
}

std::string supportedAudioExtensionsText() {
  std::string list =
      ".wav, .mp3, .flac, .m4a, .m4b, .m4r, .m4p, .webm, .mp4, .mov, "
      ".mkv, .ogg, .wma, .aac, .ac3, .eac3, .aif, .aiff, .aifc, "
      ".opus, .oga, .mka, .wv, .tta, .caf, .au, .mp2, .ape, .tak, "
      ".amr, .ra, .dts, .dsf, .qcp, .spx, .mpc, .xwma, .w64, .voc, "
      ".awb, .gsm, .oma, .aa, .aax, .mlp, .truehd, .ac4, .loas, "
      ".latm, .kss, .nsf, ";
#if !RADIOIFY_DISABLE_GSF_GPL
  list += ".gsf, .minigsf, ";
#endif
  list += ".mid, .midi, .vgm, .vgz, .psf, .minipsf, .psf2, .minipsf2";
  return list;
}
