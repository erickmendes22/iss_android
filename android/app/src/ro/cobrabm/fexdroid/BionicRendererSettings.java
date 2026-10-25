package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.Map;

/** Reversible renderer changes, read only when a fresh game process starts. */
final class BionicRendererSettings {
 static void environment(SharedPreferences p,Map<String,String> env){
  final boolean precise = p.getBoolean("fix_smoke_powers_precise_readback", false);
  env.put("TLG_READBACKS", precise ? "2" : "1");
  final boolean hybrid = !precise && p.getBoolean("renderer_experimental_hybrid_readback",false);
  env.put("TLG_ISS_HYBRID_READ_PROTECT",hybrid ? "1" : "0");
  // Opt-in V6 experiment; when disabled this is exactly the confirmed V2 path.
  env.put("TLG_ISS_EAGER_BACKOFF_63",hybrid && p.getBoolean("renderer_hybrid_eager_backoff",false) ? "1" : "0");
  env.put("TLG_ISS_HYBRID_TARGETED_STAGE",
      hybrid && p.getBoolean("renderer_hybrid_targeted_staging",false) ? "1" : "0");
  // Real secondary Vulkan recording, separate from scan/hook and disabled by
  // default. This is not yet the PC's reusable-material native pass cache.
  final boolean nativeSecondary = p.getBoolean("renderer_native_passes_arm64_secondary",false);
  env.put("ISS_NATIVE_PASSES_ARM64_SECONDARY",nativeSecondary ? "1" : "0");
  env.put("ISS_NATIVE_PASSES_ARM64_COLOR_PASSES",
      nativeSecondary && p.getBoolean("renderer_native_passes_arm64_color_passes",false) ? "1" : "0");
  // Optional FEX guest draw observation only. Disabled unless explicitly enabled
  // through this separate setting, which is not an acceleration switch.
  // The FEX probe and PM4 scanner are diagnostics. Mixing them with Vulkan
  // secondary rendering obscures FPS and image regressions. Suppress both
  // whenever the rendering experiment is enabled.
  env.put("ISS_NATIVE_PASSES_ARM64_ENGINE_HOOK",
      !nativeSecondary && p.getBoolean("renderer_native_passes_arm64_guest_observe",false) ? "1" : "0");
  // Separate native-passes experimental branch. Scan-only until secondary
  // Vulkan recording and register compatibility have their own validation.
  env.put("ISS_NATIVE_PASSES_ARM64",
      !nativeSecondary && p.getBoolean("renderer_native_passes_arm64_scan",false) ? "1" : "0");
  // V2: use staged GPU->CPU copies for visibility instead of reading sparse
  // residency directly; prefetch recurring fault pages at fences to overlap waits.
  // Never apply this experiment to the proven Precise mode or plain Relaxed.
  if (hybrid) {
   env.put("TLG_MAPPED_READBACK","0");
   env.put("TLG_EAGER_READBACK","1");
  } else {
   env.put("TLG_MAPPED_READBACK","1");
   env.put("TLG_EAGER_READBACK","0");
  }
  // The PC deferred readback path moves the Precise GPU wait to guest fault handling.
  // Do not force eager downloads: they can increase synchronization cost.
  if (precise) env.put("TLG_DEFERRED_READBACK", "1");
  env.put("TLG_DYNAMIC_STRIDE",p.getBoolean("renderer_dynamic_stride",true)?"1":"0");
  env.put("TLG_OCCLUSION_MODE",p.getBoolean("renderer_real_occlusion",true)?"4":"3");
  env.put("TLG_GC_BATCH_DOWNLOAD",p.getBoolean("renderer_batch_gc",true)?"1":"0");
  env.put("ISS_VMA_BLOCK_MB",p.getBoolean("renderer_small_blocks",true)?"64":"0");
 }

