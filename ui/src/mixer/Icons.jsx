function Svg({ children }) {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" focusable="false">
      {children}
    </svg>
  );
}

export function IconBars() {
  return (
    <Svg>
      <path d="M4 6h16v2.2H4zm0 4.9h16v2.2H4zM4 15.8h16V18H4z" />
    </Svg>
  );
}

export function IconPen() {
  return (
    <Svg>
      <path d="M3 17.2V21h3.8l11-11.1-3.8-3.8L3 17.2zm17.7-10.9a1 1 0 0 0 0-1.4l-2.6-2.6a1 1 0 0 0-1.4 0l-2 2 4 4 2-2z" />
    </Svg>
  );
}

export function IconExpand() {
  return (
    <Svg>
      <path d="M4 4h7v2H6v5H4V4zm9 0h7v7h-2V6h-5V4zM4 13h2v5h5v2H4v-7zm14 0h2v7h-7v-2h5v-5z" />
    </Svg>
  );
}

export function IconCompress() {
  return (
    <Svg>
      <path d="M9 4H7v5H2v2h7V4zm8 0h-2v7h7V9h-5V4zM2 13v2h5v5h2v-7H2zm13 0v7h2v-5h5v-2h-7z" />
    </Svg>
  );
}

export function IconLoop() {
  return (
    <Svg>
      <path d="M12 6a6 6 0 1 1-5.2 3H9v2H4V6h2v2.1A8 8 0 1 0 12 4v2z" />
    </Svg>
  );
}

export function IconSlots() {
  return (
    <Svg>
      <path d="M4 5h6v6H4zm10 0h6v6h-6zM4 13h6v6H4zm10 0h6v6h-6z" />
    </Svg>
  );
}

export function IconWaveSlash() {
  return (
    <Svg>
      <path d="M2.1 4.3 3.5 3l17.4 17.4-1.4 1.4-4.7-4.7c-.8 1.3-1.8 2.4-3.2 3.6l-1.3-1.6c1.2-1 2-1.9 2.7-3zm8.2 3.6 1.6 1.6c.2-.8.3-1.6.3-2.5V4h2v3c0 1.2-.2 2.3-.5 3.4l3.1 3.1c.7-1.5 1.1-3.2 1.1-5.5h2c0 2.8-.5 5-1.5 6.9zM4.2 9.1C3.4 10.8 3 12.8 3 16h2c0-2.6.3-4.2.9-5.5z" />
    </Svg>
  );
}

export function IconBulb() {
  return (
    <Svg>
      <path d="M9 21h6v-1.2H9zm.3-3.2h5.4c.2-.6.5-1.1.8-1.6A6 6 0 1 0 8 16.2c.3.5.6 1 .9 1.6zM11 8h2v3h-2z" />
    </Svg>
  );
}

export function IconBan() {
  return (
    <Svg>
      <path d="M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20zm7 10a7 7 0 0 1-11.7 5.2L17.2 7.3A6.9 6.9 0 0 1 19 12zM6.8 16.7 16.7 6.8A7 7 0 0 0 6.8 16.7z" />
    </Svg>
  );
}

export function IconCircle() {
  return (
    <Svg>
      <circle cx="12" cy="12" r="6.5" />
    </Svg>
  );
}

export function IconPlay() {
  return (
    <Svg>
      <path d="M8 5.5v13l11-6.5z" />
    </Svg>
  );
}

export function IconStop() {
  return (
    <Svg>
      <path d="M6.5 6.5h11v11h-11z" />
    </Svg>
  );
}

export function IconTrash() {
  return (
    <Svg>
      <path d="M9 3h6l1 2h4v2H4V5h4zm1 6h2v8h-2zm4 0h2v8h-2zM7 9h2v8H7z" />
    </Svg>
  );
}

export function IconSd() {
  return (
    <Svg>
      <path d="M6 4h9l3 3v13H6zm3 3h1.6v3H9zm3 0h1.6v3H12zm3 0H18v3h-2z" />
    </Svg>
  );
}

