// Monitor dashboard and the single-request queue, both fed by this server's own endpoints.
import { $, toast, copyText, esc, icon, fmt, kfmt, ctxfmt, gb, headers, state } from "./runtime.js";
import { loadMcp } from "./mcp.js";
import { renderSessions } from "./sessions.js";

// ------------------------------------------------------------------ Monitor
const METRICS = [
  {key: "speed", label: "Speed", icon: "gauge", unit: "t/s", series: "tok_s"},
  {key: "gpu", label: "GPU load", icon: "gpu", unit: "%", series: "gpu_util", max: 100},
  {key: "vram", label: "VRAM", icon: "layers", unit: "GB", series: "gpu_mem_used"},
  {key: "temp", label: "GPU temp", icon: "thermometer", unit: "°C", series: "gpu_temp", tone: "warn"},
  {key: "power", label: "Power", icon: "bolt", unit: "W", series: "gpu_power"},
  {key: "pcie", label: "PCIe", icon: "link", unit: "", series: "gpu_pcie_rx_mb", tone: "info"},
  {key: "cpu", label: "CPU", icon: "cpu", unit: "%", series: "cpu", max: 100},
  {key: "disk", label: "Disk read", icon: "disk", unit: "MB/s", series: "disk_read_mb", tone: "info"},
];
$("metrics").innerHTML = METRICS.map((m) => `
  <div class="st-card metric-card"><div class="st-metric">
    <span class="st-metric__label">${icon(m.icon, "st-icon st-icon--sm")}${esc(m.label)}</span>
    ${m.key === "speed" ? `<div class="speed-values">
      <div><span class="st-metric__value" id="mv-speed">-</span><span class="st-metric__sub" id="ms-speed">Decode</span></div>
      <div class="speed-prefill"><span class="st-metric__value" id="mv-prefill">-</span><span class="st-metric__sub" id="ms-prefill">Prefill</span></div>
    </div>` : `<span class="st-metric__value" id="mv-${m.key}">–</span>
    <span class="st-metric__sub" id="ms-${m.key}"></span>`}
    <svg class="st-metric__spark" id="sp-${m.key}" viewBox="0 0 100 32" preserveAspectRatio="none"${m.tone ? ` data-tone="${m.tone}"` : ""}>
      <path class="area" fill="currentColor" opacity=".12"/><path class="line" fill="none" stroke="currentColor"
      stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round" vector-effect="non-scaling-stroke"/>
      ${m.key === "speed" ? `<g id="sp-prefill" class="speed-prefill"><path class="area" fill="currentColor" opacity=".12"/>
        <path class="line" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"
        stroke-linecap="round" vector-effect="non-scaling-stroke"/></g>` : ""}</svg>
  </div></div>`).join("");

function spark(id, values, max) {
  const svg = $(id);
  const v = (values || []).map((x) => (x == null ? 0 : x));
  if (v.length < 2) { svg.querySelector(".line").setAttribute("d", ""); svg.querySelector(".area").setAttribute("d", ""); return; }
  const top = Math.max(max || 0, ...v, 1);
  const pts = v.map((x, i) => [(i / (v.length - 1)) * 100, 30 - (x / top) * 26]);
  const line = pts.map((p, i) => `${i ? "L" : "M"}${p[0].toFixed(2)},${p[1].toFixed(2)}`).join("");
  svg.querySelector(".line").setAttribute("d", line);
  svg.querySelector(".area").setAttribute("d", `${line}L100,32L0,32Z`);
}
function setMetric(key, value, unit, sub) {
  $(`mv-${key}`).innerHTML = value == null ? "–" : `${esc(value)}${unit ? `<small>${esc(unit)}</small>` : ""}`;
  $(`ms-${key}`).textContent = sub || "";
}

export async function poll() {
  try {
    const r = await fetch(state.reqShowAll ? "metrics?requests=all" : "metrics", {headers: headers()});
    if (r.status === 401) {
      setPill("error", "API key needed");
      if (!state.keyWarned) { state.keyWarned = true; toast("warn", "API key needed", "This server needs a key: add it under About > Settings.", 6000); }
    } else if (r.ok) {
      state.lastMetrics = await r.json();
      state.metricsFailures = 0;
      render(state.lastMetrics);
    } else {
      throw new Error(`HTTP ${r.status}`);
    }
  } catch (e) {
    if (++state.metricsFailures === 3) setPill("error", "Server not reachable");
  }
  if (state.tab === "monitor" && ++state.mcpTick % 10 === 0) loadMcp();       // server states change rarely: every 10 s
  setTimeout(poll, 1000);
}

function setPill(state, text) {
  $("pill").dataset.state = state === "error" ? "queued" : state;
  $("pill-text").textContent = text;
}

export function render(m) {
  const live = m.live || {}, hw = m.hardware || {}, st = m.hardware_static || {}, eng = m.engine || {}, h = m.history || {};
  const last = (m.requests || [])[0];
  // the header pill
  if (live.state === "reading") {
    const pct = live.prompt_total ? Math.round((100 * live.prompt_read) / live.prompt_total) : null;
    setPill("reading", pct != null ? `Reading prompt · ${pct}%` : "Reading prompt");
  } else if (live.state === "generating") {
    setPill("generating", `Generating · ${fmt(live.tok_s, 1)} tok/s`);
  } else {
    setPill("idle", "Idle");
  }
  if (live.queued > 0) setPill("queued", `${live.queued} queued`);
  // The cache card is independent of the session list: render it even if another panel fails.
  if (state.tab === "monitor") renderConvCache(m.conversation_cache);
  if (state.tab === "monitor") renderMonitor(live, hw, st, eng, h, last, m.requests || [], m.totals, m.requests_kept);
  if (state.tab === "monitor") pollQueue();
  if (state.tab === "monitor") pollLCBadge();
  if (state.tab === "about") renderAbout(eng, hw, st);
  if (state.tab === "models") state.renderMemLive?.(m.conversation_cache);
}

