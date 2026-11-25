// ------------------------------------------------------------------ swipes
// Each assistant message carries its own variants. m.text/m.reasoning/m.tools/m.meta always hold the
// *current* one; m.swipes[m.swipeIndex] mirrors it, so navigating is just save-current / load-target.
// Regenerating discards any variants after the current one and appends the new one at the end.
function swipeInit(m) {
  if (!Array.isArray(m.swipes) || !m.swipes.length) {
    m.swipes = [{text: m.text || "", reasoning: m.reasoning || "", tools: m.tools || null, meta: m.meta || null, thinkSecs: m.thinkSecs || null}];
    m.swipeIndex = 0;
  }
}
function swipeSnapshot(m) {
  return {text: m.text || "", reasoning: m.reasoning || "", tools: m.tools || null, meta: m.meta || null, thinkSecs: m.thinkSecs || null};
}
function swipeSave(m) {
  swipeInit(m);
  m.swipes[m.swipeIndex] = swipeSnapshot(m);
}
function swipeLoad(m) {
  swipeInit(m);
  const s = m.swipes[m.swipeIndex] || {};
  m.text = s.text || ""; m.reasoning = s.reasoning || ""; m.tools = s.tools || null; m.meta = s.meta || null;
  m.thinkSecs = s.thinkSecs || null; m.stopped = false; m.error = null; m.limit = null;
}

export { swipeInit, swipeSnapshot, swipeSave, swipeLoad };
