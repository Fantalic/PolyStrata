"""JSON Schema ??? GBNF compiler for our C++ grammar parser.

Dialect: ``name ::= body``, quantifiers ``?``/``*``/``+`` only, raw UTF-8,
  byte-class members, no ``{n}`` ??? exact counts lowered to atoms.
"""
from __future__ import annotations

import json, re
from typing import Any, Dict, List, Optional, Set, Tuple

# ?????? Escaping (needed before the primitive tables below) ??????????????????????????????????????????????????????????????????
def _fmt(s: str) -> str:
    r"""Escape string ??? GBNF double-quoted literal. Non-ASCII ??? \xNN byte escapes.

    Backslash is escaped FIRST, then the quote ??? applied once each. The result
    is always ASCII-safe (raw UTF-8 bytes become \xNN sequences).
    """
    out: List[str] = []
    for c in s:
        if c == "\\": out.append("\\\\")
        elif c == '"': out.append('\\"')
        elif c == "\r": out.append("\\r")
        elif c == "\n": out.append("\\n")
        elif c == "\t": out.append("\\t")
        elif ord(c) > 127:
            for b in c.encode("utf-8"): out.append(f"\\x{b:02X}")
        else: out.append(c)
    return '"' + "".join(out) + '"'

_Q = _fmt('"')  # the GBNF literal for one double-quote char:  "\""  (4 chars)


def _hexrun(n: int) -> str:
    """n enumerated hex-digit class atoms ??? our dialect has no {n} quantifiers."""
    return " ".join(["[0-9a-fA-F]"] * n)


# ?????? Primitives & format rules ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
# Quote-bearing bodies are built via _Q so every quote literal is exactly
# "\"" (one escaped quote between the surrounding quotes) ??? hand-written
# escape nests double-escaped the closing quote and produced backslash
# literals instead.
_PR: Dict[str, str] = {
    "boolean": '"true" | "false"', "null": '"null"',
    "char": r'[^"\\\x7F\x00-\x1F] | "\\" (["\\bfnrt] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F])',
    "string": _Q + ' char* ' + _Q, "int_part": '"0" | [1-9] [0-9]*', "dec_part": '[0-9] [0-9]*',
    "integer": r'"-"? int_part', "number": r'"-"? int_part ("." dec_part)? ([eE] [-+]? dec_part)?',
    "value": 'object | array | string | number | boolean | null',
    "object": r'"{" ws ( string ":" ws value ("," ws string ":" ws value)* )? ws "}"',
    "array": r'"[" ws ( value ("," ws value)* )? ws "]"', "ws": '[ \\t\\n\\r]*',
}
_FR: Dict[str, str] = {
    "date": '[0-9] [0-9] [0-9] [0-9] "-" ( "0" [1-9] | "1" [0-2] ) "-" ( "0" [1-9] | [1-2] [0-9] | "3" [0-1] )',
    "time": '([01] [0-9] | "2" [0-3]) ":" [0-5] [0-9] ":" [0-5] [0-9] ( "." [0-9] [0-9] [0-9] )? ( "Z" | ( "+" | "-" ) ([01] [0-9] | "2" [0-3]) ":" [0-5] [0-9] )',
    "date_time": 'date "T" time',
    "date_string": _Q + ' date ' + _Q,
    "time_string": _Q + ' time ' + _Q,
    "date_time_string": _Q + ' date_time ' + _Q,
    "uuid": _Q + ' ' + _hexrun(8) + ' "-" ' + _hexrun(4) + ' "-" ' + _hexrun(4)
            + ' "-" ' + _hexrun(4) + ' "-" ' + _hexrun(12) + ' ' + _Q,
}
_RESERVED = frozenset(_PR) | frozenset(_FR) | {"root"}

