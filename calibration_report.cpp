#include "calibration_report.h"

#include <iomanip>
#include <iostream>

#include "radio.h"

void printCalibrationReport(const Radio1938& radio1938,
                           const std::string& label) {
  if (!radio1938.calibration.enabled) return;

  auto oldFlags = std::cout.flags();
  auto oldPrecision = std::cout.precision();
  std::cout << std::fixed << std::setprecision(3);
  std::cout << label << "\n";
  for (size_t i = 0; i < radio1938.calibration.passes.size(); ++i) {
    PassId id = static_cast<PassId>(i);
    const auto& pass = radio1938.calibration.passes[i];
    if (pass.sampleCount == 0) continue;
    std::cout << "  " << Radio1938::passName(id)
              << " rms_in=" << pass.rmsIn
              << " rms_out=" << pass.rmsOut
              << " mean_out=" << pass.meanOut
              << " peak_out=" << pass.peakOut
              << " crest_out=" << pass.crestOut
              << " centroid_hz=" << pass.spectralCentroidHz
              << " bw3_hz=" << pass.bandwidth3dBHz
              << " bw6_hz=" << pass.bandwidth6dBHz
              << " clips_in=" << pass.clipCountIn
              << " clips_out=" << pass.clipCountOut
              << "\n";
  }
  std::cout << "  limiter duty=" << radio1938.calibration.limiterDutyCycle
            << " avg_gr=" << radio1938.calibration.limiterAverageGainReduction
            << " max_gr=" << radio1938.calibration.limiterMaxGainReduction
            << " avg_gr_db="
            << radio1938.calibration.limiterAverageGainReductionDb
            << " max_gr_db="
            << radio1938.calibration.limiterMaxGainReductionDb
            << "\n";
  const auto& receiverPass =
      radio1938.calibration.passes[static_cast<size_t>(PassId::ReceiverCircuit)];
  std::cout << "  receiver_out_rms=" << receiverPass.rmsOut
            << " receiver_out_peak=" << receiverPass.peakOut
            << " interstage_secondary_rms="
            << radio1938.calibration.interstageSecondaryRmsVolts
            << " interstage_secondary_peak="
            << radio1938.calibration.interstageSecondaryPeakVolts
            << "\n";
  std::cout << "  driver_grid_positive="
            << radio1938.calibration.driverGridPositiveFraction
            << " output_grid_a_positive="
            << radio1938.calibration.outputGridAPositiveFraction
            << " output_grid_b_positive="
            << radio1938.calibration.outputGridBPositiveFraction
            << " output_grid_positive="
            << radio1938.calibration.outputGridPositiveFraction
            << " max_mixer_ip=" << radio1938.calibration.maxMixerPlateCurrentAmps
            << " max_receiver_ip="
            << radio1938.calibration.maxReceiverPlateCurrentAmps
            << " max_driver_ip="
            << radio1938.calibration.maxDriverPlateCurrentAmps
            << " max_output_ip_a="
            << radio1938.calibration.maxOutputPlateCurrentAAmps
            << " max_output_ip_b="
            << radio1938.calibration.maxOutputPlateCurrentBAmps
            << " max_speaker_v="
            << radio1938.calibration.maxSpeakerSecondaryVolts
            << " max_digital_output="
            << radio1938.calibration.maxDigitalOutput
            << "\n";
  std::cout << "  detector_if_crackle_events="
            << radio1938.calibration.detectorIfCrackleEventCount
            << " detector_if_crackle_max_burst="
            << radio1938.calibration.detectorIfCrackleMaxBurstAmp
            << " detector_if_crackle_max_env="
            << radio1938.calibration.detectorIfCrackleMaxEnv
            << "\n";
  std::cout << "  validation failed="
            << (radio1938.calibration.validationFailed ? 1 : 0)
            << " driver_grid_over="
            << (radio1938.calibration.validationDriverGridPositive ? 1 : 0)
            << " output_grid_a_over="
            << (radio1938.calibration.validationOutputGridAPositive ? 1 : 0)
            << " output_grid_b_over="
            << (radio1938.calibration.validationOutputGridBPositive ? 1 : 0)
            << " output_grid_over="
            << (radio1938.calibration.validationOutputGridPositive ? 1 : 0)
            << " interstage_over="
            << (radio1938.calibration.validationInterstageSecondary ? 1 : 0)
            << " speaker_over="
            << (radio1938.calibration.validationSpeakerOverReference ? 1 : 0)
            << " dc_shift="
            << (radio1938.calibration.validationDcShift ? 1 : 0)
            << " digital_clip="
            << (radio1938.calibration.validationDigitalClip ? 1 : 0)
            << "\n";
  std::cout.flags(oldFlags);
  std::cout.precision(oldPrecision);
}

