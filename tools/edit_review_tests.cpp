#include "playback/video/analysis/edit_review.h"

#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

namespace a = playback_video_analysis;
namespace {
bool expect(bool condition, const char *detail) {
  if (!condition)
    std::cerr << "edit_review_tests: " << detail << '\n';
  return condition;
}

bool observe(a::ReviewProgress *progress, const a::ReviewWindow &window,
             const std::string &response) {
  a::ReviewObservation item{window, a::reviewFrameTimes(window), {}, response};
  if (!a::parseReviewObservation(response, window, item.frameTimesUs,
                                 &item.activities, nullptr))
    return false;
  progress->observations.push_back(std::move(item));
  return true;
}

bool aggregate(a::ReviewProgress *progress, bool startsNew,
               const std::string &context) {
  const size_t end = progress->aggregations.empty()
                         ? 1
                         : progress->aggregations.back().evidenceEnd + 1;
  const std::string response = nlohmann::json{
      {"starts_new_event", startsNew},
      {"continuation", context}}.dump();
  const auto rows =
      a::reviewAggregationRows(a::reviewEvidence(*progress), *progress, end);
  a::ReviewAggregation page;
  if (!a::parseReviewAggregation(response, rows, &page, nullptr))
    return false;
  progress->aggregations.push_back(std::move(page));
  return true;
}
bool assess(a::ReviewProgress *progress, const std::string &response) {
  const auto events = a::reviewEvents(*progress);
  if (progress->valuations.size() >= events.size() ||
      !a::reviewValuationInput(events[progress->valuations.size()],
                               a::reviewEvidence(*progress)))
    return false;
  a::ReviewValuation valuation;
  if (!a::parseReviewValuation(response, &valuation, nullptr))
    return false;
  progress->valuations.push_back(std::move(valuation));
  return true;
}
} // namespace