# Primitive dependency closure: emitting a primitive also emits every rule its
# body references (the reference implementation walks PRIMITIVE_RULES deps).
_PRIM_DEPS: Dict[str, list] = {
    "value": ["object", "array", "string", "number", "boolean", "null"],
    "object": ["string", "value", "ws"],
    "array": ["value", "ws"],
    "string": ["char"],
    "number": ["int_part", "dec_part"],
    "integer": ["int_part"],
    "date_time": ["date", "time"],
    "date_string": ["date"], "time_string": ["time"], "date_time_string": ["date_time"],
}

class CompileError(ValueError):
    """Raised when a JSON Schema cannot be compiled to our GBNF dialect."""

def _sanitize(name: str) -> str:
    # Our parser's rule names are [A-Za-z0-9_] only (no dashes).
    return re.sub(r"[^a-zA-Z0-9_]", "_", name)

def _repetition(item: str, lo: int, hi: int, sep: str = "") -> str:
    """Lower lo..hi repetitions to ``?``/``*``/``+`` (no ``{n}`` in our dialect).

    The FIRST occurrence is always emitted, then mandatory trailing repeats
    (for lo > 1), then optional or star tails. With a separator, lo == 0
    wraps the whole thing in one optional group ??? emitting only ``(sep item)*``
    would match bare separators with no item at all.
    """
    if hi == 0:
        return ""
    if hi > 16:
        hi = -1  # cap: bounded counts above 16 degrade to unbounded (min stays precise)
    if lo == 0 and hi == 1:
        return f"({item})?"
    trailing = f"{sep} {item}" if sep else item
    parts: List[str] = [item]
    if hi < 0:
        if lo > 1:
            parts.extend([trailing] * (lo - 1))
        parts.append(f"({trailing})*")
    else:
        parts.extend([trailing] * max(0, lo - 1))
        parts.extend([f"({trailing})?"] * (hi - max(1, lo)))
    body = " ".join(parts)
    return f"({body})?" if lo == 0 else body

# ?????? Integer digit-range algorithm ???????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
def _int_range(lo: int, hi: int) -> str:
    if lo == hi:
        return _fmt(str(lo))
    if lo < 0 and hi < 0:
        return f'"-" ({_int_range(-hi, -lo)})'
    if lo < 0:
        neg = f'"-" ({_int_range(0, -lo)})'
        pos = _int_range(0, hi)
        return f"{neg} | {pos}"
    lo_s, hi_s = str(lo), str(hi)
    if len(lo_s) == len(hi_s):
        return _same_len(lo_s, hi_s)
    parts: List[str] = []
    for length in range(len(lo_s) + 1, len(hi_s)):
        head = "[1-9]" if length > 1 else "[0-9]"
        parts.append(head + " " + " ".join(["[0-9]"] * (length - 1)))
    parts.append(_same_len(lo_s, "9" * len(lo_s)))
    tail_start = "1" + "0" * (len(hi_s) - 1)
    parts.append(_same_len(tail_start, hi_s))
    return " | ".join(parts)

def _ge_fill(x: str) -> str:
    """GBNF alternation matching every digit string of length |x| that is >= x.

    Flat one-pass form: exceed x at some position p (exact prefix + a digit
    class above x[p] + free tail), or match x exactly.  This replaces the old
    recursive `_same_len(tail, '9'*k)` branch — same accepted set, but linear
    size instead of a call tree that doubled per digit (a `minimum: 0` with no
    maximum — the 19-digit range to 2**63-1 — used to build ~41M chars).
    """
    n = len(x)
    if n == 1:
        return _fmt("9") if x == "9" else f"[{x}-9]"
    parts: List[str] = []
    for p in range(n):
        if x[p] == "9":
            continue                     # no digit above 9: nothing to exceed with
        prefix = _fmt(x[:p]) if p else ""
        cls = f"[{chr(ord(x[p]) + 1)}-9]"
        fill = " ".join(["[0-9]"] * (n - p - 1))
        parts.append(f"{prefix}{cls}{' ' + fill if fill else ''}")
    parts.append(_fmt(x))                # exactly x
    return " | ".join(parts)

