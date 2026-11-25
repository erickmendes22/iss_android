package ro.cobrabm.fexdroid;
import android.app.*;
import android.content.SharedPreferences;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;

/** One persisted setting for launcher and in-game UI; atomic publication to the presenter. */
final class BionicPerformanceSettings {
 static final int[] LIMITS={30,60,120,240};
 static int limit(SharedPreferences p){int n=p.getInt("fps_limit",60);for(int v:LIMITS)if(n==v)return n;return 60;}
 static String summary(Activity a){SharedPreferences p=a.getSharedPreferences("iss",0);return limit(p)+" FPS · VSync "+(p.getBoolean("vsync",true)?AppLanguage.text("on", "ligado"):AppLanguage.text("off", "desligado"));}
 static synchronized void write(Activity a)throws IOException{
  SharedPreferences p=a.getSharedPreferences("iss",0);File dir=new File(a.getFilesDir(),"issport");dir.mkdirs();File tmp=new File(dir,"performance.state.tmp");
  String data="fps_limit="+limit(p)+"\nvsync="+(p.getBoolean("vsync",true)?1:0)+"\nauto_quality="+(p.getBoolean("auto_quality",false)?1:0)+"\ncomplete=1\n";
  try(FileOutputStream out=new FileOutputStream(tmp)){out.write(data.getBytes(StandardCharsets.US_ASCII));out.getFD().sync();}
  if(!tmp.renameTo(new File(dir,"performance.state")))throw new IOException(AppLanguage.text("Could not apply FPS and VSync.", "Não foi possível aplicar FPS e VSync."));
 }
 static void show(Activity a,Runnable updated){
  SharedPreferences p=a.getSharedPreferences("iss",0);LinearLayout content=new LinearLayout(a);content.setOrientation(1);int pad=Math.round(20*a.getResources().getDisplayMetrics().density);content.setPadding(pad,pad,pad,pad);
  Switch sync=new Switch(a);sync.setText("VSync");sync.setChecked(p.getBoolean("vsync",true));content.addView(sync);
  Switch quality=new Switch(a);
  quality.setText(AppLanguage.text("Adaptive game quality", "Qualidade adaptativa do jogo"));
  quality.setChecked(p.getBoolean("auto_quality",false));
  content.addView(quality);
  TextView title=new TextView(a);title.setText(AppLanguage.text("FPS limit", "Limite de FPS"));title.setTextSize(18);title.setPadding(0,pad,0,8);content.addView(title);
  RadioGroup rates=new RadioGroup(a);for(int n:LIMITS){RadioButton b=new RadioButton(a);b.setId(android.view.View.generateViewId());b.setTag(n);b.setText(n+" FPS");rates.addView(b);if(n==limit(p))rates.check(b.getId());}content.addView(rates);
  TextView note=new TextView(a);note.setText(AppLanguage.text("This limits real game frames. Bloodframe multiplies the output. With VSync off, output does not throttle the game to the display refresh rate; visible frames still depend on the screen.", "O limite vale para quadros reais. Bloodframe multiplica a saída. Com VSync desligado, a saída não limita o jogo à frequência da tela; os quadros visíveis ainda dependem da tela."));content.addView(note);
  ScrollView scroll=new ScrollView(a);scroll.addView(content);
  AlertDialog dialog=new AlertDialog.Builder(a).setTitle(AppLanguage.text("FPS and synchronization", "FPS e sincronização")).setView(scroll).setNegativeButton(AppLanguage.text("Cancel", "Cancelar"),null).setPositiveButton(AppLanguage.text("Apply", "Aplicar"),null).create();
  dialog.setOnShowListener(v->dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(b->{int value=(Integer)rates.findViewById(rates.getCheckedRadioButtonId()).getTag();int old=limit(p);boolean oldQuality=p.getBoolean("auto_quality",false);boolean oldSync=p.getBoolean("vsync",true);p.edit().putBoolean("auto_quality",quality.isChecked()).putInt("fps_limit",value).putBoolean("vsync",sync.isChecked()).commit();try{write(a);if(a instanceof BionicGameActivity)((BionicGameActivity)a).updateDisplayRate();if(updated!=null)updated.run();dialog.dismiss();}catch(IOException e){p.edit().putBoolean("auto_quality",oldQuality).putInt("fps_limit",old).putBoolean("vsync",oldSync).commit();note.setText(e.getMessage());}}));dialog.show();
 }
}