static uint64_t sumPassClipsIn(const Radio1938& radio1938) {
  uint64_t total = 0;
  for (size_t i = 0; i < radio1938.calibration.passes.size(); ++i) {
    total += radio1938.calibration.passes[i].clipCountIn;
  }
  return total;
}

static uint64_t sumPassClipsOut(const Radio1938& radio1938) {
  uint64_t total = 0;
  for (size_t i = 0; i < radio1938.calibration.passes.size(); ++i) {
    total += radio1938.calibration.passes[i].clipCountOut;
  }
  return total;
}

void printNodeStepSummaryHeader() {
  std::cout
      << "node_step,disabled_node,max_digital,delta_max_digital,pre_lim_clip,"
         "delta_pre_lim_clip,post_lim_clip,delta_post_lim_clip,limiter_duty,"
         "delta_limiter_duty,max_speaker_v,delta_max_speaker_v,max_pass_clips_in,"
         "delta_pass_clips_in,max_pass_clips_out,delta_pass_clips_out,"
         "pre_limiter_active,validation_digital_clip,validation_failed\n";
}

void printNodeStepSummaryLine(const std::string& disabledNode,
                             const Radio1938& result,
                             const Radio1938* baseline) {
  const Radio1938* base = baseline ? baseline : &result;
  const auto& currentCal = result.calibration;
  const auto& baseCal = base->calibration;
  const auto baseMaxDigital = static_cast<float>(baseCal.maxDigitalOutput);
  const auto baseMaxSpeaker = static_cast<float>(baseCal.maxSpeakerSecondaryVolts);
  const auto basePreClip = baseCal.preLimiterClipCount;
  const auto basePostClip = baseCal.postLimiterClipCount;
  const auto baseLimiterDuty = baseCal.limiterDutyCycle;
  const auto baseInClips = sumPassClipsIn(*base);
  const auto baseOutClips = sumPassClipsOut(*base);

  float deltaMaxDigital = currentCal.maxDigitalOutput - baseMaxDigital;
  float deltaMaxSpeaker = currentCal.maxSpeakerSecondaryVolts - baseMaxSpeaker;
  auto deltaPreClip =
      static_cast<long long>(currentCal.preLimiterClipCount - basePreClip);
  auto deltaPostClip =
      static_cast<long long>(currentCal.postLimiterClipCount - basePostClip);
  float deltaLimiterDuty = currentCal.limiterDutyCycle - baseLimiterDuty;
  auto currentInClips = static_cast<long long>(sumPassClipsIn(result));
  auto currentOutClips = static_cast<long long>(sumPassClipsOut(result));
  auto baseInClipsSigned = static_cast<long long>(baseInClips);
  auto baseOutClipsSigned = static_cast<long long>(baseOutClips);

  std::cout << std::fixed << std::setprecision(6);
  std::cout << "step"
            << ",\"" << disabledNode << "\""
            << "," << currentCal.maxDigitalOutput << "," << deltaMaxDigital
            << "," << currentCal.preLimiterClipCount << "," << deltaPreClip
            << "," << currentCal.postLimiterClipCount << "," << deltaPostClip
            << "," << currentCal.limiterDutyCycle << "," << deltaLimiterDuty
            << "," << currentCal.maxSpeakerSecondaryVolts << ","
            << deltaMaxSpeaker << "," << currentInClips << ","
            << (currentInClips - baseInClipsSigned) << "," << currentOutClips
            << "," << (currentOutClips - baseOutClipsSigned) << ","
            << (currentCal.totalSamples > 0
                    ? (currentCal.limiterActiveSamples * 100.0 /
                       static_cast<double>(currentCal.totalSamples))
                    : 0.0)
            << "," << (currentCal.validationDigitalClip ? 1 : 0) << ","
            << (currentCal.validationFailed ? 1 : 0) << "\n";
}
