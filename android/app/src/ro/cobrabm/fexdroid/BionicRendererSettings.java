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
  // The UI label "Precise 2" intentionally executes the Hybrid V6 path.
  // Never select real Readbacks=2; treat older Precise preferences as Hybrid.
  env.put("TLG_READBACKS", "1");
  final boolean hybrid = p.getBoolean("renderer_experimental_hybrid_readback",false)
      || p.getBoolean("fix_smoke_powers_precise_readback",false);
  env.put("TLG_ISS_HYBRID_READ_PROTECT",hybrid ? "1" : "0");
  // Opt-in V6 experiment; when disabled this is exactly the confirmed V2 path.
  env.put("TLG_ISS_EAGER_BACKOFF_63",hybrid && p.getBoolean("renderer_hybrid_eager_backoff",false) ? "1" : "0");
  env.put("TLG_ISS_HYBRID_TARGETED_STAGE",
      hybrid && p.getBoolean("renderer_hybrid_targeted_staging",false) ? "1" : "0");
  // V2: use staged GPU->CPU copies for visibility instead of reading sparse
  // residency directly; prefetch recurring fault pages at fences to overlap waits.
  // Apply these parameters only to Hybrid; plain Relaxed is unchanged.
  if (hybrid) {
   env.put("TLG_MAPPED_READBACK","0");
   env.put("TLG_EAGER_READBACK","1");
  } else {
   env.put("TLG_MAPPED_READBACK","1");
   env.put("TLG_EAGER_READBACK","0");
  }

  env.put("TLG_DYNAMIC_STRIDE",p.getBoolean("renderer_dynamic_stride",true)?"1":"0");
  env.put("TLG_OCCLUSION_MODE",p.getBoolean("renderer_real_occlusion",true)?"4":"3");
  env.put("TLG_GC_BATCH_DOWNLOAD",p.getBoolean("renderer_batch_gc",true)?"1":"0");
  env.put("ISS_VMA_BLOCK_MB",p.getBoolean("renderer_small_blocks",true)?"64":"0");
  // Vulkan recording fast paths. Compare same scene and driver with these OFF/ON.
  env.put("ISS_STAGE_BIND_MEMO",p.getBoolean("renderer_stage_bind_memo",true)?"1":"0");
  env.put("ISS_FAST_PIPELINE_BIND",p.getBoolean("renderer_fast_pipeline_bind",true)?"1":"0");
  env.put("ISS_FAST_DRAW_KEY",p.getBoolean("renderer_fast_draw_key",false)?"1":"0");
  env.put("TLG_DESCRIPTOR_REPLAY",p.getBoolean("renderer_descriptor_replay",true)?"1":"0");
  env.put("TLG_DESCRIPTOR_DELTA",p.getBoolean("renderer_descriptor_delta",false)?"1":"0");
  // Depth-only secondary Vulkan scopes: no guest-engine PM4 bypass yet.
  // Color-attachment secondaries previously produced missing output; never enable
  // them from the ordinary Android UI.
  env.put("ISS_NATIVE_PASSES_ARM64_SECONDARY",
      p.getBoolean("renderer_native_secondary_depth",false)?"1":"0");
  env.put("ISS_NATIVE_PASSES_ARM64_COLOR_PASSES","0");
 }
 static void show(Activity a){
  SharedPreferences p=a.getSharedPreferences("iss",0);
  LinearLayout panel=new LinearLayout(a);panel.setOrientation(1);panel.setPadding(24,12,24,12);
  TextView help=new TextView(a);help.setText(AppLanguage.text("Changes apply on next game start. Disable a correction to compare rendering and stability.","As alterações valem no próximo início do jogo. Desative uma correção para comparar imagem e estabilidade."));panel.addView(help);
  String[] keys={"renderer_dynamic_stride","renderer_real_occlusion","renderer_batch_gc","renderer_small_blocks"};
  String[] names={AppLanguage.text("Dynamic compute stride (smoke)","Stride dinâmico de compute (fumaça)"),AppLanguage.text("Real light occlusion","Oclusão real das luzes"),AppLanguage.text("Batched texture write-backs","Gravação de texturas em lote"),AppLanguage.text("Smaller memory blocks","Blocos menores de memória")};
  for(int i=0;i<keys.length;i++){final String key=keys[i];CheckBox control=new CheckBox(a);control.setText(names[i]);control.setChecked(p.getBoolean(key,true));control.setOnCheckedChangeListener((b,on)->p.edit().putBoolean(key,on).apply());panel.addView(control);}
  CheckBox stage=new CheckBox(a);
  stage.setText(AppLanguage.text("Stage binding cache without prep workers","Cache de recursos dos estágios sem workers de preparo"));
  stage.setChecked(p.getBoolean("renderer_stage_bind_memo",true));
  stage.setOnCheckedChangeListener((b,on)->p.edit().putBoolean("renderer_stage_bind_memo",on).apply());
  panel.addView(stage);
  CheckBox fastKey=new CheckBox(a);
  fastKey.setText(AppLanguage.text("Fast Draw: reuse shader pipeline selection (experimental)","Fast Draw: reutilizar seleção de shaders (experimental)"));
  fastKey.setChecked(p.getBoolean("renderer_fast_draw_key",false));
  fastKey.setOnCheckedChangeListener((b,on)->p.edit().putBoolean("renderer_fast_draw_key",on).apply());
  panel.addView(fastKey);
  CheckBox fastDraw=new CheckBox(a);
  fastDraw.setText(AppLanguage.text("Fast Draw: reuse identical pipeline binds","Fast Draw: reutilizar bindings de pipeline idênticos"));
  fastDraw.setChecked(p.getBoolean("renderer_fast_pipeline_bind",true));
  fastDraw.setOnCheckedChangeListener((b,on)->p.edit().putBoolean("renderer_fast_pipeline_bind",on).apply());
  panel.addView(fastDraw);
  CheckBox replay=new CheckBox(a);
  replay.setText(AppLanguage.text("Reuse identical Vulkan descriptors","Reutilizar descritores Vulkan idênticos"));
  replay.setChecked(p.getBoolean("renderer_descriptor_replay",true));
  replay.setOnCheckedChangeListener((b,on)->p.edit().putBoolean("renderer_descriptor_replay",on).apply());
  panel.addView(replay);
  CheckBox delta=new CheckBox(a);
  delta.setText(AppLanguage.text("Partial descriptor updates (experimental)","Atualizações parciais de descritores (experimental)"));
  delta.setChecked(p.getBoolean("renderer_descriptor_delta",false));
  delta.setOnCheckedChangeListener((b,on)->p.edit().putBoolean("renderer_descriptor_delta",on).apply());
  panel.addView(delta);
  CheckBox secondary=new CheckBox(a);
  secondary.setText(AppLanguage.text("Vulkan secondary passes: depth only (experimental)","Passes Vulkan secundários: apenas profundidade (experimental)"));
  secondary.setChecked(p.getBoolean("renderer_native_secondary_depth",false));
  secondary.setOnCheckedChangeListener((b,on)->p.edit().putBoolean("renderer_native_secondary_depth",on).apply());
  panel.addView(secondary);
  new AlertDialog.Builder(a).setTitle(AppLanguage.text("Renderer corrections","Correções da renderização")).setView(panel).setPositiveButton(AppLanguage.text("Done","Concluir"),null).show();
 }
}
