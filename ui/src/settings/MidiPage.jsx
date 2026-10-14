import { ServiceNode } from "./ServiceUi.jsx";
import { useServices } from "./services.jsx";

export function MidiPage() {
  const { error, toggle, transport } = useServices();
  const din = transport("din");
  const rtp = transport("wifi");
  const bleP = transport("bleMidiPeripheral");
  const bleH = transport("bleMidiHost");
  const usbP = transport("usbMidiPeripheral");
  const usbH = transport("usbMidiHost");

  return (
    <div class="mgmt-page">
      <p id="error">{error}</p>
      <p class="hint">MIDI ports this module can speak. USB MIDI shares the USB-C port with serial and HID — use the gear to change that mode.</p>
      <div class="svc-tree">
        <ServiceNode
          title="5-pin DIN"
          info={din.reason || "In/Out/Through on the DIN jacks."}
          checked={!!din.enabled}
          available={din.available !== false}
          note={din.note}
          onChange={(event) => toggle("din", event)}
        />
        <ServiceNode
          title="RTP-MIDI"
          info={rtp.reason || "AppleMIDI / RTP-MIDI on UDP 5004. Name AppleMidi."}
          href="/network"
          checked={!!rtp.enabled}
          available={rtp.available !== false}
          note={rtp.note}
          onChange={(event) => toggle("wifi", event)}
        />
        <ServiceNode
          title="BLE MIDI peripheral"
          info={bleP.reason || "Advertise BLE MIDI so phones and computers can connect in."}
          href="/bluetooth"
          checked={!!bleP.enabled}
          available={bleP.available !== false}
          onChange={(event) => toggle("bleMidiPeripheral", event)}
        />
        <ServiceNode
          title="BLE MIDI host"
          info={bleH.reason || "Connect out to BLE MIDI pedals."}
          href="/bluetooth"
          checked={!!bleH.enabled}
          available={bleH.available !== false}
          onChange={(event) => toggle("bleMidiHost", event)}
        />
        <ServiceNode
          title="USB MIDI device"
          info={usbP.reason || "This USB-C port becomes a MIDI gadget. Reboot required; turns off serial/JTAG and HID."}
          href="/usb"
          checked={!!usbP.enabled}
          available={usbP.available !== false}
          onChange={(event) => toggle("usbMidiPeripheral", event)}
        />
        <ServiceNode
          title="USB MIDI host"
          info={usbH.reason || "Plug a USB MIDI controller into this port. Reboot required."}
          href="/usb"
          checked={!!usbH.enabled}
          available={usbH.available !== false}
          onChange={(event) => toggle("usbMidiHost", event)}
        />
      </div>
    </div>
  );
}
