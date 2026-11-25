package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.CheckBox;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;

/** Relaxed or Hybrid V6. The Hybrid uses the legacy Precise 2 label only. */
final class BionicReadbackSettings {
 static final String KEY = "fix_smoke_powers_precise_readback";
 static final String HYBRID_KEY = "renderer_experimental_hybrid_readback";
 static final String BACKOFF_KEY = "renderer_hybrid_eager_backoff";
 static final String TARGETED_KEY = "renderer_hybrid_targeted_staging";

 static int mode(SharedPreferences preferences) {
  // Old Precise installations migrate to Hybrid behavior, never Precise GPU mode.
  return preferences.getBoolean(HYBRID_KEY, false) || preferences.getBoolean(KEY, false) ? 1 : 0;
 }
 static boolean enabled(Activity activity) {
  return mode(activity.getSharedPreferences("iss",0)) == 1;
 }
 static String summary(Activity activity) {
  switch (mode(activity.getSharedPreferences("iss",0))) {
   case 1: return "Precise 2";
   default: return "Relaxed 1";
  }
 }
 static void show(Activity activity, Runnable refreshed) {
  SharedPreferences preferences = activity.getSharedPreferences("iss",0);
  LinearLayout body = new LinearLayout(activity);
  body.setOrientation(LinearLayout.VERTICAL);
  int pad = Math.round(18 * activity.getResources().getDisplayMetrics().density);
  body.setPadding(pad,pad,pad,pad);

  RadioGroup choices = new RadioGroup(activity);
  choices.setOrientation(RadioGroup.VERTICAL);
  String[] labels = {
   AppLanguage.text("Relaxed 1 — faster; smoke/powers may be incorrect",
                    "Relaxed 1 — mais rápido; fumaça/poderes podem falhar"),
   AppLanguage.text("Precise 2 — correct smoke/powers; major FPS impact",
                    "Precise 2 — corrige fumaça/poderes; grande queda de FPS")
  };
  int[] ids = new int[labels.length];
  for (int i=0;i<labels.length;i++) {
   RadioButton choice = new RadioButton(activity);
   ids[i] = View.generateViewId();
   choice.setId(ids[i]);
   choice.setText(labels[i]);
   choice.setTextSize(15);
   choice.setPadding(0,pad/2,0,pad/2);
   choices.addView(choice);
  }
  choices.check(ids[mode(preferences)]);
  body.addView(choices);

  CheckBox optimized = new CheckBox(activity);
  optimized.setText(AppLanguage.text(
      "Experimental: reduce unused GPU fence copies (backoff 63 instead of 7)",
      "Experimental: reduzir cópias GPU não utilizadas (backoff 63 em vez de 7)"));
  optimized.setChecked(preferences.getBoolean(BACKOFF_KEY,true));
  optimized.setPadding(0,pad/2,0,pad/2);
  body.addView(optimized);
  CheckBox targeted = new CheckBox(activity);
  targeted.setText(AppLanguage.text(
      "Performance experiment: demand-driven staged readbacks",
      "Teste de desempenho: readbacks seletivos por páginas solicitadas"));
  targeted.setChecked(preferences.getBoolean(TARGETED_KEY,true));
  targeted.setPadding(0,pad/2,0,pad/2);
  body.addView(targeted);
  int initialVisibility=choices.getCheckedRadioButtonId()==ids[1] ? View.VISIBLE : View.GONE;
  optimized.setVisibility(initialVisibility);
  targeted.setVisibility(initialVisibility);
  choices.setOnCheckedChangeListener((group,selectedId)->{
      int visibility=selectedId==ids[1] ? View.VISIBLE : View.GONE;
      optimized.setVisibility(visibility);
      targeted.setVisibility(visibility);
  });

  TextView help = new TextView(activity);
  help.setText(AppLanguage.text(
   "Restart the game to apply the selected Readback mode.",
   "Reinicie o jogo para aplicar o modo Readback escolhido."));
  help.setPadding(0,pad,0,0);
  body.addView(help);

  new AlertDialog.Builder(activity)
   .setTitle(AppLanguage.text("Smoke and powers — Readback mode",
                              "Fumaça e poderes — modo Readback"))
   .setView(body)
   .setNegativeButton(AppLanguage.text("Cancel","Cancelar"),null)
   .setPositiveButton(AppLanguage.text("Apply","Aplicar"),(dialog,which)->{
    int selected = choices.getCheckedRadioButtonId();
    int choice = selected == ids[1] ? 1 : 0;
    preferences.edit().putBoolean(KEY,false)
     .putBoolean(HYBRID_KEY,choice==1)
     .putBoolean(BACKOFF_KEY,optimized.isChecked())
     .putBoolean(TARGETED_KEY,targeted.isChecked()).apply();
    if (refreshed!=null) refreshed.run();
   }).show();
 }
}