// #596: the conversation cache - the prompt's state the engine keeps between requests (always), and the whole
// conversations it parks in RAM when "--conversation-cache-mib N" is in the run config's args (opt-in)
let ccBusy = false;
function since(t) {
  if (!t) return "";
  const s = Math.max(0, Date.now() / 1000 - t);
  return s < 60 ? "just now" : s < 3600 ? `${fmt(s / 60)} min ago` : `${fmt(s / 3600, 1)} h ago`;
}
function renderConvCache(c) {
  const pct = (a, b) => (b ? `${Math.min(100, (100 * a) / b)}%` : "0%");
  if (!c) {                                        // an older server: say so rather than showing an empty card
    $("cc-card").hidden = false;
    $("cc-bars").hidden = true;
    $("cc-sum").textContent = "";
    facts($("cc-facts"), [["Cache data", "This server does not expose conversation-cache data."]]);
    $("cc-note").textContent = "";
    const clear = $("cc-clear");
    if (clear) clear.disabled = true;
    return;
  }
  $("cc-card").hidden = false;
  $("cc-bars").hidden = !c.enabled;
  if (c.enabled) {
    $("cc-slots-text").textContent = `${fmt(c.parked)} / ${fmt(c.slots)}`;
    $("cc-slots-bar").style.width = pct(c.parked, c.slots);
    const budget = c.budget_mib * 1048576;
    $("cc-mem-text").textContent = `${gb(c.bytes)} / ${gb(budget)} GB`;
    $("cc-mem-bar").style.width = pct(c.bytes, budget);
  }
  $("cc-sum").textContent = c.requests ? `${fmt(c.requests_reused)} of ${fmt(c.requests)} requests reused part of their prompt` : "";
  const share = c.prompt_tokens ? ` (${fmt((100 * c.reused_tokens) / c.prompt_tokens)}% of all prompt tokens)` : "";
  const event = c.last_event ? `${c.last_event === "parked" ? "Parked" : "Restored"} ${fmt(c.last_tokens)} tokens, ${since(c.last_at)}` : null;
  const ccRows = [
    ["Last request", c.last_prompt != null ? `${fmt(c.last_reused || 0)} of ${fmt(c.last_prompt)} prompt tokens reused` : null],
    ["Reused since start", c.requests ? `${fmt(c.reused_tokens)} tokens${share}` : null],
    ["Parked / restored", c.enabled ? `${fmt(c.parks)} / ${fmt(c.restores)}${c.evictions ? ` · ${fmt(c.evictions)} evicted` : ""}` : null],
    ["Last switch", c.enabled ? event : null],
  ];
  if (!ccRows.some((r) => r[1] != null && r[1] !== "")) ccRows.push(["Cache data", "No conversation-cache activity recorded yet."]);
  facts($("cc-facts"), ccRows);
  $("cc-note").textContent = c.enabled
    ? "A request that continues a parked conversation gets its state back instead of reading it again; the oldest goes when the slots or the memory are full."
    : "The engine keeps the last conversation's state, so a follow-up reads only what is new. To keep several conversations (agents taking turns), add \"--conversation-cache-mib\", \"8192\" to the run config's args (docs/DETAILS.md).";
  const clear = $("cc-clear");
  if (clear) {
    clear.disabled = ccBusy || !c.enabled || !c.parked;
    clear.onclick = async () => {
      clear.disabled = true;
      try {
        const r = await fetch("/v1/cache/drop", {method: "POST", headers: headers(true), body: "{}"});
        const d = await r.json();
        if (r.ok && d.status === "dropped") {
          $("cc-clear-note").textContent = `Cleared ${fmt(d.dropped)} bank${d.dropped === 1 ? "" : "s"} (${gb(d.bytes)})`;
        } else {
          $("cc-clear-note").textContent = d.status === "not loaded" ? d.note : (d.error?.message || "could not clear");
        }
      } catch { $("cc-clear-note").textContent = "could not clear"; }
    };
  }
}

