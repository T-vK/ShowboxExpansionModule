import { ServiceNode } from "./ServiceUi.jsx";
import { useServices } from "./services.jsx";
import { PwaInstallBanner } from "../pwa/Install.jsx";

export function SystemPage() {
  const { data, error, hideUnsupported, setHide, transport, toggle, post, setError } = useServices();
  const bluetoothOn = data ? data.bluetoothRadio !== false : true;
  const wifiRadioOn = data ? data.wifiRadio !== false : true;
  const httpsOn = data ? !!data.https : false;
  const httpsListening = !!(data && data.httpsListening);
  const httpsHasCa = !!(data && data.httpsHasCa);
  const periOn = !!(transport("bleHid").enabled || transport("bleMidiPeripheral").enabled);
  const hostOn = !!transport("bleMidiHost").enabled;
  const link = (data && data.showboxLink) || {};
  const linkOk = !!link.ok;
  const agePart = (label, ms) => (ms != null && ms < 0xFFFFFF ? (" · " + label + " " + ms + " ms") : "");
  const linkLabel = !data ? "Mixer ↔ base: checking…"
    : linkOk ? ("Mixer ↔ base: live" + agePart("base", link.baseAgeMs) + agePart("mixer", link.mixerAgeMs) + (data.showboxBattery != null ? " · battery " + data.showboxBattery + "%" : ""))
    : ("Mixer ↔ base: not talking" + agePart("base", link.baseAgeMs) + agePart("mixer", link.mixerAgeMs));
  const bleHid = transport("bleHid");
  const bleMidiP = transport("bleMidiPeripheral");
  const usbJtag = !!(data && data.usbJtag);
  const classic = transport("bleClassic");

  return (
    <div class="mgmt-page">
      <p id="error">{error}</p>
      <PwaInstallBanner />
      <p class={"hint link-status" + (linkOk ? " ok" : " bad")}>{linkLabel}</p>
      <p class="toolbar">
        <label>
          <input type="checkbox" checked={hideUnsupported} onChange={(event) => setHide(event.target.checked)} />
          Hide unsupported
        </label>
      </p>
      <div class="svc-tree">
        <ServiceNode
          title="Bluetooth"
          info="ESP32-S3 Low Energy radio. Off reboots. Remembered hosts and pedals stay saved."
          href="/bluetooth"
          checked={bluetoothOn}
          onChange={(event) => toggle("bluetooth", event)}
        >
          {hideUnsupported || classic.available === false ? null : (
            <ServiceNode
              title="MIDI over Serial"
              info="The ESP32-S3 has no Bluetooth Classic radio."
              available={false}
              checked={false}
            />
          )}
          <ServiceNode
            title="Low Energy peripheral"
            info="Phones and computers that connect in to ShowboxMod."
            href="/bluetooth"
            checked={periOn}
            onChange={(event) => {
              const on = event.target.checked ? "1" : "0";
              post("/api/v1/midi/settings?bleHid=" + on + "&bleMidiPeripheral=" + on).catch((err) => {
                event.target.checked = !event.target.checked;
                setError(err.message);
              });
            }}
          >
            <ServiceNode
              title="HID"
              info={bleHid.reason || "Keyboard, mouse, absolute pointer, and touch on the ShowboxMod name."}
              href="/bluetooth"
              checked={!!bleHid.enabled}
              available={bleHid.available !== false}
              onChange={(event) => toggle("bleHid", event)}
            />
            <ServiceNode
              title="MIDI"
              info={bleMidiP.reason || "Advertise BLE MIDI on the same ShowboxMod name as HID."}
              href="/midi"
              checked={!!bleMidiP.enabled}
              available={bleMidiP.available !== false}
              onChange={(event) => toggle("bleMidiPeripheral", event)}
            />
            <ServiceNode
              title="Accept new hosts"
              info="Advertising stops once a computer or phone is connected so mixer↔base stays stable. Press the blue button for a 30 second pairing window, or turn this on here for 90 seconds."
              href="/bluetooth"
              checked={!!(data && data.acceptNewHosts)}
              onChange={(event) => post("/api/v1/midi/settings?acceptNewHosts=" + (event.target.checked ? "1" : "0")).catch((err) => { event.target.checked = !event.target.checked; setError(err.message); })}
            />
          </ServiceNode>
          <ServiceNode
            title="Low Energy host"
            info={data && data.scanHeld
              ? "Discovery scans wait while a host is live. Remembered pedals still connect by address into free peripheral slots."
              : "Connect out to BLE MIDI devices such as the AIRSTEP."}
            href="/bluetooth"
            checked={hostOn}
            onChange={(event) => toggle("bleMidiHost", event)}
          />
        </ServiceNode>

        <ServiceNode
          title="Network"
          info="Wi-Fi station for this page, OTA, and mDNS. Ethernet keeps working if Wi-Fi is off."
          href="/network"
          checked={wifiRadioOn}
          onChange={(event) => toggle("wifiRadio", event)}
        >
          <ServiceNode
            title="Ethernet"
            info={transport("ethernet").reason || "W5500 link. MIDI sessions use UDP 5004/5005."}
            href="/network"
            checked={!!transport("ethernet").enabled}
            available={transport("ethernet").available !== false}
            note={transport("ethernet").note}
            onChange={transport("ethernet").available === false ? undefined : (event) => toggle("ethernet", event)}
          />
          <ServiceNode
            title="RTP-MIDI"
            info={transport("wifi").reason || "AppleMIDI / RTP-MIDI on UDP 5004. Name AppleMidi."}
            href="/midi"
            checked={!!transport("wifi").enabled}
            available={transport("wifi").available !== false}
            onChange={(event) => toggle("wifi", event)}
          />
          <ServiceNode
            title="HTTPS"
            info="Optional TLS on port 443 for a trusted PWA install. Off by default. Enabling it can stall Wi-Fi while Bluetooth is up. HTTP on port 80 stays up — there is no redirect."
            href="/network"
            checked={httpsOn}
            note={httpsOn
              ? (httpsListening ? "https://showbox.local/" : "Starting…")
              : "Off — HTTP on port 80 stays up."}
            onChange={(event) => toggle("https", event)}
          >
            {httpsHasCa ? (
              <p class="hint https-ca">
                <a href="/api/v1/https/ca.pem" download="showbox-ca.pem">Download device CA</a>
                {" "}then open https://showbox.local/ after the browser trusts it.
              </p>
            ) : null}
          </ServiceNode>
          {hideUnsupported ? null : (
            <>
              <ServiceNode title="mDNS" info="Advertises showbox.local whenever an IP is up. Answers Android one-shot mDNS (legacy unicast). If a phone still fails, use the STA IP or a router DNS override showbox → that IP; keep Private DNS off." checked available={false} />
              <ServiceNode title="DHCP client" info="Gets the station address from the access point. Always on with Wi-Fi." checked available={false} />
              <ServiceNode title="OTA" info="Arduino OTA on port 3232. Flash still works over USB serial/JTAG and the UART connector." checked available={false} danger />
            </>
          )}
        </ServiceNode>

        <ServiceNode title="USB" info="This USB-C port is either serial/JTAG, MIDI, or HID. Changing MIDI or HID reboots." href="/usb" defaultOpen>
          <ServiceNode title="Peripheral" href="/usb" defaultOpen>
            <ServiceNode
              title="JTAG/Serial"
              info="Default USB port. Turning on MIDI or HID takes this away until you reboot back."
              href="/usb"
              checked={usbJtag}
              danger
              onChange={async (event) => {
                if (event.target.checked) {
                  if (!window.confirm(usbRebootText())) {
                    event.target.checked = false;
                    return;
                  }
                  try { await post("/api/v1/midi/settings?usbHid=0&usbMidiHost=0&usbMidiPeripheral=0"); }
                  catch (err) { event.target.checked = false; setError(err.message); }
                  return;
                }
                event.target.checked = true;
                setError("Pick USB MIDI or USB HID on the USB page. That reboot is what turns serial/JTAG off.");
              }}
            />
            <ServiceNode
              title="MIDI"
              info={transport("usbMidiPeripheral").reason || "USB MIDI gadget. Reboot required."}
              href="/usb"
              checked={!!transport("usbMidiPeripheral").enabled}
              available={transport("usbMidiPeripheral").available !== false}
              onChange={(event) => toggle("usbMidiPeripheral", event)}
            />
            <ServiceNode
              title="HID"
              info={transport("usbHid").reason || "USB HID gadget. Reboot required."}
              href="/usb"
              checked={!!transport("usbHid").enabled}
              available={transport("usbHid").available !== false}
              onChange={(event) => toggle("usbHid", event)}
            />
          </ServiceNode>
          <ServiceNode title="Host" href="/usb">
            <ServiceNode
              title="MIDI"
              info={transport("usbMidiHost").reason || "Plug a MIDI device into the USB port. Reboot required."}
              href="/usb"
              checked={!!transport("usbMidiHost").enabled}
              available={transport("usbMidiHost").available !== false}
              onChange={(event) => toggle("usbMidiHost", event)}
            />
          </ServiceNode>
        </ServiceNode>

        <ServiceNode title="Other" defaultOpen>
          <ServiceNode
            title="Mackie Showbox interceptor"
            info="Always forwards mixer ↔ base. When parsing is on, this module also reads entities, battery, and looper. Turn parsing off only to test UART stability."
            checked={link.parsing !== false}
            danger
            onChange={(event) => toggle("interceptor", event)}
          />
          <ServiceNode
            title="5-pin DIN MIDI"
            info={transport("din").reason || "In/Out/Through on the DIN jacks."}
            href="/midi"
            checked={!!transport("din").enabled}
            available={transport("din").available !== false}
            onChange={(event) => toggle("din", event)}
          />
        </ServiceNode>
      </div>
    </div>
  );
}

function usbRebootText() {
  return "Requires reboot. Turns off USB MIDI and USB HID on this port so serial/JTAG comes back. OTA still works.";
}
