// The five memory knobs belong to the Conversation memory panel, not the generic Model settings form.
// They are filtered out of cfgKeys below so each setting is edited in exactly one place.
const MEM_KEYS = ["cache_mib", "cache_disk_mib", "cache_slots", "cache_focus", "cache_ttl_s"];

// Each knob: its plain-language name, the config key, what it is / what it does / an example (the tooltip),
// a short hint under the input, and how to read the value from and send it to the engine.  "auto" is a value
// the mib/disk keys accept, so those two are text inputs; the rest are numbers.
const MEM_FIELDS = [
  {key: "cache_mib", name: "RAM for parked chats", auto: true,
   what: "How much of your computer's free memory (RAM) the engine is allowed to use to keep whole past chats ready.",
   does: "When you switch back to an older chat, a parked copy lets the engine continue instead of reading that chat's entire prompt again — seconds instead of a minute on a long conversation. The engine only uses this much if your PC still has memory spare.",
   eg: 'Type "auto" to let the engine size it from the free RAM when the model loads (the default). Type a number in MiB to fix it, or 0 to keep no chats in RAM. 8192 = about 8 GB.',
   hint: "MiB · auto, or a number · 0 turns parking off"},
  {key: "cache_disk_mib", name: "Disk space for parked chats", auto: true,
   what: "Extra room on your disk (in a temporary file) for chats whose copy would not fit in RAM.",
   does: "When a chat is too big for the RAM budget, the engine writes its copy to a file instead of throwing it away. Coming back to it is a little slower than from RAM, but far faster than re-reading the whole prompt. Old files are removed when this budget runs out.",
   eg: 'Type "auto" (the default) to grow this on demand so at least 6 chats stay parkable, up to 64 GB. Type a number in MiB to cap it, or 0 to never use the disk. 4096 = about 4 GB.',
   hint: "MiB · auto, or a number · 0 turns the disk tier off"},
  {key: "cache_slots", name: "Chats kept ready", auto: false,
   what: "The number of different chats the engine keeps a copy of at the same time.",
   does: "Park more chats and you can jump between more conversations without re-reading them. If you use more chats than this, the least recently used copy is replaced. Each chat still takes RAM or disk from the budgets above.",
   eg: "A number. With the default memory policy the engine keeps at least 6. Raise it if you work across many chats at once; lower it to save memory.",
   hint: "a count · the memory policy keeps at least 6"},
  {key: "cache_focus", name: "Recently used chats get priority", auto: false,
   what: "How many requests count as 'recent' when the engine decides which parked chat to replace.",
   does: "A chat used within the last few requests keeps its copy even when a newer chat needs room; an older, unused copy is the one that goes. This stops a quick switch away and back from throwing away the chat you just used.",
   eg: "A number of requests (default 12). Higher protects recently used chats longer; 0 lets the smallest copy always go first.",
   hint: "requests · higher protects chats you just used"},
  {key: "cache_ttl_s", name: "Forget idle chats after", auto: false,
   what: "How many seconds a parked chat is kept when nobody uses it.",
   does: "After this much time without a request, an old parked copy is dropped to give memory back to your PC. It does not affect the chat itself — only the ready copy; the next visit re-reads it.",
   eg: "A number of seconds (0 means never drop by time, the default). 900 = drop a chat unused for 15 minutes.",
   hint: "seconds · 0 keeps them until memory is needed"},
];

