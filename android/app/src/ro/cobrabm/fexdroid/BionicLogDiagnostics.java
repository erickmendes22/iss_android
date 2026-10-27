package ro.cobrabm.fexdroid;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Independent Bionic run checkpoints and safe text-log export.
 *
 * Android document providers may keep old bytes when opened in non-truncating
 * write mode. A crash can also leave the stdout capture unusable. Never export
 * a zero-only file as though it were a real crash log.
 */
final class BionicLogDiagnostics {
 private static final String PREFS="iss_bionic_run_diagnostics";
 private static final int BUFFER=65536;

 private BionicLogDiagnostics(){}

 static void mark(Context context,String phase,String detail){
  // commit(), not apply(): survives activity/process termination without
  // depending on flushing the game stdout logfile.
  SharedPreferences.Editor e=context.getSharedPreferences(PREFS,Context.MODE_PRIVATE).edit();
  e.putString("phase",phase);
  e.putString("detail",detail==null?"":detail);
  e.putLong("time",System.currentTimeMillis());
  e.commit();
 }

 private static final class Scan {
  long size,zeros,printable;
  boolean valid(){return size>0 && printable>=8;}
 }

 private static Scan scan(File file)throws IOException{
  Scan s=new Scan();
  if(!file.isFile())return s;
  try(InputStream in=new BufferedInputStream(new FileInputStream(file))){
   byte[] buffer=new byte[BUFFER];
   int n;
   while((n=in.read(buffer))!=-1){
    s.size+=n;
    for(int i=0;i<n;i++){
     int c=buffer[i]&255;
     if(c==0)s.zeros++;
     else if(c==9||c==10||c==13||c>=32)s.printable++;
    }
   }
  }
  return s;
 }

 private static void write(OutputStream out,String text)throws IOException{
  out.write(text.getBytes(StandardCharsets.UTF_8));
 }

 private static void copyText(File source,OutputStream output)throws IOException{
  // The source is a UTF-8 text log. Strip embedded NUL bytes from a partially
  // corrupted log rather than transferring a binary-looking document.
  try(InputStream in=new BufferedInputStream(new FileInputStream(source))){
   byte[] buffer=new byte[BUFFER];
   int n;
   while((n=in.read(buffer))!=-1){
    int count=0;
    for(int i=0;i<n;i++)if(buffer[i]!=0)buffer[count++]=buffer[i];
    if(count>0)output.write(buffer,0,count);
   }
  }
 }

 static void export(Context context,Uri document)throws IOException{
  File primary=new File(context.getFilesDir(),"iss.log");
  File nativeLog=new File(context.getFilesDir(),"iss-native.log");
  Scan a=scan(primary),b=scan(nativeLog);
  // The input is intentionally inspected before we open/truncate the output
  // document: a URI pointing at a source file must never truncate the source.
  SharedPreferences prefs=context.getSharedPreferences(PREFS,Context.MODE_PRIVATE);
  String when=new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z",Locale.US)
   .format(new Date(prefs.getLong("time",0L)));
  String last=prefs.getString("phase","unavailable");
  String detail=prefs.getString("detail","unavailable");
  if(detail==null)detail="unavailable";

  try(OutputStream output=context.getContentResolver().openOutputStream(document,"wt")){
   if(output==null)throw new IOException("Android document provider did not return an output stream");
   write(output,"inFAMOUS Second Son — Daedalus 0.2 · Android Bionic diagnostics\n");
   write(output,"Exported: "+new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z",Locale.US)
       .format(new Date())+"\n");
   write(output,"Last durable checkpoint: "+last+" at "+when+"\n");
   write(output,"Checkpoint detail: "+detail+"\n");
   write(output,"iss.log: "+a.size+" bytes, "+a.zeros+" NUL bytes, "+a.printable+" text-like bytes\n");
   write(output,"iss-native.log: "+b.size+" bytes, "+b.zeros+" NUL bytes, "+b.printable+" text-like bytes\n");
   File source=a.valid()?primary:(b.valid()?nativeLog:null);
   if(source!=null){
    write(output,"\n--- "+source.getName()+" ---\n");
    copyText(source,output);
    write(output,"\n--- end of captured log ---\n");
   }else{
    write(output,"\nNO RECOVERABLE TEXT LOG: the capture was missing, empty or only NUL bytes.\n");
    write(output,"The durable checkpoint above records how far the Android launcher reached.\n");
   }
   output.flush();
  }
 }
}
