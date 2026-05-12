#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_PREVIEW_RADIO_PREVIEW_CONFIG_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_PREVIEW_RADIO_PREVIEW_CONFIG_H

struct RadioPreviewConfig {
  float programBandwidthHz = 2400.0f;
  // Preview runs on playback/export hot paths; calibration uses Radio1938
  // directly when it needs the full detector island.
  int detectorMaxSubsteps = 1;
};

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_PREVIEW_RADIO_PREVIEW_CONFIG_H
