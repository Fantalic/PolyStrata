// Chat UI: messages, streaming, tools, swipe/edit, attachments and composer events.
import { $, toast, copyText, esc, icon, fmt, kfmt, headers, store, markdown,
         samplingUntouched, DEFAULTS, swipeInit, swipeSnapshot, swipeSave, swipeLoad,
         projectionLoaded, state } from "./runtime.js";

export function saveChat() {
  store.set("chat", state.messages.map((m) => ({...m, images: (m.images || []).map((i) => ({name: i.name})),
                                           files: (m.files || []).map((f) => ({name: f.name}))})));
}
function timeStr(t) { return new Date(t).toLocaleTimeString([], {hour: "2-digit", minute: "2-digit"}); }

// The streaming itself, shared by send(), regenerateMessage() and editUserMessage(). On abort it sets
// m.stopped and returns normally (so the caller can still save the partial answer); on any other error it
// rethrows.
async function streamInto(m, el, signal) {
  const body = {model: state.health.model, messages: apiMessages(), stream: true, reasoning_effort: state.settings.thinking};
  if (!(state.settings.temperature > 0)) {
    body.temperature = 0;
  } else if (!samplingUntouched(state.settings)) {
    Object.assign(body, {temperature: +state.settings.temperature, top_p: +state.settings.top_p, top_k: +state.settings.top_k});
  }
  if (state.settings.seed) body.seed = +state.settings.seed;
  if (state.settings.max) body.max_tokens = +state.settings.max;
  if (projectionLoaded()) body.experimental_speed_projection = !!state.settings.esp;
  if (state.settings.mcp !== false && state.mcpInfo.tools > 0) body.strata_mcp = true;

  let firstAt = null, thinkStart = null, usage = null, frame = 0;
  const paint = () => { frame = 0; updateAssistant(el, m, true); scrollDown(); };
  try {
    const r = await fetch("v1/chat/completions", {method: "POST", headers: headers(true),
                                                   body: JSON.stringify(body), signal});
    if (!r.ok) {
      let msg = `HTTP ${r.status}`;
      try { msg = (await r.json()).error.message || msg; } catch (e) { /* not json */ }
      if (r.status === 401) msg = "This server needs an API key: add it under About > Settings.";
      throw new Error(msg);
    }
    const reader = r.body.getReader(), dec = new TextDecoder();
    let buf = "";
    for (;;) {
      const {value, done} = await reader.read();
      if (done) break;
      buf += dec.decode(value, {stream: true});
      let nl;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, nl).trim();
        buf = buf.slice(nl + 1);
        if (!line.startsWith("data:")) continue;              // ": keep-alive" comments while a long prompt is read
        const data = line.slice(5).trim();
        if (data === "[DONE]") continue;
        let j;
        try { j = JSON.parse(data); } catch (e) { continue; }
        if (j.error) throw new Error(j.error.message || "the engine reported an error");
        if (j.usage) usage = j.usage;
        if (j.strata_mcp) onTool(m, j.strata_mcp);
        const d = (j.choices && j.choices[0] && j.choices[0].delta) || {};
        const lastTool = m.tools && m.tools.length ? m.tools[m.tools.length - 1] : null;   // a new round after a tool
        if (d.reasoning_content) {
          if (!firstAt) firstAt = performance.now();
          if (!thinkStart) thinkStart = performance.now();
          if (lastTool && m.reasoning && lastTool.rat === m.reasoning.length) m.reasoning += "\n\n";
          m.reasoning += d.reasoning_content;
        }
        if (d.content) {
          if (!firstAt) firstAt = performance.now();
          if (thinkStart && m.thinkSecs == null) m.thinkSecs = (performance.now() - thinkStart) / 1000;
          if (lastTool && m.text && lastTool.at === m.text.length) m.text += "\n\n";
          m.text += d.content;
        }
        if (!frame) frame = requestAnimationFrame(paint);
      }
    }
  } catch (e) {
    if (e.name === "AbortError") {
      m.stopped = true;
    } else {
      if (thinkStart && m.thinkSecs == null) m.thinkSecs = (performance.now() - thinkStart) / 1000;
      if (frame) cancelAnimationFrame(frame);
      throw e;
    }
  }
  if (thinkStart && m.thinkSecs == null) m.thinkSecs = (performance.now() - thinkStart) / 1000;
  if (frame) cancelAnimationFrame(frame);
  const n = usage ? usage.completion_tokens : null;
  if (n && firstAt) {
    const secs = (performance.now() - firstAt) / 1000;
    m.meta = `${fmt(n)} tokens${secs > 0.25 ? ` · ${fmt(n / secs, 1)} tok/s` : ""}${m.stopped ? " · stopped" : ""}` +
             (projectionLoaded() ? (state.settings.esp ? " · projection on" : " · projection off") : "");
  } else if (m.stopped) {
    m.meta = "Stopped";
  }
  for (const t of m.tools || []) if (t.state === "writing" || t.state === "running") { t.state = "skipped"; t.ms = null; }
  const ran = (m.tools || []).filter((t) => t.state === "done" || t.state === "error").length;
  if (ran) m.meta = `${m.meta ? `${m.meta} · ` : ""}${ran} tool call${ran > 1 ? "s" : ""}`;
  if (m.limit) m.meta = `${m.meta || ""} · stopped at the limit of ${m.limit} tool rounds (mcp.max_rounds)`;
  updateAssistant(el, m, false);
}

