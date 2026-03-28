#include "audio_pipeline.h"

const Radio1938AudioPipeline& radio1938AudioPipeline() {
  static const auto pipeline = [] {
    AudioPipelineBuilder<Radio1938,
                         RadioBlockControl,
                         RadioSampleContext,
                         RadioInitContext,
                         PassId>
        builder;
    builder.block(PassId::Tuning, "MagneticTuning")
        .init(&RadioTuningNode::init)
        .reset(&RadioTuningNode::reset)
        .prepare(&RadioTuningNode::prepare);

    builder.program(PassId::Input, "Input")
        .init(&RadioInputNode::init)
        .reset(&RadioInputNode::reset)
        .run(&RadioInputNode::run);
    builder.control(PassId::AVC, "AVC")
        .init(&RadioAVCNode::init)
        .reset(&RadioAVCNode::reset)
        .run(&RadioAVCNode::run);
    builder.control(PassId::AFC, "AFC")
        .init(&RadioAFCNode::init)
        .reset(&RadioAFCNode::reset)
        .dependsOn(PassId::AVC)
        .run(&RadioAFCNode::run);
    builder.control(PassId::ControlBus, "ControlBus")
        .init(&RadioControlBusNode::init)
        .reset(&RadioControlBusNode::reset)
        .dependsOn(PassId::AFC)
        .run(&RadioControlBusNode::run);
    builder.control(PassId::InterferenceDerived, "InterferenceDerived")
        .init(&RadioInterferenceDerivedNode::init)
        .reset(&RadioInterferenceDerivedNode::reset)
        .dependsOn(PassId::ControlBus)
        .run(&RadioInterferenceDerivedNode::run);
    builder.program(PassId::FrontEnd, "RFFrontEnd")
        .init(&RadioFrontEndNode::init)
        .reset(&RadioFrontEndNode::reset)
        .dependsOn(PassId::Input)
        .run(&RadioFrontEndNode::run);
    builder.program(PassId::Mixer, "Mixer")
        .init(&RadioMixerNode::init)
        .reset(&RadioMixerNode::reset)
        .dependsOn(PassId::FrontEnd)
        .run(&RadioMixerNode::run);
    builder.program(PassId::IFStrip, "IFStrip")
        .init(&RadioIFStripNode::init)
        .reset(&RadioIFStripNode::reset)
        .dependsOn(PassId::Mixer)
        .run(&RadioIFStripNode::run);
    builder.program(PassId::Demod, "AMDetector")
        .init(&RadioAMDetectorNode::init)
        .reset(&RadioAMDetectorNode::reset)
        .dependsOn(PassId::IFStrip)
        .run(&RadioAMDetectorNode::run);
    builder.program(PassId::DetectorAudio, "DetectorAudio")
        .init(&RadioDetectorAudioNode::init)
        .reset(&RadioDetectorAudioNode::reset)
        .dependsOn(PassId::Demod)
        .run(&RadioDetectorAudioNode::run);
    builder.program(PassId::ReceiverInputNetwork, "ReceiverInputNetwork")
        .init(&RadioReceiverInputNetworkNode::init)
        .reset(&RadioReceiverInputNetworkNode::reset)
        .dependsOn(PassId::DetectorAudio)
        .stateOnly()
        .run(&RadioReceiverInputNetworkNode::run);
    builder.program(PassId::ReceiverCircuit, "AudioStage")
        .init(&RadioReceiverCircuitNode::init)
        .reset(&RadioReceiverCircuitNode::reset)
        .dependsOn(PassId::ReceiverInputNetwork)
        .run(&RadioReceiverCircuitNode::run);
    builder.program(PassId::Tone, "Tone")
        .init(&RadioToneNode::init)
        .reset(&RadioToneNode::reset)
        .dependsOn(PassId::ReceiverCircuit)
        .run(&RadioToneNode::run);
    builder.program(PassId::Power, "Power")
        .init(&RadioPowerNode::init)
        .reset(&RadioPowerNode::reset)
        .dependsOn(PassId::Tone)
        .run(&RadioPowerNode::run);
    builder.program(PassId::Noise, "Noise")
        .init(&RadioNoiseNode::init)
        .reset(&RadioNoiseNode::reset)
        .dependsOn(PassId::Power)
        .run(&RadioNoiseNode::run);
    builder.program(PassId::Speaker, "Speaker")
        .init(&RadioSpeakerNode::init)
        .reset(&RadioSpeakerNode::reset)
        .dependsOn(PassId::Noise)
        .run(&RadioSpeakerNode::run);
    builder.program(PassId::Cabinet, "Cabinet")
        .init(&RadioCabinetNode::init)
        .reset(&RadioCabinetNode::reset)
        .dependsOn(PassId::Speaker)
        .run(&RadioCabinetNode::run);
    builder.program(PassId::OutputScale, "OutputScale")
        .init(&RadioOutputScaleNode::init)
        .reset(&RadioOutputScaleNode::reset)
        .dependsOn(PassId::Cabinet)
        .run(&RadioOutputScaleNode::run);
    builder.program(PassId::FinalLimiter, "FinalLimiter")
        .init(&RadioFinalLimiterNode::init)
        .reset(&RadioFinalLimiterNode::reset)
        .dependsOn(PassId::OutputScale)
        .run(&RadioFinalLimiterNode::run);
    builder.program(PassId::OutputClip, "OutputClip")
        .init(&RadioOutputClipNode::init)
        .reset(&RadioOutputClipNode::reset)
        .dependsOn(PassId::FinalLimiter)
        .run(&RadioOutputClipNode::run);

    return builder.build(static_cast<size_t>(PassId::OutputClip) + 1u);
  }();
  return pipeline;
}

RadioGraph makeRadioGraph() {
  return radio1938AudioPipeline().buildGraph();
}

RadioLifecycle makeRadioLifecycle() {
  return radio1938AudioPipeline().buildLifecycle();
}