export function IconFork() {
  return (
    <Svg>
      <path d="M11 3h2v10.1a4.5 4.5 0 1 1-2 0zm1 16.2a2.5 2.5 0 0 0 2.5-2.5H9.5A2.5 2.5 0 0 0 12 19.2zM8.2 5.2c1.1 0 1.8.7 1.8 1.8H8.2zm5.8 1.8c0-1.1.7-1.8 1.8-1.8V8z" />
    </Svg>
  );
}

export function IconRecall() {
  return (
    <Svg>
      <path d="M12 5a7 7 0 1 1-6.5 9.6l1.9-.6A5 5 0 1 0 12 7v2.4L7.8 6.2 12 3z" />
    </Svg>
  );
}

// Load snapshot: arrow lifting a mix out of a slot.
export function IconLoad() {
  return (
    <Svg>
      <path d="M11 3.4h2v8.2l2.9-2.9 1.4 1.4L12 15.5 6.7 10.1l1.4-1.4L11 11.6z" />
      <path d="M4 15.6h2v3h12v-3h2v5H4z" />
    </Svg>
  );
}

export function IconStore() {
  return (
    <Svg>
      <path d="M4 4h12l4 4v12H4zm2 2v5h10V6zm8 9H8v2h6z" />
    </Svg>
  );
}

export function IconMute() {
  return (
    <Svg>
      <path d="M3 9h3.2L12 4.5v15L6.2 15H3zm13.2.3 1.4-1.4 1.9 1.9 1.9-1.9 1.4 1.4-1.9 1.9 1.9 1.9-1.4 1.4-1.9-1.9-1.9 1.9-1.4-1.4 1.9-1.9z" />
    </Svg>
  );
}

export function IconBattery({ pct }) {
  const known = pct != null && pct !== "" && Number.isFinite(Number(pct));
  const n = Math.max(0, Math.min(100, known ? Number(pct) : 0));
  const fill = 2.2 + (n / 100) * 31.2;
  return (
    <svg class="batt-icon" viewBox="0 0 40 16" aria-hidden="true" focusable="false">
      <rect x="1" y="2.2" width="35.2" height="11.6" rx="2" fill="none" stroke="currentColor" stroke-width="1.5" />
      <rect x="36.6" y="5.4" width="2.4" height="5.2" rx="0.6" fill="currentColor" />
      <rect x="2.4" y="3.8" width={fill} height="8.4" rx="1" fill="currentColor" opacity="0.42" />
      <text
        x="18.6"
        y="11.15"
        text-anchor="middle"
        fill="#ffffff"
        font-size="7.1"
        font-weight="700"
        font-family="ui-sans-serif, system-ui, sans-serif"
      >{known ? `${Math.round(Number(pct))}%` : "—"}</text>
    </svg>
  );
}

export function IconSignalStream({ live, on }) {
  return (
    <svg class={"signal-icon" + (live ? " live" : "") + (on && !live ? " wait" : "")} viewBox="0 0 24 24" aria-hidden="true" focusable="false">
      <circle cx="5" cy="12" r="2.35" />
      <path class="stream-arc a1" d="M8.86 7.4A6 6 0 0 1 8.86 16.6" />
      <path class="stream-arc a2" d="M11.69 4.03A10.4 10.4 0 0 1 11.69 19.97" />
      <path class="stream-arc a3" d="M14.39.82A14.6 14.6 0 0 1 14.39 23.18" />
    </svg>
  );
}

export function IconShowbox({ live }) {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" focusable="false">
      <rect x="3.2" y="6.2" width="17.6" height="12.2" rx="2.2" fill="none" stroke="currentColor" stroke-width="1.7" />
      <path d="M6.2 16.4V10.6M9.6 16.4V9.2M13 16.4v-5.4M16.4 16.4V11.4" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" />
      <circle class={live ? "showbox-led" : ""} cx="18.2" cy="8.4" r="1.15" fill="currentColor" />
    </svg>
  );
}