function renderTotals(t) {
  if (!t || !t.requests) return "";
  const since = new Date(t.since * 1000).toLocaleString([], {weekday: "short", hour: "2-digit", minute: "2-digit"});
  const read = t.prompt_tokens - t.reused;
  const pSpeed = t.prompt_ms > 0 && read > 0 ? ` at ${fmt(read / (t.prompt_ms / 1000))} tok/s` : "";
  const oSpeed = t.decode_ms > 0 && t.output_tokens > 0 ? ` at ${fmt(t.output_tokens / (t.decode_ms / 1000), 1)} tok/s` : "";
  return `Since ${since}: ${fmt(t.requests)} requests · ${fmt(read)} prompt tokens read${pSpeed} (${fmt(t.reused)} reused) · ` +
         `${fmt(t.output_tokens)} written${oSpeed}`;
}
function renderMonitor(live, hw, st, eng, h, last, requests, totals, kept) {
  // model state
  const on = live.queued > 0 ? "queued" : live.state;
  ccBusy = on === "queued" || live.state === "reading" || live.state === "generating" || live.state === "error";
  for (const b of document.querySelectorAll("#state-badges .st-badge")) b.classList.toggle("on", b.dataset.s === on || b.dataset.s === live.state);
  const activeBadge = $("session-active-badge");
  if (activeBadge) {
    const cls = live.state === "error" ? "error" : live.state === "reading" ? "reading"
      : live.state === "generating" ? "generating" : on === "queued" ? "queued" : "idle";
    activeBadge.className = `st-badge session-badge session-badge--${cls}`;
    activeBadge.textContent = {idle: "Idle", reading: "Reading", generating: "Generating", queued: "Queued", error: "Error"}[cls];
  }
  const prog = $("state-progress");
  let label = "Waiting for a request", detail = "", pct = 0;
  if (live.state === "reading") {
    label = "Reading prompt";
    prog.dataset.tone = "info";
    if (live.prompt_total) {
      pct = (100 * live.prompt_read) / live.prompt_total;
      detail = `${fmt(live.prompt_read)} / ${fmt(live.prompt_total)} tokens · ${fmt(pct)}%`;
    } else {
      detail = `${fmt(live.prompt_tokens)} tokens`;
    }
  } else if (live.state === "generating") {
    label = live.phase ? live.phase[0].toUpperCase() + live.phase.slice(1) : "Generating";
    delete prog.dataset.tone;
    pct = live.max_tokens ? Math.min(100, (100 * live.generated) / live.max_tokens) : 0;
    detail = `${fmt(live.generated)} tokens · ${fmt(live.tok_s, 1)} tok/s`;
  } else if (last) {
    delete prog.dataset.tone;
    detail = `last: ${fmt(last.output_tokens)} tokens${last.decode_tok_s ? ` at ${fmt(last.decode_tok_s, 1)} tok/s` : ""}`;
  }
  $("state-label").textContent = label;
  $("state-detail").textContent = detail;
  $("state-bar").style.width = `${pct}%`;

  // the eight cards
  const speed = live.state === "generating" ? live.tok_s : last ? last.decode_tok_s : null;
  setMetric("speed", speed == null ? null : fmt(speed, 1), "t/s",
            live.state === "generating" ? "Decode now" : last ? "Decode last request" : "Decode");
  const prefill = live.state !== "idle" ? live.prefill_tok_s_mean
                : last && last.prompt_ms > 0 ? Math.max(0, last.prompt_tokens - (last.reused || 0)) / (last.prompt_ms / 1000) : null;
  setMetric("prefill", prefill == null ? null : fmt(prefill), "t/s",
            live.state === "reading" ? "Prefill now" : live.state === "generating" ? "Prefill this request" : last ? "Prefill last request" : "Prefill");
  spark("sp-speed", h.tok_s);
  spark("sp-prefill", h.prefill_tok_s_mean);
  // a model split across several cards (issue #112): the cards show their total / mean / hottest, and each card's own
  const per = (f) => (hw.gpus || []).map((g) => `GPU ${g.index} ${f(g)}`).join(" · ");
  const multi = (hw.gpus || []).length > 1;
  setMetric("gpu", hw.gpu_util == null ? null : fmt(hw.gpu_util), "%",
            multi ? per((g) => (g.util == null ? "–" : `${fmt(g.util)}%`)) : st.gpu_name || (st.gpu_note ? "not available" : ""));
  spark("sp-gpu", h.gpu_util, 100);
  setMetric("vram", hw.gpu_mem_used == null ? null : gb(hw.gpu_mem_used), hw.gpu_mem_total ? `/ ${gb(hw.gpu_mem_total, 0)} GB` : "GB",
            multi ? per((g) => (g.mem_used == null ? "–" : `${gb(g.mem_used)} GB`))
                  : eng.expert_slots ? `${fmt(eng.expert_slots)} experts cached` : (st.gpu_note ? "not available on Windows AMD yet" : ""));
  spark("sp-vram", h.gpu_mem_used, hw.gpu_mem_total);
  setMetric("temp", hw.gpu_temp == null ? null : fmt(hw.gpu_temp), "°C",
            multi ? per((g) => (g.temp == null ? "–" : `${fmt(g.temp)}°`)) : "");
  spark("sp-temp", h.gpu_temp, 90);
  setMetric("power", hw.gpu_power == null ? null : fmt(hw.gpu_power), "W", hw.gpu_power_limit ? `of ${fmt(hw.gpu_power_limit)} W limit` : "");
  spark("sp-power", h.gpu_power, hw.gpu_power_limit);
  const gen = hw.gpu_pcie_gen_max || hw.gpu_pcie_gen;
  setMetric("pcie", gen ? `Gen${gen}` : null, hw.gpu_pcie_width ? `x${hw.gpu_pcie_width}` : "",
            hw.gpu_pcie_rx_mb == null ? "" : `to GPU ${fmt(hw.gpu_pcie_rx_mb, hw.gpu_pcie_rx_mb < 10 ? 1 : 0)} MB/s` +
            (hw.gpu_pcie_gen && gen && hw.gpu_pcie_gen < gen ? ` · idle Gen${hw.gpu_pcie_gen}` : ""));
  spark("sp-pcie", h.gpu_pcie_rx_mb);
  setMetric("cpu", hw.cpu == null ? null : fmt(hw.cpu), "%", st.threads ? `${st.cores ? `${st.cores} cores · ` : ""}${st.threads} threads` : "");
  spark("sp-cpu", h.cpu, 100);
  if (hw.disk_read_mb == null) {
    setMetric("disk", null, "", st.psutil ? "" : "needs psutil (setup installs it)");
  } else {
    const big = hw.disk_read_mb >= 1000;
    setMetric("disk", big ? fmt(hw.disk_read_mb / 1024, 2) : fmt(hw.disk_read_mb, hw.disk_read_mb < 10 ? 1 : 0), big ? "GB/s" : "MB/s",
              hw.disk_write_mb == null ? "" : `write ${fmt(hw.disk_write_mb, 1)} MB/s`);
  }
  spark("sp-disk", h.disk_read_mb);

  // context fill: the running request, else the last one
  const ctx = eng.max_context || 0;
  let used = 0;
  if (live.state !== "idle") used = (live.prompt_tokens || 0) + (live.generated || 0);
  else if (last) used = (last.prompt_tokens || 0) + (last.output_tokens || 0);
  const frac = ctx ? Math.min(1, used / ctx) : 0;
  $("ctx-fill").setAttribute("stroke-dasharray", `${(235.6 * frac).toFixed(1)} 314.2`);
  $("ctx-fill").style.opacity = 235.6 * frac >= 3 ? "1" : "0";         // a near-zero arc would draw just its round cap
  $("ctx-pct").textContent = `${Math.round(frac * 100)}%`;
  $("ctx-sub").textContent = ctx ? `${kfmt(used)} / ${ctxfmt(ctx)}` : "–";
  const cacheBytes = (eng.expert_cache_mib || 0) * 1048576;
  $("slots-text").textContent = eng.expert_slots ? `${fmt(eng.expert_slots)} · ${gb(cacheBytes)} GB` : "–";
  $("slots-bar").style.width = hw.gpu_mem_total ? `${Math.min(100, (100 * cacheBytes) / hw.gpu_mem_total)}%` : "0%";
  $("ram-text").textContent = hw.ram_total ? `${gb(hw.ram_used)} / ${gb(hw.ram_total, 0)} GB` : "–";
  const ramPct = hw.ram_total ? (100 * hw.ram_used) / hw.ram_total : 0;
  $("ram-bar").style.width = `${ramPct}%`;
  if (ramPct > 92) $("ram-progress").dataset.tone = "danger"; else delete $("ram-progress").dataset.tone;
  $("temp-text").textContent = hw.gpu_temp == null ? "–" : `${fmt(hw.gpu_temp)} °C`;
  $("temp-bar").style.width = hw.gpu_temp == null ? "0%" : `${Math.min(100, hw.gpu_temp)}%`;

  // recent requests
  const body = $("req-body");
  if (!requests.length) {
    body.innerHTML = `<tr><td colspan="9" class="muted">No requests yet</td></tr>`;
  } else {
    const badge = {stop: ["", "Done"], length: ["", "Max tokens"], cancel: ["st-badge--queued", "Stopped"],
                   disconnect: ["st-badge--queued", "Closed"], error: ["st-badge--error", "Error"]};
    body.innerHTML = requests.slice(0, state.reqShowAll ? requests.length : 12).map((r) => {
      const [cls, text] = badge[r.finish] || ["", r.finish || "–"];
      const t = new Date(r.time * 1000).toLocaleTimeString([], {hour: "2-digit", minute: "2-digit", second: "2-digit"});
      const proj = r.projection == null ? "" : ` <span class="st-badge${r.projection ? " st-badge--reading" : ""}" title="experimental speed projection ${r.projection ? "on" : "off"}">${r.projection ? "ESP" : "stock"}</span>`;
      // #588: the VRAM share; the PCIe share (--pcie-frac) beside it when there is one
      const hit = r.hit_rate == null ? "–" : `${(r.hit_rate * 100).toFixed(1)}%` +
        (r.pcie_share ? ` <span class="muted" title="routed experts the GPU read over PCIe (--pcie-frac) or another GPU computed">+${(r.pcie_share * 100).toFixed(1)}% PCIe</span>` : "");
      // Reused tokens are not prefetched again. Match the Monitor's existing Prefill metric.
      const fresh = r.prompt_tokens == null ? null : Math.max(0, r.prompt_tokens - (r.reused || 0));
      const prefillRate = fresh > 0 && r.prompt_ms > 0 ? fresh / (r.prompt_ms / 1000) : null;
      return `<tr><td>${esc(t)}</td><td><span class="st-badge ${cls}">${esc(text)}</span>${proj}</td><td class="num">${fmt(r.prompt_tokens)}</td>
        <td class="num">${fmt(r.reused)}</td><td class="num">${fmt(r.output_tokens)}</td><td class="num">${fmt(prefillRate)}</td><td class="num">${fmt(r.decode_tok_s, 1)}</td>
        <td class="num">${hit}</td><td class="num">${fmt(r.duration_s, 1)} s</td></tr>`;
    }).join("");
  }
  const all = $("req-all");
  kept = kept == null ? requests.length : kept;
  all.hidden = kept <= 12;
  all.textContent = state.reqShowAll ? "Show fewer" : `Show all (${kept})`;
  $("req-wrap").classList.toggle("all", state.reqShowAll);
  $("req-totals").textContent = renderTotals(totals);
}

