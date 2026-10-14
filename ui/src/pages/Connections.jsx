import { MgmtNav } from "../Nav.jsx";
import { pathOf } from "../route.js";
import { ServicesProvider, useServices } from "../settings/services.jsx";
import { SystemPage } from "../settings/SystemPage.jsx";
import { UsbPage } from "../settings/UsbPage.jsx";
import { NetworkPage } from "../settings/NetworkPage.jsx";
import { BluetoothPage } from "../settings/BluetoothPage.jsx";
import { MidiPage } from "../settings/MidiPage.jsx";

const PAGES = {
  system: SystemPage,
  usb: UsbPage,
  network: NetworkPage,
  bluetooth: BluetoothPage,
  midi: MidiPage
};

function PageGate({ View }) {
  const { data, error } = useServices();
  if (!data && !error) {
    return <p class="hint">Loading services…</p>;
  }
  return <View />;
}

export function Connections({ embedded, page }) {
  const kind = page || "system";
  const View = PAGES[kind] || SystemPage;
  return (
    <ServicesProvider>
      <div class={embedded ? "panel mgmt-wrap" : "page mgmt-wrap"}>
        {embedded ? null : <h1>Services</h1>}
        <MgmtNav current={pathOf()} />
        <PageGate View={View} />
      </div>
    </ServicesProvider>
  );
}