export function IconShowboxOff() {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" focusable="false">
      <rect x="3.2" y="6.2" width="17.6" height="12.2" rx="2.2" fill="none" stroke="currentColor" stroke-width="1.7" />
      <path d="M6.2 16.4V10.6M9.6 16.4V9.2M13 16.4v-5.4M16.4 16.4V11.4" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" />
      <path d="M4 5 19.6 19.4" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" />
    </svg>
  );
}

export function IconGear() {
  return (
    <Svg>
      <path d="M10.1 3h3.8l.4 2.2a6.8 6.8 0 0 1 1.8 1l2.1-.8 1.9 3.3-1.7 1.4a6.8 6.8 0 0 1 0 2.1l1.7 1.4-1.9 3.3-2.1-.8a6.8 6.8 0 0 1-1.8 1L13.9 21h-3.8l-.4-2.2a6.8 6.8 0 0 1-1.8-1l-2.1.8-1.9-3.3 1.7-1.4a6.8 6.8 0 0 1 0-2.1L3.9 9.7 5.8 6.4l2.1.8a6.8 6.8 0 0 1 1.8-1zm1.9 5.2A3.8 3.8 0 1 0 16 12a3.8 3.8 0 0 0-4-3.8z" />
    </Svg>
  );
}

export function IconInfo() {
  return (
    <Svg>
      <path d="M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zm.9 13.2h-1.8v-6h1.8zm0-7.4h-1.8V7h1.8z" />
    </Svg>
  );
}

export function IconLinkOn() {
  return <IconSignalStream live on />;
}

export function IconLinkOff() {
  return <IconSignalStream />;
}

export function IconMixer() {
  return (
    <Svg>
      <path d="M6 3h2.2v7.2H11V12H8.2V21H6v-9H3V10.2h3zm9.8 0H18v9h3V14h-3v7h-2.2v-7H13v-1.8h2.8z" />
    </Svg>
  );
}

export function IconBolt() {
  return (
    <Svg>
      <path d="M13.2 2 4 13.2h6.2L9 22l11-12.4h-6.4z" />
    </Svg>
  );
}

export function IconGauge() {
  return (
    <Svg>
      <path d="M12 4a9 9 0 0 1 9 9h-2a7 7 0 1 0-14 0H3a9 9 0 0 1 9-9z" />
      <path d="M12 13.6 16.2 8l1.6 1.2-4.2 5.6a2 2 0 1 1-1.6-1.2z" />
    </Svg>
  );
}

export function IconCpu() {
  return (
    <Svg>
      <path d="M9 3h6v2h3v3h2v6h-2v3h-3v2H9v-2H6v-3H4V8h2V5h3zm1.2 5.2v7.6h3.6V8.2z" />
    </Svg>
  );
}

export function IconTerm() {
  return (
    <Svg>
      <path d="M3 4h18v16H3zm3.2 4.2 4 3.8-4 3.8 1.5 1.5 5.6-5.3-5.6-5.3zm7.6 7.6H18v2h-4.2z" />
    </Svg>
  );
}

export function IconBug() {
  return (
    <Svg>
      <path d="M9 7.2a3 3 0 1 1 6 0H18v2h-2.1c.1.5.1 1 .1 1.5h2.2v2H16c0 .6-.1 1.1-.2 1.6l2.4 1.4-.9 1.6-2.2-1.3A6.2 6.2 0 0 1 12 19.2a6.2 6.2 0 0 1-3.1-1.2l-2.2 1.3-.9-1.6 2.4-1.4A8 8 0 0 1 8 12.7H5.8v-2H8c0-.5 0-1 .1-1.5H6V7.2zm2.1.8v8.2h1.8V8z" />
    </Svg>
  );
}

export function IconInstall() {
  return (
    <Svg>
      <path d="M11 3h2v9.2l3.2-3.2 1.4 1.4L12 16.2 6.4 10.4l1.4-1.4L11 12.2zm-6 16h14v2H5z" />
    </Svg>
  );
}
