package ro.cobrabm.fexdroid;

import android.app.*;
import android.content.*;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.*;
import android.widget.*;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

/** Android drawer above the persistent game surface, adapted from the GoW3 player. */
final class BionicPlayerMenu {
 private final Activity activity;private final FrameLayout root;private final TouchControllerOverlay controls;private final PadBridge pad;
 private final LinearLayout panel,items;private final View scrim,handle;private final PerfHudOverlay hud;
 private final File hudFile;private final Runnable restart;private boolean open;
 BionicPlayerMenu(Activity a,FrameLayout frame,TouchControllerOverlay controller,PadBridge bridge,Runnable restartGame){
  activity=a;root=frame;controls=controller;pad=bridge;restart=restartGame;
  hudFile=new File(a.getFilesDir(),"issport/hud.ini");hudFile.getParentFile().mkdirs();HudConfig.migrateLegacy(a,hudFile);
  hud=new PerfHudOverlay(a);root.addView(hud,new FrameLayout.LayoutParams(-1,-1));
  FrameLayout grip=new FrameLayout(a);grip.setContentDescription(AppLanguage.text("Open game menu", "Abrir menu do jogo"));
  View bar=new View(a);GradientDrawable gold=new GradientDrawable();gold.setColor(0xccff642d);gold.setCornerRadius(dp(3));bar.setBackground(gold);grip.addView(bar,new FrameLayout.LayoutParams(dp(4),dp(48),Gravity.CENTER));
  handle=grip;handle.setElevation(dp(4));root.addView(handle,new FrameLayout.LayoutParams(dp(24),dp(80),Gravity.LEFT|Gravity.CENTER_VERTICAL));handle.setOnClickListener(v->show());
  scrim=new View(a);scrim.setBackgroundColor(0x88000000);scrim.setVisibility(View.GONE);scrim.setOnClickListener(v->close());root.addView(scrim,new FrameLayout.LayoutParams(-1,-1));
  panel=new LinearLayout(a);panel.setOrientation(1);panel.setElevation(dp(16));panel.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,new int[]{0xff291116,0xff12090c}));panel.setVisibility(View.GONE);
  ScrollView scroll=new ScrollView(a);items=new LinearLayout(a);items.setOrientation(1);items.setPadding(dp(20),dp(22),dp(18),dp(18));scroll.addView(items);panel.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));
  TextView footer=text("inFAMOUS SECOND SON\nDaedalus 0.2 · Android Bionic",12,0xff8e939c);footer.setPadding(dp(22),dp(10),dp(16),dp(18));panel.addView(footer);
  root.addView(panel,new FrameLayout.LayoutParams(Math.min(dp(340),Math.round(a.getResources().getDisplayMetrics().widthPixels*0.87f)),-1,Gravity.LEFT));
  apply();
 }
 private int dp(float n){return Math.round(n*activity.getResources().getDisplayMetrics().density);}
 private TextView text(String value,int size,int color){TextView t=new TextView(activity);t.setText(value);t.setTextSize(size);t.setTextColor(color);return t;}
 private void section(String value){TextView t=text(value,13,0xffff642d);t.setTypeface(Typeface.DEFAULT_BOLD);t.setPadding(dp(5),dp(24),0,dp(8));items.addView(t);}
 private void row(String icon,String title,String detail,Runnable action){
  LinearLayout row=new LinearLayout(activity);row.setGravity(Gravity.CENTER_VERTICAL);row.setPadding(dp(4),dp(13),dp(4),dp(13));row.setFocusable(true);row.setClickable(true);
  android.util.TypedValue selected=new android.util.TypedValue();activity.getTheme().resolveAttribute(android.R.attr.selectableItemBackground,selected,true);row.setBackgroundResource(selected.resourceId);
  TextView symbol=text(icon,25,0xffff642d);row.addView(symbol,new LinearLayout.LayoutParams(dp(38),-2));
  LinearLayout words=new LinearLayout(activity);words.setOrientation(1);TextView label=text(title,18,0xfffff1e9);words.addView(label);if(detail!=null){TextView sub=text(detail,12,0xffc7aba8);sub.setPadding(0,dp(3),0,0);words.addView(sub);}row.addView(words,new LinearLayout.LayoutParams(0,-2,1));row.setOnClickListener(v->action.run());items.addView(row);
 }
 private void contents(){
  items.removeAllViews();items.addView(text(AppLanguage.text("Swipe left or press B to go back", "Deslize para a esquerda ou pressione B para voltar"),12,0xff8e939c));
  section(AppLanguage.text("GAME", "JOGO"));row("▶",AppLanguage.text("Resume game", "Retomar jogo"),null,this::close);
  items.addView(BionicStartupSettings.toggle(activity));
  row("↻",AppLanguage.text("Restart game", "Reiniciar jogo"),AppLanguage.text("Unsaved progress will be lost", "O progresso não salvo será perdido"),()->new AlertDialog.Builder(activity).setTitle(AppLanguage.text("Restart inFAMOUS?", "Reiniciar inFAMOUS?")).setMessage(AppLanguage.text("Unsaved progress will be lost.", "O progresso não salvo será perdido.")).setNegativeButton(AppLanguage.text("Cancel", "Cancelar"),null).setPositiveButton(AppLanguage.text("Restart", "Reiniciar"),(d,w)->{close();restart.run();}).show());
  section(AppLanguage.text("GRAPHICS", "GRÁFICOS"));row("◷",AppLanguage.text("FPS and synchronization", "FPS e sincronização"),BionicPerformanceSettings.summary(activity),()->BionicPerformanceSettings.show(activity,this::contents));row("☷",AppLanguage.text("Patches", "Correções"),AppLanguage.text("Applied on next game start", "Aplicados no próximo início"),()->BionicVideoSettings.show(activity));
  int m=activity.getSharedPreferences("iss",0).getInt("fg_multiplier",0);
  row("◉",AppLanguage.text("Frame generation (BloodFrame)", "Geração de quadros (BloodFrame)"),m>=2?m+"×":AppLanguage.text("Off", "Desligado"),()->FramegenSettings.show(activity,()->{apply();contents();}));
  HudConfig config=HudConfig.load(hudFile);row("☷","HUD — FPS / GPU / CPU",config.enabled?AppLanguage.text("On", "Ligado"):AppLanguage.text("Off", "Desligado"),()->HudControls.show(activity,hudFile,()->{apply();contents();}));
  row("◈",AppLanguage.text("Readback — smoke and powers", "Readback — fumaça e poderes"),BionicReadbackSettings.summary(activity),()->BionicReadbackSettings.show(activity,this::contents));
  if(activity.getSharedPreferences("iss",0).getBoolean("fsr3",false)){
   row("▧",AppLanguage.text("Capture FSR3 image (2 frames)", "Capturar imagem FSR3 (2 quadros)"),
       AppLanguage.text("Input, depth, motion and output — export ZIP in Settings",
                        "Entrada, profundidade, movimento e saída — exporte ZIP nas Config."),
       this::captureFsr3);
  }
  section(AppLanguage.text("CONTROLS", "CONTROLES"));boolean visible=activity.getSharedPreferences("iss",0).getBoolean("controller_overlay",true);
  row("✚",AppLanguage.text("On-screen controller", "Controle na tela"),visible?AppLanguage.text("On", "Ligado"):AppLanguage.text("Off", "Desligado"),()->{activity.getSharedPreferences("iss",0).edit().putBoolean("controller_overlay",!visible).apply();apply();contents();});
  Switch labels=new Switch(activity);labels.setText(AppLanguage.text("Button labels", "Legendas dos botões"));labels.setTextColor(0xfffff1e9);labels.setChecked(activity.getSharedPreferences("iss",0).getBoolean("controller_labels",true));items.addView(labels);
  labels.setOnCheckedChangeListener((button,checked)->{activity.getSharedPreferences("iss",0).edit().putBoolean("controller_labels",checked).apply();controls.applyPreferences();});
  row("◈",AppLanguage.text("Control layout","Layout dos controles"),activity.getSharedPreferences("iss",0).getBoolean("mobile_controls",true)?AppLanguage.text("Mobile","Mobile"):AppLanguage.text("Classic","Clássico"),()->{android.content.SharedPreferences p=activity.getSharedPreferences("iss",0);p.edit().putBoolean("mobile_controls",!p.getBoolean("mobile_controls",true)).apply();apply();contents();});
  row("⇄",AppLanguage.text("Input mode", "Modo dos controles"),pad.isAutomaticMode()?AppLanguage.text("Automatic", "Automático"):pad.isMenuMode()?"Menu":AppLanguage.text("Gameplay", "Jogo"),()->{pad.cycleInputMode();controls.release();contents();});
  boolean mobileLayout=activity.getSharedPreferences("iss",0).getBoolean("mobile_controls",true);
  final String swipeKey=mobileLayout?"mobile_swipepad_enabled":"swipepad_enabled";
  row("▱",AppLanguage.text("On-screen swipepad", "Swipepad na tela"),activity.getSharedPreferences("iss",0).getBoolean(swipeKey,mobileLayout)?AppLanguage.text("On", "Ligado"):AppLanguage.text("Off", "Desligado"),()->{android.content.SharedPreferences p=activity.getSharedPreferences("iss",0);p.edit().putBoolean(swipeKey,!p.getBoolean(swipeKey,mobileLayout)).apply();apply();contents();});
  row("✚",AppLanguage.text("D-pad swipes", "Swipe pelo D-pad"),activity.getSharedPreferences("iss",0).getBoolean("dpad_swipe",true)?AppLanguage.text("Swipe the touchpad", "Deslizar o touchpad"):AppLanguage.text("Directional buttons", "Botões direcionais"),()->controlSettings(activity,pad,()->{apply();contents();}));
  row("✎",AppLanguage.text("Edit controller layout", "Editar layout do controle"),AppLanguage.text("Drag to move · two fingers to resize", "Arraste para mover · use dois dedos para redimensionar"),()->{close();activity.getSharedPreferences("iss",0).edit().putBoolean("controller_overlay",true).apply();apply();controls.edit(null);});
  row("↺",AppLanguage.text("Reset controller layout", "Restaurar layout do controle"),null,()->{controls.resetLayout();close();});
  row("⇥",AppLanguage.text("Exit game", "Sair do jogo"),null,()->new AlertDialog.Builder(activity).setTitle(AppLanguage.text("Exit inFAMOUS?", "Sair de inFAMOUS?")).setMessage(AppLanguage.text("Unsaved progress will be lost.", "O progresso não salvo será perdido.")).setNegativeButton(AppLanguage.text("Cancel", "Cancelar"),null).setPositiveButton(AppLanguage.text("Exit", "Sair"),(d,w)->activity.finishAffinity()).show());
 }
 private void captureFsr3(){
  File base=new File(activity.getFilesDir(),"issport");
  File directory=new File(base,"fsr-capture");
  if(!directory.isDirectory()&&!directory.mkdirs()){
   Toast.makeText(activity,AppLanguage.text("Cannot create capture folder.","Falha ao criar pasta da captura."),Toast.LENGTH_LONG).show();
   return;
  }
  // Clear only prior captures, never user saves or unrelated diagnostic files.
  File[] old=directory.listFiles();
  if(old!=null)for(File file:old){
   if(file.isFile()&&file.getName().matches("f[0-9]{3}_(input|motion|depth|output|aa_before_fxaa)_[0-9]+x[0-9]+_(rgba|rg16f|f32)\\.raw"))
    file.delete();
  }
  File trigger=new File(base,"fsr-capture.trigger");
  try(FileOutputStream stream=new FileOutputStream(trigger,false)){
   stream.write('1');
  }catch(IOException error){
   Toast.makeText(activity,AppLanguage.text("FSR capture request failed: ","Falha ao pedir captura FSR: ")+error.getMessage(),Toast.LENGTH_LONG).show();
   return;
  }
  close();
  Toast.makeText(activity,AppLanguage.text("FSR capture requested; play normally, then export ZIP in Settings.",
                                           "Captura FSR solicitada; jogue normalmente e exporte o ZIP nas Config."),
                 Toast.LENGTH_LONG).show();
 }
 static void controlSettings(Activity a,PadBridge pad,Runnable updated){
  android.content.SharedPreferences p=a.getSharedPreferences("iss",0);LinearLayout l=new LinearLayout(a);l.setOrientation(1);l.setPadding(24,12,24,12);
  TextView mobileHelp=new TextView(a);mobileHelp.setText(AppLanguage.text("Mobile: drag the right side to look. Action buttons keep the original game mappings. Mobile is the default layout. The swipepad starts enabled and can be switched off.","No celular, arraste o lado direito para mover a câmera. Os botões mantêm suas funções originais. O layout Mobile é o padrão; o painel tátil começa ativado e pode ser desligado."));l.addView(mobileHelp);
  CheckBox mobile=new CheckBox(a);mobile.setText(AppLanguage.text("Mobile action controls","Controles mobile por ações"));mobile.setChecked(p.getBoolean("mobile_controls",true));l.addView(mobile);
  CheckBox swipe=new CheckBox(a);swipe.setText(AppLanguage.text("D-pad swipes the touchpad (without holding Select)", "D-pad desliza o touchpad (sem segurar Select)"));swipe.setChecked(p.getBoolean("dpad_swipe",true));l.addView(swipe);
  CheckBox surface=new CheckBox(a);surface.setText(AppLanguage.text("Show swipepad on screen", "Mostrar swipepad na tela"));surface.setChecked(p.getBoolean(mobile.isChecked()?"mobile_swipepad_enabled":"swipepad_enabled",mobile.isChecked()));l.addView(surface);
  mobile.setOnCheckedChangeListener((b,on)->surface.setChecked(p.getBoolean(on?"mobile_swipepad_enabled":"swipepad_enabled",on)));
  final float[] sensitivityValues={p.getFloat("camera_sensitivity",1f),p.getFloat("spray_sensitivity",1f)};
  for(String kind:new String[]{"camera","spray"}){
   final int sensitivityIndex=kind.equals("camera")?0:1;
   TextView caption=new TextView(a);String title=kind.equals("camera")?AppLanguage.text("Camera sensitivity","Sensibilidade da câmera"):AppLanguage.text("Spray sensitivity","Sensibilidade do spray");
   float initial=Math.max(.25f,Math.min(3f,p.getFloat(kind+"_sensitivity",1f)));
   caption.setText(title+" · "+Math.round(initial*100)+"%");l.addView(caption);
   SeekBar slider=new SeekBar(a);slider.setMax(275);slider.setProgress(Math.round(initial*100)-25);l.addView(slider);
   slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
    public void onProgressChanged(SeekBar s,int progress,boolean user){caption.setText(title+" · "+(progress+25)+"%");if(user)sensitivityValues[sensitivityIndex]=(progress+25)/100f;}
    public void onStartTrackingTouch(SeekBar s){}public void onStopTrackingTouch(SeekBar s){}
   });
  }
  CheckBox swap=new CheckBox(a);swap.setText(AppLanguage.text("Swap □ / △", "Trocar □ / △"));swap.setChecked(p.getBoolean("swap_square_triangle",false));l.addView(swap);
  TextView help=new TextView(a);help.setText(AppLanguage.text("Automatic mode reads the game menu state. In menus, D-pad and X work normally, even with graffiti mode enabled. Gameplay mapping returns when the menu closes. If this executable is unsupported, Start/Options is the fallback.\n\nTap AUTO/MENU/GAME or hold L1 + R1 and tap Start to cycle the input override without pressing Start in the game. Pressing Start normally returns to Automatic.\n\nIn Game, hold a direction for gradual swipes, or drag the swipepad. Tap or hold inside it to press both ends and lift the car. Edit the layout to move/resize controls.", "O modo Automático lê o estado do menu do jogo. Nos menus, D-pad e X funcionam normalmente, mesmo com o grafite ativado. O mapeamento de jogo volta quando o menu fecha. Se o executável não for compatível, Start/Options é a alternativa.\n\nToque em AUTO/MENU/JOGO ou segure L1 + R1 e aperte Start para alternar o modo sem enviar Start ao jogo. Apertar Start normalmente volta ao Automático.\n\nEm Jogo, segure uma direção para swipes graduais ou arraste no swipepad. Toque ou segure dentro dele para pressionar as duas pontas e levantar o carro. Edite o layout para mover/redimensionar os controles."));l.addView(help);
  new AlertDialog.Builder(a).setTitle(AppLanguage.text("Controls", "Controles")).setView(controlScroll(a,l)).setNegativeButton(AppLanguage.text("Cancel", "Cancelar"),null).setPositiveButton(AppLanguage.text("Apply", "Aplicar"),(d,w)->{p.edit().putFloat("camera_sensitivity",sensitivityValues[0]).putFloat("spray_sensitivity",sensitivityValues[1]).putBoolean("mobile_controls",mobile.isChecked()).putBoolean("dpad_swipe",swipe.isChecked()).putBoolean("swap_square_triangle",swap.isChecked()).putBoolean(mobile.isChecked()?"mobile_swipepad_enabled":"swipepad_enabled",surface.isChecked()).apply();if(pad!=null){pad.setSwapFaceButtons(swap.isChecked());pad.setDpadSwipe(swipe.isChecked());}if(updated!=null)updated.run();}).show();
 }
 private static ScrollView controlScroll(Activity a,View content){ScrollView scroll=new ScrollView(a);scroll.addView(content);return scroll;}
 void apply(){if(activity instanceof BionicGameActivity)((BionicGameActivity)activity).updateDisplayRate();HudConfig c=HudConfig.load(hudFile);hud.setConfiguration(c);hud.setActive(c.enabled);pad.setSwapFaceButtons(activity.getSharedPreferences("iss",0).getBoolean("swap_square_triangle",false));pad.setDpadSwipe(activity.getSharedPreferences("iss",0).getBoolean("dpad_swipe",true));controls.applyPreferences();controls.setVisibility(!open&&activity.getSharedPreferences("iss",0).getBoolean("controller_overlay",true)?View.VISIBLE:View.GONE);}
 void show(){if(open)return;open=true;controls.release();pad.reset();controls.setVisibility(View.GONE);contents();handle.setVisibility(View.GONE);scrim.setVisibility(View.VISIBLE);panel.setVisibility(View.VISIBLE);panel.setTranslationX(-panel.getLayoutParams().width);panel.animate().translationX(0).setDuration(180).start();items.getChildAt(2).requestFocus();}
 void close(){if(!open)return;open=false;panel.animate().cancel();panel.setVisibility(View.GONE);scrim.setVisibility(View.GONE);handle.setVisibility(View.VISIBLE);pad.reset();apply();}
 boolean opened(){return open;}
 void toggle(){if(open)close();else show();}
 void pause(){controls.release();pad.reset();hud.setActive(false);}
 void resume(){apply();}
 void destroy(){panel.animate().cancel();hud.release();}
}
