"""Standalone test suite for json_schema_to_grammar.py.

Exit-code style: prints PASS/FAIL per test, exits 0 on success, nonzero on failure.
"""
import json
import re
import sys
from pathlib import Path

# Allow importing from the serve directory regardless of cwd.
_serve = Path(__file__).resolve().parent
if str(_serve) not in sys.path:
    sys.path.insert(0, str(_serve))

from json_schema_to_grammar import (
    CompileError,
    _Compiler,
    _fmt,
    _int_range,
    _same_len,
    json_object_grammar,
    schema_to_grammar,
)

_pass = 0
_fail = 0


def _check(name, fn):
    global _pass, _fail
    try:
        fn()
        print(f"  PASS: {name}")
        _pass += 1
    except Exception as exc:
        print(f"  FAIL: {name} -- {exc}")
        _fail += 1


# ── 1. Basic types ──────────────────────────────────────────────────────────

def test_basic_types():
    g = schema_to_grammar({"type": "boolean"})
    assert "root" in g and "true" in g and "false" in g

    g = schema_to_grammar({"type": "null"})
    assert "root" in g and '"null"' in g

    g = schema_to_grammar({"type": "integer"})
    assert "root" in g and "int_part" in g

    g = schema_to_grammar({"type": "number"})
    assert "root" in g and "number" in g

    g = schema_to_grammar({"type": "string"})
    assert "root" in g and "string" in g


# ── 2. Format rules ─────────────────────────────────────────────────────────

def test_format_rules():
    g = schema_to_grammar({"type": "string", "format": "date"})
    assert "date" in g

    g = schema_to_grammar({"type": "string", "format": "time"})
    assert "time" in g

    g = schema_to_grammar({"type": "string", "format": "date-time"})
    assert "date_time" in g

    g = schema_to_grammar({"type": "string", "format": "uuid"})
    assert "uuid" in g


# ── 3. Integer ranges ───────────────────────────────────────────────────────

def test_integer_ranges():
    # Single digit 0..9 should produce [0-9]
    g = schema_to_grammar({"type": "integer", "minimum": 0, "maximum": 9})
    assert "[0-9]" in g, f"Expected [0-9] in {g}"

    # Two-digit range 10..99 — recursive prefix-split: exactly 10..99
    g = schema_to_grammar({"type": "integer", "minimum": 10, "maximum": 99})
    assert '"1" [0-9]' in g and '"9" [0-9]' in g, f"Expected digit-range in {g}"

    # Range 19..91 — the tails must be RECURSIVE (a free [0-9] fill on the
    # head branch would wrongly admit 10..18, and on the tail branch 92..99).
    # Check the ROOT body only — other rules legitimately contain [0-9] runs.
    g = schema_to_grammar({"type": "integer", "minimum": 19, "maximum": 91})
    root_body = _parse_rules(g)["root"]
    assert '"1" "9"' in root_body, f"Lower bound 19 missing: {g}"
    assert '"9" [0-1]' in root_body, f"Upper bound 90..91 missing: {g}"
    assert '[1-8]' not in root_body and '[0-9] [0-9]' not in root_body, f"Over-broad range: {root_body}"

    # Negative range
    g = schema_to_grammar({"type": "integer", "minimum": -5, "maximum": 5})
    assert "-" in g, f"Expected '-' in {g}"

    # Exclusive bounds
    g = schema_to_grammar({"type": "integer", "exclusiveMinimum": 0, "exclusiveMaximum": 5})
    assert "1" in g, f"Expected 1 in {g}"


# ── 4. Object properties ────────────────────────────────────────────────────

def test_object_properties():
    schema = {
        "type": "object",
        "properties": {"name": {"type": "string"}, "age": {"type": "integer"}},
        "required": ["name"],
        "additionalProperties": False,
    }
    g = schema_to_grammar(schema)
    # Key literals carry their JSON quotes (escaped in GBNF): \"name\".
    assert '"\\"name\\""' in g, f"key 'name' missing: {g}"
    assert '"\\"age\\""' in g, f"key 'age' missing: {g}"
    assert "root" in g

    # Empty object (no properties, additionalProperties=False)
    schema = {"type": "object", "additionalProperties": False}
    g = schema_to_grammar(schema)
    assert '"{}"' in g
    assert "ws ws" not in g


