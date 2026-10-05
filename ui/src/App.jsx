import { useEffect, useState } from "preact/hooks";
import { Home } from "./pages/Home.jsx";
import { Connections } from "./pages/Connections.jsx";
import { Reactions } from "./pages/Reactions.jsx";
import { Mixer } from "./pages/Mixer.jsx";
import { pathOf } from "./route.js";

export function App() {
  const [path, setPath] = useState(pathOf());
  useEffect(() => {
    const onPop = () => setPath(pathOf());
    window.addEventListener("popstate", onPop);
    return () => window.removeEventListener("popstate", onPop);
  }, []);
  if (path === "/midi" || path === "/services") return <Connections />;
  if (path === "/actions") return <Reactions />;
  if (path === "/remote-ui") return <Mixer />;
  return <Home />;
}
