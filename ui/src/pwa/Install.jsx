import { useEffect, useState } from "preact/hooks";
import { Pad } from "../mixer/Controls.jsx";
import { IconInstall } from "../mixer/Icons.jsx";

const LISTENERS = new Set();

function notify() {
  LISTENERS.forEach((fn) => fn());
}

export function captureInstallEvents() {
  if (typeof window === "undefined" || window.__showboxPwaBound) return;
  window.__showboxPwaBound = true;
  window.addEventListener("beforeinstallprompt", (event) => {
    event.preventDefault();
    window.__showboxInstall = event;
    notify();
  });
  window.addEventListener("appinstalled", () => {
    window.__showboxInstall = null;
    window.__showboxInstalled = true;
    notify();
  });
}

export function isStandalone() {
  if (typeof window === "undefined") return false;
  if (window.__showboxInstalled) return true;
  if (window.navigator && window.navigator.standalone) return true;
  try {
    return window.matchMedia("(display-mode: standalone)").matches
      || window.matchMedia("(display-mode: minimal-ui)").matches;
  } catch {
    return false;
  }
}

function platformHelp() {
  const ua = (typeof navigator !== "undefined" && navigator.userAgent) || "";
  const ios = /iphone|ipad|ipod/i.test(ua)
    || (typeof navigator !== "undefined" && navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);
  const android = /android/i.test(ua);
  if (ios) {
    return {
      title: "Add to Home Screen",
      steps: [
        "Tap the Share button in Safari.",
        "Scroll to Add to Home Screen.",
        "Tap Add. ShowboxMod opens full screen next time."
      ]
    };
  }
  if (android) {
    return {
      title: "Install ShowboxMod",
      steps: [
        "Tap Install app below if your browser offers it.",
        "Otherwise open the Chrome menu (⋮) and tap Install app or Add to Home screen.",
        "Open the new ShowboxMod icon — it runs without the browser chrome."
      ]
    };
  }
  return {
    title: "Install ShowboxMod",
    steps: [
      "Tap Install app below if Chrome shows the prompt.",
      "Otherwise open the browser menu and choose Install ShowboxMod.",
      "The mixer then opens in its own window."
    ]
  };
}

export async function tryPwaPrompt() {
  const event = window.__showboxInstall;
  if (!event || typeof event.prompt !== "function") return false;
  event.prompt();
  try {
    const choice = await event.userChoice;
    window.__showboxInstall = null;
    notify();
    return choice && choice.outcome === "accepted";
  } catch {
    return false;
  }
}

export function pageIsHttps() {
  return typeof location !== "undefined" && location.protocol === "https:";
}

export function useHttpsEnabled() {
  const [on, setOn] = useState(pageIsHttps());
  useEffect(() => {
    if (pageIsHttps()) {
      setOn(true);
      return undefined;
    }
    let stop = false;
    fetch("/api/v1/https")
      .then((res) => res.ok ? res.json() : null)
      .then((json) => {
        if (stop) return;
        setOn(!!(json && (json.enabled || json.listening)));
      })
      .catch(() => { if (!stop) setOn(false); });
    return () => { stop = true; };
  }, []);
  return on;
}

export function usePwaInstall() {
  const [, bump] = useState(0);
  const httpsOn = useHttpsEnabled();
  useEffect(() => {
    captureInstallEvents();
    const onChange = () => bump((n) => n + 1);
    LISTENERS.add(onChange);
    const mq = window.matchMedia("(display-mode: standalone)");
    if (mq.addEventListener) mq.addEventListener("change", onChange);
    else if (mq.addListener) mq.addListener(onChange);
    return () => {
      LISTENERS.delete(onChange);
      if (mq.removeEventListener) mq.removeEventListener("change", onChange);
      else if (mq.removeListener) mq.removeListener(onChange);
    };
  }, []);
  return {
    standalone: isStandalone(),
    httpsOn,
    canPrompt: !!(typeof window !== "undefined" && window.__showboxInstall)
  };
}

