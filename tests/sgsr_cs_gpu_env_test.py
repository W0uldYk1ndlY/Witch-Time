"""Regression: inherited experiment flags cannot replace HDR quality with fidelity."""
import os
import unittest
from unittest.mock import patch
from sgsr_cs_gpu_test import gpu_environment


class GpuEnvironmentTest(unittest.TestCase):
    def test_inherited_flags_are_isolated(self):
        with patch.dict(os.environ, {"WT_SGSR_TEST_OFFICIAL_REFERENCE": "1", "WT_SGSR_TEST_SKIP_FINAL": "1"}):
            quality = gpu_environment(quality=True)
            self.assertNotIn("WT_SGSR_TEST_OFFICIAL_REFERENCE", quality)
            self.assertNotIn("WT_SGSR_TEST_SKIP_FINAL", quality)
            fidelity = gpu_environment(quality=False)
            self.assertEqual(fidelity["WT_SGSR_TEST_OFFICIAL_REFERENCE"], "1")
            self.assertNotIn("WT_SGSR_TEST_SKIP_FINAL", fidelity)
            self.assertEqual(os.environ["WT_SGSR_TEST_OFFICIAL_REFERENCE"], "1")
            self.assertEqual(os.environ["WT_SGSR_TEST_SKIP_FINAL"], "1")


if __name__ == "__main__":
    unittest.main()