 static String nativePassesSummary(Activity a) {
  boolean enabled=a.getSharedPreferences("iss",0)
    .getBoolean("renderer_native_passes_arm64_secondary",false);
  return enabled ? AppLanguage.text("Enabled", "Ativado")
                 : AppLanguage.text("Disabled", "Desativado");
 }
 static void showNativePasses(Activity a,Runnable onApplied) {
  SharedPreferences p=a.getSharedPreferences("iss",0);
  LinearLayout panel=new LinearLayout(a);
  panel.setOrientation(1);
  panel.setPadding(32,24,32,24);
  TextView explanation=new TextView(a);
  explanation.setText(AppLanguage.text(
    "Compare the same scene with secondary Vulkan rendering OFF and ON. Keep readback settings unchanged, and restart the game after each change. No FPS improvement or visual stability is confirmed yet on Adreno.",
    "Compare a mesma cena com renderização Vulkan secundária desligada e ligada. Mantenha o Readback igual e reinicie o jogo após cada alteração. Ainda não há ganho de FPS nem estabilidade visual confirmados no Adreno."));
  explanation.setTextSize(15);
  panel.addView(explanation);

  CheckBox secondary=new CheckBox(a);
  secondary.setText(AppLanguage.text(
    "Enable secondary Vulkan passes (experimental rendering)",
    "Ativar passes Vulkan secundários (renderização experimental)"));
  secondary.setChecked(p.getBoolean("renderer_native_passes_arm64_secondary",false));
  secondary.setPadding(0,24,0,20);
  panel.addView(secondary);
  CheckBox colorPasses=new CheckBox(a);
  colorPasses.setText(AppLanguage.text(
      "Unsafe: also use secondary Vulkan for color passes (may cause a black screen)",
      "Arriscado: usar Vulkan secundário também nos passes de cor (pode deixar a tela preta)"));
  colorPasses.setChecked(p.getBoolean("renderer_native_passes_arm64_color_passes",false));
  panel.addView(colorPasses);

  TextView note=new TextView(a);
  note.setText(AppLanguage.text(
    "Diagnostics only (leave OFF for FPS comparisons):",
    "Somente diagnóstico (deixe DESLIGADO ao comparar FPS):"));
  note.setTextSize(14);
  panel.addView(note);

  CheckBox scan=new CheckBox(a);
  scan.setText(AppLanguage.text("Analyze PM4 draws (no acceleration)",
                                 "Analisar draws PM4 (sem aceleração)"));
  scan.setChecked(p.getBoolean("renderer_native_passes_arm64_scan",false));
  panel.addView(scan);
  CheckBox fex=new CheckBox(a);
  fex.setText(AppLanguage.text(
    "Observe FEX drawIndex calls (may reduce FPS)",
    "Observar drawIndex do FEX (pode reduzir FPS)"));
  fex.setChecked(p.getBoolean("renderer_native_passes_arm64_guest_observe",false));
  panel.addView(fex);

  android.widget.ScrollView scroll=new android.widget.ScrollView(a);
  scroll.addView(panel);
  new AlertDialog.Builder(a)
   .setTitle(AppLanguage.text("Native Passes ARM64 — experimental",
                              "Native Passes ARM64 — experimental"))
   .setView(scroll)
   .setNegativeButton(AppLanguage.text("Cancel","Cancelar"),null)
   .setPositiveButton(AppLanguage.text("Apply","Aplicar"),(d,which)->{
    p.edit()
     .putBoolean("renderer_native_passes_arm64_secondary",secondary.isChecked())
     .putBoolean("renderer_native_passes_arm64_color_passes",colorPasses.isChecked())
     .putBoolean("renderer_native_passes_arm64_scan",scan.isChecked())
     .putBoolean("renderer_native_passes_arm64_guest_observe",fex.isChecked())
     .apply();
    if(onApplied!=null)onApplied.run();
   }).show();
 }
 static void show(Activity a){
  SharedPreferences p=a.getSharedPreferences("iss",0);
  LinearLayout panel=new LinearLayout(a);panel.setOrientation(1);panel.setPadding(24,12,24,12);
  TextView help=new TextView(a);help.setText(AppLanguage.text("Changes apply on next game start. Disable a correction to compare rendering and stability.","As alterações valem no próximo início do jogo. Desative uma correção para comparar imagem e estabilidade."));panel.addView(help);
  String[] keys={"renderer_dynamic_stride","renderer_real_occlusion","renderer_batch_gc","renderer_small_blocks"};
  String[] names={AppLanguage.text("Dynamic compute stride (smoke)","Stride dinâmico de compute (fumaça)"),AppLanguage.text("Real light occlusion","Oclusão real das luzes"),AppLanguage.text("Batched texture write-backs","Retorno de texturas em lote"),AppLanguage.text("Smaller memory blocks","Blocos menores de memória")};
  for(int i=0;i<keys.length;i++){final String key=keys[i];CheckBox control=new CheckBox(a);control.setText(names[i]);control.setChecked(p.getBoolean(key,true));control.setOnCheckedChangeListener((b,on)->p.edit().putBoolean(key,on).apply());panel.addView(control);}
  CheckBox nativePassScan=new CheckBox(a);
  nativePassScan.setText(AppLanguage.text(
    "Native Passes ARM64 (experimental PM4 analysis only; no acceleration yet)",
    "Native Passes ARM64 (análise experimental PM4; ainda sem aceleração)"));
  nativePassScan.setChecked(p.getBoolean("renderer_native_passes_arm64_scan",false));
  nativePassScan.setOnCheckedChangeListener((b,on)->
      p.edit().putBoolean("renderer_native_passes_arm64_scan",on).apply());
  panel.addView(nativePassScan);
  CheckBox nativeSecondary=new CheckBox(a);
  nativeSecondary.setText(AppLanguage.text(
    "Native Vulkan secondary passes (experimental; 1x MSAA, fallback for incompatible passes)",
    "Passes Vulkan secundários (experimental; 1x MSAA, fallback em passes incompatíveis)"));
  nativeSecondary.setChecked(p.getBoolean("renderer_native_passes_arm64_secondary",false));
  nativeSecondary.setOnCheckedChangeListener((b,on)->
      p.edit().putBoolean("renderer_native_passes_arm64_secondary",on).apply());
  panel.addView(nativeSecondary);
  CheckBox nativeGuestObserver=new CheckBox(a);
  nativeGuestObserver.setText(AppLanguage.text(
    "Native Passes FEX bridge (advanced observation only; may reduce FPS)",
    "Ponte Native Passes FEX (apenas observação avançada; pode reduzir FPS)"));
  nativeGuestObserver.setChecked(p.getBoolean("renderer_native_passes_arm64_guest_observe",false));
  nativeGuestObserver.setOnCheckedChangeListener((b,on)->
      p.edit().putBoolean("renderer_native_passes_arm64_guest_observe",on).apply());
  panel.addView(nativeGuestObserver);
  CheckBox hybrid=new CheckBox(a);
  hybrid.setText(AppLanguage.text("Experimental Relaxed + Precise read protection (smoke/powers; may reduce FPS)",
    "Experimental: Relaxed + proteção de leitura Precise (fumaça/poderes; pode reduzir FPS)"));
  hybrid.setChecked(p.getBoolean("renderer_experimental_hybrid_readback",false));
  hybrid.setOnCheckedChangeListener((b,on)->p.edit().putBoolean("renderer_experimental_hybrid_readback",on).apply());
  panel.addView(hybrid);
  new AlertDialog.Builder(a).setTitle(AppLanguage.text("Renderer corrections","Correções do renderer")).setView(panel).setPositiveButton(AppLanguage.text("Done","Concluir"),null).show();
 }
}
