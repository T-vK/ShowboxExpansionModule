/** Official Showbox FX1 types (per-channel EFFECT1n_TYPEID). Reverse delay and acoustic sim are CH2/CH4 only. */
export const FX1 = [
  { id: 0, name: "None", short: "None" },
  { id: 1, name: "Small chorus", short: "Chorus" },
  { id: 2, name: "Large chorus", short: "Chorus+" },
  { id: 3, name: "Fast delay", short: "Delay" },
  { id: 4, name: "Slow delay", short: "Delay+" },
  { id: 5, name: "Delay + chorus", short: "Dly+Cho" },
  { id: 6, name: "Reverse delay", short: "Reverse", inst: true },
  { id: 7, name: "Overdrive", short: "Drive" },
  { id: 8, name: "Fuzz", short: "Fuzz" },
  { id: 9, name: "Crunch", short: "Crunch" },
  { id: 10, name: "Flanger", short: "Flanger" },
  { id: 11, name: "Phaser", short: "Phaser" },
  { id: 12, name: "Tube model", short: "Tube" },
  { id: 13, name: "Acoustic guitar sim", short: "Acoustic", inst: true },
  { id: 14, name: "Tremolo", short: "Tremolo" }
];

/** Official Showbox FX2 types. EFFECT21 is shared by CH1+CH3; EFFECT22 by CH2+CH4. Reverse delay is CH2/CH4 only. */
export const FX2 = [
  { id: 0, name: "None", short: "None" },
  { id: 1, name: "Small plate", short: "Plate" },
  { id: 2, name: "Large plate", short: "Plate+" },
  { id: 3, name: "Small room", short: "Room" },
  { id: 4, name: "Large room", short: "Room+" },
  { id: 5, name: "Small hall", short: "Hall" },
  { id: 6, name: "Large hall", short: "Hall+" },
  { id: 7, name: "Spring", short: "Spring" },
  { id: 8, name: "Small chorus", short: "Chorus" },
  { id: 9, name: "Large chorus", short: "Chorus+" },
  { id: 10, name: "Fast delay", short: "Delay" },
  { id: 11, name: "Slow delay", short: "Delay+" },
  { id: 12, name: "Delay + reverb", short: "Dly+Rvb" },
  { id: 13, name: "Delay + chorus", short: "Dly+Cho" },
  { id: 14, name: "Reverse delay", short: "Reverse", inst: true },
  { id: 15, name: "Flanger", short: "Flanger" },
  { id: 16, name: "Phaser", short: "Phaser" },
  { id: 17, name: "Tremolo", short: "Tremolo" }
];

export function fxOptions(list, channel) {
  const inst = channel === 2 || channel === 4;
  return list
    .filter((row) => inst || !row.inst)
    .map((row) => [row.id, row.name]);
}

export function fxShort(list, id) {
  const n = Number(id);
  const row = list.find((item) => item.id === n);
  return row ? row.short : Number.isFinite(n) ? `Type ${n}` : "—";
}

export function fxName(list, id) {
  const n = Number(id);
  const row = list.find((item) => item.id === n);
  return row ? row.name : Number.isFinite(n) ? `Type ${n}` : "—";
}

export function fx1Id(channel) {
  return `EFFECT1${channel}_TYPEID`;
}

export function fx2Id(channel) {
  return channel === 1 || channel === 3 ? "EFFECT21_TYPEID" : "EFFECT22_TYPEID";
}
