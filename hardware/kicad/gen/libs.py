"""Load and flatten KiCad library symbols, and report their pins."""
import copy
import os

from sexp import Sym, find, find_all, parse

KICAD_SYMBOLS = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols"
HERE = os.path.dirname(os.path.abspath(__file__))
LOCAL_SYMBOLS = os.path.join(HERE, "..", "lib")

_cache = {}


def _lib(name):
    if name not in _cache:
        path = os.path.join(KICAD_SYMBOLS, name + ".kicad_sym")
        if not os.path.exists(path):
            path = os.path.join(LOCAL_SYMBOLS, name + ".kicad_sym")
        with open(path, encoding="utf-8") as f:
            tree = parse(f.read())
        _cache[name] = {s[1]: s for s in find_all(tree, "symbol")}
    return _cache[name]


def flat_symbol(lib, name):
    """Return the symbol definition with any `extends` resolved, named lib:name."""
    syms = _lib(lib)
    sym = copy.deepcopy(syms[name])
    ext = find(sym, "extends")
    if ext:
        parent = flat_symbol(lib, ext[1])
        child_props = {p[1]: p for p in find_all(sym, "property")}
        merged = [Sym("symbol"), name]
        for c in parent[2:]:
            if isinstance(c, list) and c[0] == "property" and c[1] in child_props:
                merged.append(child_props.pop(c[1]))
            elif isinstance(c, list) and c[0] == "symbol":
                sub = copy.deepcopy(c)
                sub[1] = name + sub[1][len(ext[1]):]
                merged.append(sub)
            else:
                merged.append(c)
        merged.extend(child_props.values())
        sym = merged
    out = copy.deepcopy(sym)
    out[1] = f"{lib}:{name}"
    return out


def pins(sym):
    """[(unit, number, name, type, x, y, angle)] in symbol coordinates (y up)."""
    res = []
    base = sym[1].split(":")[-1]
    for sub in find_all(sym, "symbol"):
        suffix = sub[1][len(base) + 1:]
        unit = int(suffix.split("_")[0])
        style = int(suffix.split("_")[1])
        if style > 1:
            continue  # De Morgan alternate body
        for p in find_all(sub, "pin"):
            at = find(p, "at")
            res.append((unit, find(p, "number")[1], find(p, "name")[1], str(p[1]),
                        float(at[1]), float(at[2]), int(float(at[3])) if len(at) > 3 else 0))
    return res


if __name__ == "__main__":
    import sys
    for arg in sys.argv[1:]:
        lib, name = arg.split(":")
        s = flat_symbol(lib, name)
        print(f"== {arg}")
        for p in sorted(pins(s), key=lambda p: (p[0], p[1].zfill(4))):
            print("  u%d %-4s %-14s %-14s (%.2f, %.2f) %d" % p)
