import { ServiceNode } from "./ServiceUi.jsx";
import { useServices } from "./services.jsx";
import { WifiPanel } from "./WifiPanel.jsx";

export function NetworkPage() {
  const { data, error, toggle, transport } = useServices();
  const wifiRadioOn = data ? data.wifiRadio !== false : true;
  const httpsOn = data ? !!data.https : false;
  const httpsListening = !!(data && data.httpsListening);
  const httpsHasCa = !!(data && data.httpsHasCa);
  const eth = transport("ethernet");
  const rtp = transport("wifi");

  return (
    <div class="mgmt-page">
      <p id="error">{error}</p>
      <div class="svc-tree">
        <ServiceNode
          title="Wi-Fi radio"
          info="Station for this page, OTA, and mDNS. Ethernet keeps working if Wi-Fi is off."
          checked={wifiRadioOn}
          onChange={(event) => toggle("wifiRadio", event)}
        />
        <ServiceNode
          title="Ethernet"
          info={eth.reason || "W5500 link. MIDI sessions use UDP 5004/5005."}
          checked={!!eth.enabled}
          available={eth.available !== false}
          note={eth.note}
          onChange={eth.available === false ? undefined : (event) => toggle("ethernet", event)}
        />
        <ServiceNode
          title="RTP-MIDI"
          info={rtp.reason || "AppleMIDI / RTP-MIDI on UDP 5004."}
          href="/midi"
          checked={!!rtp.enabled}
          available={rtp.available !== false}
          onChange={(event) => toggle("wifi", event)}
        />
        <ServiceNode
          title="HTTPS"
          info="Optional TLS on port 443 for a trusted PWA install. Off by default. Enabling it can stall Wi-Fi while Bluetooth is up. HTTP on port 80 stays up — there is no redirect."
          checked={httpsOn}
          note={httpsOn
            ? (httpsListening ? "Listening on 443 · https://showbox.local/" : "Starting… minting a device CA if needed")
            : "Off — HTTP on port 80 stays up."}
          onChange={(event) => toggle("https", event)}
        />
      </div>
      <p class="hint https-ca">
        {httpsHasCa
          ? <a href="/api/v1/https/ca.pem" download="showbox-ca.pem">Download device CA</a>
          : (httpsOn ? "The device CA appears here once TLS is up." : "Turn HTTPS on to mint a unique device CA.")}
        {" "}Install that CA on your phone or computer, then open https://showbox.local/. Clicking through a cert warning is not enough for Chrome to offer Install app.
      </p>
      <WifiPanel />
    </div>
  );
}
