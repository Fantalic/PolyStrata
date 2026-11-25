// MCP servers listed by GET /mcp; chat opts into their tools per request. Alpine renders the list.
import { fmt, headers, state } from "./runtime.js";

export async function loadMcp() {
  try {
    const r = await fetch("mcp", {headers: headers()});
    if (!r.ok) return;
    state.mcpInfo = await r.json();
  } catch (e) { return; /* an older server: no MCP */ }
  renderMcp();
  clearTimeout(state.mcpRetry);                          // right after the start, servers may still be starting (npx downloads)
  if ((state.mcpInfo.servers || []).some((s) => s.status === "starting")) state.mcpRetry = setTimeout(loadMcp, 3000);
}
const MCP_STATE = {ready: ["st-badge--generating", "Connected"], starting: ["st-badge--reading", "Starting"],
                   failed: ["st-badge--error", "Failed"], stopped: ["st-badge--queued", "Stopped"], idle: ["", "Waiting"]};
function renderMcp() {
  const servers = state.mcpInfo.servers || [];
  state.mcpServers = servers.map((s) => {
    const [cls, text] = MCP_STATE[s.status] || ["", s.status];
    const info = s.info && s.info.name ? ` · ${s.info.name}${s.info.version ? ` ${s.info.version}` : ""}` : "";
    return {
      badgeClass: cls, badgeText: text, name: s.name, transport: s.transport,
      toolsCount: fmt((s.tools || []).length), info, error: s.error || "",
      tools: (s.tools || []).map((t) => ({name: t.tool, description: t.description || ""}))
    };
  });
  state.mcpCardHidden = !servers.length;
  state.mcpRowHidden = !servers.length;
  const ready = servers.filter((s) => s.status === "ready" || s.status === "stopped");
  state.mcpSummary = servers.length ? `${fmt(state.mcpInfo.tools)} tools · ${ready.length} of ${servers.length} servers connected` : "";
  state.mcpRowSub = state.mcpInfo.tools ? `${fmt(state.mcpInfo.tools)} tools from ${ready.map((s) => s.name).join(", ")}; the model calls them when it decides to`
                                        : "no server is connected yet (see the Monitor)";
}