// Regenerates the answer at state.messages[i]: keeps the previous one as a variant, discards any variants after
// the current one, streams a new answer into the same message. On failure the previous answer is restored.
async function regenerateMessage(i) {
  if (state.busy) { toast("warn", "Still writing", "Stop the answer first."); return; }
  const m = state.messages[i];
  if (!m || m.role !== "assistant") return;
  const el = $("chat").querySelector(`.st-msg[data-i="${i}"]`);
  if (!el) return;
  swipeInit(m);
  swipeSave(m);
  m.swipes.length = m.swipeIndex + 1;          // a new variant discards the ones after the current one
  const keep = swipeSnapshot(m);
  m.text = ""; m.reasoning = ""; m.tools = null; m.meta = null;
  m.thinkSecs = null; m.stopped = false; m.error = null; m.limit = null;
  const controller = new AbortController();
  state.busy = {controller, msg: m, cutoff: i};
  if (i < state.messages.length - 1) {
    toast("info", "Regenerating an earlier message",
          `${state.messages.length - i - 1} later message(s) stay in the chat, and the next request will still see them.`,
          7000);
  }
  setBusy(true);
  updateAssistant(el, m, true);
  try {
    await streamInto(m, el, controller.signal);
    m.swipes.push(swipeSnapshot(m));
    m.swipeIndex = m.swipes.length - 1;
  } catch (e) {
    m.error = e.message || String(e);
    toast("error", "Regeneration failed", m.error, 6000);
    Object.assign(m, keep);
    m.swipes[m.swipeIndex] = keep;
  }
  state.busy = null;
  setBusy(false);
  updateAssistant(el, m, false);
  saveChat();
  scrollDown();
}

// Moves between variants: saves the current answer, loads the neighbour.
function swipeGo(i, delta) {
  if (state.busy) return;
  const m = state.messages[i];
  if (!m || m.role !== "assistant") return;
  swipeInit(m);
  const n = m.swipes.length;
  if (n < 2) return;
  swipeSave(m);
  m.swipeIndex = (m.swipeIndex + delta + n) % n;
  swipeLoad(m);
  const el = $("chat").querySelector(`.st-msg[data-i="${i}"]`);
  if (el) updateAssistant(el, m, false);
  saveChat();
}