def _le_fill(y: str) -> str:
    """GBNF alternation matching every digit string of length |y| that is <= y (mirror of _ge_fill)."""
    n = len(y)
    if n == 1:
        return _fmt("0") if y == "0" else f"[0-{y}]"
    parts: List[str] = []
    for p in range(n):
        if y[p] == "0":
            continue                     # no digit below 0
        prefix = _fmt(y[:p]) if p else ""
        cls = f"[0-{chr(ord(y[p]) - 1)}]"
        fill = " ".join(["[0-9]"] * (n - p - 1))
        parts.append(f"{prefix}{cls}{' ' + fill if fill else ''}")
    parts.append(_fmt(y))                # exactly y
    return " | ".join(parts)

def _same_len(a: str, b: str) -> str:
    """Alternation for same-length numeric strings a..b via prefix-split.

    At the first differing digit d (shared prefix P, k remaining digits):
      (a) P + a[d] + <every tail >= a[d+1..]>   (flat _ge_fill — NOT a free
           [0-9] fill, which would wrongly admit values below a)
      (b) P + d + [0-9] fill for each mid digit
      (c) P + b[d] + <every tail <= b[d+1..]>   (flat _le_fill)
    Digit literals are quoted (raw digits would read as rule references).
    """
    if a == b: return _fmt(a)
    i = 0
    while i < len(a) and a[i] == b[i]: i += 1
    common = a[:i]
    cpfx = _fmt(common) if common else ""
    k = len(a) - i - 1
    fill = " ".join(["[0-9]"] * k)
    oda, odb = ord(a[i]), ord(b[i])
    if k == 0:
        # Single differing last digit: one byte-class covers a[i]..b[i].
        return f"{cpfx}[{a[i]}-{b[i]}]"
    parts: List[str] = []
    # The _ge_fill/_le_fill expansions are alternations; parens keep them
    # attached to the leading digit (unparenthesized, the later branches would
    # lose the prefix and match wrong full numbers).  Single-piece expansions
    # (one class/literal) need no parens.
    ge = _ge_fill(a[i + 1:])
    le = _le_fill(b[i + 1:])
    parts.append(f"{cpfx}{_fmt(a[i])} ({ge})" if " | " in ge else f"{cpfx}{_fmt(a[i])} {ge}")
    for dd in range(oda + 1, odb):
        parts.append(f"{cpfx}{_fmt(chr(dd))} {fill}")
    parts.append(f"{cpfx}{_fmt(b[i])} ({le})" if " | " in le else f"{cpfx}{_fmt(b[i])} {le}")
    return " | ".join(parts)

def _pattern_to_grammar(pattern: str) -> str:
    if not (pattern.startswith("^") and pattern.endswith("$")):
        raise CompileError(f"pattern must be anchored: {pattern!r}")
    inner = pattern[1:-1]
    if "{" in inner:
        raise CompileError(f"{{n,m}} quantifiers not supported: {pattern!r}")
    pos = [0]
    return _parse_alt(inner, pos)

def _parse_alt(s: str, pos: List[int]) -> str:
    parts = [_parse_seq(s, pos)]
    while pos[0] < len(s) and s[pos[0]] == "|":
        pos[0] += 1
        parts.append(_parse_seq(s, pos))
    return " | ".join(parts) if len(parts) > 1 else parts[0]

def _parse_seq(s: str, pos: List[int]) -> str:
    parts: List[str] = []
    while pos[0] < len(s) and s[pos[0]] not in ("|", ")"):
        atom = _parse_atom(s, pos)
        if pos[0] < len(s) and s[pos[0]] in ("*", "+", "?"):
            q = s[pos[0]]; pos[0] += 1
            atom = f"({atom}){'*' if q == '*' else '+' if q == '+' else '?'}"
        parts.append(atom)
    if not parts:
        return ""
    merged: List[str] = []
    for p in parts:
        if merged and merged[-1].startswith('"') and merged[-1].endswith('"') and p.startswith('"') and p.endswith('"'):
            merged[-1] = merged[-1] + p[1:-1] + '"'
        else:
            merged.append(p)
    return " ".join(merged)

