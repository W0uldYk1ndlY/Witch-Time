from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JAVA_DIR = ROOT / "android/app/src/main/java/org/witchtime/android"
LAUNCHER = (JAVA_DIR / "LauncherActivity.java").read_text()
CHOOSER = (JAVA_DIR / "GpuDriverActivity.java").read_text()
OWNERSHIP = (JAVA_DIR / "GpuDriverStageOwnership.java").read_text()
RESTART_UI = (JAVA_DIR / "GpuDriverRestartUi.java").read_text()
MANIFEST = (ROOT / "android/app/src/main/AndroidManifest.xml").read_text()
MENU = (ROOT / "runtime/pcmenu.c").read_text()


class LauncherDriverUiTest(unittest.TestCase):
    def test_launcher_skips_driver_picker_and_reports_safe_recovery(self):
        self.assertIn("startActivity(new Intent(this, WitchTimeActivity.class))", LAUNCHER)
        self.assertIn("hasRecoveryPending()", LAUNCHER)
        self.assertIn("will use the system driver", LAUNCHER)
        self.assertNotIn("startActivity(new Intent(this, GpuDriverActivity.class))", LAUNCHER)

    def test_only_matching_helper_nonce_is_consumed_before_game_start(self):
        consume = LAUNCHER.index("store.consumeRestart(nonce)")
        start = LAUNCHER.index("startActivity(new Intent(this, WitchTimeActivity.class))")
        self.assertLess(consume, start)
        self.assertIn("GpuDriverActivity.EXTRA_RESTART_NONCE", LAUNCHER)
        self.assertIn("if (nonce == null || !store.consumeRestart(nonce)) { showRecoveryError(store.sealedRestartNonce()); return; }", LAUNCHER)
        self.assertIn("catch (IOException e) { showRecoveryError(store.sealedRestartNonce()); return; }", LAUNCHER)
        self.assertIn("store.sealedRestartNonce() != null", LAUNCHER)
        self.assertNotIn("consumeRestart(store.sealedRestartNonce())", LAUNCHER)
        self.assertIn('EXTRA_RESTART_NONCE = "org.witchtime.android.extra.GPU_RESTART_NONCE"', CHOOSER)
        self.assertNotIn("consumeRestart", CHOOSER)

    def test_orphan_sealed_request_requires_explicit_cancel_before_safe_startup(self):
        self.assertIn("showRecoveryError(store.sealedRestartNonce())", LAUNCHER)
        self.assertIn("message.setText(sealedNonce != null", LAUNCHER)
        self.assertIn('recover.setText("Cancel pending driver change")', LAUNCHER)
        self.assertIn("store.discardRestart(nonce)", LAUNCHER)
        self.assertIn("if (store.sealedRestartNonce() != null) throw new IOException", LAUNCHER)
        startup = LAUNCHER[LAUNCHER.index("protected void onCreate"):LAUNCHER.index("private void showRecoveryError")]
        self.assertNotIn("discardRestart", startup)
        cancel = LAUNCHER.index("private void cancelPendingRestart(String nonce")
        discard = LAUNCHER.index("store.discardRestart(nonce)", cancel)
        start = LAUNCHER.index("startActivity(new Intent(this, WitchTimeActivity.class))", cancel)
        self.assertLess(discard, start)
        self.assertNotIn('store.select("system")', LAUNCHER)
        self.assertIn("No driver was changed. Try again.", LAUNCHER)

    def test_restart_helper_is_private_and_isolated_from_sdl_process(self):
        self.assertIn('android:name=".GpuDriverRestartActivity" android:exported="false" android:process=":restart"', MANIFEST)
        self.assertIn("showProgress(Activity activity, String message)", RESTART_UI)
        self.assertIn("showError(Activity activity, String message)", RESTART_UI)

    def test_chooser_offers_system_turnip_and_imported_then_stages_nonce(self):
        self.assertIn("store.ensureBundledDriver()", CHOOSER)
        self.assertIn("GpuDriverStore.BUNDLED_ID", CHOOSER)
        self.assertIn('addOption("system", "System default"', CHOOSER)
        self.assertIn('"Turnip', CHOOSER)
        self.assertIn('Adreno 830"', CHOOSER)
        self.assertIn("store.importZip(in)", CHOOSER)
        self.assertIn("store.stageRestart(id, !\"system\".equals(id))", CHOOSER)
        self.assertIn("EXTRA_RESTART_NONCE", CHOOSER)
        self.assertNotIn("store.sealRestart", CHOOSER)
        self.assertNotIn("store.select(", CHOOSER)

    def test_trust_and_unsaved_progress_confirmation_precede_staging(self):
        self.assertIn('else confirm("This driver runs with Witch Time', CHOOSER)
        self.assertIn('confirm("The game will close and restart to apply this driver. Progress not yet saved', CHOOSER)
        self.assertIn('() -> stage(chosenId)', CHOOSER)
        self.assertIn("RESULT_CANCELED", CHOOSER)

    def test_staged_nonce_is_discarded_if_activity_cannot_deliver_it(self):
        self.assertIn("stageOwnership.stageCompleted(result)", CHOOSER)
        self.assertIn("if (orphan != null) { discardUndelivered(orphan); return; }", CHOOSER)
        self.assertIn("stageOwnership.abandon()", CHOOSER)
        delivered = CHOOSER.index("stageOwnership.claimDelivery(result)")
        result = CHOOSER.index("setResult(RESULT_OK", delivered)
        self.assertIn("if (result != null && !stageOwnership.claimDelivery(result)) return;", CHOOSER)
        self.assertLess(delivered, result)
        self.assertIn("cleanupClaimed", OWNERSHIP)
        cleanup = CHOOSER.index("private void discardUndelivered(String nonce)")
        self.assertIn("store.discardRestart(nonce)", CHOOSER[cleanup:])
        self.assertIn("catch (IOException e)", CHOOSER[cleanup:])
        self.assertIn("Log.e(TAG, \"Could not discard an undelivered staged driver restart\", e)", CHOOSER[cleanup:])
        self.assertIn("Close and reopen Witch Time before changing drivers.", CHOOSER[cleanup:])

    def test_executable_stage_ownership_race_harness(self):
        harness = ROOT / "tests/GpuDriverStageOwnershipHarness.java"
        with tempfile.TemporaryDirectory(prefix="gpu-stage-test-") as output:
            compiled = subprocess.run(
                ["javac", "-d", output, str(JAVA_DIR / "GpuDriverStageOwnership.java"), str(harness)],
                capture_output=True, text=True, check=False)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run(
                ["java", "-cp", output, "org.witchtime.android.GpuDriverStageOwnershipHarness"],
                capture_output=True, text=True, check=False)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
            self.assertIn("GpuDriverStageOwnershipHarness: PASS", executed.stdout)

    def test_internal_chooser_activity_is_not_exported(self):
        self.assertIn('android:name=".GpuDriverActivity" android:exported="false"', MANIFEST)

    def test_android_menu_action_requests_picker_outside_the_menu_lock_and_holds(self):
        self.assertIn('{"gpu_driver", "Graphics driver"', MENU)
        self.assertIn("pcmenu_android_set_driver_chooser", MENU)
        release = MENU.index("ReleaseSRWLockExclusive(&g_lock);", MENU.index("int open_driver_chooser"))
        callback = MENU.index("if (open_driver_chooser)", release)
        self.assertLess(release, callback)
        self.assertIn("pcmenu_android_driver_chooser_cancelled", MENU)
        self.assertIn("if (pcmenu_android_driver_chooser_is_held())", MENU)


if __name__ == "__main__":
    unittest.main()