// Edits a user message in place. Variant B: saving removes every message after it, then immediately asks
// the model for a fresh answer against the new wording. The old tail is kept aside for Undo.
function editUserMessage(i) {
  if (state.busy) { toast("warn", "Still writing", "Stop the answer first."); return; }
  const m = state.messages[i];
  if (!m || m.role !== "user") return;
  const el = $("chat").querySelector(`.st-msg[data-i="${i}"]`);
  if (!el) return;
  const bubble = el.querySelector(".st-bubble");
  if (!bubble || el.querySelector(".edit-box")) return;

  const original = m.text || "";
  const box = document.createElement("textarea");
  box.className = "edit-box st-input";
  box.value = original;
  box.rows = Math.min(20, Math.max(2, original.split("\n").length));

  const actions = document.createElement("div");
  actions.className = "edit-actions";
  const save = document.createElement("button");
  save.className = "st-btn st-btn--primary";
  save.textContent = "Save";
  const cancel = document.createElement("button");
  cancel.className = "st-btn st-btn--secondary";
  cancel.textContent = "Cancel";

  actions.append(save, cancel);
  bubble.replaceWith(box);
  box.after(actions);
  box.focus();
  box.setSelectionRange(box.value.length, box.value.length);

  const close = () => { box.replaceWith(bubble); actions.remove(); };
  cancel.onclick = close;

  save.onclick = () => {
    const next = box.value.trim();
    if (next === original) { close(); return; }

    // Variant B: keep the tail aside for Undo, then drop it
    const dropped = state.messages.slice(i + 1);
    m.text = next;
    state.messages = state.messages.slice(0, i + 1);
    state.editUndo = dropped.length ? {at: i + 1, items: dropped, wasText: original} : null;
    close();

    // append a fresh assistant placeholder and start generation right away
    const a = {role: "assistant", text: "", reasoning: "", time: Date.now()};
    state.messages.push(a);
    saveChat();
    renderChat();

    const el2 = $("chat").lastElementChild;
    const controller = new AbortController();
    state.busy = {controller, msg: a};
    setBusy(true);

    (async () => {
      try {
        await streamInto(a, el2, controller.signal);
      } catch (e) {
        a.error = e.message || String(e);
        toast("error", "The request failed", a.error, 6000);
      }
      state.busy = null;
      setBusy(false);
      updateAssistant(el2, a, false);
      saveChat();
      scrollDown();
    })();

    if (state.editUndo) {
      toast("info", "Edited",
            `${dropped.length} later message(s) removed; writing a new answer.`,
            8000, {label: "Undo", run: () => {
              if (state.busy) state.busy.controller.abort();
              if (!state.editUndo) return;
              state.messages[i].text = state.editUndo.wasText;
              state.messages = state.messages.slice(0, state.editUndo.at).concat(state.editUndo.items);
              state.editUndo = null;
              saveChat();
              renderChat();
            }});
    } else {
      toast("success", "Edited", "");
    }
  };
}

