import { Nav } from "../Nav.jsx";
import { go } from "../route.js";

export function Home() {
  return (
    <main>
      <h1>ShowboxMod</h1>
      <Nav current="/" />
      <section>
        <div class="row">
          <div class="copy">
            <strong>Showbox controls</strong>
            <small>Mixer, looper, and entity values from the Mackie Showbox.</small>
          </div>
          <a href="/remote-ui" onClick={(event) => go(event, "/remote-ui")}>Open</a>
        </div>
        <div class="row">
          <div class="copy">
            <strong>Services</strong>
            <small>Bluetooth, HID, USB, network, mixer link, and remembered devices.</small>
          </div>
          <a href="/services" onClick={(event) => go(event, "/services")}>Open</a>
        </div>
        <div class="row">
          <div class="copy">
            <strong>Reactions</strong>
            <small>Triggers, conditions, and actions stored on the module.</small>
          </div>
          <a href="/actions" onClick={(event) => go(event, "/actions")}>Open</a>
        </div>
      </section>
    </main>
  );
}
