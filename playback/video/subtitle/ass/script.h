#pragma once

#include <cstdint>
#include <string>

struct AssPacketEventFields {
  int layer = 0;
  std::string style = "Default";
  std::string name;
  int marginL = 0;
  int marginR = 0;
  int marginV = 0;
  std::string effect;
  std::string text;
};

void ensureEmbeddedAssScriptPreamble(std::string* script);
void appendEmbeddedAssDialogue(std::string* script, int64_t startUs,
                               int64_t endUs,
                               const AssPacketEventFields& event);