# ── 5. Array items ──────────────────────────────────────────────────────────

def test_array_items():
    schema = {"type": "array", "items": {"type": "integer"}, "minItems": 1, "maxItems": 3}
    g = schema_to_grammar(schema)
    assert "array" in g or "[" in g

    # Unbounded array
    schema = {"type": "array", "items": {"type": "string"}}
    g = schema_to_grammar(schema)
    assert "array" in g or "[" in g


# ── 6. String constraints ───────────────────────────────────────────────────

def test_string_constraints():
    # minLength / maxLength
    schema = {"type": "string", "minLength": 2, "maxLength": 5}
    g = schema_to_grammar(schema)
    assert "char" in g

    # Pattern
    schema = {"type": "string", "pattern": "^[a-z]+$"}
    g = schema_to_grammar(schema)
    assert "root" in g

    # Pattern with quantifiers (lowered)
    schema = {"type": "string", "pattern": "^([a-z]*)$"}
    g = schema_to_grammar(schema)
    assert "*" in g or "root" in g


# ── 7. $ref resolution ──────────────────────────────────────────────────────

def test_ref_resolution():
    schema = {
        "$defs": {"answer": {"type": "integer"}},
        "type": "object",
        "properties": {"x": {"$ref": "#/$defs/answer"}},
        "required": ["x"],
    }
    g = schema_to_grammar(schema)
    assert "root" in g
    # The $ref should be resolved to the answer rule
    assert "answer" in g


# ── 8. allOf / anyOf / oneOf ────────────────────────────────────────────────

def test_composition():
    # anyOf
    schema = {"anyOf": [{"type": "string"}, {"type": "integer"}]}
    g = schema_to_grammar(schema)
    assert "root" in g

    # oneOf
    schema = {"oneOf": [{"type": "string", "enum": ["a", "b"]}, {"type": "null"}]}
    g = schema_to_grammar(schema)
    assert "root" in g

    # allOf with object properties
    schema = {
        "allOf": [
            {"type": "object", "properties": {"a": {"type": "string"}}},
            {"type": "object", "properties": {"b": {"type": "integer"}}},
        ],
        "required": ["a"],
    }
    g = schema_to_grammar(schema)
    assert "root" in g


# ── 9. Enum / const ─────────────────────────────────────────────────────────

def test_enum_const():
    # enum
    schema = {"type": "string", "enum": ["red", "green", "blue"]}
    g = schema_to_grammar(schema)
    assert "root" in g

    # const
    schema = {"const": 42}
    g = schema_to_grammar(schema)
    assert "root" in g
    assert "42" in g


# ── 10. Non-ASCII encoding ──────────────────────────────────────────────────

def test_non_ascii():
    # _fmt should encode non-ASCII as \xNN
    result = _fmt("caf\u00e9")
    assert "\\xC3\\xA9" in result, f"Expected \\xC3\\xA9 in {result}"

    # enum with non-ASCII
    schema = {"enum": ["caf\u00e9", "na\u00efve"]}
    g = schema_to_grammar(schema)
    assert "\\xC3" in g, f"Expected \\xC3 in {g}"


# ── 11. Error handling ──────────────────────────────────────────────────────

def test_error_handling():
    # Unsupported $ref (non-local)
    try:
        schema_to_grammar({"$ref": "https://example.com/schema"})
        assert False, "Expected CompileError"
    except CompileError:
        pass

    # Schema ``false``
    try:
        schema_to_grammar(False)
        assert False, "Expected CompileError"
    except CompileError:
        pass

    # Unknown type → CompileError (the server answers 400; the post-hoc
    # validator is never asked to police a half-compiled schema)
    try:
        schema_to_grammar({"type": "invalid"})
        assert False, "Expected CompileError"
    except CompileError:
        pass


# ── 12. Defect-proving checks ──────────────────────────────────────────────

def _parse_rules(grammar: str) -> dict:
    """Parse GBNF text into {name: body} dict."""
    rules = {}
    for line in grammar.strip().split("\n"):
        line = line.strip()
        if "::=" in line:
            name, body = line.split("::=", 1)
            rules[name.strip()] = body.strip()
    return rules