function msgEl(m, i) {
  const el = document.createElement("div");
  el.className = `st-msg st-msg--${m.role}`;
  el.dataset.i = i;
  if (m.role === "user") {
    if (m.files && m.files.length) {
      const wrap = document.createElement("div");
      wrap.className = "msg-images";
      for (const f of m.files) {
        const c = document.createElement("span");
        c.className = "chip";
        c.innerHTML = icon("attach", "st-icon st-icon--sm");
        c.append(f.name);
        wrap.appendChild(c);
      }
      el.appendChild(wrap);
    }
    if (m.images && m.images.length) {
      const wrap = document.createElement("div");
      wrap.className = "msg-images";
      for (const im of m.images) {
        if (im.url) { const img = document.createElement("img"); img.src = im.url; img.alt = im.name || "image"; wrap.appendChild(img); }
        else { const c = document.createElement("span"); c.className = "chip"; c.innerHTML = icon("image", "st-icon st-icon--sm"); c.append(im.name || "image"); wrap.appendChild(c); }
      }
      el.appendChild(wrap);
    }
    const b = document.createElement("div");
    b.className = "st-bubble";
    b.textContent = m.text;
    el.appendChild(b);
    const meta = document.createElement("div");
    meta.className = "st-msg__meta";
    const t = document.createElement("span");
    t.textContent = `You · ${timeStr(m.time)}`;
    meta.appendChild(t);
    const edit = document.createElement("button");
    edit.className = "st-btn st-btn--icon";
    edit.dataset.msgEdit = "";
    edit.setAttribute("aria-label", "Edit");
    edit.title = "Edit (removes the state.messages after it and writes a new answer)";
    edit.textContent = "✎";
    meta.appendChild(edit);
    el.appendChild(meta);
  } else {
    el.innerHTML = `<details class="st-collapse think" hidden><summary>${icon("thinking", "st-icon st-icon--sm")}<span class="think-title"></span>` +
      `${icon("chevron", "st-icon st-icon--sm st-chev")}</summary><div class="st-collapse__body thinking"></div></details>` +
      `<div class="st-bubble"></div><div class="st-msg__meta"><span class="meta-text"></span>` +
      `<button class="st-btn st-btn--icon" data-msg-regen aria-label="Regenerate" title="Regenerate">↻</button>` +
      `<button class="st-btn st-btn--icon" data-msg-prev aria-label="Previous variant" title="Previous variant">‹</button>` +
      `<span class="swipe-count" data-swipe-count></span>` +
      `<button class="st-btn st-btn--icon" data-msg-next aria-label="Next variant" title="Next variant">›</button>` +
      `<button class="st-btn st-btn--icon" data-msg-copy aria-label="Copy the answer" title="Copy">${icon("copy")}</button></div>`;
    updateAssistant(el, m, false);
  }
  return el;
}
// One MCP tool call in the answer: a compact block (name, state, a one-line preview) that opens to the arguments and
// the result as the model read it.  Its body is built only while open: a result can be 20,000 characters.
const TOOL_STATE = {writing: ["st-badge--reading", "Writing"], running: ["st-badge--generating", "Running"], done: ["", "Done"],
                    error: ["st-badge--error", "Error"], skipped: ["st-badge--queued", "Not run"]};
