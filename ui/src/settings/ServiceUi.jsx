import { useEffect, useRef, useState } from "preact/hooks";
import { go } from "../route.js";
import { IconGear, IconInfo } from "../mixer/Icons.jsx";
import { Switch } from "../ui/Widgets.jsx";

export function InfoTip({ text }) {
  const [open, setOpen] = useState(false);
  const btn = useRef(null);
  const pop = useRef(null);

  useEffect(() => {
    if (!open) return undefined;
    function hide(event) {
      if (btn.current && btn.current.contains(event.target)) return;
      if (pop.current && pop.current.contains(event.target)) return;
      setOpen(false);
    }
    document.addEventListener("pointerdown", hide);
    return () => document.removeEventListener("pointerdown", hide);
  }, [open]);

  if (!text) return null;
  return (
    <span class="info-wrap">
      <button
        ref={btn}
        type="button"
        class="svc-ico"
        title={text}
        aria-label="About this service"
        onClick={(event) => {
          event.preventDefault();
          event.stopPropagation();
          setOpen((v) => !v);
        }}
      >
        <IconInfo />
      </button>
      {open ? <span ref={pop} class="info-pop" role="tooltip">{text}</span> : null}
    </span>
  );
}

export function GearLink({ href, title }) {
  if (!href) return null;
  return (
    <a
      class="svc-ico"
      href={href}
      title={title || "Open settings"}
      onClick={(event) => go(event, href)}
    >
      <IconGear />
    </a>
  );
}

export function ServiceNode({
  title, info, href, checked, onChange, danger, disabled, available = true, note, defaultOpen,
  children
}) {
  const hasKids = children != null && children !== false;
  const [open, setOpen] = useState(defaultOpen != null ? defaultOpen : hasKids);
  const dim = onChange && !checked;
  return (
    <div class={"svc" + (available ? "" : " off") + (dim ? " dim" : "") + (danger ? " danger" : "")}>
      <div class="svc-head">
        {hasKids ? (
          <button
            type="button"
            class={"svc-caret" + (open ? " open" : "")}
            aria-label={open ? "Collapse" : "Expand"}
            onClick={() => setOpen((v) => !v)}
          />
        ) : <span class="svc-caret spacer" />}
        <div class="svc-copy">
          <strong>{title}</strong>
          {note ? <small>{note}</small> : null}
        </div>
        <div class="svc-tools">
          <InfoTip text={info} />
          <GearLink href={href} />
          {onChange ? (
            <Switch checked={checked} disabled={disabled || !available} onChange={onChange} />
          ) : null}
        </div>
      </div>
      {hasKids && open ? <div class="svc-kids">{children}</div> : null}
    </div>
  );
}

export function ModeCard({ title, info, on, locked, danger, onSelect }) {
  return (
    <button
      type="button"
      class={"mode-card" + (on ? " on" : "") + (danger ? " danger" : "")}
      disabled={locked && on}
      onClick={() => { if (!on) onSelect && onSelect(); }}
    >
      <span class={"mode-pip" + (on ? " on" : "")} />
      <span class="mode-copy">
        <strong>{title}</strong>
        {info ? <small>{info}</small> : null}
      </span>
    </button>
  );
}
