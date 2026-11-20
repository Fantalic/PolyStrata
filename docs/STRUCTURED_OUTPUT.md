# Structured Output (JSON Enforcement)

The PolyStrata server enforces structured JSON output through **grammar-constrained decoding** — the model's token generation is restricted at every step by a GBNF grammar compiled from the requested JSON schema. This is not prompt engineering or post-hoc validation alone: the engine itself is prevented from producing tokens that would break JSON structure.

Server validation acts as a second layer (defense in depth), catching any edge cases the grammar does not cover.

## Quick Start

### json_object mode — any JSON object

```json
POST /v1/chat/completions
{
  "messages": [{"role": "user", "content": "What is 6 times 7?"}],
  "response_format": {"type": "json_object"},
  "max_tokens": 256
}
```

The model must return exactly one valid JSON object. No array, no string, no number at the top level.

### json_schema mode — strict schema validation

```json
POST /v1/chat/completions
{
  "messages": [{"role": "user", "content": "What is 6 times 7?"}],
  "response_format": {
    "type": "json_schema",
    "json_schema": {
      "name": "answer",
      "strict": true,
      "schema": {
        "type": "object",
        "properties": {
          "answer": {"type": "integer"}
        },
        "required": ["answer"],
        "additionalProperties": false
      }
    }
  },
  "max_tokens": 256
}
```

The response must match the schema exactly — correct types, all required fields, no extra fields.

## How It Works

### Two paths, selected automatically

| Condition | Path | Description |
|---|---|---|
| Engine supports grammar (`engine.grammar == 1`) | **grammar_and_validate** | GBNF grammar compiled from the schema, sent to the engine for token-level constraint. The grammar compiler (`serve/json_schema_to_grammar.py`) converts JSON Schema to GBNF rules. A second validation pass checks the result. |
| Engine does not support grammar | **prompt_and_validate** | Schema injected as a system prompt directive. The model generates free text. The server extracts and validates the JSON afterward. |

Check your server's mode:

```bash
curl http://127.0.0.1:8095/v1/status | python -m json.tool
```

Look for `structured_output.constrained_decoding` — `true` means grammar-constrained decoding is active.

### Reasoning/thinking is auto-disabled

Grammar-constrained decoding cannot coexist with thinking. The model's token selection is fully governed by the grammar mask — a thinking wrapper would place the JSON output inside the thinking channel where the mask rules every token, leaving the answer empty.

The server **automatically sets `reasoning_effort: false`** when a request includes `response_format` (either route) or a raw `grammar` field. You do not need to disable thinking yourself — but if you do, it is harmless.

**OpenAI route** (`/v1/chat/completions`): auto-disables before building the template (lines 6977-6985 of `serve/server.py`).

**Anthropic route** (`/v1/messages`): auto-disables after grammar compilation (lines 7240-7243).

### Streaming behavior

Structured output uses **buffered streaming**. The server collects all SSE chunks during generation, validates the complete JSON at the end, and only then replays the content to the client. A client never receives unvalidated structured content.

- On success: the client receives the first chunk, a content delta with the validated JSON, and the final chunk with `finish_reason: "stop"` and usage/timings.
- On failure: the client receives an SSE error event and `[DONE]` — no invalid content deltas are emitted.

### Failure handling

A failed structured request returns **502 Bad Gateway** with:

```json
{
  "error": {
    "type": "structured_output_failed",
    "code": "structured_output_failed",
    "message": "model did not return valid JSON: ..."
  }
}
```

Failure modes:
- **Malformed JSON** — the model returns prose, markdown tables, or incomplete text
- **Duplicate JSON keys** — detected during parsing
- **Non-finite numbers** — `NaN`, `Infinity`, `1e999` are rejected
- **Schema violations** — wrong type, missing required field, extra field (when `jsonschema` is installed)
- **Incomplete generation** — `finish_reason` is not `"stop"` (output ran out of tokens)

Each request generates **at most one** generation call. No hidden retries.

## Schema Constraints

### Supported `response_format` types

| Type | Behavior |
|---|---|
| `"text"` | No structured output. The model returns free text. |
| `"json_object"` | The root must be a JSON object with at least one member (`{}` is not allowed — at temperature 0 the model would otherwise emit exactly that). No schema shape checking: the content is the model's choice, and at temperature 0 it fills the cheapest legal members (measured `{"":42}` and null values). For extraction, use `json_schema`. |
| `"json_schema"` | Full schema validation. Requires a `json_schema` object with `name` (string), `strict` (boolean, optional), and `schema` (JSON Schema object). |

### json_schema root requirements

The schema root must accept **only JSON objects**. Accepted patterns:

- `"type": "object"`
- `"anyOf"` or `"oneOf"` where every branch is an object schema
- `"allOf"` with at least one member that is an object schema
- Local `"$ref": "#/..."` pointing to one of the above

Refused patterns:

- Root also accepts arrays, strings, numbers, booleans, or null (including `"type": ["object", "null"]`)
- Remote references (`"$ref": "https://..."`) — only local `#` references are supported

### jsonschema package (optional)

`json_schema` validation uses the Python `jsonschema` package when installed:

```bash
pip install "jsonschema>=4.23,<5"
```

Without it, the server falls back to "is it one JSON object?" checking. A warning is printed to the server console on first use.

### Grammar compiler limitations

The JSON Schema → GBNF compiler (`serve/json_schema_to_grammar.py`) does not support every JSON Schema feature:

- **Pattern constraints** must be anchored (`^...$`) and cannot use `{n,m}` quantifiers
- **Remote `$ref`** references are refused (only local `#` refs)
- **Unanchored patterns** under an object root produce a 400 error: `"grammar compilation error"`
- Grammar size is capped at 2,000,000 hex-encoded characters (~1 MB raw GBNF)

