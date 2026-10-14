import { go } from "./route.js";
import { IconBolt, IconBug, IconCpu, IconGauge, IconMixer, IconTerm } from "./mixer/Icons.jsx";

const tabs = [
  ["/", "Mixer", IconMixer],
  ["/actions", "Actions", IconBolt],
  ["/system", "System", IconCpu],
  ["/diag", "Tasks", IconGauge],
  ["/console", "Console", IconTerm],
  ["/debug", "Debug", IconBug]
];

const MGMT = [
  ["/system", "System"],
  ["/usb", "USB"],
  ["/network", "Network"],
  ["/bluetooth", "Bluetooth"],
  ["/midi", "MIDI"]
];

const SYSTEM_PATHS = new Set(["/system", "/services", "/midi", "/wifi", "/usb", "/network", "/bluetooth"]);

function tabOn(here, href) {
  if (href === "/") return here === "/" || here === "/remote-ui";
  if (href === "/system") return SYSTEM_PATHS.has(here);
  return here === href;
}

function mgmtOn(here, href) {
  if (href === "/system") return here === "/system" || here === "/services";
  if (href === "/network") return here === "/network" || here === "/wifi";
  return here === href;
}

export function MgmtNav({ current }) {
  const here = current === "/remote-ui" || current === "" ? "/" : current;
  return (
    <nav class="mgmt-tabs" aria-label="Service pages">
      {MGMT.map(([href, label]) => (
        <a
          key={href}
          href={href}
          class={mgmtOn(here, href) ? "on" : ""}
          onClick={(event) => go(event, href)}
        >{label}</a>
      ))}
    </nav>
  );
}

export function Nav({ current }) {
  const here = current === "/remote-ui" || current === "" ? "/" : current;
  return (
    <nav class="tabs">
      {tabs.map(([href, label, Icon]) => (
        <a
          key={href}
          href={href}
          class={tabOn(here, href) ? "on" : ""}
          onClick={(event) => go(event, href)}
        >
          <span class="tab-ico"><Icon /></span>
          <span>{label}</span>
        </a>
      ))}
    </nav>
  );
}
