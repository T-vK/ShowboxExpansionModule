function channel(n) {
  const p = `INPUT${n}_`;
  return {
    title: `Input ${n}`,
    fields: [
      { id: `${p}GAIN`, label: "Gain", type: "FLOAT" },
      { id: `${p}VOLUME`, label: "Volume", type: "FLOAT" },
      { id: `${p}MUTE`, label: "Mute", type: "BOOL" },
      { id: `${p}EQ_ENABLE`, label: "EQ", type: "BOOL" },
      { id: `${p}EQ_LOW_GAIN`, label: "EQ low", type: "FLOAT" },
      { id: `${p}EQ_MID_GAIN`, label: "EQ mid", type: "FLOAT" },
      { id: `${p}EQ_HIGH_GAIN`, label: "EQ high", type: "FLOAT" },
      { id: `${p}COMPRESSOR_ENABLE`, label: "Compressor", type: "BOOL" },
      { id: `${p}COMPRESSOR_AMOUNT`, label: "Compressor amount", type: "FLOAT" },
      { id: `${p}EFFECT_1_MUTE`, label: "FX1 mute", type: "BOOL" },
      { id: `${p}EFFECT_1_AMOUNT`, label: "FX1 amount", type: "FLOAT" },
      { id: `${p}EFFECT_2_MUTE`, label: "FX2 mute", type: "BOOL" },
      { id: `${p}EFFECT_2_AMOUNT`, label: "FX2 amount", type: "FLOAT" },
      { id: `${p}EXT_FX_MUTE`, label: "Ext FX mute", type: "BOOL" },
      { id: `${p}EXT_FX_SENDS`, label: "Ext FX send", type: "FLOAT" },
      { id: `${p}CLIP_OL_PRE`, label: "Clip pre", type: "BOOL" },
      { id: `${p}CLIP_OL_POST`, label: "Clip post", type: "BOOL" }
    ]
  };
}

export const mixerSchema = [
  {
    title: "Main",
    fields: [
      { id: "MAIN_MUTE", label: "Mute", type: "BOOL" },
      { id: "MAIN_MASTER_GAIN", label: "Master", type: "FLOAT" },
      { id: "MAIN_HEADPHONE_GAIN", label: "Headphones", type: "FLOAT" },
      { id: "MAIN_CLIP_OL", label: "Clip", type: "BOOL" },
      { id: "FX_BYPASS", label: "FX bypass", type: "BOOL" },
      { id: "SELECTED_CHAN", label: "Selected channel", type: "UINT8" }
    ]
  },
  channel(1),
  channel(2),
  channel(3),
  channel(4),
  {
    title: "Stereo input",
    fields: [
      { id: "STEREO_INPUT1_VOLUME", label: "Volume", type: "FLOAT" },
      { id: "STEREO_INPUT1_MUTE", label: "Mute", type: "BOOL" },
      { id: "STEREO_INPUT1_EQ_ENABLE", label: "EQ", type: "BOOL" },
      { id: "STEREO_INPUT1_EQ_LOW_GAIN", label: "EQ low", type: "FLOAT" },
      { id: "STEREO_INPUT1_EQ_MID_GAIN", label: "EQ mid", type: "FLOAT" },
      { id: "STEREO_INPUT1_EQ_HIGH_GAIN", label: "EQ high", type: "FLOAT" },
      { id: "STEREO_INPUT1_CLIP_OL_PRE", label: "Clip pre", type: "BOOL" },
      { id: "STEREO_INPUT1_CLIP_OL_POST", label: "Clip post", type: "BOOL" }
    ]
  },
  {
    title: "Effects",
    fields: [
      { id: "EFFECT11_TYPEID", label: "FX 1-1 type", type: "UINT8" },
      { id: "EFFECT12_TYPEID", label: "FX 1-2 type", type: "UINT8" },
      { id: "EFFECT13_TYPEID", label: "FX 1-3 type", type: "UINT8" },
      { id: "EFFECT14_TYPEID", label: "FX 1-4 type", type: "UINT8" },
      { id: "EFFECT21_TYPEID", label: "FX 2-1 type", type: "UINT8" },
      { id: "EFFECT22_TYPEID", label: "FX 2-2 type", type: "UINT8" }
    ]
  },
  {
    title: "Looper",
    fields: [
      { id: "LOOPER_LEVEL", label: "Level", type: "FLOAT" },
      { id: "LOOPER_STATE", label: "State", type: "UINT8" }
    ]
  },
  {
    title: "System",
    fields: [
      { id: "FRONT_LED", label: "Front LED", type: "BOOL" },
      { id: "FEEDBACK_ELIM", label: "Feedback elim", type: "UINT8" },
      { id: "AMP_PA_MODE", label: "Amp/PA mode", type: "BOOL" },
      { id: "LOCATION_MODE", label: "Location mode", type: "BOOL" }
    ]
  }
];
