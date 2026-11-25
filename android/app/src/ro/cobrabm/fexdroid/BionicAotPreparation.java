package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.content.SharedPreferences;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.Map;

final class BionicAotPreparation {
 interface Listener { void update(String message,int percent); }

 static String patches(SharedPreferences prefs){
  java.util.ArrayList<String> patches=new java.util.ArrayList<>();
  String resolution=BionicVideoSettings.effectiveResolutionPatch(prefs);
  String resolutionPatch=BionicVideoSettings.internalPatchName(resolution);
  if(resolutionPatch!=null)patches.add(resolutionPatch);
  if(prefs.getBoolean("patch_motion_blur",false))patches.add("Disable motion blur");
  if(prefs.getBoolean("patch_dof",false))patches.add("Disable depth of field");
  if(prefs.getBoolean("patch_depth_blur",false))patches.add("Disable depth blur");
  if(prefs.getBoolean("patch_shadows",false))patches.add("Disable shadow maps");

  return android.text.TextUtils.join(";",patches);
 }
 static String stage(String value){
  switch(value){
   case "scan": return "Analisando código";
   case "discover": return "Localizando funções";
   case "compile": return "Compilando AOT ARM64";
   case "link": return "Vinculando a biblioteca AOT";
   case "done": return "Finalizando a compilação";
   default: return "Preparando AOT · "+value;
  }
 }
 static void run(Activity activity,String path,String patches,String token,Listener listener) throws Exception {
  File files=activity.getFilesDir();
  File script=new File(files,"issport/run-bionic.sh");
  ProcessBuilder builder=new ProcessBuilder("/system/bin/sh",script.getAbsolutePath()).redirectErrorStream(true);
  Map<String,String> env=builder.environment();
  env.put("FXD_FILES",files.getAbsolutePath());env.put("ISS_GAME_DIR",path);
  env.put("ISS_PATCHES",patches);env.put("ISS_AOT_MODE","prepare");env.put("ISS_AOT_TOKEN",token);
  env.put("PYTHONUNBUFFERED","1");
  Process process=builder.start();
  String previous="",detail=""; boolean ready=false;
  try(BufferedReader input=new BufferedReader(new InputStreamReader(process.getInputStream(),StandardCharsets.UTF_8));
      PrintWriter log=new PrintWriter(new FileOutputStream(new File(files,"iss.log")),true)){
   String line;
   while((line=input.readLine())!=null){
    String label=null; int percent=-1;
    if(line.startsWith("BBR-PROGRESS ")){
     String[] fields=line.split(" +");
     if(fields.length==4){
      try{
       long done=Long.parseLong(fields[2]),total=Long.parseLong(fields[3]);
       if(total>0 && done>=0)percent=(int)Math.min(100,100.0*done/total);
      }catch(NumberFormatException ignored){}
      label=stage(fields[1])+(percent<0?"":" · "+percent+"%");
     }
    }else{
     log.println(line);
     if(!line.trim().isEmpty() && !line.startsWith("BBR-"))detail=line;
     if(line.equals("ISS preparation: prepare"))label="Lendo o eboot";
     if(line.equals("ISS preparation: link_modules"))label="Vinculando os módulos do jogo";
     if(line.equals("ISS preparation: content_profile"))label="Preparando o conteúdo";
     if(line.equals("ISS preparation: patches"))label="Aplicando os patches selecionados";
     if(line.equals("ISS-AOT waiting"))label="Verificando a fila de compilação";
     if(line.equals("ISS-AOT patch-image"))label="Preparando a imagem para compilar o AOT";
     if(line.equals("ISS-AOT checking-cache"))label="Verificando o AOT em cache";
     if(line.equals("ISS-AOT cached"))label="AOT em cache verificado";
     if(line.equals("ISS-AOT verifying"))label="Validando a biblioteca ARM64";
     if(line.equals("ISS-AOT loading"))label="Testando o carregamento do AOT";
     if(line.equals("ISS-AOT ready "+token)){ready=true;label="AOT pronto";percent=100;}
    }
    if(label!=null && !label.equals(previous)){
     if(line.startsWith("BBR-PROGRESS "))log.println(line);
     previous=label;listener.update(label,percent);
    }
   }
   int code=process.waitFor();
   if(code!=0||!ready)throw new IOException("código "+code+" · "+detail);
  }finally{if(process.isAlive())process.destroyForcibly();}
 }
}