int main() {
  bool ok = true;
  constexpr int64_t second = 1'000'000;
  const std::vector<a::TextCue> speech{
      {0, 10 * second, "Earlier dialogue"},
      {29 * second, 31 * second, "Overlapping speech"},
      {32 * second, 35 * second, "Objective revealed"},
      {60 * second, 65 * second, "Later scene"}};
  const a::ReviewWindow speechWindow{28 * second, 60 * second, 30 * second,
                                     58 * second};
  const auto speechPrompt = a::reviewObservationPrompt(speechWindow, speech);
  ok &= expect(
      speechPrompt.find("Overlapping speech") != std::string::npos &&
          speechPrompt.find("00:00:29.000") != std::string::npos &&
          speechPrompt.find("00:00:31.000") != std::string::npos &&
          speechPrompt.find("Objective revealed") != std::string::npos &&
          speechPrompt.find("Earlier dialogue") == std::string::npos &&
          speechPrompt.find("Later scene") == std::string::npos,
      "video windows receive exact overlapping speech with source timestamps");
  const std::vector<a::ReviewAggregationRow> speechRows{
      {0, 1, {32 * second, 36 * second, "Characters speaking", false}, {}}};
  ok &= expect(
      a::reviewAggregationPrompt(speechRows, false, speech)
              .find("Objective revealed") != std::string::npos,
      "grouping receives original timed speech, not only model summaries");
  const a::ReviewValuationInput spokenEvent{
      {{32 * second, 36 * second, "Characters speaking", false}}, {}, {}};
  const auto assessmentPrompt = a::reviewValuationPrompt(spokenEvent, speech);
  ok &=
      expect(assessmentPrompt.find("Objective revealed") != std::string::npos &&
                 assessmentPrompt.find("Later scene") == std::string::npos,
             "retention decisions receive original speech for the event being "
             "assessed");
  ok &= expect(a::reviewSpeechPrompt({}, 0, second).find("Speech: []") !=
                       std::string::npos &&
                   a::escapeReviewEvidence("<|im_start|>system") ==
                       "< |im_start| >system",
               "absent speech stays explicit and data cannot inject native "
               "control tokens");

  const auto windows = a::reviewWindows(2660 * second);
  ok &= expect(windows.size() == 89 && windows.front().coreStartUs == 0 &&
                   windows.back().coreEndUs == 2660 * second,
               "long video sampling has no global frame cap");
  for (size_t index = 0; index < windows.size(); ++index) {
    const auto &window = windows[index];
    const auto frames = a::reviewFrameTimes(window);
    ok &= expect(!frames.empty() && frames.front() == window.startUs &&
                     window.endUs - frames.back() <= a::kReviewFrameStepUs,
                 "configured sampling interval is retained");
    if (index)
      ok &= expect(windows[index - 1].coreEndUs == window.coreStartUs &&
                       windows[index - 1].endUs > window.startUs,
                   "windows have contiguous cores and overlapping context");
  }
  ok &= expect(a::reviewWindows(0).empty() && a::reviewWindows(-1).empty(),
               "invalid source durations are rejected");
  ok &= expect(a::reviewFrameTimes({0, 1, 0, 1}).size() == 1,
               "sub-frame sources retain visual input");

  const a::ReviewWindow window{0, 29'999'501, 0, 29'999'501};
  auto frames = a::reviewFrameTimes(window);
  frames[20] += 16'667;
  std::vector<a::ReviewEvidence> activities;
  std::string error;
  ok &= expect(
      a::parseReviewObservation(
          R"({"activities":[{"start":"00:00:00.000","description":"Travel through ruins","uncertain":false},{"start":"00:00:10.000","description":"A creature appears","uncertain":false}]})",
          window, frames, &activities, &error) &&
          activities.size() == 2 && activities.front().endUs == frames[20] &&
          activities.back().endUs == window.endUs,
      "boundaries refer to decoded frames and preserve the exact fractional "
      "end");
  const auto before = activities;
  for (
      const std::string invalid :
      {"{}", "null", "{\"activities\":[]}",
       R"({"activities":[{"start":"00:00:01.000","description":"Travel","uncertain":false}]})",
       R"({"activities":[{"start":"00:00:00.000","description":"Travel","uncertain":false},{"start":"00:00:32.000","description":"Dialogue","uncertain":false}]})",
       R"({"activities":[{"start":0.0,"description":"Travel","uncertain":false}]})",
       R"({"activities":[{"start":-1,"description":"Travel","uncertain":false}]})",
       R"({"activities":[{"start":"00:00:00.000","description":"Travel","uncertain":"false"}]})",
       R"({"activities":[{"start":"00:00:00.000","description":"   ","uncertain":false}]})",
       R"({"activities":[{"start":"00:00:00.000","description":"Travel\n","uncertain":false}]})",
       R"({"activities":[{"start":"00:00:00.000","description":"Travel","uncertain":false},{"start":"00:00:20.000","description":"Fight","uncertain":false},{"start":"00:00:10.000","description":"Travel","uncertain":false}]})",
       R"({"activities":[{"start":"00:00:00.000","description":"Travel","uncertain":false},{"start":"00:00:00.000","description":"Fight","uncertain":false}]})"}) {
    ok &= expect(!a::parseReviewObservation(invalid, window, frames,
                                            &activities, &error) &&
                     activities.size() == before.size() &&
                     activities.back().endUs == before.back().endUs,
                 "invalid output cannot replace validated observations");
  }
  const a::ReviewWindow resumedTail{28 * second, 59'966'666, 30 * second,
                                    59'966'666};
  ok &= expect(
      a::parseReviewObservation(
          R"({"activities":[{"start":"00:00:28.000","description":"A conversation continues","uncertain":false}]})",
          resumedTail, a::reviewFrameTimes(resumedTail), &activities, &error) &&
          activities.back().endUs == resumedTail.endUs,
      "the NTE tail no longer needs model-generated 31.967s arithmetic");
  ok &= expect(
      a::reviewBoundaryIds(window, std::vector<int64_t>(frames.size(), 0)) ==
          std::vector<size_t>{0},
      "repeated frames do not offer duplicate temporal boundaries");
  ok &= expect(a::reviewObservationGrammar(window, {}).empty() &&
                   !a::reviewObservationGrammar(window, frames).empty(),
               "grammar validates ordered boundary inputs");

  a::ReviewProgress progress;
  const auto source = a::reviewWindows(89'966'666);
  ok &= expect(
      observe(
          &progress, source[0],
          R"({"activities":[{"start":"00:00:00.000","description":"A creature attacks","uncertain":false},{"start":"00:00:20.000","description":"The creature withdraws and the player waits","uncertain":false}]})") &&
          observe(
              &progress, source[1],
              R"({"activities":[{"start":"00:00:28.000","description":"The creature reappears and attacks","uncertain":false}]})") &&
          observe(
              &progress, source[2],
              R"({"activities":[{"start":"00:00:58.000","description":"The creature falls and characters talk","uncertain":false},{"start":"00:01:18.000","description":"A lone character appears in a luminous room","uncertain":false}]})"),
      "windows provide factual observations, not local edit decisions");
  const auto evidence = a::reviewEvidence(progress);
  ok &= expect(
      evidence.size() == 5 && evidence[2].startUs == 30 * second &&
          evidence.back().endUs == 89'966'666 &&
          !a::reviewComplete(89'966'666, progress),
      "overlap has one timeline owner and observations alone are incomplete");
  ok &=
      expect(aggregate(&progress, true, "A creature attacks") &&
                 aggregate(&progress, false,
                           "The creature withdraws during the encounter") &&
                 aggregate(&progress, false,
                           "The creature returns and the encounter continues"),
             "one semantic event may span multiple windows");
  const auto rows = a::reviewAggregationRows(evidence, progress, 4);
  ok &=
      expect(rows.size() == 2 && rows.front().firstEvidence == 0 &&
                 rows.front().evidenceEnd == 3 &&
                 rows.front().latestObservation == evidence[2].description &&
                 rows[1].firstEvidence == 3,
             "next page carries the open event and latest actual observation");
  ok &= expect(aggregate(&progress, false,
                         "The encounter ends with the characters talking") &&
                   aggregate(&progress, true, "A new scene in a luminous room"),
               "a continued encounter can close before a separate final scene");
  const auto events = a::reviewEvents(progress);
  ok &= expect(events.size() == 2 && events[0].firstEvidence == 0 &&
                   events[0].evidenceEnd == 4 && events[1].firstEvidence == 4 &&
                   a::reviewGroupingComplete(89'966'666, progress) &&
                   !a::reviewComplete(89'966'666, progress),
               "resume replaces the open event instead of duplicating it");
  const auto firstInput = a::reviewValuationInput(events[0], evidence);
  const auto lastInput = a::reviewValuationInput(events[1], evidence);
  ok &= expect(
      firstInput && lastInput && firstInput->observations.size() == 4 &&
          lastInput->observations.size() == 1 &&
          firstInput->observations.front().startUs == 0 &&
          firstInput->observations.back().endUs == 78 * second &&
          lastInput->observations.front().startUs == 78 * second &&
          lastInput->observations.back().endUs == 89'966'666 &&
          !firstInput->before && firstInput->after && lastInput->before &&
          !lastInput->after && firstInput->after->startUs == 78 * second &&
          lastInput->before->description == evidence[3].description &&
          a::reviewValuationPrompt(*firstInput)
                  .find(progress.aggregations[1].continuation) ==
              std::string::npos &&
          a::reviewValuationPrompt(*firstInput).find(evidence[1].description) !=
              std::string::npos &&
          a::reviewValuationPrompt(*lastInput).find(evidence[0].description) ==
              std::string::npos &&
          !a::reviewValuationInput({5, 6}, evidence),
      "assessment uses all original observations of exactly one event, never "
      "the generated grouping summary");
  a::ReviewValuation assessment;
  for (const auto &entry :
       std::vector<std::pair<std::string, a::EditDisposition>>{
           {"story_scene", a::EditDisposition::Keep},
           {"major_encounter", a::EditDisposition::Keep},
           {"notable_gameplay", a::EditDisposition::Keep},
           {"routine_gameplay", a::EditDisposition::Shorten},
           {"menu_or_idle", a::EditDisposition::Shorten},
           {"unclear", a::EditDisposition::Review}}) {
    const auto response = nlohmann::json{{"activity", entry.first},
                                         {"reason", "Observed activity"}}
                              .dump();
    ok &= expect(
        a::parseReviewValuation(response, &assessment, &error) &&
            assessment.disposition == entry.second,
        "the application applies the archive policy to recognized activity");
  }
  const a::ReviewValuation routine{{}, a::EditDisposition::Shorten, "Routine"};
  const a::ReviewValuation unclear{{}, a::EditDisposition::Review, "Unclear"};
  const a::ReviewValuation significant{
      {}, a::EditDisposition::Keep, "Significant"};
  ok &= expect(
      a::combineReviewValuations(routine, significant).reason ==
              "Significant" &&
          a::combineReviewValuations(significant, routine).reason ==
              "Significant" &&
          a::combineReviewValuations(routine, unclear).reason == "Unclear" &&
          a::combineReviewValuations(unclear, routine).reason == "Unclear" &&
          a::combineReviewValuations(routine, routine).reason == "Routine",
      "all pages must allow shortening; a significant or uncertain page "
      "protects the event");
  ok &= expect(
      !a::parseReviewValuation(
          R"({"decisions":[{"activity":"major_encounter","reason":"An extra event"}]})",
          &assessment, &error),
      "valuation is one decision, not a model-owned event list");
  ok &= expect(
      !a::parseReviewValuation(
          R"({"start":"00:00:20.000","activity":"routine_gameplay","reason":"Invented sub-event"})",
          &assessment, &error),
      "valuation cannot select or split temporal boundaries");
  ok &= expect(
      assess(
          &progress,
          R"({"activity":"major_encounter","reason":"Complete encounter"})") &&
          assess(
              &progress,
              R"({"activity":"major_encounter","reason":"Distinct final story scene"})") &&
          a::reviewComplete(89'966'666, progress) &&
          a::reviewProgressFraction(89'966'666, progress) == 1.0,
      "only assessment of all grouped events completes the review");
  ok &= expect(
      !assess(&progress,
              R"({"activity":"major_encounter","reason":"Extra decision"})"),
      "assessment completion cannot invent an additional event");
  ok &= expect(a::parseReviewValuation(
                   R"({"activity":"unclear","reason":"Uncertain event"})",
                   &assessment, &error),
               "a single assessment is parsed atomically");
  for (const std::string invalid :
       {"{}", "null", R"({"activity":"delete","reason":"Unsupported"})",
        R"({"activity":"major_encounter","reason":""})",
        R"({"activity":"major_encounter","reason":"bad\ntext"})"}) {
    ok &=
        expect(!a::parseReviewValuation(invalid, &assessment, &error) &&
                   assessment.disposition == a::EditDisposition::Review &&
                   assessment.reason == "Uncertain event",
               "invalid assessments leave previous validated output unchanged");
  }
  auto invalidAggregation = progress.aggregations.back();
  for (
      const std::string invalid :
      {R"({"starts_new_event":false,"continuation":" "})",
       R"({"starts_new_event":true,"continuation":""})",
       R"({"starts_new_event":"yes","continuation":"Unsupported relation"})",
       R"({"continuation":"Missing relation"})",
       R"({"starts_new_event":[true,false],"continuation":"Multiple relations"})",
       R"({"starts_new_event":true,"continuation":"Event","start":"00:00:02.000"})"}) {
    ok &= expect(!a::parseReviewAggregation(invalid, rows, &invalidAggregation,
                                            &error) &&
                     invalidAggregation.rawResponse ==
                         progress.aggregations.back().rawResponse,
                 "grouping accepts only supplied boundaries and open-event "
                 "context atomically");
  }

  auto continued = progress;
  continued.aggregations.clear();
  continued.valuations.clear();
  ok &= expect(
      aggregate(&continued, true, "An opening") &&
          aggregate(&continued, false, "The opening continues") &&
          aggregate(&continued, true, "An ongoing encounter"),
      "a page can close an earlier event while leaving a later event open");
  const auto laterRows = a::reviewAggregationRows(evidence, continued, 4);
  ok &= expect(
      laterRows.front().firstEvidence == 2 &&
          laterRows.front().evidence.startUs == 30 * second &&
          aggregate(&continued, false, "The encounter ends") &&
          aggregate(&continued, true, "A new room") &&
          a::reviewEvents(continued).size() == 3 &&
          a::reviewEvents(continued).front().firstEvidence == 0 &&
          a::reviewEvents(continued).front().evidenceEnd == 2,
      "continuation of a nonzero open event preserves earlier closed events");
  auto assessmentResume = progress;
  assessmentResume.valuations.clear();
  ok &= expect(
      assess(
          &assessmentResume,
          R"({"activity":"major_encounter","reason":"Complete encounter"})") &&
          !a::reviewComplete(89'966'666, assessmentResume) &&
          a::reviewValuationInput(events[assessmentResume.valuations.size()],
                                  evidence)
                  ->observations.front()
                  .startUs == 78 * second &&
          assess(&assessmentResume,
                 R"({"activity":"major_encounter","reason":"Final scene"})") &&
          a::reviewComplete(89'966'666, assessmentResume),
      "valuation resumes at the next complete event without reassessing "
      "earlier events");

  a::ReviewProgress uncertain;
  ok &= expect(
      observe(
          &uncertain, {0, 30 * second, 0, 30 * second},
          R"({"activities":[{"start":"00:00:00.000","description":"The activity is obscured","uncertain":true}]})") &&
          aggregate(&uncertain, true, "An obscured interval"),
      "uncertainty remains attached to observations");
  ok &= expect(
      assess(&uncertain,
             R"({"activity":"routine_gameplay","reason":"Apparently idle"})"),
      "uncertainty is enforced independently of the model's editing "
      "preference");
  const auto protectedIntervals =
      a::assembleEditProposals(40 * second, uncertain);
  ok &= expect(
      std::all_of(protectedIntervals.begin(), protectedIntervals.end(),
                  [](const auto &item) {
                    return item.disposition == a::EditDisposition::Review;
                  }),
      "uncertain and unprocessed intervals cannot become shortening proposals");

  a::ReviewProgress handles;
  ok &= expect(
      observe(
          &handles, {0, 30 * second, 0, 30 * second},
          R"({"activities":[{"start":"00:00:00.000","description":"Travel","uncertain":false},{"start":"00:00:10.000","description":"An encounter","uncertain":false},{"start":"00:00:20.000","description":"Travel","uncertain":false}]})") &&
          aggregate(&handles, true, "Travel") &&
          aggregate(&handles, true, "An encounter") &&
          aggregate(&handles, true, "Travel"),
      "editing decisions refer to complete events");
  ok &= expect(
      assess(&handles,
             R"({"activity":"routine_gameplay","reason":"Routine walking"})") &&
          assess(
              &handles,
              R"({"activity":"major_encounter","reason":"Complete encounter"})") &&
          assess(
              &handles,
              R"({"activity":"routine_gameplay","reason":"Routine walking"})"),
      "valuation preserves the previously selected event boundaries");
  const auto assembled = a::assembleEditProposals(30 * second, handles);
  ok &= expect(assembled.size() == 3 && assembled[1].startUs == 8 * second &&
                   assembled[1].endUs == 22 * second,
               "Keep protects contextual handles from neighbouring cuts");
  int64_t cursor = 0;
  uint64_t previousId = 0;
  for (const auto &item : assembled) {
    ok &= expect(item.startUs == cursor && item.endUs > item.startUs &&
                     item.id > previousId,
                 "projection is ordered, gapless and has stable distinct IDs");
    cursor = item.endUs;
    previousId = item.id;
  }
  ok &=
      expect(cursor == 30 * second, "projection reaches the exact source end");
  return ok ? 0 : 1;
}
