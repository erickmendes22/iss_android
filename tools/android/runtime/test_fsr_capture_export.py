"""Exercise the actual Android capture selector with files and a round-trip ZIP."""
import pathlib,subprocess,tempfile,unittest,shutil
ROOT=pathlib.Path(__file__).resolve().parents[3]
SRC=ROOT/'android/app/src/ro/cobrabm/fexdroid'
class CaptureExport(unittest.TestCase):
 def test_original_fxaa_capture_survives_export_and_partial_files_do_not(self):
  if not shutil.which('java'):self.skipTest('Java required')
  harness=r'''
package ro.cobrabm.fexdroid;
import java.io.*;import java.nio.file.*;import java.util.zip.*;
public class CaptureExportTest {
 static void file(File dir,String name,int bytes)throws Exception{Files.write(new File(dir,name).toPath(),new byte[bytes]);}
 public static void main(String[] args)throws Exception {
  File dir=new File(args[0]);dir.mkdirs();
  for(int f=0;f<2;f++){
   String p=String.format("f%03d_",f);
   file(dir,p+"input_2x2_rgba.raw",16);file(dir,p+"output_4x4_rgba.raw",64);
   file(dir,p+"motion_2x2_rg16f.raw",16);file(dir,p+"depth_2x2_f32.raw",16);
   Files.write(new File(dir,p+"aa_info.txt").toPath(),"pre_fxaa_available=1\n".getBytes("UTF-8"));
  }
  assert !FsrCaptureFiles.complete(FsrCaptureFiles.select(dir)); // GPU copies not ready
  file(dir,"f000_aa_before_fxaa_2x2_rgba16f.raw",32);
  file(dir,"f000_aa_after_fxaa_2x2_rgba.raw",16);
  file(dir,"f001_aa_after_fxaa_2x2_rgba.raw",16);
  file(dir,"f001_aa_before_fxaa_2x2_rgba.raw",15); // partial: must not enter ZIP
  assert !FsrCaptureFiles.complete(FsrCaptureFiles.select(dir));
  file(dir,"f001_aa_before_fxaa_2x2_rgba.raw",16);
  file(dir,"f000_input_999999999999999999999x2_rgba.raw",16);
  file(dir,"user-save.raw",16);file(dir,"f001_input_2x2_rgba.raw.tmp",16);
  File[] selected=FsrCaptureFiles.select(dir);assert selected.length==14;
  assert FsrCaptureFiles.complete(selected);
  ByteArrayOutputStream bytes=new ByteArrayOutputStream();
  try(ZipOutputStream z=new ZipOutputStream(bytes)){for(File f:selected){z.putNextEntry(new ZipEntry(f.getName()));Files.copy(f.toPath(),z);z.closeEntry();}}
  int pre=0,post=0,metadata=0,total=0;
  try(ZipInputStream z=new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))){for(ZipEntry e;(e=z.getNextEntry())!=null;){
   total++;if(e.getName().contains("aa_before_fxaa"))pre++;if(e.getName().contains("aa_after_fxaa"))post++;if(e.getName().contains("aa_info"))metadata++;
   assert !e.getName().contains("save");assert FsrCaptureFiles.captureName(e.getName());z.closeEntry();}}
  assert total==14&&pre==2&&post==2&&metadata==2;
  assert !FsrCaptureFiles.captureName("savedata/foo.bin");
  System.out.println("PASS: both pre/post-FXAA frames + metadata reach ZIP; partial, oversized and unrelated files excluded");
 }
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'CaptureExportTest.java').write_text(harness)
   subprocess.run(['java','com.sun.tools.javac.Main','-d',d,str(SRC/'FsrCaptureFiles.java'),str(p/'CaptureExportTest.java')],check=True)
   subprocess.run(['java','-ea','-cp',d,'ro.cobrabm.fexdroid.CaptureExportTest',str(p/'capture')],check=True)
  launcher=(SRC/'BionicMainActivity.java').read_text();menu=(SRC/'BionicPlayerMenu.java').read_text()
  self.assertIn('FsrCaptureFiles.select(dir)',launcher)
  self.assertIn('FsrCaptureFiles.select(capture)',launcher)
  self.assertIn('FsrCaptureFiles.captureName(file.getName())',menu)
 def test_passive_capture_layout_and_fsr_input_preserved(self):
  code=(ROOT/'port/gpu/shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp').read_text()
  at=code.index('auto& pre = texture_cache.GetImage(iss_aa_prefilter);')
  block=code[at:at+1100]
  self.assertLess(block.index('runtime.Transit(&pre'),block.index('"aa_before_fxaa"'))
  self.assertLess(block.index('runtime.FlushBarriers()'),block.index('"aa_before_fxaa"'))
  self.assertNotIn('ldr_target = iss_aa_prefilter',code)
  self.assertIn('ldr_target = image_id;',code)
if __name__=='__main__':unittest.main()