function toolHtml(t, k) {
  const [cls, label] = TOOL_STATE[t.state] || ["", t.state];
  const args = t.arguments == null ? "" : JSON.stringify(t.arguments, null, 2);
  const preview = t.result != null ? t.result : args.replace(/\s+/g, " ");
  let body = "";
  if (t.open) {
    body = `<div class="tool-call__label">Arguments</div><pre class="tool-call__pre">${esc(args || "(being written)")}</pre>`;
    if (t.result != null) {
      body += `<div class="tool-call__label">${t.ok ? "Result" : "Error"}${t.chars ? ` · ${fmt(t.chars)} characters` : ""}` +
              `${t.truncated ? ", cut for the model" : ""}</div><pre class="tool-call__pre">${esc(t.result)}</pre>`;
    }
  }
  return `<details class="st-collapse tool-call" data-tool="${k}" data-state="${esc(t.state)}"${t.open ? " open" : ""}>` +
    `<summary>${icon("tool", "st-icon st-icon--sm")}<span class="tool-call__name" title="${esc(t.name || "")}">${esc(t.tool || t.name || "tool")}</span>` +
    (t.server ? `<span class="muted small">${esc(t.server)}</span>` : "") +
    `<span class="tool-call__preview muted">${esc(preview.slice(0, 200))}</span>` +
    `<span class="st-badge ${cls}">${esc(label)}</span>${t.ms != null && t.state !== "skipped" ? `<span class="muted small">${fmt(t.ms / 1000, 1)} s</span>` : ""}` +
    `${icon("chevron", "st-icon st-icon--sm st-chev")}</summary><div class="st-collapse__body">${body}</div></details>`;
}
// the answer's text with the tool blocks where the model called them
function answerHtml(m) {
  if (!m.tools || !m.tools.length) return markdown(m.text || "");
  let html = "", pos = 0;
  m.tools.forEach((t, k) => {
    const at = Math.min(Math.max(t.at || 0, pos), m.text.length);
    if (at > pos) html += markdown(m.text.slice(pos, at));
    pos = at;
    html += toolHtml(t, k);
  });
  return html + markdown(m.text.slice(pos));
}
// a tool event from the stream (the `strata_mcp` field of a chunk)
function onTool(m, x) {
  if (x.event === "limit") { m.limit = x.max_rounds; return; }
  m.tools = m.tools || [];
  let t = m.tools.find((y) => y.id === x.id);
  if (!t) { t = {id: x.id, name: x.name, at: m.text.length, rat: m.reasoning.length, state: "writing"}; m.tools.push(t); }
  if (x.event === "call") {
    Object.assign(t, {name: x.name, server: x.server, tool: x.tool, arguments: x.arguments, round: x.round, state: "running"});
  } else if (x.event === "result") {
    Object.assign(t, {result: x.text, ok: x.ok, chars: x.chars, truncated: x.truncated, ms: x.ms,
                      state: x.skipped ? "skipped" : x.ok ? "done" : "error"});
  }
}
function updateAssistant(el, m, streaming) {
  const det = el.querySelector("details.think");
  if (m.reasoning) {
    det.hidden = false;
    const thinkingNow = streaming && !m.text;
    el.querySelector(".think-title").textContent = thinkingNow ? "Thinking…" :
      m.thinkSecs != null ? `Thought for ${fmt(m.thinkSecs, 1)} s` : "Thoughts";
    const body = el.querySelector(".thinking");
    if (det.open || thinkingNow) body.textContent = m.reasoning;
    else body.dataset.pending = "1";
    // open while it streams (if wanted), closed once the answer starts - unless the user toggled it themselves
    if (thinkingNow && state.settings.show && !det.dataset.touched && !det.open) { det._auto = true; det.open = true; }
    if (!thinkingNow && det.open && !det.dataset.touched) { det._auto = true; det.open = false; }
  }
  const bubble = el.querySelector(".st-bubble");
  if (m.error) {
    bubble.innerHTML = `<div class="msg-error"></div>`;
    bubble.firstChild.textContent = m.error;
  } else if (!m.text && streaming && !(m.tools && m.tools.length)) {
    bubble.innerHTML = m.reasoning ? `<span class="muted cursor">Writing</span>` : `<span class="cursor"></span>`;
  } else {
    bubble.innerHTML = answerHtml(m);
    if (streaming) bubble.classList.add("cursor"); else bubble.classList.remove("cursor");
  }
  el.querySelector(".meta-text").textContent = m.meta || (streaming ? "" : m.stopped ? "Stopped" : "");
  el.querySelector("[data-msg-copy]").hidden = streaming || !m.text;
  // swipe controls: regenerate disabled while streaming; prev/next shown when there is more than one variant
  const rg = el.querySelector("[data-msg-regen]");
  const pv = el.querySelector("[data-msg-prev]");
  const nx = el.querySelector("[data-msg-next]");
  const cnt = el.querySelector("[data-swipe-count]");
  const n = Array.isArray(m.swipes) ? m.swipes.length : 1;
  const idx = m.swipeIndex || 0;
  if (rg) rg.disabled = streaming;
  if (pv) pv.hidden = streaming || n < 2;
  if (nx) nx.hidden = streaming || n < 2;
  if (cnt) { cnt.hidden = streaming || n < 2; cnt.textContent = n > 1 ? `${idx + 1}/${n}` : ""; }
}
export function renderChat() {
  const chat = $("chat");
  chat.querySelectorAll(".st-msg").forEach((e) => e.remove());
  $("chat-empty").hidden = state.messages.length > 0;
  state.messages.forEach((m, i) => chat.appendChild(msgEl(m, i)));
  scrollDown(true);
}
function nearBottom() { const s = $("chat-scroll"); return s.scrollHeight - s.scrollTop - s.clientHeight < 120; }
function scrollDown(force) { const s = $("chat-scroll"); if (force || nearBottom()) s.scrollTop = s.scrollHeight; }

