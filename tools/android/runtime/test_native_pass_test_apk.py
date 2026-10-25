"""Ensure APK evaluation is installable without altering the existing app identity."""
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
class NativePassTestApk(unittest.TestCase):
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
