"""ISS PC native-passes submit-mode qualification on Android/FEX.

This verifies structural pass boundaries; actual Vulkan native draw execution
is not wired and must never be reported as a performance gain.
"""
import pathlib
import shutil
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[3]
CXX=shutil.which("c++")
@unittest.skipUnless(CXX,"C++ compiler unavailable")
class NativeSubmitTests(unittest.TestCase):
    def test_analysis_rejects_malformed_and_never_rewrites_guest_pm4(self):
        source=r'''
#include <array>
#include <cassert>
#include <cstdint>
#include "video_core/amdgpu/native_pass_submit.h"
int main() {
    using Scanner=AmdGpu::NativePassSubmitScan;
    constexpr auto pkt=[](unsigned op, unsigned words) {
        return (3U<<30) | (((words-1U)&0x3fffU)<<16) | (op<<8);
    };
    std::array<std::uint32_t,27> commands{
        pkt(0x69,2),0,9,             // register 0
        pkt(0x27,5),1,2,3,4,5,       // draw 1
        pkt(0x10,2),0,0,             // draw's short NOP
        pkt(0x27,5),1,2,3,4,5,       // draw 2
        pkt(0x20,2),0,0,             // predication: hard boundary
        pkt(0x27,5),1,2,3,4,5        // draw 3, alone
    };
    static_assert(commands.size()==27);
    const auto original=commands;
    const auto x=Scanner::Analyze(commands);
    assert(x.valid && x.direct_draws==3);
    assert(x.candidate_passes==1 && x.candidate_draws==2);
    assert(x.largest_pass==2 && commands==original);
    auto bad=commands;
    bad[3]=(3U<<30) | (0x3fffU<<16) | (0x27U<<8);
    const auto y=Scanner::Analyze(bad);
    assert(!y.valid && y.candidate_passes==0);
    std::array<std::uint32_t,6> pred{
        pkt(0x27,5)|1U,1,2,3,4,5
    };
    assert(Scanner::Analyze(pred).candidate_passes==0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src=pathlib.Path(tmp)/"native_submit.cpp"
            exe=pathlib.Path(tmp)/"native_submit"
            src.write_text(source)
            built=subprocess.run([CXX,"-std=c++20","-O2","-Wall","-Wextra",
                                  "-Werror","-I",str(ROOT/"port/gpu/shadps4"),
                                  str(src),"-o",str(exe)],capture_output=True,text=True)
            self.assertEqual(built.returncode,0,built.stderr)
            ran=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(ran.returncode,0,ran.stderr)
    def test_opt_in_is_read_only_and_separate_from_hybrid(self):
        app=(ROOT/"android/app/src/ro/cobrabm/fexdroid/BionicRendererSettings.java").read_text()
        core=(ROOT/"port/gpu/shadps4/video_core/amdgpu/liverpool.cpp").read_text()
        header=(ROOT/"port/gpu/shadps4/video_core/amdgpu/native_pass_submit.h").read_text()
        self.assertIn('ISS_NATIVE_PASSES_ARM64',app)
        self.assertIn('renderer_native_passes_arm64_scan',app)
        self.assertIn('NativePassSubmitScan::Analyze(dcb)',core)
        self.assertIn('seq != NoSeq',core)
        self.assertIn('execution=original',header)
        self.assertNotIn('vkCmdExecuteCommands',header)
if __name__=="__main__":
    unittest.main()