def _parse_atom(s: str, pos: List[int]) -> str:
    if pos[0] >= len(s):
        raise CompileError("unexpected end of pattern")
    ch = s[pos[0]]
    if ch == "(":
        pos[0] += 1
        if pos[0] < len(s) and s[pos[0]] == "?":
            pos[0] += 1
            if pos[0] < len(s) and s[pos[0]] == ":":
                pos[0] += 1
            alt = _parse_alt(s, pos)
            if pos[0] >= len(s) or s[pos[0]] != ")":
                raise CompileError("unbalanced parens")
            pos[0] += 1
            return f"({alt})"
        alt = _parse_alt(s, pos)
        if pos[0] >= len(s) or s[pos[0]] != ")":
            raise CompileError("unbalanced parens")
        pos[0] += 1
        return f"({alt})"
    if ch == "[":
        return _parse_class(s, pos)
    if ch == ".":
        pos[0] += 1
        return "[^\\x0A\\x0D]"
    if ch == "\\":
        pos[0] += 1
        return _escape_atom(s, pos)
    if ch in ("^", "$"):
        raise CompileError("anchors inside pattern unsupported")
    # Plain character run ??? one quoted GBNF literal (raw characters would
    # parse as rule references). Stop at '\\' so the next atom is the escape.
    result: List[str] = []
    while pos[0] < len(s) and s[pos[0]] not in ("|", ")", "*", "+", "?", "[", "(", "^", "$", "\\"):
        result.append(s[pos[0]]); pos[0] += 1
    if not result:
        raise CompileError(f"unsupported pattern char: {s[pos[0]]!r}" if pos[0] < len(s) else "unexpected end of pattern")
    return _fmt("".join(result))

def _escape(s: str, pos: List[int]) -> str:
    """Regex escape INSIDE a char class: the result is spliced into the class
    body raw, so every returned byte must already be a \\xNN escape."""
    if pos[0] >= len(s):
        raise CompileError("trailing backslash")
    n = s[pos[0]]; pos[0] += 1
    emap = {"n": "\\n", "r": "\\r", "t": "\\t", "\\": "\\\\", '"': '\\"', "[": "\\[", "]": "\\]", "-": "\\-"}
    if n in emap:
        return emap[n]
    if n == "d": return "[0-9]"
    if n == "w": return "[0-9a-zA-Z_]"
    if n == "s": return "[ \\t\\n\\r]"
    if n == "D": return "[^0-9]"
    if n == "W": return "[^0-9a-zA-Z_]"
    if n == "S": return "[^ \\t\\n\\r]"
    if n == "x":
        h = s[pos[0]:pos[0]+2]; pos[0] += 2
        return f"\\x{int(h, 16):02X}"
    if n == "u":
        h = s[pos[0]:pos[0]+4]; pos[0] += 4
        # Class member: the codepoint's UTF-8 bytes as individual \xNN members.
        return "".join(f"\\x{b:02X}" for b in chr(int(h, 16)).encode("utf-8"))
    raise CompileError(f"unsupported escape: \\{n}")


def _escape_atom(s: str, pos: List[int]) -> str:
    """Regex escape OUTSIDE a class: shorthands stay class atoms, plain
    characters become quoted GBNF literals."""
    if pos[0] >= len(s):
        raise CompileError("trailing backslash")
    n = s[pos[0]]; pos[0] += 1
    if n == "d": return "[0-9]"
    if n == "w": return "[0-9a-zA-Z_]"
    if n == "s": return "[ \\t\\n\\r]"
    if n == "D": return "[^0-9]"
    if n == "W": return "[^0-9a-zA-Z_]"
    if n == "S": return "[^ \\t\\n\\r]"
    if n == "x":
        h = s[pos[0]:pos[0] + 2]; pos[0] += 2
        return _fmt(chr(int(h, 16)))
    if n == "u":
        h = s[pos[0]:pos[0] + 4]; pos[0] += 4
        return _fmt(chr(int(h, 16)))
    if n in ("n", "r", "t"):
        return _fmt({"n": "\n", "r": "\r", "t": "\t"}[n])
    if n in ('"', "\\", "[", "]", "-", "^"):
        return _fmt(n)
    raise CompileError(f"unsupported escape: \\{n}")

