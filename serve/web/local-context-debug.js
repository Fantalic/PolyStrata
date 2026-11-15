"use strict";
// ------------------------------------------------------------------ Local Context Debugger - PolyStrata UI integration
const API = "http://127.0.0.1:8765/api/local-context";
const $ = id => document.getElementById(id);

let data = null;
let currentSession = null;
let selectedChunkIndex = -1;
let autoPoll = true;
let pollTimer = null;
let activeSessionID = null; // Set by the /active endpoint

// ------------------------------------------------------------------ Fetch
async function fetchData() {
  try {
    const res = await fetch(API + "/state");
    data = await res.json();
    // Fetch active session indicator (separate endpoint)
    try {
      const resp = await fetch(API + "/active");
      if (resp.ok) {
        const actData = await resp.json();
        activeSessionID = actData.sessionID || null;
      }
    } catch {}
    renderAll();
  } catch (e) {
    console.error("fetch error:", e);
  }
}

// ------------------------------------------------------------------ Render all
function renderAll() {
  renderSessionSelect();
  if (currentSession !== null) {
    renderDashboard();
  }
  $("last-update").textContent = new Date().toLocaleTimeString();
}

// ------------------------------------------------------------------ Session select
function renderSessionSelect() {
  const sel = $("session-select");
  const delBtn = $("lc-delete-btn");
  if (!data || !data.sessions || data.sessions.length === 0) {
    sel.innerHTML = '<option value="">- No sessions -</option>';
    currentSession = null;
    delBtn.style.display = "none";
    return;
  }

  // Group sessions by rootSessionID for parent-child display
  // rootSessionID === null or rootSessionID === sessionID => root session
  // rootSessionID !== sessionID => child session
  const rootSessions = [];
  const childMap = {}; // rootID -> [children]
  data.sessions.forEach(s => {
    const rootID = s.rootSessionID || null;
    if (!rootID || rootID === s.id) {
      // This is a root session
      rootSessions.push(s);
    } else {
      // This is a child session
      if (!childMap[rootID]) childMap[rootID] = [];
      childMap[rootID].push(s);
    }
  });

  // Format a session label: use sessionLabel if available, fall back to id prefix
  function formatLabel(s) {
    const display = s.sessionLabel || s.id.slice(0, 16);
    const tasks = (s.todo || []).length;
    const chunks = (s.chunks || []).length;
    return display + " (tasks:" + tasks + " chunks:" + chunks + ")";
  }

  // Check if a session is the active one
  function isActive(s) {
    return activeSessionID === s.id;
  }

  const currentVal = sel.value;
  let options = "";

  // Render root sessions first
  rootSessions.forEach(s => {
    const label = formatLabel(s);
    const activeTag = isActive(s) ? " (active)" : "";
    const selected = s.id === currentVal ? ' selected' : "";
    const activeClass = isActive(s) ? " lc-option-active" : "";
    options += '<option value="' + s.id + '"' + selected + activeClass + '>' + label + activeTag + '</option>';

    // Then render its children (indented)
    if (childMap[s.id]) {
      childMap[s.id].forEach(c => {
        const childLabel = formatLabel(c);
        const childActiveTag = isActive(c) ? " (active)" : "";
        const childSelected = c.id === currentVal ? ' selected' : "";
        const childActiveClass = isActive(c) ? " lc-option-active" : "";
        options += '<option value="' + c.id + '"' + childSelected + childActiveClass + '>' +
          "  \u2514\u2500 " + childLabel + childActiveTag + '</option>';
      });
    }
  });

  sel.innerHTML = options;

  // Show/hide delete button based on selection
  delBtn.style.display = sel.value ? "inline-block" : "none";

  sel.onchange = () => {
    currentSession = sel.value || null;
    selectedChunkIndex = -1;
    delBtn.style.display = currentSession ? "inline-block" : "none";
    renderDashboard();
  };

  // Auto-select first session with chunks
  if (currentSession === null) {
    const first = data.sessions.find(s => (s.chunks || []).length > 0);
    if (first) {
      currentSession = first.id;
      sel.value = currentSession;
      delBtn.style.display = "inline-block";
      renderDashboard();
    }
  }
}

// ------------------------------------------------------------------ Delete session
async function deleteSession(sessionID) {
  if (!confirm("Delete session " + sessionID + "? This cannot be undone.")) return;
  try {
    const resp = await fetch(API + "/delete?key=lc:" + sessionID, { method: "POST" });
    if (resp.ok) {
      // Clear current session if it was the deleted one
      if (currentSession === sessionID) currentSession = null;
      fetchData(); // Refresh everything
    } else {
      alert("Delete failed");
    }
  } catch (e) {
    alert("Delete error: " + e.message);
  }
}

