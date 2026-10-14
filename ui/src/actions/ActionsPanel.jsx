import { useEffect, useState } from "preact/hooks";
import { getJson, postJson, postQuery } from "../api.js";
import { ConditionTree, EditorRows, blankAction, blankTrigger } from "./Editor.jsx";
import { summarize } from "./summarize.js";

function grouped(rules) {
  const groups = [];
  const index = new Map();
  for (const rule of rules) {
    const name = (rule.group && String(rule.group).trim()) || "Ungrouped";
    if (!index.has(name)) {
      index.set(name, groups.length);
      groups.push({ name, rules: [] });
    }
    groups[index.get(name)].rules.push(rule);
  }
  return groups;
}

export function ActionsPanel() {
  const [data, setData] = useState({ automations: [] });
  const [error, setError] = useState("");
  const [editing, setEditing] = useState(null);
  const [busyGroup, setBusyGroup] = useState("");

  async function refresh() {
    setData(await getJson("/api/v1/actions"));
    setError("");
  }

  useEffect(() => {
    refresh().catch((err) => setError(String(err.message || err)));
  }, []);

  function openEditor(rule) {
    setEditing({
      id: rule ? rule.id : 0,
      name: rule && rule.name ? rule.name : "",
      group: rule && rule.group ? rule.group : "",
      on: rule ? !!rule.on : true,
      builtin: rule && rule.builtin ? rule.builtin : "",
      triggers: rule && rule.triggers && rule.triggers.length ? rule.triggers : [blankTrigger()],
      condition: rule && rule.condition ? rule.condition : (rule && rule.conditions && rule.conditions.length ? { op: "and", items: rule.conditions } : null),
      actions: rule && rule.actions && rule.actions.length ? rule.actions : [blankAction()]
    });
    setError("");
  }

  async function setGroupOn(group, on) {
    setBusyGroup(group.name);
    try {
      let last = data;
      for (const rule of group.rules) {
        if (!!rule.on === on) continue;
        last = await postJson("/api/v1/actions", { ...rule, on });
      }
      setData(last);
      await refresh();
    } catch (err) {
      setError(err.message);
      refresh().catch(() => {});
    } finally {
      setBusyGroup("");
    }
  }

  const rules = data.automations || [];
  const groups = grouped(rules);

  return (
    <div class="panel actions-panel">
      <div class="actions-head">
        <p class="actions"><button type="button" onClick={() => openEditor(null)}>Add action</button></p>
      </div>
      <p id="error">{error}</p>
      {groups.length ? groups.map((group) => {
        const onCount = group.rules.filter((rule) => rule.on).length;
        const allOn = onCount === group.rules.length;
        const noneOn = onCount === 0;
        return (
          <section class={"action-group" + (noneOn ? " off" : "")} key={group.name}>
            <header class="action-group-head">
              <h2>{group.name}</h2>
              <small>{group.rules.length}</small>
              <label class="switch" title={allOn ? "Turn this group off" : "Turn this group on"}>
                <input
                  type="checkbox"
                  checked={allOn}
                  disabled={busyGroup === group.name}
                  ref={(el) => { if (el) el.indeterminate = !allOn && !noneOn; }}
                  onChange={() => setGroupOn(group, !allOn)}
                />
                <span />
              </label>
            </header>
            <div class="action-grid">
              {group.rules.map((rule) => (
                <article class={"card" + (rule.on ? "" : " off")} key={rule.id}>
                  <label class="switch">
                    <input type="checkbox" checked={!!rule.on} onChange={(event) => {
                      postJson("/api/v1/actions", { ...rule, on: event.target.checked }).then(setData).catch((err) => {
                        event.target.checked = !event.target.checked;
                        setError(err.message);
                      });
                    }} />
                    <span />
                  </label>
                  <div class="body">
                    <strong>{rule.name || ("Action " + rule.id)}</strong>
                    <small class="meta">{rule.builtin ? "Built-in · " : ""}{summarize(rule)}</small>
                    <div class="actions">
                      <button type="button" class="ghost" onClick={() => postQuery("/api/v1/actions/run?id=" + rule.id).catch((err) => setError(err.message))}>Run</button>
                      <button type="button" class="ghost" onClick={() => openEditor(rule)}>Edit</button>
                      <button type="button" class="warn" onClick={() => {
                        if (rule.builtin && !window.confirm("This is a built-in action. Delete it? It will come back after the next reboot.")) return;
                        postQuery("/api/v1/actions/delete?id=" + rule.id).then(setData).catch((err) => setError(err.message));
                      }}>Delete</button>
                    </div>
                  </div>
                </article>
              ))}
            </div>
          </section>
        );
      }) : <p class="empty">No actions yet.</p>}
      <div class={"overlay" + (editing ? " open" : "")} onClick={(event) => { if (event.currentTarget === event.target) setEditing(null); }}>
        {editing ? (
          <div class="sheet" onClick={(event) => event.stopPropagation()}>
            <h2>{editing.id ? "Edit action" : "Add action"}</h2>
            <div class="grid">
              <label class="field">Name
                <input type="text" maxlength="80" placeholder="Optional" value={editing.name} onInput={(event) => setEditing({ ...editing, name: event.target.value })} />
              </label>
              <label class="field">Group
                <input type="text" maxlength="40" placeholder="e.g. TRS Looper" value={editing.group} onInput={(event) => setEditing({ ...editing, group: event.target.value })} />
              </label>
            </div>
            <h2>When</h2>
            <EditorRows group="triggers" items={editing.triggers} onChange={(triggers) => setEditing({ ...editing, triggers })} blank={blankTrigger} />
            <h2>And if</h2>
            <p class="hint">Leave empty to always run. Groups can nest.</p>
            <ConditionTree node={editing.condition} onChange={(condition) => setEditing({ ...editing, condition })} />
            <h2>Then</h2>
            <EditorRows group="actions" items={editing.actions} onChange={(actions) => setEditing({ ...editing, actions })} blank={blankAction} />
            <p class="actions">
              <button type="button" onClick={() => {
                postJson("/api/v1/actions", editing).then((next) => { setEditing(null); setData(next); }).catch((err) => setError(err.message));
              }}>Save</button>
              <button type="button" class="ghost" onClick={() => setEditing(null)}>Cancel</button>
            </p>
          </div>
        ) : null}
      </div>
    </div>
  );
}
