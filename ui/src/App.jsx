import { lazy, Suspense } from "preact/compat";
import { useEffect, useRef, useState } from "preact/hooks";
import { MixerBoard } from "./mixer/MixerBoard.jsx";
import { LiveProvider } from "./live/LiveContext.jsx";
import { StatusCluster } from "./status/StatusCluster.jsx";
import { Nav } from "./Nav.jsx";
import { Pad } from "./mixer/Controls.jsx";
import { IconBars, IconCompress, IconExpand } from "./mixer/Icons.jsx";
import { pathOf } from "./route.js";

// Tools are only needed when the user opens them; keep them out of the
// first-load bundle that crosses the BLE-coex radio.
const ActionsPanel = lazy(() => import("./actions/ActionsPanel.jsx").then((m) => ({ default: m.ActionsPanel })));
const Connections = lazy(() => import("./pages/Connections.jsx").then((m) => ({ default: m.Connections })));
const ConsolePanel = lazy(() => import("./console/ConsolePanel.jsx").then((m) => ({ default: m.ConsolePanel })));
const DebugPanel = lazy(() => import("./debug/DebugPanel.jsx").then((m) => ({ default: m.DebugPanel })));
const DiagnosticsPanel = lazy(() => import("./diag/DiagnosticsPanel.jsx").then((m) => ({ default: m.DiagnosticsPanel })));
const PwaInstallNavButton = lazy(() => import("./pwa/Install.jsx").then((m) => ({ default: m.PwaInstallNavButton })));

function tabOf(path) {
  if (path === "/actions") return "actions";
  if (path === "/diag") return "diag";
  if (
    path === "/midi" || path === "/services" || path === "/wifi" || path === "/system"
    || path === "/usb" || path === "/network" || path === "/bluetooth"
  ) return "system";
  if (path === "/console") return "console";
  if (path === "/debug") return "debug";
  return "mixer";
}

function pageOf(path) {
  if (path === "/usb") return "usb";
  if (path === "/network" || path === "/wifi") return "network";
  if (path === "/bluetooth") return "bluetooth";
  if (path === "/midi") return "midi";
  if (path === "/system" || path === "/services") return "system";
  return tabOf(path);
}

const PAGE_TITLE = {
  mixer: "Mixer",
  actions: "Actions",
  system: "System",
  usb: "USB",
  network: "Network",
  bluetooth: "Bluetooth",
  midi: "MIDI",
  console: "Console",
  debug: "Debug",
  diag: "Tasks"
};

function PageHud({ title, stage, onMenu, onToggleStage }) {
  return (
    <div class="mixer-hud page-hud">
      <div class="hud-left">
        <Pad icon={<IconBars />} title="Pages" onClick={onMenu} />
        <span class="hud-view">{title}</span>
      </div>
      <div class="hud-right">
        <StatusCluster />
        <Pad
          extra="hud-fs"
          icon={stage ? <IconCompress /> : <IconExpand />}
          title={stage ? "Exit fullscreen" : "Fullscreen"}
          on={!!stage}
          onClick={onToggleStage}
        />
      </div>
    </div>
  );
}

export function App() {
  const [path, setPath] = useState(pathOf());
  const [menuOpen, setMenuOpen] = useState(false);
  const [stage, setStage] = useState(false);
  const shell = useRef(null);

  useEffect(() => {
    const onPop = () => { setPath(pathOf()); setMenuOpen(false); };
    window.addEventListener("popstate", onPop);
    return () => window.removeEventListener("popstate", onPop);
  }, []);

  useEffect(() => {
    const onFs = () => {
      const on = !!(document.fullscreenElement || document.webkitFullscreenElement);
      setStage(on);
    };
    document.addEventListener("fullscreenchange", onFs);
    document.addEventListener("webkitfullscreenchange", onFs);
    return () => {
      document.removeEventListener("fullscreenchange", onFs);
      document.removeEventListener("webkitfullscreenchange", onFs);
    };
  }, []);

  async function toggleStage() {
    const node = shell.current || document.documentElement;
    const active = document.fullscreenElement || document.webkitFullscreenElement;
    try {
      if (!active) {
        if (node.requestFullscreen) await node.requestFullscreen();
        else if (node.webkitRequestFullscreen) node.webkitRequestFullscreen();
        else setStage(true);
      } else if (document.exitFullscreen) {
        await document.exitFullscreen();
      } else if (document.webkitExitFullscreen) {
        document.webkitExitFullscreen();
      } else {
        setStage(false);
      }
    } catch {
      setStage((prev) => !prev);
    }
  }

  const tab = tabOf(path);
  const page = pageOf(path);
  const title = PAGE_TITLE[page] || PAGE_TITLE[tab];
  return (
    <LiveProvider>
    <div ref={shell} class={"shell" + (tab === "mixer" ? " mixer-shell" : " page-shell " + tab + "-shell " + page + "-shell") + (stage ? " stage" : "")}>
      {tab === "mixer" ? (
        <MixerBoard
          onMenu={() => setMenuOpen(true)}
          stage={stage}
          onToggleStage={toggleStage}
        />
      ) : (
        <>
          <PageHud
            title={title}
            stage={stage}
            onMenu={() => setMenuOpen(true)}
            onToggleStage={toggleStage}
          />
          <div class="page-body">
            <Suspense fallback={<div class="lead">Loading…</div>}>
              {tab === "actions" ? <ActionsPanel /> : null}
              {tab === "system" ? <Connections embedded page={page} /> : null}
              {tab === "diag" ? <DiagnosticsPanel /> : null}
              {tab === "console" ? <ConsolePanel /> : null}
              {tab === "debug" ? <DebugPanel /> : null}
            </Suspense>
          </div>
        </>
      )}
      <div class={"nav-drawer" + (menuOpen ? " open" : "")} onClick={(event) => { if (event.currentTarget === event.target) setMenuOpen(false); }}>
        {menuOpen ? (
          <div class="nav-drawer-panel" onClick={(event) => event.stopPropagation()}>
            <h2>ShowboxMod</h2>
            <Nav current={path} />
            <Suspense fallback={null}>
              <PwaInstallNavButton />
            </Suspense>
            <button type="button" class="ghost" onClick={() => setMenuOpen(false)}>Close</button>
          </div>
        ) : null}
      </div>
    </div>
    </LiveProvider>
  );
}
