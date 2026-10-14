import { createContext } from "preact";
import { useContext, useEffect, useRef, useState } from "preact/hooks";
import { connectLive } from "../api.js";

const EMPTY_LINK = {
  ok: false, battery: null, tunerOn: false, tunerChan: 1, sdCard: 0,
  tunerMidi: 0, tunerNoteId: 0, tunerCents: 0, tunerHz: 0, tunerHeard: false,
  analogHoldPhones: true, analogHoldLoop: true
};

const LiveCtx = createContext(null);

export function LiveProvider({ children }) {
  const [esp, setEsp] = useState({
    connected: !!import.meta.env.DEV,
    since: import.meta.env.DEV ? Date.now() : 0,
    lastDisconnect: 0,
    disconnects: 0,
    sessions: []
  });
  const [link, setLink] = useState(() => (
    import.meta.env.DEV ? { ...EMPTY_LINK, ok: true, battery: 96 } : EMPTY_LINK
  ));
  const listeners = useRef(new Set());

  useEffect(() => {
    return connectLive((msg) => {
      if (msg.type === "link") {
        setLink((prev) => ({
          ...prev,
          ...msg,
          battery: msg.battery == null || msg.battery === "" ? null : Number(msg.battery)
        }));
      }
      listeners.current.forEach((fn) => fn(msg));
    }, {
      onOpen() {
        const now = Date.now();
        setEsp((prev) => ({
          connected: true,
          since: now,
          lastDisconnect: prev.lastDisconnect,
          disconnects: prev.disconnects,
          sessions: [...prev.sessions.slice(-8), { start: now, end: null }]
        }));
      },
      onClose() {
        const now = Date.now();
        setEsp((prev) => {
          if (!prev.connected && prev.sessions.length === 0) return prev;
          const sessions = prev.sessions.map((row, i) => (
            i === prev.sessions.length - 1 && !row.end ? { ...row, end: now } : row
          ));
          return {
            connected: false,
            since: prev.since,
            lastDisconnect: now,
            disconnects: prev.disconnects + (prev.connected ? 1 : 0),
            sessions
          };
        });
      }
    });
  }, []);

  function subscribe(fn) {
    listeners.current.add(fn);
    return () => listeners.current.delete(fn);
  }

  function patchLink(partial) {
    setLink((prev) => ({ ...prev, ...partial }));
  }

  return (
    <LiveCtx.Provider value={{ esp, link, subscribe, patchLink }}>
      {children}
    </LiveCtx.Provider>
  );
}

export function useLive() {
  const value = useContext(LiveCtx);
  if (!value) throw new Error("useLive needs LiveProvider");
  return value;
}
