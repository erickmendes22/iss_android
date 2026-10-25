"""Ensure APK evaluation is installable without altering the existing app identity."""
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
class NativePassTestApk(unittest.TestCase):
 def test_native_passes_button_reachable_from_home(self):
  home=(ROOT/"android/app/src/ro/cobrabm/fexdroid/BionicMainActivity.java").read_text()
  settings=(ROOT/"android/app/src/ro/cobrabm/fexdroid/BionicRendererSettings.java").read_text()
  self.assertIn('BionicRendererSettings.showNativePasses(this,()->showPage(0))',home)
  self.assertIn('BionicRendererSettings.nativePassesSummary(this)',home)
  self.assertIn('static void showNativePasses(Activity a,Runnable onApplied)',settings)
  self.assertIn('.putBoolean("renderer_native_passes_arm64_secondary",secondary.isChecked())',settings)
  self.assertIn('.setPositiveButton(AppLanguage.text("Apply","Aplicar")',settings)

 def test_separate_package_signing_and_artifact(self):
  s=(ROOT/"tools/android/bionic/build_apk.sh").read_text()
  w=(ROOT/".github/workflows/bionic-experimental.yml").read_text()
  self.assertIn('ISS_NATIVE_PASSES_TEST_APK',s)
  self.assertIn('com.daedalus.iss.bionic.nativepasses',s)
  self.assertIn('--rename-manifest-package "$package_id"',s)
  self.assertIn('native-passes-test.keystore',s)
  self.assertIn('NativePasses-EXPERIMENTAL',s)
  self.assertIn('if [[ "$native_test" = 1 && "${ISS_RELEASE_BUILD:-1}" != 0 ]]',s)
  self.assertIn("github.ref_name == 'experimental-native-passes-arm64'",w)
  self.assertIn("ISS_RELEASE_BUILD: '0'",w)
  self.assertIn("ISS_NATIVE_PASSES_TEST_APK: '1'",w)
  self.assertIn("iss-native-passes-EXPERIMENTAL-apk-",w)
  manifest=(ROOT/"android/app/BionicAndroidManifest.xml").read_text()
  self.assertIn('package="com.daedalus.iss.bionic"',manifest)
if __name__ == "__main__":
 unittest.main()