If a schema passes the validator but fails the grammar compiler, the request returns **400** with a descriptive error message.

## Constraints

### Not supported

| Feature | Detail |
|---|---|
| **Tools / MCP with structured output** | Refused with 400: `"structured response_format with tools/MCP is not supported"`. The grammar mask and tool call extraction are incompatible. |
| **Batched requests with grammar** | Refused with 400: `"grammar-constrained decoding is not supported with batched requests"`. |
| **Streaming with invalid output** | The server buffers; the client never sees unvalidated content. On failure, only an SSE error event is emitted. |
| **Retry on failure** | Each structured request generates exactly one call to the engine. No hidden retries. |

### Opt-in features

| Feature | How to enable |
|---|---|
| Raw GBNF grammar | `{"grammar": "root ::= ... GBNF text ..."}` in the request body (hex-encoded by the server for transmission) |
| `jsonschema` package | `pip install "jsonschema>=4.23,<5"` on the server |

## Example: Knowledge Graph Extraction

This is the pattern that motivated the feature — Taiji_RAG's knowledge graph extraction pipeline:

```json
POST /v1/chat/completions
{
  "messages": [
    {
      "role": "system",
      "content": "You extract structured knowledge from text. Return only JSON."
    },
    {
      "role": "user",
      "content": "Extract terms, claims, and units from: 'The contract period is 12 months starting January 1, 2025.'"
    }
  ],
  "response_format": {
    "type": "json_schema",
    "json_schema": {
      "name": "knowledge_extraction",
      "strict": true,
      "schema": {
        "type": "object",
        "properties": {
          "terms": {
            "type": "array",
            "items": {
              "type": "object",
              "properties": {
                "name": {"type": "string"},
                "definition": {"type": "string"}
              },
              "required": ["name", "definition"],
              "additionalProperties": false
            }
          },
          "claims": {
            "type": "array",
            "items": {
              "type": "object",
              "properties": {
                "claim_text": {"type": "string"},
                "source": {"type": "string"}
              },
              "required": ["claim_text", "source"],
              "additionalProperties": false
            }
          },
          "units": {
            "type": "array",
            "items": {"type": "string"}
          }
        },
        "required": ["terms", "claims", "units"],
        "additionalProperties": false
      }
    }
  },
  "max_tokens": 1024
}
```

Expected successful response:

```json
{
  "choices": [{
    "message": {
      "content": "{\"terms\":[{\"name\":\"contract period\",\"definition\":\"12 months starting January 1, 2025\"}],\"claims\":[{\"claim_text\":\"The contract period is 12 months.\",\"source\":\"text\"}],\"units\":[]}"
    },
    "finish_reason": "stop"
  }]
}
```

## Troubleshooting

### "The model returns markdown prose instead of JSON"

The grammar compiler rejected the schema (400 error), or the engine does not support grammar-constrained decoding. Check:

1. `curl http://127.0.0.1:8095/v1/status` → `structured_output.constrained_decoding`
2. If `false`, the engine falls back to prompt-and-validate. The model may produce prose despite the system prompt directive.
3. If `true` but the model still produces prose, the schema may have features the grammar compiler cannot handle (unanchored patterns, unsupported constructs). Try a simpler schema.

### "I get 502 structured_output_failed"

The model generated output that failed validation. Common causes:

- The prompt was too complex for the model to follow the JSON constraint
- `max_tokens` was too low, causing truncation (`finish_reason` was not `"stop"`)
- The schema was too restrictive — the model produced a near-miss that failed validation
- The model has a habit of wrapping output in markdown code fences (the server strips these, but if the content inside is not valid JSON, it fails)

Solutions:
- Increase `max_tokens`
- Simplify the prompt — avoid asking for "thinking" alongside JSON output
- Loosen the schema slightly (e.g., allow `additionalProperties: true` during testing)

### "json_object returns null values or empty keys at temperature 0"

That is the mode's contract: json_object constrains the shape only. The grammar makes a member required, and at
temperature 0 the model writes the cheapest one it can (`{"":42}` was measured). Raise the temperature (0.8
returned real content), or switch to `json_schema` — there the grammar forces the schema's keys and value
shapes, which is why extraction calls come out clean at any temperature. Schema keys are written exactly as
the schema names them.

### "Reasoning thinking makes structured output fail"

If you see the model producing a thinking block before the JSON, the `reasoning_effort` was not disabled. This should not happen with the current server — the code at lines 6977-6985 (OpenAI) and 7240-7243 (Anthropic) auto-disables it. If you are using an older server version, upgrade or set `"reasoning_effort": false` explicitly.

### "Tools + JSON don't work together"

This is a known limitation: `"structured response_format with tools/MCP is not supported"`. The grammar mask governs every token pick; tool call extraction would need to read tokens the grammar is constraining. If you need both, call the API twice: once with tools (no response_format), then once with response_format for the final structured answer.

## Files

| File | Role |
|---|---|
| `serve/structured.py` | Core module: validators, schema preparation, JSON extraction, validation |
| `serve/json_schema_to_grammar.py` | JSON Schema → GBNF compiler (668 lines) |
| `serve/server.py:6977-6985` | OpenAI route: auto-disable reasoning for structured requests |
| `serve/server.py:5879-5901` | `structured_chunks()`: buffered streaming for structured output |
| `serve/test_structured.py` | HTTP-layer tests for structured output |
| `serve/test_grammar_constrained.py` | Tests for grammar compilation and status flags |
| `docs/DETAILS.md:2377-2397` | Summary in the main details document |
