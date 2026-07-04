#!/usr/bin/env python3
import tempfile
import unittest
from pathlib import Path

from tools.unity_astc.audio_resource_validate import AudioRef, validate_ref


class AudioResourceValidateTest(unittest.TestCase):
    def test_accepts_external_clip_starting_with_fsb5(self):
        with tempfile.TemporaryDirectory() as td:
            data_dir = Path(td)
            (data_dir / "sharedassets1.resource").write_bytes(
                b"gap" + b"FSB5" + b"\x00" * 12)

            problems = validate_ref(
                data_dir,
                AudioRef("Title", "sharedassets1.resource", 3, 16),
            )

            self.assertEqual(problems, [])

    def test_rejects_external_clip_that_does_not_start_with_audio_header(self):
        with tempfile.TemporaryDirectory() as td:
            data_dir = Path(td)
            (data_dir / "sharedassets1.resource").write_bytes(
                b"gap" + b"not-a-valid-audio-chunk")

            problems = validate_ref(
                data_dir,
                AudioRef("Title", "sharedassets1.resource", 3, 16),
            )

            self.assertTrue(problems)
            self.assertIn("invalid audio header", problems[0])
            self.assertIn("Title", problems[0])


if __name__ == "__main__":
    unittest.main()
