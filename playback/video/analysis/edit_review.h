#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "playback/video/analysis/operation.h"
#include "playback/video/analysis/text_evidence.h"

namespace playback_video_analysis {

enum class EditDisposition : uint8_t { Keep, Shorten, Review };

struct EditProposal {
  uint64_t id = 0;
  int64_t startUs = 0;
  int64_t endUs = 0;
  EditDisposition disposition = EditDisposition::Review;
  std::string reason;
};

// Every core interval is assessed with neighbouring context. These are model
// inputs, not edit boundaries. A sampled frame is not a claim about every
// intervening source frame.
inline constexpr int64_t kReviewWindowUs = 30'000'000;
inline constexpr int64_t kReviewContextUs = 2'000'000;
inline constexpr int64_t kReviewFrameStepUs = 500'000;
inline constexpr int kReviewImageTokens = 256;
inline constexpr int kReviewContextTokens = 16384;
inline constexpr int kReviewObservationTokens = 2048;
inline constexpr int kReviewAggregationTokens = 4096;
inline constexpr int kReviewValuationTokens = 512;

struct ReviewWindow {
  int64_t startUs = 0;
  int64_t endUs = 0;
  int64_t coreStartUs = 0;
  int64_t coreEndUs = 0;
};

struct ReviewEvidence {
  int64_t startUs = 0;
  int64_t endUs = 0;
  std::string description;
  bool uncertain = false;
};

struct ReviewObservation {
  ReviewWindow window;
  std::vector<int64_t> frameTimesUs;
  std::vector<ReviewEvidence> activities;
  std::string rawResponse;
};

struct ReviewEvent {
  size_t firstEvidence = 0;
  size_t evidenceEnd =
      0; // Exclusive; owned by the input partition, not the model.
};

struct ReviewAggregation {
  size_t evidenceEnd = 0;
  std::string rawResponse;
  std::vector<ReviewEvent> events;
  // Memory of the final, still-open event for the next grouping page only.
  // Editing assessments always read the original observations instead.
  std::string continuation;
};

struct ReviewValuation {
  std::string rawResponse;
  EditDisposition disposition = EditDisposition::Review;
  std::string reason;
};

struct ReviewValuationInput {
  std::vector<ReviewEvidence> observations;
  std::optional<ReviewEvidence> before;
  std::optional<ReviewEvidence> after;
};

struct ReviewProgress {
  std::vector<ReviewObservation> observations;
  std::vector<ReviewAggregation> aggregations;
  std::vector<ReviewValuation> valuations;
};

// One new observation, preceded by the open event's context when present.
// Its evidence range remains attached to the original observations.
struct ReviewAggregationRow {
  size_t firstEvidence = 0;
  size_t evidenceEnd = 0;
  ReviewEvidence evidence;
  std::string latestObservation;
};

struct ReviewRequest {
  std::filesystem::path sourcePath;
  int videoStreamIndex = -1;
  int64_t durationUs = 0;
  std::filesystem::path model;
  std::filesystem::path projector;
  std::vector<TextCue> speech;
  // Exact source/model/prompt/runtime identity, also checked before
  // publication.
  std::string identity;
};

struct ReviewResult {
  playback_video_analysis::OperationStatus status =
      playback_video_analysis::OperationStatus::Failed;
  std::string detail;
  ReviewProgress progress;
  // Rejected model output for diagnostics; never a displayed editing reason.
  std::string rawOutput{};
};

const char *editDispositionName(EditDisposition disposition);
std::vector<ReviewWindow> reviewWindows(int64_t durationUs);
std::vector<int64_t> reviewFrameTimes(const ReviewWindow &window);
std::string reviewTimestamp(int64_t timeUs);
std::string reviewSpeechPrompt(const std::vector<TextCue> &speech,
                               int64_t startUs, int64_t endUs);
std::string escapeReviewEvidence(std::string_view text);
std::string reviewObservationPrompt(const ReviewWindow &window,
                                    const std::vector<TextCue> &speech = {});
std::vector<size_t> reviewBoundaryIds(const ReviewWindow &window,
                                      const std::vector<int64_t> &frameTimesUs);
std::string reviewObservationGrammar(const ReviewWindow &window,
                                     const std::vector<int64_t> &frameTimesUs);
bool parseReviewObservation(std::string_view response,
                            const ReviewWindow &window,
                            const std::vector<int64_t> &frameTimesUs,
                            std::vector<ReviewEvidence> *activities,
                            std::string *error);
std::vector<ReviewEvidence> reviewEvidence(const ReviewProgress &progress);
std::vector<ReviewEvent> reviewEvents(const ReviewProgress &progress);
std::vector<ReviewAggregationRow>
reviewAggregationRows(const std::vector<ReviewEvidence> &evidence,
                      const ReviewProgress &progress, size_t evidenceEnd);
std::string
reviewAggregationPrompt(const std::vector<ReviewAggregationRow> &rows,
                        bool endOfVideo,
                        const std::vector<TextCue> &speech = {});
std::string
reviewAggregationGrammar(const std::vector<ReviewAggregationRow> &rows);
bool parseReviewAggregation(std::string_view response,
                            const std::vector<ReviewAggregationRow> &rows,
                            ReviewAggregation *aggregation, std::string *error);
// Assessments keep the target event distinct from its neighboring context.
// Large events are paged without thinning; Keep > Review > Shorten reduces the
// page decisions.
std::optional<ReviewValuationInput>
reviewValuationInput(const ReviewEvent &event,
                     const std::vector<ReviewEvidence> &evidence);
std::string reviewValuationPrompt(const ReviewValuationInput &input,
                                  const std::vector<TextCue> &speech = {});
std::string reviewValuationGrammar();
bool parseReviewValuation(std::string_view response, ReviewValuation *valuation,
                          std::string *error);
ReviewValuation combineReviewValuations(const ReviewValuation &left,
                                        const ReviewValuation &right);
bool reviewGroupingComplete(int64_t durationUs, const ReviewProgress &progress);
bool reviewComplete(int64_t durationUs, const ReviewProgress &progress);
double reviewProgressFraction(int64_t durationUs,
                              const ReviewProgress &progress);
// Keep beats uncertainty; uncertainty beats Shorten. Keep intervals include
// handles so applying a neighbouring shortening proposal preserves context.
std::vector<EditProposal> assembleEditProposals(int64_t durationUs,
                                                const ReviewProgress &progress);

} // namespace playback_video_analysis