function facts(el, rows) {
  el.innerHTML = rows.filter((r) => r[1] != null && r[1] !== "").map(([k, v, copy]) =>
    `<dt>${esc(k)}</dt><dd>${copy ? `<code>${esc(v)}</code><button class="st-btn st-btn--icon" data-copy="${esc(v)}" aria-label="Copy">${icon("copy")}</button>` : esc(v)}</dd>`).join("");
}
// INFO cvec=project:4-44[:singleL] | add:A-B | 0
function projectionText(c) {
  if (!c || c === "0" || c === 0) return null;
  const [mode, range, single] = String(c).split(":");
  const [a, b] = (range || "").split("-");
  return `${mode === "project" ? "Projection" : "Additive"} control vector on layers ${a}–${b}` +
         `${single ? ` (layer ${single.replace("single", "")}'s direction)` : ""}. Per chat in Sampling. Its package ` +
         "describes the vector as a refusal-direction projection; measure the speed yourself";
}
function renderAbout(eng, hw, st) {
  const kv = {int8: "8-bit", q4_0: "4-bit (Hadamard-rotated)", fp16: "16-bit"}[eng.kv] || eng.kv;
  facts($("facts-engine"), [
    ["Model", eng.model],
    ["Engine", eng.version ? `v${eng.version}` : "built from source"],
    ["Context", eng.max_context ? `${fmt(eng.max_context)} tokens` : null],
    ["KV cache", kv ? `${kv}${eng.kv_resident ? `, streamed: ${fmt(eng.kv_resident)} positions per layer in VRAM, the rest in RAM` : ", all in VRAM"}` : null],
    ["Experts in VRAM", eng.expert_slots ? `${fmt(eng.expert_slots)} (${gb((eng.expert_cache_mib || 0) * 1048576)} GB)` : null],
    ["Speculation", eng.spec ? `MTP drafts up to ${Math.max(0, (eng.mtp_max || eng.spec) - 1)} tokens${eng.lookup ? ", prompt lookup on" : ""}` : null],
    ["Images", eng.images ? "on" : "off"],
    ["Experimental speed projection", projectionText(eng.cvec)],
  ]);
  facts($("facts-hw"), [
    ["GPU", st.gpu_name ? `${st.gpu_name}${hw.gpu_mem_total ? `, ${gb(hw.gpu_mem_total, 0)} GB` : ""}` : (st.gpu_note || "not readable (NVML)")],
    ["CPU", st.cpu_name ? `${st.cpu_name}${st.threads ? `, ${st.threads} threads` : ""}` : null],
    ["RAM", hw.ram_total ? `${gb(hw.ram_total, 0)} GB` : null],
  ]);
  const base = location.origin;
  facts($("facts-api"), [
    ["OpenAI base URL", `${base}/v1`, true],
    ["Anthropic base URL", base, true],
    ["Model name", eng.model, true],
  ]);
}
document.addEventListener("click", (e) => {
  const b = e.target.closest("[data-copy]");
  if (b) copyText(b.dataset.copy, b);
});
$("req-all").addEventListener("click", () => { state.reqShowAll = !state.reqShowAll; if (state.lastMetrics) render(state.lastMetrics); });

// ------------------------------------------------------------------ request queue (serve/affinity.py)
// Who waits for the single-request turn: per waiting call its arrival, size, wait reason and the
// session identifier (the last user message's start), plus the hold and the focus.  The controls
// POST to /api/queue/... (own page + key, like api/models/load).  An older server has no
// /api/queue: when it is missing, the card shows a note instead of disappearing.
const queueBadge = {queued: ["", "queued"], affinity: ["st-badge--queued", "affinity"],
                    hold: ["st-badge--reading", "hold"], focus: ["st-badge--generating", "focus"]};

async function pollQueue() {
  try {
    const r = await fetch("api/queue", {headers: headers()});
    const q = await r.json();
    if (!r.ok) throw new Error(q?.error?.message || `HTTP ${r.status}`);
    state.lastQueue = q;
    renderQueue(q);
    renderSessions(q, state.lastMetrics);
  } catch (e) {
    const note = $("queue-note");
    if (note) note.textContent = "Queue controls are unavailable on this server.";
  }
}

// Local-context debug badge: poll the debug server and show compression event count

async function pollLCBadge() {
  const badge = $("lc-badge");
  if (!badge) return;
  try {
    const r = await fetch("http://127.0.0.1:8765/api/local-context/config", {signal: AbortSignal.timeout(2000)});
    if (!r.ok) { badge.style.display = "none"; return; }
    badge.style.display = "inline";
  } catch {
    badge.style.display = "none";
    return;
  }
  try {
    const r = await fetch("http://127.0.0.1:8765/api/local-context/state", {signal: AbortSignal.timeout(2000)});
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const state = await r.json();
    let count = 0;
    if (state.compression && Array.isArray(state.compression)) {
      count = state.compression.length;
    }
    if (count !== state.lcBadgeCount) {
      state.lcBadgeCount = count;
      badge.textContent = count;
      badge.style.display = count > 0 ? "inline" : "none";
      badge.classList.toggle("on", count > 0);
    }
  } catch {
    // not available yet, badge stays hidden
  }
}

// drag-and-drop state

async function reorderQueue(order) {
  try {
    const r = await fetch("api/queue/reorder", {method: "POST", headers: headers(true),
                                                body: JSON.stringify({order})});
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    await pollQueue();
  } catch (e) {
    toast("error", "Reorder failed", e.message);
  }
}

function renderQueue(q) {
  const card = $("queue-card");
  card.hidden = false;
  const aff = $("queue-aff");
  if (aff.checked !== q.settings.enabled) aff.checked = q.settings.enabled;
  const holdIn = $("queue-hold");
  if (document.activeElement !== holdIn) holdIn.value = q.settings.hold_s;
  $("queue-skip").disabled = !(q.hold || q.focused);
  $("queue-play").hidden = !q.paused;
  $("queue-pause").hidden = q.paused;
  const note = $("queue-note");
  if (note) note.textContent = q.paused ? "Queue paused: new requests will wait." : "";

  // Collapsed summary text
  const collapsedText = $("queue-collapsed-text");
  if (q.focused) {
    const f = q.focused;
    const agent = f.agent ? ` · ${f.agent}` : "";
    const root = f.root_session && f.root_session !== f.session_id ? ` [${f.root_session.slice(0, 8)}]` : "";
    collapsedText.textContent = `Session: ${f.session_id?.slice(0, 8) || f.id}${root}${agent}`;
  } else if (q.paused) {
    collapsedText.textContent = "Queue is paused";
  } else if (q.waiting.length) {
    const w = q.waiting[0];
    const agent = w.agent ? ` · ${w.agent}` : "";
    const root = w.root_session && w.root_session !== w.session_id ? ` [${w.root_session.slice(0, 8)}]` : "";
    collapsedText.textContent = `Waiting: ${w.session_id?.slice(0, 8) || w.id}${root}${agent}`;
  } else {
    collapsedText.textContent = "No active session";
  }

  // Expanded state line
  const lines = [];
  if (q.paused) lines.push("Queue is paused: requests wait until you press Play");
  if (q.focused) lines.push(`Focused: ${q.focused.snippet || q.focused.id} - waits without timeout for its follow-up`);
  if (q.hold) lines.push(`Holding for the last conversation's follow-up: ${fmt(q.hold.remaining_s, 1)} s left`);
  if (!q.settings.enabled) lines.push("Affinity is off: requests take the turn in arrival order.");
  $("queue-state").textContent = lines.join("  |  ");

  // Sort by context size (smaller requests move up)
  const sorted = [...q.waiting].sort((a, b) => a.prompt_tokens - b.prompt_tokens);

  const rows = $("queue-rows");
  if (!sorted.length) {
    // Only replace the rows when there were any, so the empty state stays stable on every poll.
    if (rows.querySelector("tr[data-id].queue-row")) {
      rows.innerHTML = `<tr data-placeholder><td colspan="6" class="muted">No requests waiting</td></tr>`;
    }
    return;
  }
  rows.querySelector("tr[data-placeholder]")?.remove();

  // Diff-based update: only change what changed.
  // Only select main rows (queue-row class) — detail rows have the same data-id and must not conflict.
  const existing = new Map([...rows.querySelectorAll("tr[data-id].queue-row")].map(r => [r.dataset.id, r]));
  const newIds = new Set(sorted.map(w => w.id));

  // Remove rows that are no longer waiting (both the main row and its detail row)
  existing.forEach((mainRow, id) => {
    if (!newIds.has(id)) {
      const detailRow = rows.querySelector(`tr[data-id="${id}"].queue-row-expanded`);
      if (detailRow) detailRow.remove();
      mainRow.remove();
    }
  });

  // Update or create rows
  for (const w of sorted) {
    let mainRow = existing.get(w.id);
    if (!mainRow) {
      const { mainRow: newRow, detailRow } = createQueueRow(w);
      rows.appendChild(newRow);
      rows.appendChild(detailRow);
      existing.set(w.id, newRow);
    } else {
      updateQueueRow({ mainRow, detailRow: rows.querySelector(`tr[data-id="${w.id}"].queue-row-expanded`) }, w);
    }
  }
}

function createQueueRow(w) {
  const [cls, text] = queueBadge[w.state] || ["", w.state];
  const t = new Date(w.arrived * 1000).toLocaleTimeString([], {hour: "2-digit", minute: "2-digit", second: "2-digit"});
  const snip = w.snippet || w.id;
  const shown = snip.length > 90 ? snip.slice(0, 90) + "..." : snip;
  const sessionName = w.session_name || snip;
  const sessionShown = sessionName.length > 60 ? sessionName.slice(0, 60) + "..." : sessionName;
  const focused = w.id === $("queue-card")?.querySelector(":scope > div.queue-expanded")?.dataset?.focusedId;
  const agent = w.agent ? `<span class="muted small" title="Agent: ${esc(w.agent)}">· ${esc(w.agent)}</span>` : "";
  const root = w.root_session && w.root_session !== w.session_id
    ? `<span class="muted small" title="Root session: ${esc(w.root_session)}">[${esc(w.root_session.slice(0, 8))}]</span>`
    : "";

  const mainRow = document.createElement("tr");
  mainRow.className = "queue-row";
  mainRow.dataset.id = w.id;
  mainRow.dataset.focused = focused ? "true" : "";
  const isPaused = w.paused && w.paused.includes(w.id);
  mainRow.innerHTML = `
    <td class="qgrip" data-action="expand">${icon("grip")}</td>
    <td class="muted small">${esc(t)}</td>
    <td class="num">${fmt(w.prompt_tokens)}${w.small ? ' <span class="muted small" title="small prompts run at once: they never wait">small</span>' : ""}</td>
    <td><span class="st-badge ${cls}">${esc(text)}</span></td>
    <td title="${esc(sessionName)}">${esc(sessionShown)}${root}${agent}</td>
    <td class="muted small">${esc(w.path.replace(/^\/v1/, ""))}</td>
    <td>
      <button class="st-btn st-btn--secondary queue-pause-btn" data-pause-id="${isPaused ? "" : w.id}" title="${isPaused ? "Unpause" : "Pause"}">
        ${isPaused ? icon("play") : icon("pause")}
      </button>
      <button class="st-btn st-btn--secondary" data-queue-focus="${focused ? "" : w.id}" title="${focused ? "Unfocus" : "Focus"}">
        ${focused ? icon("lock-fill") : icon("lock")}
      </button>
    </td>`;

  const detailRow = document.createElement("tr");
  detailRow.className = "queue-row-expanded";
  detailRow.dataset.id = w.id;
  detailRow.hidden = true;
  detailRow.innerHTML = `
    <td colspan="7">
      <div class="queue-expanded-detail">
        <div><strong>Session ID:</strong> <span class="detail-session-id">${esc(w.session_id || w.id)}</span></div>
        <div><strong>Root session:</strong> <span class="detail-root">${esc(w.root_session || w.session_id || "")}</span></div>
        <div><strong>Agent:</strong> <span class="detail-agent">${esc(w.agent || "—")}</span></div>
        <div><strong>Session name:</strong> <span class="detail-session">${esc(sessionName)}</span></div>
        <div><strong>Last user message:</strong> <span class="detail-message">${esc(snip)}</span></div>
        <div><strong>Prompt tokens:</strong> <span class="detail-tokens">${fmt(w.prompt_tokens)}</span> <span class="muted small">(${w.small ? "small" : "big"})</span></div>
        <div><strong>Endpoint:</strong> <span class="detail-path">${esc(w.path.replace(/^\/v1/, ""))}</span></div>
        <div><strong>Waited:</strong> <span class="detail-wait">${w.waited_s.toFixed(1)} s</span></div>
      </div>
    </td>`;

  return { mainRow, detailRow };
}

function updateQueueRow(row, w) {
  const [cls, text] = queueBadge[w.state] || ["", w.state];
  const t = new Date(w.arrived * 1000).toLocaleTimeString([], {hour: "2-digit", minute: "2-digit", second: "2-digit"});
  const snip = w.snippet || w.id;
  const shown = snip.length > 90 ? snip.slice(0, 90) + "..." : snip;
  const sessionName = w.session_name || snip;
  const sessionShown = sessionName.length > 60 ? sessionName.slice(0, 60) + "..." : sessionName;
  const focused = w.id === $("queue-card")?.querySelector(":scope > div.queue-expanded")?.dataset?.focusedId;
  const agent = w.agent ? `<span class="muted small" title="Agent: ${esc(w.agent)}">· ${esc(w.agent)}</span>` : "";
  const root = w.root_session && w.root_session !== w.session_id
    ? `<span class="muted small" title="Root session: ${esc(w.root_session)}">[${esc(w.root_session.slice(0, 8))}]</span>`
    : "";

  const cells = row.mainRow.querySelectorAll("td");
  // Cell 0: grip (expand trigger)
  cells[0].dataset.action = "expand";
  cells[0].innerHTML = icon("grip");
  // Cell 1: arrived time
  cells[1].textContent = t;
  cells[1].className = "muted small";
  // Cell 2: tokens
  cells[2].innerHTML = `${fmt(w.prompt_tokens)}${w.small ? ' <span class="muted small" title="small prompts run at once: they never wait">small</span>' : ""}`;
  // Cell 3: state badge
  const badge = cells[3].querySelector(".st-badge");
  if (badge) {
    badge.className = `st-badge ${cls}`;
    badge.textContent = text;
  }
  // Cell 4: session name (with root and agent)
  cells[4].title = sessionName;
  cells[4].innerHTML = `${esc(sessionShown)}${root}${agent}`;
  // Cell 5: endpoint
  cells[5].textContent = w.path.replace(/^\/v1/, "");
  cells[5].className = "muted small";
  // Cell 6: pause button and focus button
  const pauseBtn = cells[6].querySelector(".queue-pause-btn");
  if (pauseBtn) {
    const isPaused = w.paused && w.paused.includes(w.id);
    pauseBtn.dataset.pauseId = isPaused ? "" : w.id;
    pauseBtn.title = isPaused ? "Unpause" : "Pause";
    pauseBtn.innerHTML = isPaused ? icon("play") : icon("pause");
  }
  const focusBtn = cells[6].querySelector("button[data-queue-focus]");
  if (focusBtn) {
    focusBtn.dataset.queueFocus = focused ? "" : w.id;
    focusBtn.title = focused ? "Unfocus" : "Focus";
    focusBtn.innerHTML = focused ? icon("lock-fill") : icon("lock");
  }

  // Update detail row if visible
  const detailRow = row.detailRow;
  if (detailRow && !detailRow.hidden) {
    const detail = detailRow.querySelector(".queue-expanded-detail");
    if (detail) {
      let sessionIdEl = detail.querySelector(".detail-session-id");
      if (!sessionIdEl) {
        sessionIdEl = document.createElement("div");
        sessionIdEl.innerHTML = `<strong>Session ID:</strong> <span class="detail-session-id"></span>`;
        detail.prepend(sessionIdEl);
      }
      sessionIdEl.querySelector(".detail-session-id").textContent = w.session_id || w.id;

      let rootEl = detail.querySelector(".detail-root");
      if (!rootEl) {
        rootEl = document.createElement("div");
        rootEl.innerHTML = `<strong>Root session:</strong> <span class="detail-root"></span>`;
        detail.prepend(rootEl);
      }
      rootEl.querySelector(".detail-root").textContent = w.root_session || w.session_id || "";

      let agentEl = detail.querySelector(".detail-agent");
      if (!agentEl) {
        agentEl = document.createElement("div");
        agentEl.innerHTML = `<strong>Agent:</strong> <span class="detail-agent"></span>`;
        detail.prepend(agentEl);
      }
      agentEl.querySelector(".detail-agent").textContent = w.agent || "—";

      detail.querySelector(".detail-session").textContent = sessionName;
      detail.querySelector(".detail-message").textContent = snip;
      detail.querySelector(".detail-tokens").innerHTML = `${fmt(w.prompt_tokens)} <span class="muted small">(${w.small ? "small" : "big"})</span>`;
      detail.querySelector(".detail-path").textContent = w.path.replace(/^\/v1/, "");
      detail.querySelector(".detail-wait").textContent = `${w.waited_s.toFixed(1)} s`;
    }
  }
}

// The waiting-requests table is always visible: there is no expand toggle.
$("queue-aff").addEventListener("change", async () => {
  const r = await fetch("api/queue/settings", {method: "POST", headers: headers(true),
                                               body: JSON.stringify({enabled: $("queue-aff").checked})});
  if (!r.ok) toast("error", "Not applied", `HTTP ${r.status}`);
});
$("queue-hold").addEventListener("change", async () => {
  const v = Number($("queue-hold").value);
  if (Number.isNaN(v)) return;
  const r = await fetch("api/queue/settings", {method: "POST", headers: headers(true),
                                               body: JSON.stringify({hold_s: v})});
  if (!r.ok) toast("error", "Not applied", `HTTP ${r.status}`);
});
$("queue-skip").addEventListener("click", () => queueAction("api/queue/skip", "Skip"));
$("queue-rows").addEventListener("click", async (e) => {
  // Handle expand/collapse (grip cell)
  const gripCell = e.target.closest("td[data-action='expand']");
  if (gripCell) {
    const mainRow = gripCell.closest("tr[data-id]");
    if (!mainRow) return;
    const detailRow = mainRow.parentElement.querySelector(`tr[data-id="${mainRow.dataset.id}"].queue-row-expanded`);
    if (detailRow) {
      detailRow.hidden = !detailRow.hidden;
    }
    return;
  }

  // Handle pause/unpause button
  const pauseBtn = e.target.closest("button.queue-pause-btn");
  if (pauseBtn) {
    const id = pauseBtn.dataset.pauseId || undefined;
    const url = id ? "api/queue/pause-request" : "api/queue/unpause-request";
    const r = await fetch(url, {method: "POST", headers: headers(true), body: JSON.stringify(id ? {id} : {})});
    if (!r.ok) toast("error", "Not applied", `HTTP ${r.status}`);
    return;
  }

  // Handle focus/unfocus button
  const btn = e.target.closest("button[data-queue-focus]");
  if (!btn) return;
  const id = btn.dataset.queueFocus || undefined;
  const r = await fetch(id ? "api/queue/focus" : "api/queue/unfocus",
                        {method: "POST", headers: headers(true), body: JSON.stringify(id ? {id} : {})});
  if (!r.ok) toast("error", "Not applied", `HTTP ${r.status}`);
});

// Drag-and-drop for manual queue reordering (state.dragId already declared at line 483)
$("queue-rows").addEventListener("dragstart", (e) => {
  const row = e.target.closest("tr[data-id].queue-row");
  if (!row) return;
  state.dragId = row.dataset.id;
  row.classList.add("dragging");
  e.dataTransfer.effectAllowed = "move";
  e.dataTransfer.setData("text/plain", state.dragId);
});
$("queue-rows").addEventListener("dragend", (e) => {
  const row = e.target.closest("tr[data-id].queue-row");
  if (row) row.classList.remove("dragging");
  state.dragId = null;
});
$("queue-rows").addEventListener("dragover", (e) => {
  const row = e.target.closest("tr[data-id].queue-row");
  if (!row) return;
  e.preventDefault();
  e.dataTransfer.dropEffect = "move";
  const prev = $("queue-rows").querySelector("tr[data-id].queue-row.drag-over");
  if (prev) prev.classList.remove("drag-over");
  row.classList.add("drag-over");
});
$("queue-rows").addEventListener("dragleave", (e) => {
  const row = e.target.closest("tr[data-id].queue-row");
  if (row) row.classList.remove("drag-over");
});
$("queue-rows").addEventListener("drop", async (e) => {
  const targetRow = e.target.closest("tr[data-id].queue-row");
  if (!targetRow || !state.dragId || state.dragId === targetRow.dataset.id) return;
  e.preventDefault();
  targetRow.classList.remove("drag-over");
  // Get the current order from the DOM, find positions, swap
  const allRows = [...$("queue-rows").querySelectorAll("tr[data-id].queue-row")];
  const dragIdx = allRows.findIndex(r => r.dataset.id === state.dragId);
  const targetIdx = allRows.findIndex(r => r.dataset.id === targetRow.dataset.id);
  if (dragIdx === -1 || targetIdx === -1) return;
  // Reorder the DOM elements
  const [dragEl] = allRows.splice(dragIdx, 1);
  allRows.splice(targetIdx, 0, dragEl);
  allRows.forEach(r => $("queue-rows").appendChild(r));
  // Send new order to server
  const newOrder = allRows.map(r => r.dataset.id);
  await reorderQueue(newOrder);
});
async function queueAction(path, label) {
  try {
    const r = await fetch(path, {method: "POST", headers: headers(true), body: "{}"});
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const q = await r.json();
    state.lastQueue = q;
    renderQueue(q);
    renderSessions(q, state.lastMetrics);
  } catch (e) {
    toast("error", `${label} failed`, e.message);
  }
}
$("queue-play").addEventListener("click", () => queueAction("api/queue/play", "Resume"));
$("queue-pause").addEventListener("click", () => queueAction("api/queue/pause", "Pause"));
