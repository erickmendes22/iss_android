package ro.cobrabm.fexdroid;

import java.io.File;
import java.util.Arrays;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** One filename/size contract shared by capture cleanup, readiness and ZIP export. */
final class FsrCaptureFiles {
 private static final Pattern RAW=Pattern.compile("f[0-9]{3}_(input|motion|depth|output|aa_before_fxaa|aa_after_fxaa)_([0-9]+)x([0-9]+)_(rgba|bgra|rg16f|f32|rgba16f|rgba32f|b10g11r11uf|a2b10g10r10)\\.raw");
 static boolean captureName(String name){return RAW.matcher(name).matches()||name.matches("f[0-9]{3}_aa_info\\.txt");}
 static boolean exportable(File file){
  if(!file.isFile())return false;
  if(file.getName().matches("f[0-9]{3}_aa_info\\.txt"))return file.length()>0&&file.length()<4096;
  Matcher m=RAW.matcher(file.getName());if(!m.matches())return false;
  try{
   long w=Long.parseLong(m.group(2)),h=Long.parseLong(m.group(3));
   int bytes=m.group(4).equals("rgba16f")?8:m.group(4).equals("rgba32f")?16:4;
   return w>0&&h>0&&w<=8192&&h<=8192&&w*h*bytes<=20000000L&&file.length()==w*h*bytes;
  }catch(NumberFormatException e){return false;}
 }
 static File[] select(File dir){File[] f=dir.listFiles(FsrCaptureFiles::exportable);if(f==null)return new File[0];Arrays.sort(f,(a,b)->a.getName().compareTo(b.getName()));return f;}
 static boolean complete(File[] files){
  // A ready export needs all four original buffers from the same frame, not
  // merely four arbitrary files (e.g. two half-written captures).
  for(int frame=0;frame<2;frame++){
   String prefix=String.format(java.util.Locale.ROOT,"f%03d_",frame);int bits=0;boolean needAa=false,needPost=false;
   for(File f:files){String n=f.getName();if(!n.startsWith(prefix))continue;
    if(n.startsWith(prefix+"input_"))bits|=1;if(n.startsWith(prefix+"motion_"))bits|=2;
    if(n.startsWith(prefix+"depth_"))bits|=4;if(n.startsWith(prefix+"output_"))bits|=8;
    if(n.startsWith(prefix+"aa_before_fxaa_"))bits|=16;
    if(n.startsWith(prefix+"aa_after_fxaa_"))bits|=32;
    if(n.equals(prefix+"aa_info.txt"))try{
     String info=new String(java.nio.file.Files.readAllBytes(f.toPath()),java.nio.charset.StandardCharsets.UTF_8);
     needAa=info.contains("pre_fxaa_available=1");needPost=info.contains("fsr_input=pre_fxaa_original");
    }catch(java.io.IOException e){return false;}
   }
   if((bits&15)!=15||(needAa&&(bits&16)==0)||(needPost&&(bits&32)==0))return false;
  }
  return true;
 }
}
