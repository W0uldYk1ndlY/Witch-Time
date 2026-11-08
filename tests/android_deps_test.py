"""Fast command-line contract tests; real native builds are verified separately."""
import os
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/android-deps.sh"


class AndroidDependenciesTest(unittest.TestCase):
    def run_script(self, *args):
        env = dict(os.environ, ANDROID_NDK_HOME="/nonexistent/witch-time-ndk")
        return subprocess.run(["bash", str(SCRIPT), *args], env=env,
                              text=True, capture_output=True)

    def test_help_does_not_require_ndk_or_download(self):
        result = self.run_script("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ANDROID_NDK_HOME", result.stdout)
        self.assertIn("--verify", result.stdout)

    def test_unknown_argument_is_rejected(self):
        result = self.run_script("--not-a-real-option")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unknown argument", result.stderr)

    def test_missing_ndk_is_actionable(self):
        result = self.run_script("--verify")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ANDROID_NDK_HOME", result.stderr)
        self.assertIn("Android NDK", result.stderr)


if __name__ == "__main__":
    unittest.main()