def _parse_class(s: str, pos: List[int]) -> str:
    pos[0] += 1
    neg = pos[0] < len(s) and s[pos[0]] == "^"
    if neg: pos[0] += 1
    parts: List[str] = []
    while pos[0] < len(s) and s[pos[0]] != "]":
        if s[pos[0]] == "\\":
            parts.append(_escape(s, pos))
        elif s[pos[0]] == "-" and parts:
            pos[0] += 1
            end = s[pos[0]] if pos[0] < len(s) else ""
            pos[0] += 1
            parts[-1] = f"[{parts[-1].strip('[]')}-{end}]"
        else:
            c = s[pos[0]]; pos[0] += 1
            if ord(c) > 127:
                # Multi-byte source char ??? its UTF-8 bytes as class members.
                c = "".join(f"\\x{b:02X}" for b in c.encode("utf-8"))
            elif c in ("\\", "]", "-", "^"):
                c = "\\" + c
            parts.append(c)
    if pos[0] >= len(s):
        raise CompileError("unterminated class")
    pos[0] += 1
    body = "".join(parts)
    return f"[^{body}]" if neg else f"[{body}]"

# ?????? Compiler ???????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????

class _Compiler:
    def __init__(self, schema: Dict[str, Any]) -> None:
        self._schema = schema
        self._rules: Dict[str, str] = {}
        self._seen: Set[str] = set()
        self._prim_seen: Set[str] = set()
        self._in_prog: Set[str] = set()
    def _add_rule(self, name: str, body: str) -> str:
        body = body.strip()
        if name in self._seen:
            return name
        self._rules[name] = body
        self._seen.add(name)
        return name
    def _ensure_prim(self, name: str) -> str:
        if name in self._prim_seen:
            return name
        if name not in _PR and name not in _FR:
            raise CompileError(f"unknown primitive rule: {name}")
        # Own guard set (NOT _seen ??? _add_rule's dedup must still fire). Mark
        # before walking deps: object <-> value reference each other (legal
        # GBNF forward references); the dep walk must not recurse.
        self._prim_seen.add(name)
        for dep in _PRIM_DEPS.get(name, []):
            if dep not in self._prim_seen:
                self._ensure_prim(dep)
        return self._add_rule(name, _PR.get(name) or _FR.get(name))
    def _resolve_ref(self, ref: str) -> str:
        if not ref.startswith("#/"):
            raise CompileError(f"only local $ref: {ref!r}")
        parts = ref[2:].split("/")
        node: Any = self._schema
        for part in parts:
            part = part.replace("~1", "/").replace("~0", "~")
            if not isinstance(node, dict) or part not in node:
                raise CompileError(f"$ref target not found: {ref!r}")
            node = node[part]
        if node is True:
            return self._ensure_prim("value")
        if not isinstance(node, dict):
            raise CompileError(f"$ref target must be an object schema: {ref!r}")
        # Name the rule after the LAST fragment segment (readable + matches
        # how schema authors think about their defs).
        rn = _sanitize(ref.rsplit("/", 1)[-1])
        if rn in _RESERVED:
            rn += "-"
        return self.visit(node, rn)

    def visit(self, schema: Any, name: str = "") -> str:
        if schema is True:
            return self._ensure_prim("value")
        if not isinstance(schema, dict):
            raise CompileError(f"unsupported schema: {schema!r}")
        rn = _sanitize(name) if name else "root"
        # Suffix reserved names (primitives, format rules) but never "root"
        if rn in _RESERVED and rn != "root":
            rn = rn + "-"
        if "$ref" in schema and rn in self._in_prog:
            return rn
        self._in_prog.add(rn)
        try:
            body = self._visit(schema, rn)
            if body is not None:
                self._add_rule(rn, body)
        finally:
            self._in_prog.discard(rn)
        return rn
    def _visit(self, s: Dict[str, Any], rn: str) -> Optional[str]:
        if s is True:
            return None
        if s is False:
            raise CompileError("schema ``false`` unsupported")
        if "enum" in s:
            return self._add_rule(rn, " | ".join(self._jval(v) for v in s["enum"]))
        if "const" in s:
            return self._add_rule(rn, self._jval(s["const"]))
        if "allOf" in s:
            return self._visit_all_of(s["allOf"], rn)
        if "anyOf" in s:
            alts = [self.visit(sub, f"{rn}_alt_{i}") for i, sub in enumerate(s["anyOf"])]
            return self._add_rule(rn, " | ".join(alts))
        if "oneOf" in s:
            alts = [self.visit(sub, f"{rn}_alt_{i}") for i, sub in enumerate(s["oneOf"])]
            return self._add_rule(rn, " | ".join(alts))
        if "$ref" in s:
            return self._resolve_ref(s["$ref"])
        type_spec = s.get("type")
        if isinstance(type_spec, str):
            return self._visit_type(type_spec, s, rn)
        if isinstance(type_spec, list):
            alts = [self._visit_type(t, s, f"{rn}_") for t in type_spec]
            return self._add_rule(rn, " | ".join(alts))
        if not s:
            return self._ensure_prim("value")
        return self._visit_type(s.get("type") or "string", s, rn)
    def _visit_type(self, t: str, s: Dict[str, Any], rn: str) -> str:
        if t == "object":
            # A plain object (no properties, no additionalProperties override)
            # IS the primitive ??? reference it instead of duplicating its body
            # under a schema rule name (the reference implementation does the
            # same via _add_primitive).
            if not s.get("properties") and s.get("additionalProperties") is None:
                return self._ensure_prim("object")
            return self._visit_obj(s, rn)
        if t == "array":
            return self._visit_arr(s, rn)
        if t == "string":
            return self._visit_str(s, rn)
        if t == "integer":
            return self._visit_int(s, rn)
        # Primitives: ensure canonical rule exists, return its name
        if t == "number":
            return self._ensure_prim("number")
        if t == "boolean":
            return self._ensure_prim("boolean")
        if t == "null":
            return self._ensure_prim("null")
        raise CompileError(f"unsupported type: {t!r}")
    def _jval(self, v: Any) -> str:
        # json.dumps gives the JSON TEXT of the value (strings keep their
        # quotes, true/false/null spell themselves) — the grammar must match
        # that text, so escape it as one GBNF literal.
        if isinstance(v, str):
            return _fmt(json.dumps(v, ensure_ascii=False))
        if isinstance(v, bool) or v is None:
            return _fmt(json.dumps(v))
        if isinstance(v, (int, float)):
            return _fmt(json.dumps(v, allow_nan=False))
        return _fmt(json.dumps(v, ensure_ascii=False, allow_nan=False))

    def _visit_all_of(self, schemas: List[Dict[str, Any]], rn: str) -> str:
        props: List[Tuple[str, Dict[str, Any]]] = []
        req: List[str] = []
        seen_req: Set[str] = set()
        add_prop: Any = None
        for sub in schemas:
            if not isinstance(sub, dict) or not sub:
                continue
            # required accumulates from EVERY member (constraint-only members
            # like {"required": [...]} are valid mergeable parts of an allOf).
            for k in sub.get("required", []):
                if k not in seen_req:
                    seen_req.add(k)
                    req.append(k)
            if sub.get("type") == "object" or "properties" in sub:
                props.extend(sub.get("properties", {}).items())
                # additionalProperties: false from any member propagates to
                # the merged object; a schema-form additionalProperties
                # cannot be intersected here.
                ap = sub.get("additionalProperties")
                if ap is False:
                    add_prop = False
                elif isinstance(ap, dict):
                    raise CompileError("allOf: additionalProperties schema cannot be merged")
                continue
            if set(sub) <= {"description", "title", "default", "examples", "required"}:
                continue
            # Non-object children (string constraints, enums, ...) cannot be
            # merged here ??? silently dropping them would under-constrain.
            raise CompileError("allOf: only object schemas can be merged")
        if props:
            return self._build_obj(props, req, rn, add_prop)
        return self._add_rule(rn, self._ensure_prim("value"))

    def _visit_obj(self, s: Dict[str, Any], rn: str) -> str:
        props = list(s.get("properties", {}).items())
        req = set(s.get("required", []))
        # Deterministic order (schema declaration order): set iteration order
        # is hash-randomized and would change the grammar between runs.
        req_list = [k for k, _ in props if k in req]
        opt_list = [k for k, _ in props if k not in req]
        return self._build_obj(props, req_list, rn, s.get("additionalProperties"), opt_list)

    def _build_obj(self, props: List[Tuple[str, Any]], req: List[str], rn: str,
                   add_prop: Any, opt_list: Optional[List[str]] = None) -> str:
        if not props and not opt_list:
            if add_prop is False:
                return self._add_rule(rn, '"{}"')
            return self._ensure_prim("object")
        kv: Dict[str, str] = {}
        pname_to_kv: Dict[str, str] = {}
        for pname, pschema in props:
            val = self.visit(pschema, f"{rn}_{pname}")
            kv_name = f"{rn}_{pname}_kv"
            # The JSON key INCLUDES its quotes: literal "name" in GBNF is
            # the bytes name — the JSON text needs \"name\".
            key_lit = _fmt('"' + pname + '"')
            kv[kv_name] = f'{key_lit} ws ":" ws {val}'
            self._add_rule(kv_name, kv[kv_name])
            pname_to_kv[pname] = kv_name
        # A required name without a property schema accepts any value.
        for k in req:
            if k not in pname_to_kv:
                kv_name = f"{rn}_{k}_kv"
                kv[kv_name] = f'{_fmt(k)} ws ":" ws {self._ensure_prim("value")}'
                self._add_rule(kv_name, kv[kv_name])
                pname_to_kv[k] = kv_name

        add_name = None
        if add_prop is not None and add_prop is not False:
            # Reference the value RULE ??? the inline body text would leak its
            # '|' alternation outside the kv rule.
            add_val = self._ensure_prim("value") if add_prop is True else self.visit(add_prop, f"{rn}_additional")
            add_name = f"{rn}_additional_kv"
            self._add_rule(add_name, f'{self._ensure_prim("string")} ws ":" ws {add_val}')

        parts: List[str] = ['"{"', ' ws']
        for i, k in enumerate(req):
            if i > 0: parts.append('"," ws')
            parts.append(kv[pname_to_kv[k]])

        if opt_list:
            # Optional kvs form ONE optional group whose FIRST kv is mandatory
            # inside it ??? otherwise a bare trailing comma would parse. The
            # group's leading comma only applies after required kvs.
            chain: List[str] = []
            for i, pname in enumerate(opt_list):
                k = f"{rn}_{pname}_kv"
                if i == 0:
                    chain.append(f'"," ws {k}' if req else k)
                else:
                    chain.append(f'("," ws {k})?')
            parts.append(' (' + ' '.join(chain) + ')?')

        if add_name:
            parts.append(' ("," ws ' + add_name + ')*')
        parts.extend([' ws', '"}"'])
        return self._add_rule(rn, " ".join(str(p) for p in parts if p))

    def _visit_arr(self, s: Dict[str, Any], rn: str) -> str:
        items = s.get("items")
        min_i = s.get("minItems", 0)
        max_i = s.get("maxItems", -1)
        if items is None or not isinstance(items, dict) or not items:
            # tuple items / missing / empty items schema: unconstrained element
            if not min_i and max_i in (None, -1):
                return self._ensure_prim("array")
            item_rule = self._ensure_prim("value")
        else:
            item_rule = self.visit(items, f"{rn}_item")
        rep = _repetition(item_rule, min_i, max_i, '"," ws')
        return self._add_rule(rn, f'"[" ws {rep} ws "]"')

    def _visit_str(self, s: Dict[str, Any], rn: str) -> str:
        fmt = s.get("format")
        pat = s.get("pattern")
        min_l = s.get("minLength", 0)
        max_l = s.get("maxLength", -1)
        if fmt in ("date", "time", "date_time", "date-time"):
            # Map the schema's format value (which uses the JSON ecosystem's
            # dashed spelling) to the underscore-named primitive.
            prim = {"date": "date_string", "time": "time_string",
                    "date_time": "date_time_string", "date-time": "date_time_string"}[fmt]
            return self._add_rule(rn, self._ensure_prim(prim))
        if fmt == "uuid":
            return self._add_rule(rn, self._ensure_prim("uuid"))
        if pat is not None:
            return self._add_rule(rn, f'{_Q} {_pattern_to_grammar(pat)} {_Q}')
        if min_l > 0 or max_l >= 0:
            char_r = self._ensure_prim("char")
            if max_l == 0:
                body = f'{_Q} {_Q}'
            elif max_l > 16 or max_l < 0:
                body = f'{_Q} {char_r}* {_Q}'
            else:
                body = f'{_Q} {_repetition(char_r, min_l, max_l)} {_Q}'
            return self._add_rule(rn, body)
        return self._add_rule(rn, self._ensure_prim("string"))

    def _visit_int(self, s: Dict[str, Any], rn: str) -> str:
        lo = s.get("minimum")
        hi = s.get("maximum")
        # exclusiveMinimum/exclusiveMaximum are ignored (as the reference does);
        # the post-hoc validator remains the second line of defense.
        if lo is None and hi is None:
            return self._add_rule(rn, self._ensure_prim("integer"))
        lo = lo if lo is not None else -2**63
        hi = hi if hi is not None else 2**63 - 1
        return self._add_rule(rn, f"({_int_range(lo, hi)})")

