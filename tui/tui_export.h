#ifndef TUI_EXPORT_H
#define TUI_EXPORT_H

#include <filesystem>

#include "app_common.h"
#include "audioplayback.h"
#include "radio.h"

std::filesystem::path defaultRadioOutputFor(
    const std::filesystem::path& input);

void renderToFile(const Options& o,
                  const Radio1938& radio1938Template,
                  bool useRadio1938,
                  Radio1938* renderedRadioOut = nullptr,
                  bool writeOutput = true);

int runRenderRadioCli(const Options& o);
int runExtractSheetCli(const Options& o,
                       const AudioPlaybackConfig& audioConfig);

#endif  // TUI_EXPORT_H
