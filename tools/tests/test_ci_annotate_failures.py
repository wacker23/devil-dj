import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import ci_annotate_failures as annotate  # noqa: E402

LOG = """
117/229 Test #117: Loading a 10-minute MP3 doesn't glitch the audio callback ...***Failed    3.94 sec
-------------------------------------------------------------------------------
C:\a\tests\engine\TrackLoaderTests.cpp(306): FAILED:
  CHECK( after.xruns == before.xruns )
with expansion:
  1 == 0

===============================================================================
"""


class AnnotateTests(unittest.TestCase):
    def test_finds_test_name_and_check(self):
        found = list(annotate.failures(LOG))
        self.assertEqual(len(found), 1)
        title, detail = found[0]
        self.assertEqual(title, "Loading a 10-minute MP3 doesn't glitch the audio callback")
        self.assertIn("CHECK( after.xruns == before.xruns )", "\n".join(detail))
        self.assertIn("1 == 0", "\n".join(detail))

    def test_clean_log_has_no_failures(self):
        self.assertEqual(list(annotate.failures("100% tests passed, 0 tests failed out of 10")), [])

    def test_escape_keeps_one_line(self):
        self.assertEqual(annotate.escape("a\nb%"), "a%0Ab%25")


if __name__ == "__main__":
    unittest.main()
