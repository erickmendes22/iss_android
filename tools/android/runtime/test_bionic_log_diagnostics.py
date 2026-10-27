"""Regression coverage for nul-filled Android log exports after Mali crashes."""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
JAVA = ROOT / "android/app/src/ro/cobrabm/fexdroid"


class BionicLogDiagnosticsTests(unittest.TestCase):
    def test_export_validates_input_and_truncates_document(self):
        helper = (JAVA / "BionicLogDiagnostics.java").read_text()
        self.assertIn('openOutputStream(document,"wt")', helper)
        self.assertIn('if(c==0)s.zeros++', helper)
        self.assertIn('source=a.valid()?primary:(b.valid()?nativeLog:null)', helper)
        self.assertIn('NO RECOVERABLE TEXT LOG', helper)
        self.assertIn('new File(context.getFilesDir(),"iss-native.log")', helper)
        self.assertIn('getSharedPreferences(PREFS,Context.MODE_PRIVATE)', helper)

    def test_durable_phase_works_independently_of_native_output(self):
        helper = (JAVA / "BionicLogDiagnostics.java").read_text()
        self.assertIn('e.commit()', helper)
        game = (JAVA / "BionicGameActivity.java").read_text()
        for phase in ('"activity-ready"', '"native-launch-requested"',
                      '"native-process-started"', '"native-frames-presented"',
                      '"native-exited"', '"launcher-exception"', '"error"'):
            self.assertIn(phase, game)

    def test_game_preserves_an_independent_stdout_mirror(self):
        game = (JAVA / "BionicGameActivity.java").read_text()
        self.assertIn('new File(files,"iss-native.log"),false', game)
        self.assertIn('mirror.write(buf,0,n)', game)
        self.assertIn('out.write(buf,0,n)', game)
        self.assertIn('mirror.getFD().sync()', game)

    def test_launcher_uses_exporter_instead_of_raw_copy(self):
        main = (JAVA / "BionicMainActivity.java").read_text()
        self.assertIn('BionicLogDiagnostics.export(this,data.getData())', main)
        self.assertNotIn('new FileInputStream(new File(getFilesDir(),"iss.log"))', main)


if __name__ == "__main__":
    unittest.main()