// ------------------------------------------------------------------ Dashboard
function renderDashboard() {
  if (!data || !currentSession) return;

  const sess = data.sessions.find(s => s.id === currentSession);
  if (!sess) return;

  const compSummary = sess.compressionSummary || null;

  // Stats
  const chunks = sess.chunks || [];
  const totalChars = chunks.reduce((sum, c) => sum + (c.chars || 0), 0);

  $("stat-chunks").textContent = chunks.length;
  $("stat-chars").textContent = totalChars.toLocaleString();
  $("stat-foldings").textContent = compSummary ? compSummary.totalEvents : 0;
  $("stat-tokens").textContent = compSummary ? compSummary.totalTokensSaved.toLocaleString() : 0;

  const todoDone = (sess.todo || []).filter(t => t.done).length;
  const todoTotal = (sess.todo || []).length;
  $("stat-tasks").textContent = todoDone + "/" + todoTotal;

  // Fold status badge
  const badge = $("fold-status");
  if (sess.pendingFold) {
    badge.textContent = "pending fold";
    badge.className = "lc-badge lc-badge-folding";
  } else if (sess.folded || chunks.length > 0) {
    badge.textContent = "folded (" + chunks.length + " chunks)";
    badge.className = "lc-badge lc-badge-folded";
  } else {
    badge.textContent = "idle";
    badge.className = "lc-badge lc-badge-idle";
  }

  // Timeline - show a single bar representing total events
  renderTimeline(compSummary);

  // Chunk list
  renderChunkList(chunks);

  // Todo
  renderTodo(sess.todo || []);

  // Log
  renderLog(sess.log || []);
}

// ------------------------------------------------------------------ Timeline
function renderTimeline(summary) {
  const bar = $("lc-timeline-bar");
  if (!summary || summary.totalEvents === 0) {
    bar.innerHTML = '<div style="padding:var(--st-s-2);color:var(--st-ink-muted);font-size:var(--st-fs-xs);">No compression events yet</div>';
    return;
  }

  // Show a single bar with opacity based on folding intensity
  const intensity = Math.min(1, summary.totalEvents / 100);
  const opacity = 0.3 + intensity * 0.7;
  bar.innerHTML = '<div class="lc-bar-segment" style="opacity:' + opacity + '; flex:1;" title="' + summary.totalEvents + ' compression events, ' + summary.totalTokensSaved + ' tokens saved, last kept: ' + summary.lastKeptTokens + ' tokens"></div>';
}

// ------------------------------------------------------------------ Chunk list
function renderChunkList(chunks) {
  const list = $("lc-chunk-list");
  const preview = $("lc-chunk-preview");

  if (chunks.length === 0) {
    list.innerHTML = '<div class="lc-empty">No chunks yet</div>';
    preview.classList.remove("visible");
    preview.textContent = "";
    return;
  }

  let html = "";
  for (let i = 0; i < chunks.length; i++) {
    const c = chunks[i];
    const isActive = i === selectedChunkIndex ? " active" : "";
    const size = (c.chars || 0).toLocaleString();
    const num = c.key ? c.key.split(":").pop() : (i + 1);
    html += '<div class="lc-chunk-item' + isActive + '" data-index="' + i + '">' +
      '<span class="lc-chunk-index">#' + num + '</span>' +
      '<span class="lc-chunk-size">' + size + ' chars</span>' +
      '</div>';
  }
  list.innerHTML = html;

  // Click handlers
  list.querySelectorAll(".lc-chunk-item").forEach(el => {
    el.addEventListener("click", () => {
      const idx = parseInt(el.dataset.index);
      selectedChunkIndex = idx;
      showChunkPreview(chunks[idx]);
      renderChunkList(chunks);
    });
  });

  // Show preview if one was selected
  if (selectedChunkIndex >= 0 && chunks[selectedChunkIndex]) {
    showChunkPreview(chunks[selectedChunkIndex]);
  }
}

function showChunkPreview(chunk) {
  const preview = $("lc-chunk-preview");
  const text = chunk.content || "";
  preview.textContent = text.slice(0, 3000) + (text.length > 3000 ? "\n\n... [truncated]" : "");
  preview.classList.add("visible");
}

// ------------------------------------------------------------------ Todo
function renderTodo(todo) {
  const list = $("lc-todo-list");
  if (todo.length === 0) {
    list.innerHTML = '<div class="lc-empty">No tasks</div>';
    return;
  }

  let html = "";
  for (let i = 0; i < todo.length; i++) {
    const t = todo[i];
    const done = t.done;
    const check = done ? "\u2713" : "\u25cb";
    html += '<div class="lc-todo-item' + (done ? ' done' : '') + '">' +
      '<span class="lc-todo-check ' + (done ? 'done' : 'pending') + '">' + check + '</span>' +
      '<span>' + escapeHtml(t.text) + '</span>' +
      '</div>';
  }
  list.innerHTML = html;
}

// ------------------------------------------------------------------ Log
function renderLog(log) {
  const list = $("lc-log-list");
  if (log.length === 0) {
    list.innerHTML = '<div class="lc-empty">No entries</div>';
    return;
  }

  let html = "";
  for (let i = 0; i < log.length; i++) {
    const e = log[i];
    html += '<div class="lc-log-entry">' +
      '<span class="lc-log-time">' + escapeHtml(e.t || "") + '</span>' +
      '<span class="lc-log-text">' + escapeHtml(e.text || "") + '</span>';
    if (e.reason) {
      html += '<div class="lc-log-reason">' + escapeHtml(e.reason) + '</div>';
    }
    html += '</div>';
  }
  list.innerHTML = html;
}

function escapeHtml(text) {
  return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

// ------------------------------------------------------------------ Auto-refresh
$("auto-refresh").addEventListener("change", (e) => {
  autoPoll = e.target.checked;
  startPolling();
});

// Delete button handler
$("lc-delete-btn").addEventListener("click", () => {
  if (currentSession) deleteSession(currentSession);
});

function startPolling() {
  if (pollTimer) clearInterval(pollTimer);
  if (autoPoll) {
    pollTimer = setInterval(fetchData, 3000);
  }
}

// ------------------------------------------------------------------ Init
fetchData();
startPolling();
