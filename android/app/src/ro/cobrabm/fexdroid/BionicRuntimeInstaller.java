package ro.cobrabm.fexdroid;
import android.content.Context;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
final class BionicRuntimeInstaller {
 static synchronized void install(Context c)throws Exception{
  File root=c.getFilesDir(),marker=new File(root,"runtime-bionic.sha256");String expected;try(InputStream in=c.getAssets().open("runtime.sha256")){expected=new String(read(in),StandardCharsets.UTF_8).trim();}
  if(marker.isFile()&&new String(Files.readAllBytes(marker.toPath()),StandardCharsets.UTF_8).trim().equals(expected)&&complete(root))return;
  File archive=new File(root,"runtime-bionic.tar.gz");MessageDigest sha=MessageDigest.getInstance("SHA-256");
  try(InputStream in=c.getAssets().open("runtime.tar.gz");OutputStream out=new FileOutputStream(archive)){byte[] b=new byte[262144];int n;while((n=in.read(b))>0){sha.update(b,0,n);out.write(b,0,n);}}
  StringBuilder hash=new StringBuilder();for(byte b:sha.digest())hash.append(String.format("%02x",b&255));if(!expected.equals(hash.toString()))throw new IOException("Runtime checksum mismatch.");
  Process p=new ProcessBuilder("/system/bin/tar","-xzf",archive.getPath(),"-C",root.getPath()).redirectErrorStream(true).start();String error=new String(read(p.getInputStream()),StandardCharsets.UTF_8);if(p.waitFor()!=0)throw new IOException("Extraction failed: "+error);
  if(!complete(root))throw new IOException("Extracted ARM64 runtime incomplete: loader, FEX, Vulkan, Python or startup scripts missing.");for(String path:new String[]{"issport/arm64/bin/iss-probe","issport/arm64/bin/kgsl-chip-id","issport/arm64/bin/orbis-pkg-util","issport/arm64/python/bin/python3.13","issport/run-bionic.sh"})if(!new File(root,path).setExecutable(true,true))throw new IOException("Cannot execute Bionic runtime: "+path);
  Files.write(marker.toPath(),expected.getBytes(StandardCharsets.UTF_8));archive.delete();
 }
 private static boolean complete(File root){
  String[] required={"issport/arm64/bin/iss-probe","issport/arm64/bin/kgsl-chip-id","issport/arm64/bin/orbis-pkg-util",
   "issport/arm64/lib/libisscpu.so","issport/arm64/lib/libtlggpu.so",
   "issport/arm64/lib/libc++_shared.so","issport/arm64/lib/libbloodframe.so","issport/arm64/python/bin/python3.13",
   "issport/arm64/drivers/gen8/libvulkan_freedreno.so",
   "issport/arm64/drivers/gen7/libvulkan_freedreno.so",
    "issport/arm64/drivers/panvk/libvulkan_panvk.so",
   "issport/run-bionic.sh","issport/launch.py",
   "issport/scripts/prepare.py","issport/scripts/link_modules.py",
   "issport/scripts/patches.py","issport/scripts/iss_mask_patches.py",
   "issport/patches/InfamousSecondSon.xml"};
  for(String path:required)if(!new File(root,path).isFile())return false;
  return new File(root,"issport/arm64/bin/iss-probe").isFile();
 }
 static byte[] read(InputStream in)throws IOException{ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] b=new byte[65536];int n;while((n=in.read(b))>0)out.write(b,0,n);return out.toByteArray();}
}
