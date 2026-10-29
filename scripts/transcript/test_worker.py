"""Regression tests for repetitions, window ownership and overlapping speech."""
import unittest
from pathlib import Path
import sys

# Embedded Python deliberately excludes the invoking script directory.
# Always test the adjacent source, never an older worker in the runtime cache.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from worker import build_cues, language_code, recognize_window, same_occurrence


def phrase(text, start=1., end=2., source=0, score=1.):
    return dict(text=text, start=start, end=end, source=source, score=score)


class TranscriptTests(unittest.TestCase):
    def test_distinct_overlapping_speakers_survive(self):
        cues = build_cues([phrase("Hello!"), phrase("How are you?", source=1)], 10)
        self.assertEqual(len(cues), 2)
        self.assertLess(cues[1]["start_us"], cues[0]["end_us"])

    def test_repeated_phrases_in_one_result_survive(self):
        cues = build_cues([phrase("Again!"), phrase("Again!", 1.5, 2.5)], 10)
        self.assertEqual(len(cues), 2)

    def test_overlapping_window_duplicate_uses_better_alignment(self):
        cues = build_cues([phrase("Hello!", 1., 4., 0, .2), phrase("Hello!", 1., 2., 1, .8)], 10)
        self.assertEqual(len(cues), 1)
        self.assertEqual(cues[0]["end_us"], 2180000)

    def test_same_text_at_different_times_survives(self):
        self.assertFalse(same_occurrence(phrase("Hello!"), phrase("Hello!", 8, 9, 1)))

    def test_window_variants_of_one_phrase_are_reconciled(self):
        self.assertTrue(same_occurrence(phrase("The meeting starts at noon."),
                                        phrase("The meeting starts", source=1)))

    def test_different_overlapping_phrases_are_not_merged(self):
        self.assertFalse(same_occurrence(phrase("I've brought coffee."),
                                         phrase("The train is late.", source=1)))

    def test_adjacent_window_variants_select_stronger_alignment(self):
        cues = build_cues([
            phrase("Welcome to the meeting.", source=0, score=.1),
            phrase("Welcome to the morning meeting.", source=1, score=.8),
        ], 10)
        self.assertEqual(len(cues), 1)
        self.assertEqual(cues[0]["text"], "Welcome to the morning meeting.")

    def test_one_shared_onset_word_does_not_merge_distinct_speech(self):
        cues = build_cues([
            phrase("Attention, the train is arriving.", source=0),
            phrase("Attention, please remain seated.", source=1),
        ], 10)
        self.assertEqual(len(cues), 2)

    def test_output_limit_retries_auto_language(self):
        calls = []
        def infer(start, end, language):
            calls.append(language)
            return dict(text="Hello!", language="en", limited=language is not None)
        records = recognize_window(infer, 20, 0, 10, "en")
        self.assertEqual(calls, ["en", None])
        self.assertEqual(records[0]["text"], "Hello!")

    def test_limit_subdivides_and_preserves_core_coverage(self):
        def infer(start, end, language):
            return dict(text="Hello!", language="en", limited=end - start > 5)
        records = recognize_window(infer, 40, 10, 20, None)
        self.assertEqual(records[0]["core_start"], 10)
        self.assertEqual(records[-1]["core_end"], 20)
        for left, right in zip(records, records[1:]):
            self.assertEqual(left["core_end"], right["core_start"])
        self.assertTrue(all(record["end"] - record["start"] <= 5 for record in records))

    def test_unresolved_loop_cannot_be_published(self):
        with self.assertRaisesRegex(RuntimeError, "output limit"):
            recognize_window(lambda *args: dict(text="uh " * 100, limited=True), 20, 0, 10, None)

    def test_long_valid_repetition_is_not_filtered(self):
        records = recognize_window(lambda *args: dict(text="Again! " * 12, limited=False),
                                   20, 0, 10, None)
        self.assertEqual(records[0]["text"].count("Again!"), 12)

    def test_file_boundaries_keep_full_context(self):
        calls = []
        def infer(start, end, language):
            calls.append((start, end))
            return dict(text="Please close the door.", limited=False)
        recognize_window(infer, 20, 0, 10, None)
        recognize_window(infer, 20, 10, 20, None)
        self.assertEqual(calls, [(0, 20), (0, 20)])

    def test_timeline_is_bounded(self):
        cue = build_cues([phrase("Yes!", 0., .1)], .2)[0]
        self.assertEqual((cue["start_us"], cue["end_us"]), (0, 200000))

    def test_language_detection_and_override(self):
        self.assertEqual(language_code("English"), "en")
        self.assertEqual(language_code("nl"), "nl")
        self.assertIsNone(language_code("auto"))
        with self.assertRaises(ValueError):
            language_code("not-a-language")


if __name__ == "__main__":
    unittest.main()