// ------------------------------------------------------------------ Model settings fields (22 keys from runconfig.EDITABLE)
const MODEL_FIELDS = [
  {key: "sampling.temperature",  name: "Temperature", what: "Sets how creative or predictable the model's answers are when a request gives no temperature of its own.", does: "Higher values make replies more varied; lower values make them focused and repeatable. When unset, the model card decides the value.", eg: "With no value here the model card uses 1.0 for thinking and 0.7 for chat. Set 0 for greedy (always the most likely word).", hint: "number · the model card decides when unset"},
  {key: "sampling.top_p",        name: "Top-p", what: "Limits each word choice to the most likely words whose combined probability reaches this fraction.", does: "Lower values keep replies predictable; higher values allow more variety. Used only when a request does not set top_p itself.", eg: "When unset the engine uses 1 (no filtering). Try 0.9 if replies wander off-topic.", hint: "0 – 1 · 1 when unset"},
  {key: "sampling.top_k",        name: "Top-k", what: "Limits the model to the k most likely next words at each step.", does: "A smaller k keeps output predictable; a larger k allows more choice. Works alongside top-p.", eg: "When unset the engine uses 20. Allowed range 1–64; try 10 for very safe text.", hint: "1 – 64 · 20 when unset"},
  {key: "sampling.min_p",        name: "Min-p", what: "Drops words less likely than this fraction of the most likely word.", does: "A floor that removes weak guesses, useful for cutting nonsense at low temperature.", eg: "When unset the engine uses 0 (off). Try 0.1 to avoid weak guesses.", hint: "0 – 1 · 0 when unset"},
  {key: "reasoning_budget_tokens", name: "Thinking budget", what: "Caps how many thinking tokens every request may use.", does: "Limits the model's reasoning before it answers. Useful to bound slow or very long thinking on models that support it.", eg: "When unset there is no cap. Set 4096 to cap thinking at about 4096 tokens.", hint: "0 = no cap · a number to cap"},
  {key: "fit_max_tokens",        name: "Fit long answers to context", what: "Shortens a requested max_tokens that would not fit the context instead of answering 400.", does: "When an app asks for more output tokens than fit in the context window, the server trims the request so it can still answer instead of failing.", eg: "When unset this is off and the server answers 400. Turn on for apps that ask for very large max_tokens.", hint: "on / off · off when unset"},
  {key: "drop_oldest",           name: "Drop oldest turns when full", what: "Leaves out a conversation's oldest turns when it no longer fits the context, instead of answering 400.", does: "The system prompt and the last turn stay; older turns are removed to make room.", eg: "When unset this is off. Turn on for long chats that should keep answering instead of failing.", hint: "on / off · off when unset"},
  {key: "truncate_middle",       name: "Trim middle of long prompts", what: "Trims the middle of an oversized text prompt, keeping its beginning and ending.", does: "Useful when both the first and last part of a long prompt matter.", eg: "When unset this is off and the oldest turns are dropped instead. Turn on to keep both ends.", hint: "on / off · off when unset"},
  {key: "preserve_thinking",     name: "Keep earlier thinking", what: "Keeps the thinking of earlier turns in the prompt.", does: "Only templates that support it (Qwen3.6, Ornith, Tiel Coder) use this. Off removes earlier thinking from the prompt.", eg: "When unset this is off. Turn on to let the model see its earlier reasoning.", hint: "on / off · off when unset"},
  {key: "anthropic_thinking",    name: "Thinking in Anthropic requests", what: "Sets whether Anthropic requests that do not ask for thinking think anyway.", does: "model = think as the model normally does; on_request = do not think unless the request asks for it.", eg: "When unset this is model. Choose on_request to save time on Anthropic calls that never ask for thinking.", hint: "model / on_request · model when unset"},
  {key: "effort_position",       name: "Effort change position", what: "Where a non-default reasoning effort goes in the prompt.", does: "start is the simple default. end keeps the prompt cache when the reasoning effort changes between requests.", eg: "When unset this is start. Choose end if you change effort often and want to keep cached prompts.", hint: "start / end · start when unset"},
  {key: "aliases",               name: "Other model names", what: "Other names the server lists and answers to for this model.", does: "Apps can use any of these names; they all point to the same model.", eg: "Comma-separated names. When unset there are no extra names.", hint: "comma-separated names · empty when unset"},
  {key: "idle_unload_s",         name: "Unload when idle", what: "Seconds without requests before the model is unloaded to free memory.", does: "A new request reloads the model, so this trades reload time for free memory when idle.", eg: "When unset this is 0 (never unload). Set 3600 to unload after an hour of silence.", hint: "seconds · 0 = never · 0 when unset"},
  {key: "lazy_load",             name: "Load on first request", what: "Starts the server without the model loaded; the first request loads it.", does: "Text only — models that read images cannot use this. Speeds up startup but delays the first reply.", eg: "When unset this is off (load at startup). Turn on to start faster.", hint: "on / off · off when unset"},
  {key: "engine_silence_s",      name: "Engine silence timeout", what: "Seconds without engine output before a request ends.", does: "Ends a request that has gone quiet instead of waiting forever. Not the same as idle unloading.", eg: "When unset the engine uses 300 seconds. Set 0 to wait indefinitely.", hint: "seconds · 300 when unset · 0 = wait"},
  {key: "api_monitor",           name: "Keep request history", what: "Keeps the last 100 requests' prompts and answers in memory for /api-monitor.", does: "Needed by the Requests tab. Uses some RAM and keeps prompt text in memory.", eg: "When unset this is off. Turn on to inspect recent requests in the web app.", hint: "on / off · off when unset"},
  {key: "switch_on_request",     name: "Load model named in a request", what: "Lists every installed model and loads the one a request names.", does: "The loaded model is stopped first. Uses the config the server started with.", eg: "When unset this is off. Turn on when several apps use different models on one server.", hint: "on / off · off when unset"},
  {key: "open_browser",          name: "Open chat in browser", what: "Opens the chat page in your browser when the model is ready.", does: "Convenient on a personal PC; unwanted on a remote or headless server.", eg: "When unset this is off. Turn on to open the chat page automatically.", hint: "on / off · off when unset"},
  {key: "vram_reserve_mib",      name: "VRAM left for other apps", what: "VRAM in MiB the engine leaves free for your display and other programs.", does: "An engine-sized reserve applies when unset, based on the card's VRAM and the free RAM.", eg: "Leave unset to let the engine size it (about 1/32 of VRAM). Set 1024 to reserve about 1 GB.", hint: "MiB · engine sized when unset"},
  {key: "cache_checkpoints",     name: "Prompt checkpoints", what: "Checkpoints kept per conversation for the mid-prompt salvage ladder.", does: "More checkpoints let a changed prompt reuse more of the earlier read. Used from the next start of the model.", eg: "When unset the engine uses 6. Set 0 to read a prompt from its start unless it continues the last token.", hint: "a count · 6 when unset · start-only"},
  {key: "kv_resident",           name: "KV rows in VRAM", what: "Keeps the newest N attention rows in VRAM and streams the rest from RAM.", does: "Frees VRAM at long contexts but decodes slower when the context is full. Off means all KV rows stay in VRAM.", eg: "When unset this is 0 (off). See the KV resident note above for measured numbers.", hint: "a count · 0 = off when unset"},
  {key: "kv",                    name: "KV cache precision", what: "KV cache quantization used when kv-resident is on.", does: "Lower precision uses less RAM for streamed KV rows; higher precision keeps more detail.", eg: "When unset the engine uses fp16 (unchanged). Other options are fp16, q8_0, q4_0, int8 and int8k.", hint: "fp16 / q8_0 / q4_0 / int8 · fp16 when unset"},
];

export { MEM_KEYS, MEM_FIELDS, MODEL_FIELDS };
