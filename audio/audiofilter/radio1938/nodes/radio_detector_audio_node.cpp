#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

uint32_t detectorAudioConfigRevision(const Radio1938& radio) {
  return radio.ifStrip.enabled ? radio.ifStrip.appliedConfigRevision
                               : radio.tuning.configRevision;
}

}  // namespace

void RadioDetectorAudioNode::init(Radio1938& radio, RadioInitContext&) {
  radio.detectorAudio.appliedConfigRevision = detectorAudioConfigRevision(radio);
}

void RadioDetectorAudioNode::reset(Radio1938& radio) {
  auto& detectorAudio = radio.detectorAudio;
  detectorAudio.audioNode = 0.0f;
  detectorAudio.audioEnv = 0.0f;
  detectorAudio.warmStartPending = true;
  detectorAudio.postLp.reset();
  detectorAudio.appliedConfigRevision = 0;
}

float RadioDetectorAudioNode::run(Radio1938& radio,
                                  float y,
                                  RadioSampleContext&) {
  auto& detectorAudio = radio.detectorAudio;
  detectorAudio.appliedConfigRevision = detectorAudioConfigRevision(radio);
  float detectorNode = 0.0f;
  if (!radio.demod.am.warmStartPending) {
    detectorNode = std::max(radio.demod.am.detectorNode, 0.0f);
  } else {
    // Standalone detector-audio tests may drive this node without the AM
    // detector pass in front of it. In the real pipeline the source is always
    // the physical detector storage node solved in AMDetector.
    detectorNode = std::max(y, 0.0f);
  }
  detectorAudio.audioNode = detectorNode;
  detectorAudio.audioEnv = detectorNode;
  detectorAudio.warmStartPending = false;
  return detectorAudio.audioEnv;
}
