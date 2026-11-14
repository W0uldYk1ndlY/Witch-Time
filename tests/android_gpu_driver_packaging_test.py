"""Source contract: no bundled driver, including stale generated asset inputs."""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class PackagingTest(unittest.TestCase):
    def test_build_has_no_bundle_generator_or_asset_source(self):
        gradle = (ROOT / 'android/app/build.gradle').read_text()
        for obsolete in ('turnip', 'Turnip', 'bundle.gradle', 'generateTurnipBundleAssets'):
            self.assertNotIn(obsolete, gradle)
        self.assertIn("assets.srcDir '../third_party/gpu_driver'", gradle)
        self.assertFalse(list((ROOT / 'android/third_party/turnip_adreno830').rglob('*.*')))

    def test_store_never_provisions_and_lists_only_imported_packages(self):
        store = (ROOT / 'android/app/src/main/java/org/witchtime/android/GpuDriverStore.java').read_text()
        self.assertNotIn('ensureBundledDriver', store)
        self.assertNotIn('getAssets()', store)
        self.assertIn('if (!file.getName().startsWith("pkg-")) continue;', store)
        self.assertIn('BUNDLED_HASH.equals(hash.toString())', store)


if __name__ == '__main__':
    unittest.main()
