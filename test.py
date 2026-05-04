"""Tests for zmk-driver-animation module."""

import platform
import shutil
import subprocess
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
BUILD_DIR = REPO_ROOT.parent / "build"


def run_west(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["west", *args],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )


class ZmkBuildTest(unittest.TestCase):
    def test_zmk_build_generates_expected_configs(self):
        artifact = "zmk_driver_animation_xiao_ble"
        shutil.rmtree(BUILD_DIR / artifact, ignore_errors=True)

        result = run_west(["zmk-build", "tests/zmk-config"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        config_path = BUILD_DIR / artifact / "zephyr" / ".config"
        self.assertTrue(config_path.exists(), f"{artifact} .config is missing")
        config_text = config_path.read_text()
        for entry in [
            "CONFIG_SHIELD_TESTER_XIAO=y",
            "CONFIG_ZMK_ANIMATION=y",
        ]:
            self.assertIn(entry, config_text, f"{entry} not found in {artifact}")

    @unittest.skipUnless(platform.system() == "Linux", "zmk-test is only supported on Linux")
    def test_zmk_test_runs(self):
        tests_build = BUILD_DIR / "tests"
        if tests_build.exists():
            shutil.rmtree(tests_build)

        result = run_west(["zmk-test", "tests", "-m", "."])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
