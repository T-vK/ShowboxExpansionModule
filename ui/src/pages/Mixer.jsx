import { useEffect, useMemo, useState } from "preact/hooks";
import { Nav } from "../Nav.jsx";
import { fetchRetry, getJson } from "../api.js";
import { mixerSchema } from "../mixerSchema.js";

function Switch({ checked, disabled, onChange }) {
  return (
    <label class="switch">
      <input type="checkbox" checked={!!checked} disabled={disabled} onChange={onChange} />
      <span />
    </label>
  );
}

function Field({ field, value, onChange }) {
  const [draft, setDraft] = useState(null);
  if (field.type === "BOOL") {
    return (
      <div class="mixer-field">
        <div class="copy"><strong>{field.label}</strong><small>{field.id}</small></div>
        <Switch checked={!!value} onChange={(event) => onChange(event.target.checked)} />
      </div>
    );
  }
  const shown = draft == null ? (value == null ? "" : value) : draft;
  return (
    <div class="mixer-field">
      <div class="copy"><strong>{field.label}</strong><small>{field.id}</small></div>
      <input
        type="number"
        step={field.type === "FLOAT" ? "0.01" : "1"}
        value={shown}
        onInput={(event) => setDraft(event.target.value)}
        onBlur={(event) => {
          setDraft(null);
          onChange(event.target.value);
        }}
      />
    </div>
  );
}

export function Mixer() {
  const [entities, setEntities] = useState({});
  const [error, setError] = useState("");
  const known = useMemo(() => {
    const ids = new Set();
    for (const group of mixerSchema) {
      for (const field of group.fields) ids.add(field.id);
    }
    return ids;
  }, []);

  async function refresh() {
    const data = await getJson("/api/v1/showbox/entities");
    const next = {};
    for (const row of data.entities || []) next[row.id] = row;
    setEntities(next);
    setError("");
  }

  useEffect(() => {
    let stop = false;
    const tick = () => refresh().catch((err) => { if (!stop) setError(String(err.message || err)); });
    tick();
    const id = setInterval(tick, 2500);
    return () => { stop = true; clearInterval(id); };
  }, []);

  async function write(id, value) {
    const encoded = encodeURIComponent(String(value));
    const response = await fetchRetry(`/api/v1/showbox/entities/${id}?value=${encoded}`);
    const text = await response.text();
    if (!response.ok) throw new Error(text || "Could not set value.");
    setEntities((prev) => {
      const row = { ...(prev[id] || { id }), value: rowValue(prev[id], value, text) };
      return { ...prev, [id]: row };
    });
    setError("");
  }

  const extras = Object.values(entities).filter((row) => row && !known.has(row.id));

  return (
    <main>
      <h1>Showbox controls</h1>
      <Nav current="/remote-ui" />
      <p class="hint">Values load after the page. One request reads the mixer; changing a control writes that entity only.</p>
      <p id="error">{error}</p>
      {mixerSchema.map((group) => (
        <section key={group.title}>
          <h2>{group.title}</h2>
          <div class="mixer-grid">
            {group.fields.map((field) => (
              <Field
                key={field.id}
                field={field}
                value={entities[field.id] ? entities[field.id].value : ""}
                onChange={(value) => write(field.id, value).catch((err) => setError(err.message))}
              />
            ))}
          </div>
        </section>
      ))}
      {extras.length ? (
        <section>
          <h2>Other</h2>
          {extras.map((row) => (
            <Field
              key={row.id}
              field={{ id: row.id, label: row.id, type: row.type || "FLOAT" }}
              value={row.value}
              onChange={(value) => write(row.id, value).catch((err) => setError(err.message))}
            />
          ))}
        </section>
      ) : null}
    </main>
  );
}

function rowValue(row, next, text) {
  const type = row && row.type;
  if (type === "BOOL") return text === "true" || next === true || next === "true";
  if (type === "UINT8") return Number(text);
  const n = Number(text);
  return Number.isFinite(n) ? n : next;
}
