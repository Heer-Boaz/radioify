#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_AUDIO_PIPELINE_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_AUDIO_PIPELINE_H

#include "../../radio.h"
#include "../graph/audio_pipeline.h"

using Radio1938AudioPipeline = AudioPipeline<Radio1938,
                                             RadioBlockControl,
                                             RadioSampleContext,
                                             RadioInitContext,
                                             PassId>;

const Radio1938AudioPipeline& radio1938AudioPipeline();

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_AUDIO_PIPELINE_H