$("chat").addEventListener("click", (e) => {
  const ed = e.target.closest("[data-msg-edit]");
  if (ed) { editUserMessage(+ed.closest(".st-msg").dataset.i); return; }
  const rg = e.target.closest("[data-msg-regen]");
  if (rg) { regenerateMessage(+rg.closest(".st-msg").dataset.i); return; }
  const pv = e.target.closest("[data-msg-prev]");
  if (pv) { swipeGo(+pv.closest(".st-msg").dataset.i, -1); return; }
  const nx = e.target.closest("[data-msg-next]");
  if (nx) { swipeGo(+nx.closest(".st-msg").dataset.i, +1); return; }
  const cc = e.target.closest("[data-code-copy]");
  if (cc) { copyText(cc.closest(".st-code").querySelector("pre").textContent, cc); return; }
  const mc = e.target.closest("[data-msg-copy]");
  if (mc) { const i = +mc.closest(".st-msg").dataset.i; copyText(state.messages[i].text, mc); return; }
  // a tool block: its open state lives in the message (the answer is rebuilt while it streams), so the click sets it
  const sum = e.target.closest(".tool-call > summary");
  if (sum) {
    e.preventDefault();
    const el = sum.closest(".st-msg"), m = state.messages[+el.dataset.i], t = m && m.tools && m.tools[+sum.parentElement.dataset.tool];
    if (!t) return;
    t.open = !t.open;
    updateAssistant(el, m, !!state.busy && state.busy.msg === m);
  }
});
$("chat").addEventListener("toggle", (e) => {
  const d = e.target;
  if (d.tagName !== "DETAILS" || !d.classList.contains("think")) return;
  if (d._auto) { d._auto = false; return; }          // our own open/close, not the user's
  d.dataset.touched = "1";
  const body = d.querySelector(".thinking");
  if (d.open && body.dataset.pending) { body.textContent = state.messages[+d.closest(".st-msg").dataset.i].reasoning; delete body.dataset.pending; }
}, true);

function apiMessages() {
  const out = [];
  const end = state.busy && state.busy.cutoff != null ? state.busy.cutoff + 1 : state.messages.length;
  for (let k = 0; k < end; k++) {
    const m = state.messages[k];
    if (m.role === "user") {
      const imgs = (m.images || []).filter((i) => i.url);
      const text = userText(m);
      out.push({role: "user", content: imgs.length ? [{type: "text", text},
        ...imgs.map((i) => ({type: "image_url", image_url: {url: i.url}}))] : text});
    } else if (!(state.busy && state.busy.msg === m)) {            // the answer being asked for now is not history yet
      out.push(...assistantMessages(m));
    }
  }
  return out;
}
// An answer that used MCP tools goes back as the model wrote it: per round the text before the calls, the calls and
// their results (as the model read them), then the rest - so the next question can build on what the tools found.
function assistantMessages(m) {
  const ran = (m.tools || []).filter((t) => t.round != null && t.result != null && t.state !== "skipped");
  // #1392: a turn with no answer text (only reasoning, a stop before the first content token, or an error) still goes
  // back as an assistant turn, so the history keeps alternating and a reasoning model sees that it already answered.
  // The server leaves such a turn out of the prompt itself (serve/frontend.py, #843); reasoning_content rides along.
  if (!ran.length) {
    const msg = {role: "assistant", content: m.text || ""};
    if (!m.text && m.reasoning) msg.reasoning_content = m.reasoning;
    return [msg];
  }
  const out = [];
  let pos = 0;
  for (const r of [...new Set(ran.map((t) => t.round))]) {
    const calls = ran.filter((t) => t.round === r);
    const at = Math.min(Math.max(pos, calls[0].at || 0), m.text.length);
    out.push({role: "assistant", content: m.text.slice(pos, at).trim(),
              tool_calls: calls.map((t) => ({id: t.id, type: "function", function: {name: t.name, arguments: JSON.stringify(t.arguments || {})}}))});
    for (const t of calls) out.push({role: "tool", tool_call_id: t.id, content: t.result});
    pos = at;
  }
  const rest = m.text.slice(pos).trim();
  if (rest) out.push({role: "assistant", content: rest});
  return out;
}

