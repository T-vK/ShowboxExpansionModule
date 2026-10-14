import { ModeCard } from "./ServiceUi.jsx";
import { HidMix, usbModeOf, usbReboot, useServices } from "./services.jsx";

export function UsbPage() {
  const { data, error, setError, post, byId } = useServices();
  const mode = usbModeOf(data, byId);
  const usbMix = (data && data.usbHidMix) || { keyboard: true, mouse: true, absPointer: true, digitizer: false };
  const usbHidOn = !!(byId.usbHid && byId.usbHid.enabled);

  async function pick(next) {
    if (next === mode) return;
    if (next === "jtag") {
      if (!window.confirm(usbReboot.usbJtag[1])) return;
      try { await post("/api/v1/midi/settings?usbHid=0&usbMidiHost=0&usbMidiPeripheral=0"); }
      catch (err) { setError(err.message); }
      return;
    }
    const id = next === "hid" ? "usbHid" : next === "midiHost" ? "usbMidiHost" : "usbMidiPeripheral";
    if (!window.confirm(usbReboot[id][0])) return;
    try {
      let qs = id + "=1";
      if (id === "usbHid" && data && data.debug && data.debug.usbSafeTest) qs = "usbSafeTest=1&" + qs;
      await post("/api/v1/midi/settings?" + qs);
    } catch (err) {
      setError(err.message);
    }
  }

  return (
    <div class="mgmt-page">
      <p id="error">{error}</p>
      <p class="hint">This USB-C port can only be one thing at a time. Pick a mode; changing MIDI or HID reboots.</p>
      <div class="mode-grid">
        <ModeCard
          title="Serial / JTAG"
          info="Default. Flash, serial console, and debugging."
          on={mode === "jtag"}
          danger
          onSelect={() => pick("jtag")}
        />
        <ModeCard
          title="MIDI device"
          info="ShowboxMod appears as a USB MIDI gadget."
          on={mode === "midiPeri"}
          onSelect={() => pick("midiPeri")}
        />
        <ModeCard
          title="HID device"
          info="Keyboard, mouse, pointer, or touch over USB."
          on={mode === "hid"}
          onSelect={() => pick("hid")}
        />
        <ModeCard
          title="MIDI host"
          info="Plug a USB MIDI controller into this port."
          on={mode === "midiHost"}
          onSelect={() => pick("midiHost")}
        />
      </div>
      <section>
        <h2>USB HID layout</h2>
        <p class="hint">One profile for whoever is on the cable. Applied on reboot or unplug/replug.</p>
        <HidMix mix={usbMix} prefix="usbHid" usbHidOn={usbHidOn} post={post} setError={setError} />
      </section>
    </div>
  );
}
