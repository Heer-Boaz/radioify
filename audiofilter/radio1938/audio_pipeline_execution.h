#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_AUDIO_PIPELINE_EXECUTION_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_AUDIO_PIPELINE_EXECUTION_H

#include "../../radio.h"

void beginRadioPipelineBlock(Radio1938& radio,
                             uint32_t frames,
                             RadioBlockControl& block);

float runRadioPipelineSample(Radio1938& radio,
                             float x,
                             RadioSampleContext& ctx,
                             PassId firstProgramPass);

void finishRadioPipelineBlock(Radio1938& radio);

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_AUDIO_PIPELINE_EXECUTION_H
