import { ModeCard } from "./ServiceUi.jsx";
import { HidMix, usbModeOf, usbReboot, useServices } from "./services.jsx";

const MODE_LABEL = {
  jtag: "Serial / JTAG",
  midiHid: "MIDI + HID gadget",
  midiPeri: "MIDI gadget",
  hid: "HID gadget",
  midiHost: "MIDI host",
};

export function UsbPage() {
  const { data, error, setError, post, byId } = useServices();
  const mode = usbModeOf(data, byId);
  const usb = (data && data.usb) || {};
  const usbMix = (data && data.usbHidMix) || { keyboard: true, mouse: true, absPointer: true, digitizer: false };
  const usbHidOn = !!(byId.usbHid && byId.usbHid.enabled);
  const hostOn = mode === "midiHost";
  const gadgetOn = mode === "midiHid" || mode === "midiPeri" || mode === "hid";

  async function pick(next) {
    if (next === mode) return;
    if (next === "jtag") {
      if (!window.confirm(usbReboot.usbJtag[1])) return;
      try { await post("/api/v1/midi/settings?usbHid=0&usbMidiHost=0&usbMidiPeripheral=0"); }
      catch (err) { setError(err.message); }
      return;
    }
    if (next === "midiHid") {
      if (!window.confirm(usbReboot.usbMidiHid[0])) return;
      try {
        let qs = "usbMidiHost=0&usbMidiPeripheral=1&usbHid=1";
        if (data && data.debug && data.debug.usbSafeTest) qs = "usbSafeTest=1&" + qs;
        await post("/api/v1/midi/settings?" + qs);
      } catch (err) {
        setError(err.message);
      }
      return;
    }
    const id = next === "hid" ? "usbHid" : next === "midiHost" ? "usbMidiHost" : "usbMidiPeripheral";
    if (!window.confirm(usbReboot[id][0])) return;
    try {
      let qs = id + "=1";
      if (id === "usbHid") qs = "usbMidiPeripheral=0&usbMidiHost=0&" + qs;
      if (id === "usbMidiPeripheral") qs = "usbHid=0&usbMidiHost=0&" + qs;
      if (id === "usbMidiHost") qs = "usbHid=0&usbMidiPeripheral=0&" + qs;
      if (id === "usbHid" && data && data.debug && data.debug.usbSafeTest) qs = "usbSafeTest=1&" + qs;
      await post("/api/v1/midi/settings?" + qs);
    } catch (err) {
      setError(err.message);
    }
  }

  return (
    <div class="mgmt-page usb-page">
      <p id="error">{error}</p>

      <section class="usb-status-card">
        <h2>Port status</h2>
        <p class={"link-pop-state " + (hostOn && usb.connected ? "ok" : gadgetOn ? "ok" : "")}>
          {MODE_LABEL[mode] || mode}
        </p>
        {hostOn ? (
          <>
            <p>
              {usb.connected
                ? <>Controller: <strong>{usb.deviceName || "USB MIDI device"}</strong></>
                : "Waiting for a class-compliant USB MIDI keyboard on the module USB-C."}
            </p>
            <p class="meta">{usb.status || "—"}</p>
            {usb.connected ? (
              <p class="meta">
                MIDI in: {Number(usb.rxCount || 0)} bytes
                {Number(usb.rxCount) > 0 ? " · activity seen" : " · idle (hold a key / arpeggiator)"}
              </p>
            ) : (
              <p class="meta">
                Plug into the module USB-C (not the UART flash dongle). This board does not
                power hungry controllers — use a powered USB hub between module and keyboard.
              </p>
            )}
          </>
        ) : gadgetOn ? (
          <p class="meta">
            This computer should see ShowboxMod as a USB gadget. Native serial/JTAG is off —
            use the UART USB-serial adapter to flash.
          </p>
        ) : (
          <p class="meta">Native USB-C is serial/JTAG for flash and console.</p>
        )}
      </section>

      <p class="hint">
        One PHY on USB-C: Serial/JTAG <em>or</em> MIDI host <em>or</em> MIDI/HID gadget.
        MIDI+HID gadget can share the port. Host mode talks to a keyboard plugged into the
        module. Changing mode reboots.
      </p>

      <div class="mode-grid">
        <ModeCard
          title="Serial / JTAG"
          info="Flash, serial console, and debugging on native USB-C."
          on={mode === "jtag"}
          danger
          onSelect={() => pick("jtag")}
        />
        <ModeCard
          title="MIDI host"
          info="Plug a USB MIDI keyboard or controller into ShowboxMod."
          on={mode === "midiHost"}
          onSelect={() => pick("midiHost")}
        />
        <ModeCard
          title="MIDI + HID"
          info="ShowboxMod appears to a computer as USB MIDI and keyboard/mouse."
          on={mode === "midiHid"}
          onSelect={() => pick("midiHid")}
        />
        <ModeCard
          title="MIDI only"
          info="USB MIDI gadget without HID."
          on={mode === "midiPeri"}
          onSelect={() => pick("midiPeri")}
        />
        <ModeCard
          title="HID only"
          info="Keyboard, mouse, pointer, or touch over USB."
          on={mode === "hid"}
          onSelect={() => pick("hid")}
        />
      </div>

      {(mode === "midiHid" || mode === "hid") ? (
        <section>
          <h2>USB HID layout</h2>
          <p class="hint">Applied on reboot or unplug/replug. Only used in HID gadget modes.</p>
          <HidMix mix={usbMix} prefix="usbHid" usbHidOn={usbHidOn} post={post} setError={setError} />
        </section>
      ) : null}
    </div>
  );
}