export function PwaInstallPad() {
  const { standalone, httpsOn } = usePwaInstall();
  const [open, setOpen] = useState(false);
  if (standalone || !httpsOn) return null;
  return (
    <>
      <Pad
        icon={<IconInstall />}
        label="Install"
        title="Install ShowboxMod as an app"
        onClick={async () => {
          if (await tryPwaPrompt()) return;
          setOpen(true);
        }}
      />
      <PwaInstallSheet open={open} onClose={() => setOpen(false)} />
    </>
  );
}

export function PwaInstallNavButton() {
  const { standalone, httpsOn } = usePwaInstall();
  const [open, setOpen] = useState(false);
  if (standalone || !httpsOn) return null;
  return (
    <>
      <button
        type="button"
        class="pwa-nav-btn"
        onClick={async () => {
          if (await tryPwaPrompt()) return;
          setOpen(true);
        }}
      >
        <span class="tab-ico"><IconInstall /></span>
        <span>Install app</span>
      </button>
      <PwaInstallSheet open={open} onClose={() => setOpen(false)} />
    </>
  );
}

export function PwaInstallBanner() {
  const { standalone, canPrompt, httpsOn } = usePwaInstall();
  const [open, setOpen] = useState(false);
  if (standalone || !httpsOn) return null;
  return (
    <>
      <p class="pwa-banner">
        <button
          type="button"
          onClick={async () => {
            if (await tryPwaPrompt()) return;
            setOpen(true);
          }}
        >
          <span class="pad-ico"><IconInstall /></span>
          {canPrompt ? "Install app" : "Install as app"}
        </button>
        <span>Home screen, full screen, no browser chrome.</span>
      </p>
      <PwaInstallSheet open={open} onClose={() => setOpen(false)} />
    </>
  );
}

export function PwaInstallSheet({ open, onClose }) {
  const { canPrompt } = usePwaInstall();
  const help = platformHelp();
  const [httpsInfo, setHttpsInfo] = useState(null);
  useEffect(() => {
    if (!open) return undefined;
    let stop = false;
    fetch("/api/v1/https")
      .then((res) => res.ok ? res.json() : null)
      .then((json) => { if (!stop) setHttpsInfo(json); })
      .catch(() => { if (!stop) setHttpsInfo(null); });
    return () => { stop = true; };
  }, [open]);
  if (!open) return null;
  const onHttp = typeof location !== "undefined" && location.protocol !== "https:";
  return (
    <div class="overlay open pwa-overlay" onClick={(event) => { if (event.currentTarget === event.target) onClose(); }}>
      <div class="sheet fs-sheet pwa-sheet" onClick={(event) => event.stopPropagation()}>
        <div class="sheet-bar">
          <span class="sheet-tag">{help.title}</span>
          <button type="button" class="ghost tight" onClick={onClose}>✕</button>
        </div>
        {onHttp ? (
          <div class="pwa-https">
            <p>Chrome only offers Install app on trusted HTTPS. HTTP on port 80 stays up either way — there is no redirect.</p>
            <ol class="pwa-steps">
              <li>HTTPS is optional and off by default. Turn it on in Network if you need a trusted PWA install — it can stall Wi-Fi while Bluetooth is up.</li>
              <li>
                {httpsInfo && httpsInfo.hasCa
                  ? <a href="/api/v1/https/ca.pem" download="showbox-ca.pem">Download the device CA</a>
                  : "Download the device CA from Network once TLS is up."}
                {" "}and install it as a trusted authority (not just “continue anyway”).
              </li>
              <li>Open https://showbox.local/ and use the steps below.</li>
            </ol>
          </div>
        ) : null}
        <ol class="pwa-steps">
          {help.steps.map((step) => <li key={step}>{step}</li>)}
        </ol>
        <p class="actions">
          {canPrompt ? (
            <button
              type="button"
              onClick={async () => {
                if (await tryPwaPrompt()) onClose();
              }}
            >Install app</button>
          ) : null}
          <button type="button" class="ghost" onClick={onClose}>Close</button>
        </p>
      </div>
    </div>
  );
}
