package ro.cobrabm.fexdroid;
import android.Manifest;
import android.app.*;
import android.content.*;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.*;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.widget.*;
import java.io.*;
public final class BionicMainActivity extends Activity {
 @Override protected void attachBaseContext(android.content.Context base){super.attachBaseContext(AppLanguage.attach(base));}

 private TextView status; private boolean busy;
 private final java.util.ArrayList<Button> actions=new java.util.ArrayList<>();
 private static final int IMPORT_SAVE=10, EXPORT_SAVE=11;
 private BionicPkgInstaller pkg;
 private ProgressBar progress;
 private Dialog setupDialog;
 private static final int SETUP=1, FOLDER=2, PKG=3, UPDATE_PKG=4;
 private int pendingSetup;
 private boolean requestingAccess, waitingForSettings;

 private LinearLayout pages, navigation; private int selectedPage;
 private Switch directStart;
 @Override public void onCreate(Bundle saved){super.onCreate(saved);
  getWindow().setStatusBarColor(0xff150b0e);getWindow().setNavigationBarColor(0xff150b0e);
  android.widget.FrameLayout root=new android.widget.FrameLayout(this);
  ImageView art=new ImageView(this);art.setImageResource(getResources().getIdentifier("infamous","drawable",getPackageName()));art.setScaleType(ImageView.ScaleType.CENTER_CROP);art.setAlpha(.28f);root.addView(art,new android.widget.FrameLayout.LayoutParams(-1,-1));root.setBackgroundColor(0xff12090c);
  LinearLayout chrome=new LinearLayout(this);chrome.setOrientation(1);chrome.setPadding(dp(18),dp(12),dp(18),dp(12));root.addView(chrome,new android.widget.FrameLayout.LayoutParams(-1,-1));
  TextView title=label("inFAMOUS SECOND SON",26,0xfffff1e9);title.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);chrome.addView(title);
  chrome.addView(label("DAEDALUS  ·  0.2  ·  ANDROID BIONIC",12,0xffff642d));
  status=label("",13,0xffddc3be);status.setPadding(0,dp(12),0,dp(8));chrome.addView(status);
  progress=new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal);progress.setIndeterminate(true);progress.setVisibility(android.view.View.GONE);chrome.addView(progress);
  pages=new LinearLayout(this);pages.setOrientation(1);chrome.addView(pages,new LinearLayout.LayoutParams(-1,0,1));
  navigation=new LinearLayout(this);navigation.setPadding(dp(4),dp(4),dp(4),dp(4));navigation.setBackground(background(0xff211015,0xff86362c));chrome.addView(navigation,new LinearLayout.LayoutParams(-1,-2));
  pkg=new BionicPkgInstaller(this,(active,message)->{setBusy(active);status.setText(message);});
  setContentView(root);showPage(saved==null?0:saved.getInt("page",0));refresh();
  if(saved!=null){pendingSetup=saved.getInt("pending_setup",0);requestingAccess=saved.getBoolean("requesting_access",false);waitingForSettings=saved.getBoolean("waiting_settings",false);}
  if(!requestingAccess&&!waitingForSettings)pages.post(()->{
   // Only a fresh app launch may auto-start. Returning from the game or
   // recreating the launcher must leave its settings accessible.
   if(saved==null&&BionicStartupSettings.enabled(this)&&access()&&gameReady())launch();
   else beginSetup(false);
  });
 }
 private int dp(float value){return Math.round(value*getResources().getDisplayMetrics().density);}
 private android.graphics.drawable.GradientDrawable background(int fill,int border){android.graphics.drawable.GradientDrawable b=new android.graphics.drawable.GradientDrawable();b.setColor(fill);b.setCornerRadius(dp(22));b.setStroke(dp(1),border);return b;}
 private TextView label(String value,int size,int color){TextView t=new TextView(this);t.setText(value);t.setTextSize(size);t.setTextColor(color);return t;}
 private LinearLayout card(LinearLayout parent,String title){LinearLayout l=new LinearLayout(this);l.setOrientation(1);l.setPadding(dp(16),dp(10),dp(16),dp(18));l.setBackground(background(0xf2190d11,0xff70342d));LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);lp.topMargin=dp(16);parent.addView(l,lp);section(l,title);return l;}
 private void showPage(int page){
  selectedPage=page;actions.clear();directStart=null;pages.removeAllViews();navigation.removeAllViews();
  String[] tabs={AppLanguage.text("⌂\nHome", "⌂\nInício"),AppLanguage.text("☷\nPatches","☷\nPatches"),AppLanguage.text("⚙\nSettings", "⚙\nConfig."),AppLanguage.text("ⓘ\nAbout", "ⓘ\nSobre")};
  for(int i=0;i<tabs.length;i++){final int index=i;Button b=new Button(this);b.setText(tabs[i]);b.setAllCaps(false);b.setTextSize(12);b.setTextColor(i==page?0xfffff1e9:0xffc7aba8);b.setBackground(background(i==page?0xffad2532:0x00211015,0x00000000));navigation.addView(b,new LinearLayout.LayoutParams(0,dp(62),1));b.setOnClickListener(v->{if(!busy)showPage(index);});}
  ScrollView sc=new ScrollView(this);sc.setFillViewport(true);LinearLayout content=new LinearLayout(this);content.setOrientation(1);content.setPadding(0,0,0,dp(18));sc.addView(content);pages.addView(sc,new LinearLayout.LayoutParams(-1,-1));
  if(page==0){
   LinearLayout game=card(content,AppLanguage.text("GAME", "JOGO"));button(game,AppLanguage.text("▶  PLAY", "▶  JOGAR"),this::launch);button(game,AppLanguage.text("Select game folder", "Selecionar pasta do jogo"),this::choose);button(game,AppLanguage.text("Install game from PKG", "Instalar jogo por PKG"),()->installPkg(false));button(game,AppLanguage.text("Install update from PKG", "Instalar atualização por PKG"),()->installPkg(true));
   directStart=BionicStartupSettings.toggle(this);game.addView(directStart);actions.add(directStart);
   LinearLayout video=card(content,AppLanguage.text("DISPLAY AND PERFORMANCE", "EXIBIÇÃO E DESEMPENHO"));button(video,AppLanguage.text("FPS and VSync · ", "FPS e VSync · ")+BionicPerformanceSettings.summary(this),()->BionicPerformanceSettings.show(this,()->showPage(0)));button(video,AppLanguage.text("Video", "Vídeo"),()->BionicVideoSettings.showVideo(this));
   button(video,AppLanguage.text("Readback — smoke and powers", "Readback — fumaça e poderes")+" · "+BionicReadbackSettings.summary(this),()->BionicReadbackSettings.show(this,()->showPage(0)));
   button(video,AppLanguage.text("Frame generation — Bloodframe", "Geração de quadros — Bloodframe"),()->FramegenSettings.show(this,null));
  }else if(page==1){LinearLayout patches=card(content,AppLanguage.text("PATCHES — inFAMOUS SECOND SON","PATCHES — inFAMOUS SECOND SON"));patches.addView(BionicVideoSettings.content(this,2));
  }else if(page==2){
   LinearLayout language=card(content,AppLanguage.text("LANGUAGE","IDIOMA"));button(language,AppLanguage.summary(this),()->AppLanguage.show(this));language.addView(label(AppLanguage.text("Language changes apply on next game start. Missing game text or audio falls back to English.","O idioma muda no próximo início do jogo. Se faltarem textos ou áudio, será usado inglês."),14,0xffc7aba8));
   LinearLayout driver=card(content,AppLanguage.text("Vulkan driver","Driver Vulkan"));BionicDriverSettings.add(this,driver);
   LinearLayout display=card(content,AppLanguage.text("HUD AND CONTROLS", "HUD E CONTROLES"));File hud=new File(getFilesDir(),"issport/hud.ini");hud.getParentFile().mkdirs();HudConfig.migrateLegacy(this,hud);button(display,"HUD — FPS / GPU / CPU",()->HudControls.show(this,hud,null));button(display,AppLanguage.text("Controls and D-pad swipes", "Controles e swipe pelo D-pad"),()->BionicPlayerMenu.controlSettings(this,null,null));
   LinearLayout saves=card(content,AppLanguage.text("Saves","Salvamentos"));saves.addView(label(AppLanguage.text("Import or export saves with the game closed.", "Importe ou exporte seus salvamentos com o jogo fechado."),14,0xffc7aba8));button(saves,AppLanguage.text("Export saves (ZIP)", "Exportar salvamentos (ZIP)"),()->chooseSave(false));button(saves,AppLanguage.text("Import saves (ZIP)", "Importar salvamentos (ZIP)"),()->chooseSave(true));
   LinearLayout logs=card(content,AppLanguage.text("LOGS","REGISTROS"));button(logs,AppLanguage.text("Export latest log", "Exportar último log"),()->startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("text/plain").putExtra(Intent.EXTRA_TITLE,"iss-android.log"),3));
   LinearLayout fg=card(content,"LOSSLESS.DLL · BLOODFRAME");button(fg,AppLanguage.text("Import Lossless.dll", "Importar Lossless.dll"),()->FramegenSettings.chooseDll(this));
  }else{
   LinearLayout about=card(content,"inFAMOUS SECOND SON — DAEDALUS FORK");about.addView(label(AppLanguage.text("Version","Versão")+" 0.2\n"+AppLanguage.text("Android port with a Bionic runtime.","Port para Android com runtime Bionic.")+"\n\n"+AppLanguage.text("Fork and modifications:","Fork e modificações:")+" erickmendes22\nDaedalus Emulator\n\n"+AppLanguage.text("Base project:","Projeto base:")+" HayatoG/iss_pc\n\n"+AppLanguage.text("No game files are included.","Nenhum arquivo do jogo está incluído."),17,0xfffff1e9));button(about,"YouTube · @daedalusemulator",()->startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse("https://www.youtube.com/@daedalusemulator"))));button(about,"GitHub · iss_android",()->startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse("https://github.com/erickmendes22/iss_android"))));
   LinearLayout help=card(content,AppLanguage.text("CONTROLS", "CONTROLES"));help.addView(label(AppLanguage.text("TOUCH: two fingers to move the car.\nD-pad: gradual swipe; hold a direction to continue.\nGraffiti: L3 + R3 enables the virtual sensor; ✕ shakes the can, R2 sprays and the left stick aims.\n\nOpen the game menu from the left edge or with the Back button.", "TOUCH: dois dedos para mover o carro.\nD-pad: deslize gradual; mantenha pressionado para continuar.\nGrafite: L3 + R3 ativa o sensor virtual; ✕ sacode a lata, R2 pinta e o analógico esquerdo mira.\n\nAbra o menu do jogo pela borda esquerda ou pelo botão Voltar."),16,0xffddc3be));
  }
 }
 @Override protected void onSaveInstanceState(Bundle out){super.onSaveInstanceState(out);out.putInt("page",selectedPage);out.putInt("pending_setup",pendingSetup);out.putBoolean("requesting_access",requestingAccess);out.putBoolean("waiting_settings",waitingForSettings);}
 private void section(LinearLayout l,String title){TextView t=label(title,15,0xffff642d);t.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);t.setPadding(0,dp(8),0,dp(12));l.addView(t);}
 private void button(LinearLayout l,String text,Runnable action){Button b=new Button(this);b.setText(text);b.setTextSize(15);b.setAllCaps(false);b.setGravity(android.view.Gravity.CENTER_VERTICAL|android.view.Gravity.LEFT);b.setPadding(dp(14),dp(8),dp(14),dp(8));b.setTextColor(0xfffff1e9);b.setBackground(background((text.contains("JOGAR")||text.contains("PLAY"))?0xffb42635:0xff30171b,0xff79372c));LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);lp.topMargin=dp(8);b.setMinHeight(dp(54));l.addView(b,lp);b.setOnClickListener(v->{if(!busy&&(pkg==null||!pkg.busy()))action.run();});actions.add(b);}
 private boolean access(){return Build.VERSION.SDK_INT>=30&&Environment.isExternalStorageManager()||checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)==PackageManager.PERMISSION_GRANTED;}
 private void refresh(){status.setText(getSharedPreferences("iss",0).getString("game_dir",AppLanguage.text("Select the extracted game folder or install a PKG.", "Selecione a pasta do jogo extraído ou instale um PKG.")));}
 private boolean gameReady(){String path=getSharedPreferences("iss",0).getString("game_dir","");return !path.isEmpty()&&new File(path,"eboot.bin").canRead()&&new File(path,"art").isDirectory()&&new File(path,"art").canRead();}
 private void beginSetup(boolean force){
  if(isFinishing()||isDestroyed()||busy||pkg.busy()||requestingAccess||waitingForSettings||setupDialog!=null&&setupDialog.isShowing())return;
  if(!access()){requestAccess(SETUP);return;}
  if(!force&&gameReady())return;
  AlertDialog d=new AlertDialog.Builder(this).setTitle(AppLanguage.text("Set up inFAMOUS Second Son", "Configurar inFAMOUS Second Son"))
   .setMessage(AppLanguage.text("How would you like to add the game? Install a PKG directly into a chosen folder or select an already extracted game folder.", "Como você quer adicionar o jogo? Instale um PKG diretamente na pasta escolhida ou selecione uma pasta de jogo já extraída."))
   .setNegativeButton(AppLanguage.text("Select folder", "Selecionar pasta"),(dialog,which)->{setupDialog=null;choose();})
   .setPositiveButton(AppLanguage.text("Install PKG", "Instalar PKG"),(dialog,which)->{setupDialog=null;installPkg(false);})
   .setNeutralButton(AppLanguage.text("Not now", "Agora não"),(dialog,which)->{}).create();
  setupDialog=d;d.setOnDismissListener(dialog->{if(setupDialog==d)setupDialog=null;});d.show();
 }
 private void requestAccess(int next){
  if(requestingAccess||waitingForSettings)return;
  pendingSetup=next;requestingAccess=true;
  requestPermissions(new String[]{Manifest.permission.READ_EXTERNAL_STORAGE,Manifest.permission.WRITE_EXTERNAL_STORAGE},1);
 }
 private void continueSetup(){int next=pendingSetup;pendingSetup=0;if(next==FOLDER)choose();else if(next==PKG)installPkg(false);else if(next==UPDATE_PKG)installPkg(true);else if(next==SETUP)beginSetup(false);}
 private void installPkg(boolean update){if(!access()){requestAccess(update?UPDATE_PKG:PKG);return;}pkg.pick(update);}
 private void choose(){
  if(!access()){requestAccess(FOLDER);return;}
  if(setupDialog!=null&&setupDialog.isShowing())return;
  Dialog d=new Dialog(this);LinearLayout guide=new LinearLayout(this);guide.setOrientation(1);guide.setPadding(dp(24),dp(24),dp(24),dp(24));guide.setBackground(background(0xff211015,0xffa34432));
  TextView tag=label(AppLanguage.text("GAME SETUP", "CONFIGURAÇÃO DO JOGO"),13,0xffff642d);tag.setLetterSpacing(.12f);guide.addView(tag);
  TextView title=label(AppLanguage.text("Choose the game folder", "Escolha a pasta do jogo"),27,0xfffff1e9);title.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);title.setPadding(0,dp(14),0,dp(18));guide.addView(title);
  TextView description=label(AppLanguage.text("Select the extracted inFAMOUS Second Son folder, such as CUSA00004 or CUSA00223. It must contain eboot.bin and the art/ folder.", "Selecione a pasta extraída de inFAMOUS Second Son, por exemplo CUSA00004 ou CUSA00223. Ela deve conter eboot.bin e a pasta art/."),17,0xffddc3be);guide.addView(description);
  Button pick=new Button(this);pick.setText(AppLanguage.text("CHOOSE GAME FOLDER", "ESCOLHER PASTA DO JOGO"));pick.setTextColor(0xfffff1e9);pick.setBackground(background(0xffb42635,0xffd05a3d));pick.setMinHeight(dp(58));LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);lp.topMargin=dp(26);guide.addView(pick,lp);
  pick.setOnClickListener(v->{d.dismiss();openGameFolderPicker();});
  ScrollView scroll=new ScrollView(this);scroll.addView(guide);d.setContentView(scroll);d.getWindow().setBackgroundDrawableResource(android.R.color.transparent);setupDialog=d;d.setOnDismissListener(dialog->{if(setupDialog==d)setupDialog=null;});d.show();d.getWindow().setLayout(Math.round(getResources().getDisplayMetrics().widthPixels*.92f),android.view.ViewGroup.LayoutParams.WRAP_CONTENT);
 }
 private void openGameFolderPicker(){try{startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION),2);}catch(Exception e){status.setText(AppLanguage.text("Could not open the folder picker: ", "Não foi possível abrir o seletor de pastas: ")+e.getMessage());}}
 @Override protected void onResume(){super.onResume();if(directStart!=null)directStart.setChecked(BionicStartupSettings.enabled(this));if(waitingForSettings){waitingForSettings=false;if(access())continueSetup();else{pendingSetup=0;status.setText(AppLanguage.text("File access was not granted. Select the game to try again.", "Acesso aos arquivos não concedido. Você pode tentar novamente ao selecionar o jogo."));}}}
 @Override protected void onDestroy(){if(setupDialog!=null){setupDialog.dismiss();setupDialog=null;}super.onDestroy();}

 private void launch(){if(busy)return;if(!access()||!gameReady()){beginSetup(true);return;}
  try(BionicSession.Guard ignored=BionicSession.acquire(getFilesDir())){}catch(IOException e){status.setText(e.getMessage());return;}
  String path=getSharedPreferences("iss",0).getString("game_dir","");if(!new File(path,"eboot.bin").canRead()||!new File(path,"art").isDirectory()||!new File(path,"art").canRead()){status.setText(AppLanguage.text("Choose the inFAMOUS game folder containing eboot.bin and readable art/.","Escolha a pasta de inFAMOUS com eboot.bin e art/ acessível."));return;}
  setBusy(true);status.setText(AppLanguage.text("Preparing runtime…", "Preparando runtime…"));new Thread(()->{try{BionicRuntimeInstaller.install(this);runOnUiThread(()->{setBusy(false);refresh();startActivity(new Intent(this,BionicGameActivity.class).putExtra("game_dir",path));});}catch(Exception e){runOnUiThread(()->{setBusy(false);status.setText(e.toString());});}},"runtime-install").start();}
 private void setBusy(boolean value){busy=value;if(progress!=null)progress.setVisibility(value?android.view.View.VISIBLE:android.view.View.GONE);for(Button b:actions)b.setEnabled(!value);}
 private File saves(){return new File(getFilesDir(),"issport/user/savedata");}
 private void chooseSave(boolean importing){
  if(busy)return;
  try(BionicSession.Guard ignored=BionicSession.acquire(getFilesDir())){
   BionicSaveArchive.recover(saves());
   if(importing)BionicSaveArchive.readTitle(new File(getSharedPreferences("iss",0).getString("game_dir","")));
   else if(!BionicSaveArchive.hasSaves(saves()))throw new IOException(AppLanguage.text("There are no saves to export yet.", "Ainda não há saves para exportar."));
  }catch(Exception e){status.setText(e.getMessage());return;}
  Intent picker=new Intent(importing?Intent.ACTION_OPEN_DOCUMENT:Intent.ACTION_CREATE_DOCUMENT);
  picker.addCategory(Intent.CATEGORY_OPENABLE);
  if(importing)picker.setType("*/*").putExtra(Intent.EXTRA_MIME_TYPES,new String[]{"application/zip","application/x-zip-compressed","application/octet-stream"});
  else picker.setType("application/zip").putExtra(Intent.EXTRA_TITLE,"inFAMOUS-Saves-"+new java.text.SimpleDateFormat("yyyyMMdd-HHmmss",java.util.Locale.US).format(new java.util.Date())+".zip");
  setBusy(true);
  try{startActivityForResult(picker,importing?IMPORT_SAVE:EXPORT_SAVE);}catch(Exception e){setBusy(false);status.setText(e.getMessage());}
 }
 private void transferSave(Uri uri,boolean importing){
  setBusy(true);status.setText(importing?AppLanguage.text("Importing and validating saves…", "Importando e validando saves…"):AppLanguage.text("Exporting saves…", "Exportando saves…"));
  new Thread(()->{
   String message;
   try(BionicSession.Guard ignored=BionicSession.acquire(getFilesDir())){
    if(importing){
     String title=BionicSaveArchive.readTitle(new File(getSharedPreferences("iss",0).getString("game_dir","")));
     try(InputStream in=getContentResolver().openInputStream(uri)){
      if(in==null)throw new IOException(AppLanguage.text("Could not open the ZIP.", "Não foi possível abrir o ZIP."));
      int slots=BionicSaveArchive.importSaves(saves(),in,title);message=slots+(slots==1?AppLanguage.text(" save slot imported for "," salvamento importado para "):AppLanguage.text(" save slots imported for "," salvamentos importados para "))+title+".";
     }
    }else try(OutputStream out=getContentResolver().openOutputStream(uri,"wt")){
     if(out==null)throw new IOException(AppLanguage.text("Could not create the ZIP.", "Não foi possível criar o ZIP."));
     BionicSaveArchive.exportSaves(saves(),out);message=AppLanguage.text("Saves exported successfully.", "Saves exportados com sucesso.");
    }
   }catch(Exception e){message=AppLanguage.text("Transfer failed: ", "Falha na transferência: ")+e.getMessage();}
   final String result=message;runOnUiThread(()->{setBusy(false);status.setText(result);});
  },"iss-save-transfer").start();
 }
 @Override protected void onActivityResult(int req,int result,Intent data){super.onActivityResult(req,result,data);
  if(BionicDriverSettings.result(this,req,result,data))return;
  if(req==FramegenSettings.IMPORT_DLL){if(result==RESULT_OK&&data!=null&&data.getData()!=null)FramegenSettings.importDll(this,data.getData(),null);return;}
  if(pkg!=null&&pkg.result(req,result,data))return;
  if(req==IMPORT_SAVE||req==EXPORT_SAVE){
   if(result==RESULT_OK&&data!=null&&data.getData()!=null)transferSave(data.getData(),req==IMPORT_SAVE);
   else {setBusy(false);refresh();}return;
  }
  if(result!=RESULT_OK||data==null||data.getData()==null)return;
  try{if(req==3){try(InputStream in=new FileInputStream(new File(getFilesDir(),"iss.log"));OutputStream out=getContentResolver().openOutputStream(data.getData())){byte[] b=new byte[65536];int n;while((n=in.read(b))>0)out.write(b,0,n);}return;}
   if(req!=2)return;Uri uri=data.getData();String id=DocumentsContract.getTreeDocumentId(uri);int colon=id.indexOf(':');if(colon<0)throw new IOException(AppLanguage.text("Choose internal storage or an SD card.","Escolha o armazenamento interno ou um cartão SD."));
   String vol=id.substring(0,colon);File base="primary".equalsIgnoreCase(vol)?Environment.getExternalStorageDirectory():new File("/storage/"+vol);File dir=new File(base,id.substring(colon+1)).getCanonicalFile();
   if(!dir.toPath().startsWith(base.getCanonicalFile().toPath())||!new File(dir,"eboot.bin").canRead())throw new IOException(AppLanguage.text("No readable eboot.bin. Check permission and the selected folder.","eboot.bin inacessível. Verifique a permissão e a pasta escolhida."));
   if(!new File(dir,"art").isDirectory()||!new File(dir,"art").canRead())throw new IOException(AppLanguage.text("inFAMOUS game folder must contain accessible art/ resources.","A pasta de inFAMOUS deve conter os recursos art/ acessíveis."));
   try{getContentResolver().takePersistableUriPermission(uri,data.getFlags()&Intent.FLAG_GRANT_READ_URI_PERMISSION);}catch(SecurityException ignored){}
   getSharedPreferences("iss",0).edit().putString("game_dir",dir.getPath()).apply();refresh();
  }catch(Exception e){status.setText(e.toString());}}
 @Override public void onBackPressed(){if(busy){status.setText(AppLanguage.text("Wait for the operation to finish.", "Aguarde a operação terminar."));return;}super.onBackPressed();}
 @Override public void onRequestPermissionsResult(int req,String[] perms,int[] results){
  super.onRequestPermissionsResult(req,perms,results);if(req!=1)return;requestingAccess=false;
  if(access()){continueSetup();return;}
  if(Build.VERSION.SDK_INT>=30){
   new AlertDialog.Builder(this).setTitle(AppLanguage.text("File access", "Acesso aos arquivos"))
    .setMessage(AppLanguage.text("Allow file access to select the inFAMOUS Second Son folder or install a PKG. On the next screen, enable this app's permission.", "Permita o acesso aos arquivos para selecionar a pasta de inFAMOUS Second Son ou instalar um PKG. Na próxima tela, ative a permissão para este aplicativo."))
    .setPositiveButton(AppLanguage.text("Open settings", "Abrir configurações"),(d,w)->{waitingForSettings=true;try{startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,Uri.parse("package:"+getPackageName())));}catch(Exception e){waitingForSettings=false;pendingSetup=0;status.setText(AppLanguage.text("Open this app's permissions in Android settings.", "Abra as permissões do aplicativo nas configurações do Android."));}})
    .setNegativeButton(AppLanguage.text("Not now", "Agora não"),(d,w)->{pendingSetup=0;status.setText(AppLanguage.text("Permission was not granted. Select the game to try again.", "Permissão não concedida. Selecione o jogo para tentar novamente."));})
    .setOnCancelListener(d->{pendingSetup=0;}).show();
  }else{pendingSetup=0;status.setText(AppLanguage.text("File permission is required to select the game.", "Permissão de arquivos necessária para selecionar o jogo."));}
 }
}
