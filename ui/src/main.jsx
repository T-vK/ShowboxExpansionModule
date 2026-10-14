import { render } from "preact";
import { App } from "./App.jsx";
import { captureInstallEvents } from "./pwa/Install.jsx";
import "./styles.css";

captureInstallEvents();
if (typeof navigator !== "undefined" && "serviceWorker" in navigator) {
  navigator.serviceWorker.register("/sw.js").catch(() => {});
}

render(<App />, document.getElementById("app"));