export function setBusy(on) {
  $("stop-btn").hidden = !on;
  $("send-btn").disabled = on;
  $("composer-hint").textContent = on ? "" : "Shift+Enter: new line";
}

export async function send() {
  const text = $("input").value.trim();
  if ((!text && !state.attachments.length) || state.busy) return;
  state.messages.push({role: "user", text, images: state.attachments.filter((a) => a.kind !== "file"),
                 files: state.attachments.filter((a) => a.kind === "file"), time: Date.now()});
  state.attachments = [];
  renderAttachments();
  $("input").value = "";
  autosize();
  const m = {role: "assistant", text: "", reasoning: "", time: Date.now()};
  state.messages.push(m);
  renderChat();
  const el = $("chat").lastElementChild;
  const controller = new AbortController();
  state.busy = {controller, msg: m};
  setBusy(true);
  try {
    await streamInto(m, el, controller.signal);
  } catch (e) {
    m.error = e.message || String(e);
    toast("error", "The request failed", m.error, 6000);
    updateAssistant(el, m, false);
  }
  state.busy = null;
  setBusy(false);
  updateAssistant(el, m, false);
  saveChat();
  scrollDown();
}

$("composer").onsubmit = (e) => { e.preventDefault(); send(); };
$("stop-btn").onclick = () => { if (state.busy) state.busy.controller.abort(); };
$("input").addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey && !e.isComposing) { e.preventDefault(); send(); }
});
function autosize() { const t = $("input"); t.style.height = "auto"; t.style.height = `${Math.min(t.scrollHeight, innerHeight * 0.4)}px`; }
$("input").addEventListener("input", autosize);

$("new-btn").onclick = () => {
  if (state.busy) { toast("warn", "Still writing", "Stop the answer first."); return; }
  if (!state.messages.length) return;
  const backup = state.messages;
  state.messages = [];
  saveChat();
  renderChat();
  toast("info", "New chat", "The last one was cleared.", 6000, {label: "Undo", run: () => { state.messages = backup; saveChat(); renderChat(); }});
};
$("export-btn").onclick = () => {
  if (!state.messages.length) { toast("info", "Nothing to save yet"); return; }
  const tools = (m) => (m.tools || []).filter((t) => t.result != null).map((t) =>
    `<details><summary>Tool ${t.server ? `${t.server} / ` : ""}${t.tool || t.name}${t.ok ? "" : " (error)"}</summary>\n\n` +
    `\`\`\`json\n${JSON.stringify(t.arguments || {}, null, 2)}\n\`\`\`\n\n\`\`\`\n${t.result}\n\`\`\`\n\n</details>\n\n`).join("");
  const md = state.messages.map((m) => m.role === "user" ? `## You\n\n${m.text}\n` :
    `## ${state.health.model}\n\n${m.reasoning ? `<details><summary>Thinking</summary>\n\n${m.reasoning}\n\n</details>\n\n` : ""}${tools(m)}${m.text || m.error || ""}\n`).join("\n");
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([md], {type: "text/markdown"}));
  a.download = `strata-chat-${new Date().toISOString().slice(0, 16).replace(/[:T]/g, "-")}.md`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
};