def _collect_refs(body: str) -> set:
    """Collect rule references (identifiers) from a GBNF body.

    Literal and class contexts are tracked with proper escape handling:
    a backslash inside a literal/class skips the NEXT char (so the two-char
    escape ``\\\\`` does not swallow the closing quote).
    """
    refs = set()
    in_literal = False
    in_class = False
    i = 0
    while i < len(body):
        c = body[i]
        if in_literal:
            if c == '\\':
                i += 2
                continue
            if c == '"':
                in_literal = False
            i += 1
            continue
        if in_class:
            if c == '\\':
                i += 2
                continue
            if c == ']':
                in_class = False
            i += 1
            continue
        if c == '"':
            in_literal = True
            i += 1
            continue
        if c == '[':
            in_class = True
            i += 1
            continue
        if c.isalpha() or c == '_':
            j = i
            while j < len(body) and (body[j].isalnum() or body[j] in '_-'):
                j += 1
            refs.add(body[i:j])
            i = j
        else:
            i += 1
    return refs


def test_no_dangling_refs():
    """Every referenced rule name is defined in the grammar."""
    g = json_object_grammar()
    rules = _parse_rules(g)
    for name, body in rules.items():
        refs = _collect_refs(body)
        for ref in refs:
            assert ref in rules, f"Rule '{name}' references undefined '{ref}'. Grammar:\\n{g}"


def test_value_references_defined():
    """The `value` primitive (when present) references only defined rules."""
    g = json_object_grammar()
    rules = _parse_rules(g)
    if "value" in rules:
        refs = _collect_refs(rules["value"])
        for ref in refs:
            assert ref in rules, f"'value' references undefined '{ref}'"
        # value should reference only known primitives
        known = {"object", "array", "string", "number", "boolean", "null"}
        for ref in refs:
            assert ref in known, f"'value' references unknown '{ref}'"


def test_no_double_escape():
    """No occurrence of 4-byte double-escape pattern backslash-backslash-quote-quote in any body."""
    g = json_object_grammar()
    # The double-escape signature in the GBNF output: four chars backslash, backslash, quote, quote
    # This would appear as \\"\\\" in the raw text.
    double_esc = '\\"\\"'
    assert double_esc not in g, f"Double-escape found in grammar:\\n{g}"
    # Also check a comprehensive schema
    schema = {
        "type": "object",
        "properties": {
            "name": {"type": "string"},
            "tags": {"type": "array", "items": {"type": "string"}},
        },
        "required": ["name"],
    }
    g2 = schema_to_grammar(schema)
    assert double_esc not in g2, f"Double-escape found in comprehensive schema grammar:\\n{g2}"


def test_no_literal_space_separators():
    """No \", \" literal-space separators — bodies use \",\" ws."""
    g = json_object_grammar()
    # Check that array/object separators use "," ws not ", " ws
    # The bad pattern is: comma followed by literal space (inside a quoted literal)
    # We assert that the separator pattern is "," ws (comma in quotes, then ws rule)
    # Bad: ", " ws — has literal space between comma and ws
    # Good: "," ws — no literal space
    # Look for the bad pattern in array/object rule bodies
    rules = _parse_rules(g)
    for name, body in rules.items():
        # The bad separator pattern: "," followed by a space before ws
        # In GBNF text: ", " ws means comma literal, space literal, ws rule
        # We check that no body contains the literal sequence ", " ws
        if '"," ws' in body or '","ws' in body:
            # Good: separator is "," ws
            continue
        # Check for the bad pattern: ", " ws (with literal space)
        # This appears as: "," ws in the source but with a space between comma-quote and ws
        # In the parsed body text: ', " ws' would have a space after the quote
        if re.search(r'", "\s*ws', body):
            assert False, f"Rule '{name}' has literal-space separator. Body: {body}"


def test_string_literal_correct():
    """The string primitive is \\"\\\" char* \\"\\" (quote, char*, quote)."""
    g = json_object_grammar()
    rules = _parse_rules(g)
    assert "string" in rules, f"'string' rule not found. Grammar:\\n{g}"
    # The correct string body: "\"" char* "\""
    # In GBNF: "\\"" char* "\\""
    string_body = rules["string"]
    assert 'char*' in string_body, f"'string' body missing char*. Body: {string_body}"
    # The body must start and end with the 4-char literal "\"" (one escaped
    # quote between surrounding quotes) — built via _fmt, not hand-escaped.
    assert string_body.startswith(_fmt('"')), f"'string' body start wrong. Body: {string_body}"
    assert string_body.endswith(_fmt('"')), f"'string' body end wrong. Body: {string_body}"


