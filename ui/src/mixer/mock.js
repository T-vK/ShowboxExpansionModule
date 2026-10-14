export function mockEntities() {
  const next = {};
  function set(id, value) {
    next[id] = { id, value };
  }
  for (let n = 1; n <= 4; n++) {
    set(`INPUT${n}_VOLUME`, n === 3 ? -12 : -80 + n * 8);
    set(`INPUT${n}_GAIN`, 8 + n * 6);
    set(`INPUT${n}_MUTE`, false);
    set(`INPUT${n}_EQ_ENABLE`, n !== 2);
    set(`INPUT${n}_EQ_LOW_GAIN`, n === 1 ? 4 : -2);
    set(`INPUT${n}_EQ_MID_GAIN`, n === 3 ? 3 : -1);
    set(`INPUT${n}_EQ_HIGH_GAIN`, n === 4 ? 6 : 2);
    set(`INPUT${n}_COMPRESSOR_ENABLE`, n === 1);
    set(`INPUT${n}_COMPRESSOR_AMOUNT`, n === 1 ? 0.51 : 0.2);
    set(`INPUT${n}_EFFECT_1_MUTE`, n === 2);
    set(`INPUT${n}_EFFECT_1_AMOUNT`, n === 2 ? 0 : 0.42);
    set(`INPUT${n}_EFFECT_2_MUTE`, false);
    set(`INPUT${n}_EFFECT_2_AMOUNT`, 0.28);
    set(`INPUT${n}_EXT_FX_MUTE`, true);
    set(`INPUT${n}_EXT_FX_SENDS`, 0.12);
    set(`INPUT${n}_CLIP_OL_PRE`, false);
    set(`INPUT${n}_CLIP_OL_POST`, false);
  }
  set("EFFECT11_TYPEID", 1);
  set("EFFECT12_TYPEID", 0);
  set("EFFECT13_TYPEID", 7);
  set("EFFECT14_TYPEID", 3);
  set("EFFECT21_TYPEID", 2);
  set("EFFECT22_TYPEID", 5);
  set("MAIN_MASTER_GAIN", -6);
  set("MAIN_MUTE", false);
  set("MAIN_CLIP_OL", false);
  set("MAIN_HEADPHONE_GAIN", 0);
  set("STEREO_INPUT1_VOLUME", -18);
  set("STEREO_INPUT1_MUTE", false);
  set("STEREO_INPUT1_EQ_ENABLE", true);
  set("STEREO_INPUT1_EQ_LOW_GAIN", -3);
  set("STEREO_INPUT1_EQ_MID_GAIN", 1);
  set("STEREO_INPUT1_EQ_HIGH_GAIN", 4);
  set("LOOPER_LEVEL", 1);
  set("LOOPER_STATE", 2);
  set("AMP_PA_MODE", false);
  set("LOCATION_MODE", false);
  set("FRONT_LED", true);
  set("FEEDBACK_ELIM", 0);
  set("FX_BYPASS", false);
  set("SELECTED_CHAN", 0);
  if (typeof location !== "undefined") {
    try {
      const q = new URLSearchParams(location.search);
      const low = q.get("low");
      const mid = q.get("mid");
      const high = q.get("high");
      if (low != null) for (let n = 1; n <= 4; n++) set(`INPUT${n}_EQ_LOW_GAIN`, Number(low));
      if (mid != null) for (let n = 1; n <= 4; n++) set(`INPUT${n}_EQ_MID_GAIN`, Number(mid));
      if (high != null) for (let n = 1; n <= 4; n++) set(`INPUT${n}_EQ_HIGH_GAIN`, Number(high));
    } catch { /* ignore */ }
  }
  return next;
}
