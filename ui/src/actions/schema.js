import { mixerSchema } from "../mixerSchema.js";

export const jacks = [["1t","TRS 1 tip"],["1r","TRS 1 ring"],["2t","TRS 2 tip"],["2r","TRS 2 ring"],["3t","TRS 3 tip"],["3r","TRS 3 ring"],["4t","TRS 4 tip"],["4r","TRS 4 ring"]];
export const gestures = [["press","Press"],["long","Long press"],["shortRelease","Short release"],["longRelease","Long release"],["double","Double press"]];
export const panelButtons = [["blue","Blue (Bluetooth)"],["green","Green (Wi-Fi)"],["red","Red (USB)"]];
export const midiMessages = [
  ["noteOn","Note on"],["noteOff","Note off"],["cc","Control change"],["pc","Program change"],
  ["pitchBend","Pitch bend"],["aftertouch","Channel aftertouch"],["polyAftertouch","Poly aftertouch"],
  ["clock","Clock"],["start","Start"],["continue","Continue"],["stop","Stop"],["reset","Reset"],
  ["raw","Raw / SysEx"]
];
export const midiPorts = [["din","DIN"],["ble","Bluetooth"],["apple","AppleMIDI"],["usb","USB device"],["usbhost","USB host"],["eth","Ethernet"]];
export const links = [["bleHost","Bluetooth MIDI host"],["blePeripheral","Bluetooth MIDI peripheral"],["usbMidi","USB MIDI"],["din","DIN MIDI"],["apple","AppleMIDI"]];
export const edges = [["up","Comes up"],["down","Goes down"]];
export const states = [["up","Is up"],["down","Is down"]];
export const held = [["down","Is held"],["up","Is released"]];
export const ops = [["eq","Equals"],["ne","Does not equal"],["gt","Greater than"],["lt","Less than"],["ge","At least"],["le","At most"]];
export const hidDevices = [["keyboard","Keyboard"],["mouse","Mouse"],["abs","Absolute pointer"],["touch","Touch"]];
export const hidOps = {
  keyboard: [["tap","Tap"],["down","Hold"],["up","Release"],["text","Type text"],["media","Media key"]],
  mouse: [["move","Move"],["click","Click"],["down","Button down"],["up","Button up"],["double","Double click"],["scroll","Scroll"]],
  abs: [["move","Move A→B"],["click","Click"],["down","Down"],["up","Up"],["tap","Tap"]],
  touch: [["tap","Tap"],["down","Down"],["up","Up"],["move","Move A→B"],["swipe","Swipe"],["pinch-in","Pinch in"],["pinch-out","Pinch out"]]
};
export const mediaKeys = [["playpause","Play/Pause"],["play","Play"],["pause","Pause"],["stop","Stop"],["next","Next"],["prev","Previous"],["mute","Mute"],["volup","Volume up"],["voldown","Volume down"],["fastforward","Fast forward"],["rewind","Rewind"],["record","Record"],["eject","Eject"],["brightnessup","Brightness up"],["brightnessdown","Brightness down"]];
export const mouseButtons = [["left","Left"],["right","Right"],["middle","Middle"],["none","None"]];
export const modifiers = [["ctrl","Ctrl"],["shift","Shift"],["alt","Alt"],["gui","Cmd/Win"]];
export const keys = ["a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z","1","2","3","4","5","6","7","8","9","0","enter","escape","backspace","tab","space","f1","f2","f3","f4","f5","f6","f7","f8","f9","f10","f11","f12","pageup","pagedown","home","end","insert","delete","up","down","left","right"];
export const triggerTypes = [
  ["trs","TRS footswitch"],["panel","Front-panel button"],["midi","MIDI message"],
  ["connect","Connection change"],["showbox","Showbox control"],["showboxPacket","Showbox packet"],
  ["variable","Variable change"]
];
export const conditionTypes = [
  ["and","All of"],["or","Any of"],["not","Not"],["showbox","Showbox control"],
  ["connection","Connection"],["trs","TRS footswitch"],["midiPort","MIDI port"],["variable","Variable"]
];
export const actionTypes = [
  ["midi","MIDI message"],["hid","HID"],["sleep","Wait"],["showbox","Showbox control"],
  ["showboxPacket","Showbox packet"],["looper","Looper"],["gainBoost","Gain boost"],
  ["variable","Variable"],["system","Module / radio"],["snapshot","Recall snapshot"],
  ["sdcard","SD record"],["tuner","Tuner"]
];
export const looperOps = [["cycle","Rec / play / overdub"],["stop","Stop"],["delete","Delete"],["press","Press"],["long","Long press"],["double","Double press"],["up","Release"]];
export const varOps = [["set","Set"],["toggle","Toggle"],["add","Add"]];
export const varScopes = [["global","Global"],["local","Local (this run)"]];
export const systemOps = [
  ["bluetoothToggle","Bluetooth on/off"],["bluetoothOn","Bluetooth on"],["bluetoothOff","Bluetooth off"],
  ["pairing","Bluetooth pairing window"],["wifiToggle","Wi-Fi on/off"],["wifiOn","Wi-Fi on"],["wifiOff","Wi-Fi off"],
  ["portal","Wi-Fi setup portal"],["usbCycle","USB host → device → JTAG"],["usbHost","USB host"],
  ["usbPeripheral","USB MIDI+HID device"],["usbJtag","USB serial/JTAG"]
];
export const entityGroups = mixerSchema;
export const entities = mixerSchema.flatMap((group) => group.fields.map((field) => field.id));

export function blankTrigger() {
  return { type: "midi", message: "cc", channel: "16", number: "", value: "", ports: ["ble"] };
}
export function blankAction() {
  return { type: "midi", message: "cc", channel: "1", number: "0", value: "127", ports: ["din","ble"] };
}
export function blankCondition() {
  return { type: "showbox", entity: "MAIN_MUTE", op: "eq", value: "false" };
}
export function blankGroup(op) {
  return { op: op || "and", items: [blankCondition()] };
}
