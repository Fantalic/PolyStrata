// Image generation tab (POST /v1/images/generations). Alpine owns the form state.
import { toast, headers, state } from "./runtime.js";

export async function generateImage() {
  const prompt = state.image.prompt.trim();
  if (!prompt) return;
  const body = {prompt, size: state.image.size, response_format: "b64_json"};
  if (String(state.image.seed).trim() !== "") body.seed = Number(state.image.seed);
  state.image.busy = true;
  const t0 = performance.now();
  state.image.hint = "Generating\u2026 (a minute or more on a small GPU)";
  try {
    const r = await fetch("v1/images/generations", {method: "POST", headers: headers(true), body: JSON.stringify(body)});
    const j = await r.json();
    if (!r.ok) throw new Error((j.error && j.error.message) || r.statusText);
    for (const d of j.data) {
      state.image.results.unshift({
        id: `${Date.now()}-${Math.random()}`,
        src: "data:image/png;base64," + d.b64_json,
        alt: prompt,
        caption: `${j.size}, seed ${d.seed}, ${((performance.now() - t0) / 1000).toFixed(0)} s: ${prompt}`
      });
    }
    state.image.hint = "Done. Right-click an image to save it.";
  } catch (e) {
    state.image.hint = "";
    toast("error", "No image", String(e.message || e), 8000);
  } finally {
    state.image.busy = false;
  }
}
