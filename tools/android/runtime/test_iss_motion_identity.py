"""Run the actual history allocator against instance culling/reordering."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT=pathlib.Path(__file__).resolve().parents[3]

class IssMotionIdentity(unittest.TestCase):
    def test_history_reordering_and_existing_allocator_contract(self):
        compiler=shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('C++20 compiler required')
        with tempfile.TemporaryDirectory() as directory:
            for name in ('test_motion_history','test_iss_motion_identity'):
                executable=pathlib.Path(directory)/name
                subprocess.run([compiler,'-std=c++20','-O2','-I',str(ROOT/'port'),
                                str(ROOT/'port/tests'/f'{name}.cpp'),'-o',str(executable)],check=True)
                subprocess.run([str(executable)],check=True)

if __name__=='__main__':
    unittest.main()
