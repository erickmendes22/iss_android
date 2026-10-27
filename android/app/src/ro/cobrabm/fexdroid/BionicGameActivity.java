package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.*;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.*;
import java.util.regex.*;

/** Only the Bionic APK uses this activity.
 * No Xvfb, FEXDroid bridge, rootfs or Linux/glibc Vulkan loader.
 * Keeps the existing gamepad overlay, virtual graffiti and fullscreen modes.
 */
public final class BionicGameActivity extends Activity implements SurfaceHolder.Callback {
 @Override protected void attachBaseContext(android.content.Context base){super.attachBaseContext(AppLanguage.attach(base));}

 private FrameLayout frame;
 private SurfaceView surface;
 private TextView status;
 private TouchControllerOverlay controller;
 private BionicPlayerMenu menu;
 private boolean restarting,edgeGesture,swipeConsumed;
 private float edgeX,edgeY;
 private android.app.AlertDialog graffitiGuideDialog;
 private PadBridge pad;
 private AudioBridge audio;
 private final Handler main=new Handler(Looper.getMainLooper());
 private final ExecutorService worker=Executors.newSingleThreadExecutor();
 private volatile Process game;
 private boolean destroyed,started,failed;
 private String files,base,screenMode;
 private long lastFrames=-1;
 private float lastObservedRefresh=-1;
 private synchronized void record(String line){
  try(FileOutputStream f=new FileOutputStream(new File(files,"iss.log"),true)){
   f.write(("[Bionic] "+line+"\n").getBytes(StandardCharsets.UTF_8));
  }catch(Exception ignored){}
 }
 @Override public void onCreate(Bundle saved){
  super.onCreate(saved);
  getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
  applyFullscreen();
  files=getFilesDir().getPath();base=files+"/issport";
  screenMode=BionicVideoSettings.gameDisplayMode(getSharedPreferences("iss",0));
  try{FramegenSettings.write(this);BionicPerformanceSettings.write(this);}catch(IOException e){record(e.toString());}
  new File(base,"framegen.stats").delete();
  try(FileOutputStream out=new FileOutputStream(new File(files,"iss.log"),false)){
   out.write("inFAMOUS Second Son — Daedalus 0.2 · Android Bionic\n".getBytes(StandardCharsets.UTF_8));
   out.getFD().sync();
  }catch(Exception ignored){}
  // Never expose a native stdout mirror left over from an older game session.
  try(FileOutputStream mirror=new FileOutputStream(new File(files,"iss-native.log"),false)){
   mirror.write("Native stdout not started for this session yet.\n".getBytes(StandardCharsets.UTF_8));
   mirror.getFD().sync();
  }catch(IOException ex){record("Native log mirror initialization failed: "+ex);}
  pad=new PadBridge(new File(base,"android-pad.state"));
  audio=new AudioBridge(new File(base,"audio.fifo"));
  frame=new FrameLayout(this);frame.setBackgroundColor(0xff000000);
  surface=new SurfaceView(this);frame.addView(surface);
  updateDisplayRate();
  controller=new TouchControllerOverlay(this,pad,false);frame.addView(controller);
  status=new TextView(this);status.setText(AppLanguage.text("Starting inFAMOUS Bionic…","Iniciando inFAMOUS Bionic…"));
  status.setGravity(Gravity.CENTER);status.setTextColor(0xffe0c891);status.setTextSize(18);

  frame.addView(status);
  menu=new BionicPlayerMenu(this,frame,controller,pad,this::restartGame);
  setContentView(frame);
  frame.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or_,ob)->updateVideoSize());
  frame.post(this::updateVideoSize);
  surface.getHolder().addCallback(this);
  BionicLogDiagnostics.mark(this,"activity-ready","Android game activity initialized; native process not launched yet");
  record("Activity initialized");
 }
 private void applyFullscreen(){
  // Fullscreen selects image fill, not the visibility of Android's system bars.
  // Keep the whole landscape area available in fit (16:9), crop and stretch modes.
  getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
  getWindow().getDecorView().setSystemUiVisibility(5894);
  screenMode=BionicVideoSettings.gameDisplayMode(getSharedPreferences("iss",0));
  if(frame!=null)frame.post(this::updateVideoSize);
 }
 @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);if(focused)applyFullscreen();}
 void updateDisplayRate(){
  float hz=BionicDisplayRate.apply(this);
  IssPresent.frameRate(hz);
  record("Display: requested "+hz+" Hz; source limit="+BionicPerformanceSettings.limit(getSharedPreferences("iss",0)));
  if(surface!=null&&surface.getHolder().getSurface().isValid())try{surface.getHolder().getSurface().setFrameRate(hz,Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);}catch(Exception e){record("Display frame-rate hint: "+e.getMessage());}
 }
 private void updateVideoSize(){
  if(frame==null||surface==null)return;
  int w=frame.getWidth(),h=frame.getHeight();if(w<1||h<1)return;
  float scale="crop".equals(screenMode)?Math.max(w/1280f,h/720f):Math.min(w/1280f,h/720f);
  int vw="stretch".equals(screenMode)?w:Math.max(1,Math.round(1280*scale));
  int vh="stretch".equals(screenMode)?h:Math.max(1,Math.round(720*scale));
  FrameLayout.LayoutParams p=(FrameLayout.LayoutParams)surface.getLayoutParams();
  if(p.width!=vw||p.height!=vh||p.gravity!=Gravity.CENTER){
   p.width=vw;p.height=vh;p.gravity=Gravity.CENTER;surface.setLayoutParams(p);
  }
 }
 private Process process(String... command)throws IOException{
  ProcessBuilder p=new ProcessBuilder(command).redirectErrorStream(true);
  Map<String,String> e=p.environment();e.put("FXD_FILES",files);
  e.put("ISS_GAME_DIR",getIntent().getStringExtra("game_dir"));
  e.put("ISS_APP_LANGUAGE",AppLanguage.gameLanguage());
  e.put("ISS_BIONIC","1");e.put("ISS_ANDROID_INPUT","1");
  e.put("ISS_BIONIC_DRIVER",BionicDriverSettings.selection(this));
   e.put("ISS_MALI_ELIGIBLE",BionicDriverSettings.maliCandidate()?"1":"0");
  e.put("ISS_CUSTOM_DRIVER",BionicDriverSettings.customLibrary(this).getPath());
  String outputResolution=BionicVideoSettings.outputResolution(getSharedPreferences("iss",0));
  String[] dimensions=outputResolution.split("x");
  e.put("ISS_SCREEN_W",dimensions[0]);e.put("ISS_SCREEN_H",dimensions[1]);
  e.put("ISS_PRESENT_SOCKET",base+"/present.sock");
  e.put("TLG_PAD_FILE",base+"/android-pad.state");
  e.put("TLG_SHOW_FPS","0");
  e.put("TLG_ANDROID_FPS_ONLY","1");
  java.util.ArrayList<String> patches=new java.util.ArrayList<>();
  android.content.SharedPreferences prefs=getSharedPreferences("iss",0);
  e.put("ISS_PERFORMANCE_STATE_FILE",base+"/performance.state");
  BionicRendererSettings.environment(prefs,e);
  boolean fsr3=prefs.getBoolean("fsr3",false);
  e.put("TLG_UPSCALER",fsr3?"fsr3":"none");
  if(fsr3){
   // The upscaler exclusively owns render resolution while active:
   //   Performance 640x360 -> 1280x720; Quality 960x540 -> 1280x720.
   // The corresponding executable patch is still required internally, but
   // manual resolution-patch preferences must not be combined with it.
   e.put("TLG_UPSCALE_PRESET",Integer.toString(BionicVideoSettings.fsrEnginePreset(prefs)));
   e.put("TLG_FSR_SHARPNESS",BionicVideoSettings.fsrSharpnessValue(prefs));
   e.put("TLG_FSR_SHARPEN",BionicVideoSettings.fsrSharpenEnabled(prefs)?"1":"0");
   e.put("TLG_REACTIVE","0");
   e.put("TLG_JITTER","1");
   e.put("TLG_RENDER_RES",BionicVideoSettings.fsrInputSize(prefs));
  }
  String resolution=BionicVideoSettings.effectiveResolutionPatch(prefs);
  String resolutionPatch=BionicVideoSettings.internalPatchName(resolution);
  if(resolutionPatch!=null)patches.add(resolutionPatch);
  if(prefs.getBoolean("patch_motion_blur",false))patches.add("Disable motion blur");
  if(prefs.getBoolean("patch_dof",false))patches.add("Disable depth of field");
  if(prefs.getBoolean("patch_depth_blur",false))patches.add("Disable depth blur");
  if(prefs.getBoolean("patch_shadows",false))patches.add("Disable shadow maps");

  e.put("ISS_PATCHES",android.text.TextUtils.join(";",patches));
  e.put("TLG_BP09","0");e.put("TLG_BC_TINY","0");
  e.put("TLG_OUTPUT_RES",outputResolution);
  return p.start();
 }
 @Override public void surfaceCreated(SurfaceHolder h){
  if(destroyed||started)return;
  started=true;
  try{
   String result=IssPresent.start(h,base+"/present.sock","stretch".equals(screenMode));
   record(result);
   BionicLogDiagnostics.mark(this,"presenter-initialized",result);
   updateDisplayRate();
   if(!result.startsWith("bridge running:")){error(result);return;}
   audio.start();if(!audio.awaitReady(5000)){error(AppLanguage.text("Audio FIFO did not start","O áudio FIFO não iniciou"));return;}
   worker.execute(()->{
    try(BionicSession.Guard ignored=BionicSession.acquire(getFilesDir())){
     BionicSaveArchive.recover(new File(base,"user/savedata"));
     synchronized(this){
      if(destroyed||restarting)return;
      record("Executing native Bionic inFAMOUS");
      BionicLogDiagnostics.mark(this,"native-launch-requested","Launching run-bionic.sh");
      game=process("/system/bin/sh",base+"/run-bionic.sh");
      BionicLogDiagnostics.mark(this,"native-process-started","Bionic process started successfully");
     }
     try{
     try(InputStream in=game.getInputStream();
         FileOutputStream out=new FileOutputStream(new File(files,"iss.log"),true);
         FileOutputStream mirror=new FileOutputStream(new File(files,"iss-native.log"),false)){
      mirror.write("Bionic native stdout mirror · session start\n".getBytes(StandardCharsets.UTF_8));
      byte[] buf=new byte[65536];int n;
      while((n=in.read(buf))!=-1){
       out.write(buf,0,n);
       mirror.write(buf,0,n);
      }
      out.getFD().sync();
      mirror.getFD().sync();
     }
     int exit=game.waitFor();
     BionicLogDiagnostics.mark(this,"native-exited","Exit code: "+exit);
     record("Bionic game exited "+exit);
     if(!destroyed&&!restarting)error(AppLanguage.text("Game stopped (","O jogo parou (")+exit+AppLanguage.text("). Export log.","). Exporte o log."));
     }finally{
      // Hold the save lock until the native writer has actually exited, even on Activity teardown.
      if(game.isAlive())game.destroyForcibly();
      boolean interrupted=false;
      for(;;)try{game.waitFor();break;}catch(InterruptedException e){interrupted=true;}
      if(interrupted)Thread.currentThread().interrupt();
     }
    }catch(Exception ex){
     BionicLogDiagnostics.mark(this,"launcher-exception",ex.toString());
     if(!destroyed)error(ex.toString());
    }
   });
   poll();
  }catch(Exception ex){error(ex.toString());}
 }
 private void showGraffitiGuideIfRequested(){
  if(destroyed || failed || graffitiGuideDialog!=null)return;
  // The native GPU records a guide request without touching an unavailable
  // ImGui context. Show the full instructions using the Android Activity.
  final File marker=new File(base,"android-pad.state.guide");
  if(!marker.isFile())return;
  graffitiGuideDialog=new android.app.AlertDialog.Builder(this)
   .setTitle(AppLanguage.text("Graffiti without a gyroscope", "Grafite sem giroscópio"))
   .setMessage(AppLanguage.text("L3 + R3: toggle sensor mode and hold the can sideways.\n\n", "L3 + R3: ativar ou desativar o modo sensor e segurar a lata de lado.\n\n")
      +AppLanguage.text("✕ (X): shake the can.\n\n", "✕ (X): sacudir a lata.\n\n")
      +AppLanguage.text("R2: spray. Aim with the left stick; small movements improve precision.\n\n", "R2: pintar. Mire com o analógico esquerdo; movimentos pequenos dão mais precisão.\n\n")
      +AppLanguage.text("L3 + R3 again: return to walking.", "L3 + R3 novamente: voltar a andar."))
   .setPositiveButton(AppLanguage.text("Done","Entendi"),(dialog,which)->{})
   .create();
  graffitiGuideDialog.setOnDismissListener(dialog->{
   if(marker.exists()&&!marker.delete())record("Could not delete graffiti guide marker");
   graffitiGuideDialog=null;
  });
  graffitiGuideDialog.show();
  record("Native Android graffiti guide displayed (no ImGui Vulkan dependency)");
 }
 private void poll(){
  if(destroyed)return;
  float refresh=getWindowManager().getDefaultDisplay().getRefreshRate();
  if(refresh!=lastObservedRefresh){lastObservedRefresh=refresh;record("Display: Android reports "+refresh+" Hz");}
  showGraffitiGuideIfRequested();
  long n=IssPresent.frames();
  if(!failed&&n>0)status.setVisibility(View.GONE);
  if(n>0&&lastFrames<0){
   BionicLogDiagnostics.mark(this,"native-frames-presented","First Android AHardwareBuffer frames received");
   record("Bionic direct AHardwareBuffer frames active");lastFrames=n;
  }
  main.postDelayed(this::poll,1000);
 }
 private void error(String message){
  failed=true;
  BionicLogDiagnostics.mark(this,"error",message);
  record("ERROR: "+message);
  main.post(()->{status.setText(message);status.setVisibility(View.VISIBLE);});
 }
 @Override public void surfaceChanged(SurfaceHolder h,int format,int w,int ht){}
 @Override public void surfaceDestroyed(SurfaceHolder h){}
 private void restartGame(){
  if(restarting||destroyed)return;restarting=true;status.setText(AppLanguage.text("Restarting inFAMOUS…", "Reiniciando inFAMOUS…"));status.setVisibility(View.VISIBLE);
  synchronized(this){if(game!=null)game.destroyForcibly();}
  worker.execute(()->main.post(()->{if(!destroyed)recreate();}));
 }
 @Override protected void onActivityResult(int request,int result,android.content.Intent data){super.onActivityResult(request,result,data);if(BionicDriverSettings.result(this,request,result,data))return;if(request==FramegenSettings.IMPORT_DLL&&result==RESULT_OK&&data!=null&&data.getData()!=null)FramegenSettings.importDll(this,data.getData(),null);}
 @Override public void onBackPressed(){if(menu!=null)menu.toggle();}
 @Override public boolean dispatchKeyEvent(KeyEvent e){
  int code=e.getKeyCode();
  if(code==KeyEvent.KEYCODE_BACK||code==KeyEvent.KEYCODE_BUTTON_MODE){if(e.getAction()==KeyEvent.ACTION_UP&&menu!=null)menu.toggle();return true;}
  if(menu!=null&&menu.opened()){
   if(code==KeyEvent.KEYCODE_BUTTON_B){if(e.getAction()==KeyEvent.ACTION_UP)menu.close();return true;}
   return super.dispatchKeyEvent(e);
  }
  return pad!=null&&pad.key(e)||super.dispatchKeyEvent(e);
 }
 @Override public boolean dispatchTouchEvent(MotionEvent e){
  float density=getResources().getDisplayMetrics().density;int action=e.getActionMasked();
  if(action==MotionEvent.ACTION_DOWN){edgeX=e.getX();edgeY=e.getY();edgeGesture=menu!=null&&!menu.opened()&&edgeX<24*density;swipeConsumed=false;}
  if(swipeConsumed){if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL)swipeConsumed=false;return true;}
  if(action==MotionEvent.ACTION_MOVE&&menu!=null){float dx=e.getX()-edgeX,dy=e.getY()-edgeY;
   if((edgeGesture&&dx>55*density&&Math.abs(dy)<80*density)||(menu.opened()&&dx< -70*density&&Math.abs(dy)<80*density)){
    MotionEvent cancel=MotionEvent.obtain(e);cancel.setAction(MotionEvent.ACTION_CANCEL);super.dispatchTouchEvent(cancel);cancel.recycle();menu.toggle();edgeGesture=false;swipeConsumed=true;return true;
   }
  }
  return super.dispatchTouchEvent(e);
 }
 @Override public boolean onGenericMotionEvent(MotionEvent e){return menu!=null&&menu.opened()?super.onGenericMotionEvent(e):pad!=null&&pad.motion(e)||super.onGenericMotionEvent(e);}
 @Override protected void onPause(){
  super.onPause();if(menu!=null)menu.pause();
  if(pad!=null)pad.reset();if(audio!=null)audio.pause();
 }
 @Override protected void onResume(){
  super.onResume();applyFullscreen();
  if(audio!=null)audio.resume();if(menu!=null)menu.resume();
 }
 @Override protected void onDestroy(){
  synchronized(this){destroyed=true;if(game!=null)game.destroyForcibly();}
  main.removeCallbacksAndMessages(null);
  if(graffitiGuideDialog!=null){
   graffitiGuideDialog.setOnDismissListener(null);
   graffitiGuideDialog.dismiss();
   graffitiGuideDialog=null;
  }
  if(menu!=null)menu.destroy();
  IssPresent.stop();if(audio!=null)audio.stop();
  worker.shutdown();super.onDestroy();
 }
}