# ?????? Public API ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
def schema_to_grammar(schema: Any) -> str:
    """Compile JSON Schema dict ??? GBNF text.  ``root ::= ...`` LAST."""
    if not isinstance(schema, dict):
        if schema is True:
            c = _Compiler({})
            c._add_rule("root", "value")
            c._ensure_prim("value")
            return _format(c._rules)
        raise CompileError(f"unsupported schema type: {type(schema)}")
    c = _Compiler(schema)
    c.visit(schema, "root")
    # Ensure all primitives referenced by value are defined
    c._ensure_prim("value")
    for prim in ("object", "array", "string", "number", "integer", "boolean", "null"):
        c._ensure_prim(prim)
    c._ensure_prim("char")
    c._ensure_prim("ws")
    c._ensure_prim("int_part")
    c._ensure_prim("dec_part")
    if "root" not in c._rules:
        c._add_rule("root", _PR["value"])
    return _format(c._rules)

def json_object_grammar() -> str:
    return schema_to_grammar({"type": "object"})
def _format(rules: Dict[str, str]) -> str:
    # `root` FIRST: rule 0 is the grammar root (llama.cpp convention — our
    # parser starts the stack set at m_rules[0]).
    names = sorted(k for k in rules if k != "root")
    if "root" in rules:
        names.insert(0, "root")
    return "\n".join(f"{n} ::= {rules[n]}" for n in names) + "\n"