// pictures and text files: the attach button, dropping them on the chat, or pasting a picture (issue #30)
const TEXT_EXT = /\.(txt|md|markdown|rst|tex|py|pyi|ipynb|js|mjs|cjs|ts|tsx|jsx|vue|svelte|json|jsonl|csv|tsv|log|ya?ml|toml|ini|cfg|conf|env|xml|html?|css|scss|less|c|cc|cpp|cxx|h|hh|hpp|cu|cuh|rs|go|java|kt|kts|swift|rb|php|pl|lua|r|jl|scala|sql|sh|bash|zsh|fish|ps1|psm1|bat|cmd|diff|patch|gradle|cmake|mk|dockerfile|gitignore|proto|graphql)$/i;
const MAX_TEXT_FILE = 512 * 1024;
function isTextFile(f) {
  return f.type.startsWith("text/") || /json|xml|javascript|yaml|toml|x-sh|x-python/.test(f.type) ||
         TEXT_EXT.test(f.name) || /(^|[\\/])(makefile|dockerfile|readme|license)$/i.test(f.name);
}
function addFiles(files) {
  for (const f of files) {
    if (f.type.startsWith("image/")) {
      if (!state.health.images) { toast("warn", "Pictures are off", "This model was set up for text only."); continue; }
      if (f.size > 20e6) { toast("warn", "Picture too large", `${f.name} is over 20 MB.`); continue; }
      const r = new FileReader();
      r.onload = () => { state.attachments.push({kind: "image", name: f.name || "pasted image", url: r.result}); renderAttachments(); };
      r.readAsDataURL(f);
      continue;
    }
    if (!isTextFile(f)) { toast("warn", "Not a text file", `${f.name}: attach text files (code, notes, logs, data)${state.health.images ? " or pictures" : ""}.`); continue; }
    if (f.size > MAX_TEXT_FILE) { toast("warn", "File too large", `${f.name} is over 512 KB.`); continue; }
    const r = new FileReader();
    r.onload = () => {
      const text = String(r.result);
      if (text.includes("\u0000")) { toast("warn", "Not a text file", `${f.name} looks like a binary file.`); return; }
      state.attachments.push({kind: "file", name: f.name, text});
      renderAttachments();
    };
    r.readAsText(f);
  }
}
// a file's text in the message, fenced with more backticks than it contains itself
function fileBlock(f) {
  const longest = Math.max(2, ...(f.text.match(/`+/g) || []).map((s) => s.length));
  const fence = "`".repeat(longest + 1);
  return `File: ${f.name}\n${fence}\n${f.text}\n${fence}`;
}
function userText(m) {
  const files = (m.files || []).filter((f) => f.text != null);
  return [m.text, ...files.map(fileBlock)].filter((s) => s).join("\n\n");
}
function renderAttachments() {
  const box = $("attachments");
  box.hidden = !state.attachments.length;
  box.innerHTML = "";
  state.attachments.forEach((a, i) => {
    const c = document.createElement("span");
    c.className = "chip";
    c.innerHTML = icon(a.kind === "file" ? "attach" : "image", "st-icon st-icon--sm");
    c.append(a.name);
    const x = document.createElement("button");
    x.type = "button"; x.className = "st-btn st-btn--icon"; x.setAttribute("aria-label", "Remove");
    x.innerHTML = icon("trash");
    x.onclick = () => { state.attachments.splice(i, 1); renderAttachments(); };
    c.appendChild(x);
    box.appendChild(c);
  });
}
$("attach-btn").onclick = () => $("file").click();
// drop files on the chat or the message box
for (const id of ["chat", "composer"]) {
  const el = $(id);
  el.addEventListener("dragover", (e) => {
    if (![...(e.dataTransfer || {}).types || []].includes("Files")) return;
    e.preventDefault();
    $("composer").classList.add("dragging");
  });
  el.addEventListener("dragleave", () => $("composer").classList.remove("dragging"));
  el.addEventListener("drop", (e) => {
    $("composer").classList.remove("dragging");
    if (!e.dataTransfer || !e.dataTransfer.files.length) return;
    e.preventDefault();
    addFiles(e.dataTransfer.files);
    $("input").focus();
  });
}
$("file").onchange = () => { addFiles($("file").files); $("file").value = ""; };
$("input").addEventListener("paste", (e) => {
  if (!state.health.images) return;
  const files = [...(e.clipboardData || {}).files || []].filter((f) => f.type.startsWith("image/"));
  if (files.length) { e.preventDefault(); addFiles(files); }
});