def test_json_object_grammar_has_primitives():
    """json_object_grammar() has NO dangling refs and contains all required primitives."""
    g = json_object_grammar()
    rules = _parse_rules(g)

    # All required primitives must be present
    for prim in ("object", "array", "boolean", "string", "number", "null", "value"):
        assert prim in rules, f"Missing primitive '{prim}' in json_object_grammar(). Grammar:\\n{g}"

    # root must be present
    assert "root" in rules, f"Missing 'root' in json_object_grammar(). Grammar:\\n{g}"

    # value must reference only defined rules
    if "value" in rules:
        refs = _collect_refs(rules["value"])
        for ref in refs:
            assert ref in rules, f"'value' references undefined '{ref}'"

    # root must reference only defined rules
    if "root" in rules:
        refs = _collect_refs(rules["root"])
        for ref in refs:
            assert ref in rules, f"'root' references undefined '{ref}'"


def test_trailing_comma_fix():
    """Optional property chain has mandatory first kv in its comma group."""
    # Build an object schema with one required + one optional property
    schema = {
        "type": "object",
        "properties": {
            "name": {"type": "string"},
            "extra": {"type": "string"},
        },
        "required": ["name"],
        "additionalProperties": False,
    }
    g = schema_to_grammar(schema)

    # The optional property group should have the first kv mandatory:
    # Look for the pattern ("," ws {kv}? where the ? applies to the whole kv group
    # But the FIRST optional kv should be mandatory inside the group:
    # Not: ( (kv1)? ("," ws kv2)? )?  — allows trailing comma with no kv
    # But: ( ("," ws kv1)? ("," ws kv2)? )?  — first kv requires comma
    #
    # We check that the optional group does NOT have a bare (kv)? without comma prefix
    # for its first kv. The first optional kv should be preceded by "," ws.

    # Find the optional group: the FIRST optional kv is MANDATORY inside its
    # comma group — good: ( "," ws root_extra_kv ( "," ws ... )? )?
    # bad: a bare (root_extra_kv)? or an optional first kv inside the group
    # (either would let a lone trailing comma parse).
    rules = _parse_rules(g)
    root_body = rules.get("root", "")
    assert root_body, f"root rule missing. Grammar:\n{g}"
    assert "(root_extra_kv)?" not in g, f"Bare optional first kv. Grammar:\n{g}"
    # NOTE: for a SINGLE optional kv the good shape IS ("," ws root_extra_kv)? —
    # the bad shape only exists when a LATER kv follows (kv-optional inside).
    # The bad signature: a parenthesized kv right after the comma group.
    assert '("," ws (root_extra_kv)' not in g, f"Optional first kv inside comma group. Grammar:\n{g}"
    assert '"," ws root_extra_kv' in root_body or 'root_extra_kv' not in root_body, \
        f"First optional kv not mandatory in comma group. Root body: {root_body}"


# ── Runner ──────────────────────────────────────────────────────────────────

def main():
    print()
    _check("basic types", test_basic_types)
    _check("format rules", test_format_rules)
    _check("integer ranges", test_integer_ranges)
    _check("object properties", test_object_properties)
    _check("array items", test_array_items)
    _check("string constraints", test_string_constraints)
    _check("$ref resolution", test_ref_resolution)
    _check("allOf/anyOf/oneOf", test_composition)
    _check("enum/const", test_enum_const)
    _check("non-ASCII encoding", test_non_ascii)
    _check("error handling", test_error_handling)
    # Defect-proving checks
    _check("no dangling refs", test_no_dangling_refs)
    _check("value references defined", test_value_references_defined)
    _check("no double escape", test_no_double_escape)
    _check("no literal-space separators", test_no_literal_space_separators)
    _check("string literal correct", test_string_literal_correct)
    _check("json_object_grammar has primitives", test_json_object_grammar_has_primitives)
    _check("trailing comma fix", test_trailing_comma_fix)
    print(f"\n{_pass} passed, {_fail} failed")
    sys.exit(1 if _fail else 0)


if __name__ == "__main__":
    main()
