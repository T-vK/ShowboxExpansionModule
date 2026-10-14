import { useLayoutEffect, useRef, useState } from "preact/hooks";
import { Dropdown } from "./Dropdown.jsx";

export function Switch({ checked, disabled, role, onChange }) {
  return (
    <label class={"switch" + (role ? " role" : "")}>
      <input type="checkbox" checked={!!checked} disabled={!!disabled} onChange={onChange} />
      <span />
    </label>
  );
}

export function Row({ title, description, note, available = true, checked, disabled, role, danger, onChange, children }) {
  return (
    <div class={"row" + (available ? "" : " off") + (danger ? " danger" : "")}>
      <div class="copy">
        <strong>{title}</strong>
        {description ? <small>{description}</small> : null}
        {note ? <small>{note}</small> : null}
        {children}
      </div>
      {onChange ? <Switch checked={checked} disabled={disabled || !available} role={role} onChange={onChange} /> : null}
    </div>
  );
}

export function Branch({ title, description, checked, disabled, onChange, children }) {
  return (
    <div class={"branch" + (onChange && !checked ? " dim" : "")}>
      <div class={"branch-head" + (onChange ? " has-switch" : "")}>
        <div class="copy">
          <h3>{title}</h3>
          {description ? <small>{description}</small> : null}
        </div>
        {onChange ? <Switch checked={checked} disabled={disabled} onChange={onChange} /> : null}
      </div>
      {children ? <div class="nest">{children}</div> : null}
    </div>
  );
}

export function MenuSelect({ label, value, options, onChange }) {
  const pairs = Array.isArray(options[0]) ? options : options.map((id) => [id, id]);
  const current = pairs.find(([id]) => String(id) === String(value)) || pairs[0];
  const [open, setOpen] = useState(false);
  const btn = useRef(null);

  return (
    <label class="field">
      {label ? <span>{label}</span> : null}
      <button ref={btn} type="button" class="picker-btn" onClick={() => setOpen((v) => !v)}>
        {current ? current[1] : "Select"}
      </button>
      <Dropdown anchorRef={btn} open={open} onClose={() => setOpen(false)}>
        {pairs.map(([id, name]) => (
          <button
            type="button"
            key={id}
            class={"picker-item" + (String(id) === String(value) ? " on" : "")}
            onClick={() => { onChange(id); setOpen(false); }}
          >{name}</button>
        ))}
      </Dropdown>
    </label>
  );
}

export function EntityPicker({ label, value, groups, onChange }) {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState("");
  const btn = useRef(null);
  const search = useRef(null);
  const q = query.trim().toLowerCase();

  useLayoutEffect(() => {
    if (!open) return;
    setTimeout(() => search.current && search.current.focus(), 0);
  }, [open]);

  const shown = (groups || []).map((group) => ({
    title: group.title,
    fields: (group.fields || []).filter((field) => {
      if (!q) return true;
      return field.id.toLowerCase().includes(q) || (field.label || "").toLowerCase().includes(q);
    })
  })).filter((group) => group.fields.length);

  const current = (groups || []).flatMap((group) => group.fields).find((field) => field.id === value);

  return (
    <label class="field">
      {label ? <span>{label}</span> : null}
      <button ref={btn} type="button" class="picker-btn" onClick={() => { setQuery(""); setOpen((v) => !v); }}>
        {current ? `${current.label} · ${current.id}` : (value || "Choose control")}
      </button>
      <Dropdown anchorRef={btn} open={open} wide onClose={() => setOpen(false)}>
        <input ref={search} class="picker-search" type="search" placeholder="Search controls" value={query} onInput={(event) => setQuery(event.target.value)} />
        {shown.length ? shown.map((group) => (
          <div key={group.title} class="picker-group">
            <div class="picker-group-title">{group.title}</div>
            {group.fields.map((field) => (
              <button
                type="button"
                key={field.id}
                class={"picker-item" + (field.id === value ? " on" : "")}
                onClick={() => { onChange(field.id); setOpen(false); }}
              >
                <span>{field.label}</span>
                <small>{field.id}</small>
              </button>
            ))}
          </div>
        )) : <div class="picker-empty">No matching control</div>}
      </Dropdown>
    </label>
  );
}
