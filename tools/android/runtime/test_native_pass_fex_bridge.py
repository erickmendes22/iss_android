"""Validate the real ISS PC drawIndex signature and AArch64/FEX guest bridge bytes."""
from pathlib import Path
import subprocess,shutil,tempfile,unittest
ROOT=Path(__file__).resolve().parents[3]
CXX=shutil.which("cc") or shutil.which("clang")
@unittest.skipUnless(CXX,"C compiler unavailable")
class NativeGuestBridgeTests(unittest.TestCase):
 def test_trampoline_is_transparent_and_signature_guarded(self):
  code=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "native_pass_arm64_hook.h"
int main(void) {
    unsigned char original[ISS_NATIVE_DRAW_OFFSET+14]={0};
    memcpy(original+ISS_NATIVE_DRAW_OFFSET,iss_native_draw_prologue,14);
    assert(iss_native_draw_signature(original,sizeof original));
    original[ISS_NATIVE_DRAW_OFFSET+1]^=1;
    assert(!iss_native_draw_signature(original,sizeof original));
    assert(!iss_native_draw_signature(original,ISS_NATIVE_DRAW_OFFSET));
    unsigned char t[36]={0}, j[14]={0};
    assert(iss_native_make_trampoline(t,UINT64_C(0x10000000))==36);
    assert(t[0]==0x51 && t[1]==0x41 && t[2]==0x53);
    assert(t[3]==0x0f && t[4]==0x05);
    assert(t[5]==0x41 && t[6]==0x5b && t[7]==0x59);
    assert(!memcmp(t+8,iss_native_draw_prologue,14));
    assert(t[22]==0xff && t[23]==0x25);
    uint64_t resume=0; memcpy(&resume,t+28,8);
    assert(resume==0x10000000);
    iss_native_jump14(j,0x20000000);
    assert(j[0]==0xff && j[1]==0x25);
    uint64_t target=0;memcpy(&target,j+6,8);
    assert(target==0x20000000);
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   path=Path(tmp)/"t.c";path.write_text(code)
   exe=Path(tmp)/"t"
   c=subprocess.run([CXX,"-std=c11","-Wall","-Wextra","-Werror",
      "-I",str(ROOT/"port/src"),str(path),"-o",str(exe)],capture_output=True,text=True)
   self.assertEqual(c.returncode,0,c.stderr)
   r=subprocess.run([str(exe)],capture_output=True,text=True)
   self.assertEqual(r.returncode,0,r.stderr)
 def test_bridge_is_gated_and_does_not_use_native_sysv_pointer(self):
  probe=(ROOT/"port/src/probe.c").read_text()
  settings=(ROOT/"android/app/src/ro/cobrabm/fexdroid/BionicRendererSettings.java").read_text()
  self.assertIn('iss_native_draw_signature(image,size)',probe)
  self.assertIn('guest_cpu_hook((uintptr_t)stub+ISS_NATIVE_DRAW_TRAP_OFFSET',probe)
  self.assertIn('regs->rip = iss_native_trampoline + ISS_NATIVE_DRAW_AFTER_TRAP;',probe)
  self.assertIn('ISS_NATIVE_PASSES_ARM64_ENGINE_HOOK',settings)
  self.assertIn('renderer_native_passes_arm64_guest_observe',settings)
if __name__=="__main__":
 unittest.main()
