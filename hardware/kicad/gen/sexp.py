"""Minimal S-expression reader/writer for KiCad files. Stdlib only."""
import re

_TOKEN = re.compile(r'\s*(?:(\()|(\))|("(?:[^"\\]|\\.)*")|([^\s()"]+))')


class Sym(str):
    """An unquoted atom (keyword or number), as opposed to a quoted string."""


def parse(text):
    stack, cur, pos = [], [], 0
    while True:
        m = _TOKEN.match(text, pos)
        if not m or m.end() == pos:
            break
        pos = m.end()
        lp, rp, q, atom = m.groups()
        if lp:
            stack.append(cur)
            cur = []
        elif rp:
            done, cur = cur, stack.pop()
            cur.append(done)
        elif q is not None:
            cur.append(re.sub(r'\\(.)', lambda e: "\n" if e.group(1) == "n" else e.group(1), q[1:-1]))
        else:
            cur.append(Sym(atom))
    return cur[0]


def q(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def dump(node, indent=0):
    if not isinstance(node, list):
        if isinstance(node, Sym):
            return str(node)
        if isinstance(node, bool):
            return "yes" if node else "no"
        if isinstance(node, (int, float)):
            return fmt_num(node)
        return q(node)
    pad = "\t" * indent
    if not any(isinstance(c, list) for c in node):
        return pad + "(" + " ".join(dump(c) for c in node) + ")"
    head = [c for c in node if not isinstance(c, list)]
    out = pad + "(" + " ".join(dump(c) for c in head)
    for c in node:
        if isinstance(c, list):
            out += "\n" + dump(c, indent + 1)
    return out + "\n" + pad + ")"


def fmt_num(v):
    if isinstance(v, int):
        return str(v)
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def find(node, key):
    for c in node:
        if isinstance(c, list) and c and c[0] == key:
            return c
    return None


def find_all(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]
