"""Build hygiene for the files that cannot be compiled on Linux.

The mod is built with MSVC (`scripts\\build.bat`), so a change here can leave the tree in a state
that only the Windows build notices. That has happened: commit b6a9c16 dropped a parameter from
`OLR_EdgeFill`'s definition, left the forward declaration and one call site with the old arity, and
the branch stopped compiling while every test here stayed green.

This test closes that gap from the outside, without a compiler:

1. **declaration vs definition** - a `static`/`extern "C"` function that is forward-declared AND
   defined in the same file must agree on its parameter count.
2. **call sites** - for a curated list of internal functions, every call must pass the number of
   arguments the definition takes (default arguments are accounted for).

It cannot check types or templates; it checks the two mistakes that actually happened.

Run: python3 tests/call_arity_test.py
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
FILES = ['src/d3d9_proxy.cpp', 'src/ol_ue3.cpp', 'src/ol_xr.cpp']


def strip_comments(src: str) -> str:
    """Remove // and /* */ comments but keep line structure (and strings) intact."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c in '"\'':
            q = c
            out.append(c); i += 1
            while i < n and src[i] != q:
                if src[i] == '\\':
                    out.append(src[i]); i += 1
                if i < n:
                    out.append(src[i]); i += 1
            if i < n:
                out.append(src[i]); i += 1
        else:
            out.append(c); i += 1
    return ''.join(out)


def split_params(text: str):
    """Split a parameter list on top-level commas."""
    parts, depth, cur = [], 0, ''
    for ch in text:
        if ch in '(<[':
            depth += 1
        elif ch in ')>]':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(cur); cur = ''
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return [p.strip() for p in parts if p.strip() and p.strip() != 'void']


def params_required(params):
    """How many arguments a caller must pass: parameters without a default value."""
    return sum(1 for p in params if '=' not in p)


def find_functions(src: str):
    """{(name): [(line, params, is_definition)]} for static / extern "C" functions."""
    found = {}
    for m in re.finditer(r'(?m)^[ \t]*(?:static|extern\s+"C")[^\n;{}]*?\b([A-Za-z_]\w*)\s*\(', src):
        name = m.group(1)
        i, depth = m.end(), 1
        while i < len(src) and depth:
            if src[i] == '(':
                depth += 1
            elif src[i] == ')':
                depth -= 1
            i += 1
        params = split_params(src[m.end():i - 1])
        rest = src[i:i + 200]
        is_def = re.match(r'\s*(const\s*)?\{', rest) is not None
        line = src[:m.start()].count('\n') + 1
        found.setdefault(name, []).append((line, params, is_def))
    return found


failures = []

# ------------------------------------------------------------------ 1. declaration vs definition
for f in FILES:
    src = strip_comments((ROOT / f).read_text())
    for name, entries in find_functions(src).items():
        decls = [e for e in entries if not e[2]]
        defs = [e for e in entries if e[2]]
        if not decls or not defs:
            continue
        want = len(defs[0][1])
        for line, params, _ in decls:
            if len(params) != want:
                failures.append(f'{f}:{line}: {name} declared with {len(params)} parameters, '
                                f'defined with {want} (definition at line {defs[0][0]})')

print('1) declaration vs definition:', 'FAIL' if failures else 'ok')
for x in failures:
    print('  ', x)

# ------------------------------------------------------------------------------ 2. call sites
# Internal functions the mod calls from more than one place (and that have changed shape before).
WATCHED = ['OLR_EdgeFill', 'OLUE3_TakeFrameStats', 'OLProxy_TakeDrawStats', 'OLProxy_PresentCount',
           'BlockZc', 'GpuTimerStart', 'GpuTimerStop']

arity_failures = []
for f in FILES:
    raw = (ROOT / f).read_text()
    src = strip_comments(raw)
    funcs = find_functions(src)
    for name in WATCHED:
        entries = funcs.get(name)
        if not entries:
            continue
        defs = [e for e in entries if e[2]] or entries
        lo, hi = params_required(defs[0][1]), len(defs[0][1])
        # every occurrence of `name(` that is not the declaration/definition itself
        decl_lines = {e[0] for e in entries}
        for m in re.finditer(r'\b' + re.escape(name) + r'\s*\(', src):
            line = src[:m.start()].count('\n') + 1
            if line in decl_lines:
                continue
            i, depth = m.end(), 1
            while i < len(src) and depth:
                if src[i] == '(':
                    depth += 1
                elif src[i] == ')':
                    depth -= 1
                i += 1
            args = src[m.end():i - 1]
            n = 0 if not args.strip() else len(split_params(args))
            if not (lo <= n <= hi):
                arity_failures.append(f'{f}:{line}: {name} called with {n} arguments, '
                                      f'it takes {lo}..{hi}')

print('2) call-site arity:', 'FAIL' if arity_failures else 'ok')
for x in arity_failures:
    print('  ', x)

if failures or arity_failures:
    sys.exit(1)
print('call arity: all checks passed')
