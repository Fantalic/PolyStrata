// Local Alpine bootstrap. This module runs before the injected script, so it can register
// the shared state at alpine:init, before Alpine scans the DOM.
import { state, activateState } from "./runtime.js";

document.addEventListener("alpine:init", () => {
  const reactive = activateState(window.Alpine);
  window.poly = reactive;                 // x-data="poly" on <body>
  // x-for options are rendered after x-model installs; re-apply the selected values on the next tick.
  state.syncSelectors = () => window.Alpine.nextTick(() => {
    const m = document.getElementById("model-select");
    if (m && m.value !== state.currentModelId) m.value = state.currentModelId;
    const c = document.getElementById("context-select");
    if (c && c.value !== state.contextValue) c.value = state.contextValue;
  });
  // Panel modules attach their handlers to this object during module evaluation.
}, {once: true});

const tag = document.createElement("script");
tag.src = "web/vendor/alpine.min.js";
document.head.appendChild(tag);
