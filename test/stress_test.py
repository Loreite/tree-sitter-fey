#!/usr/bin/env python3
"""Parser-level stress tests for pair tags.

    npx tree-sitter build -o fey.so
    pip install tree-sitter
    python3 test/stress_test.py fey.so
"""
import ctypes
import random
import sys

from tree_sitter import Language, Parser

lib = ctypes.cdll.LoadLibrary(sys.argv[1] if len(sys.argv) > 1 else "./fey.so")
lib.tree_sitter_fey.restype = ctypes.c_void_p
import warnings
with warnings.catch_warnings():
    warnings.simplefilter('ignore', DeprecationWarning)
    FEY = Language(lib.tree_sitter_fey())
parser = Parser(FEY)

failures = 0
sys.setrecursionlimit(20000)  # the trees are 255 pairs deep


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", msg)


def count(node, typ):
    n = 1 if node.type == typ else 0
    return n + sum(count(c, typ) for c in node.children)


def max_pair_depth(node, d=0):
    d2 = d + (node.type == "pair_tag")
    return max([d2] + [max_pair_depth(c, d2) for c in node.children])


def sexp(node):
    return str(node)


def nest(depth, name):
    opens = "".join(f"[ {name(k)} #]" for k in range(depth))
    closes = "".join(f"[# {name(k)} ]" for k in reversed(range(depth)))
    return f"start {opens}x{closes} end\n"


# 1. 200 deep, all custom (hashed) names: no errors, all pairs nested.
src = nest(200, lambda k: f"custom_{k}")
tree = parser.parse(src.encode())
check(not tree.root_node.has_error, "200 deep all-hash parses cleanly")
check(max_pair_depth(tree.root_node) == 200, "200 deep: nesting depth 200")
check(count(tree.root_node, "stray_close") == 0, "200 deep: no strays")
print("200 deep all-hash: ok" if not tree.root_node.has_error else "200 deep: ERROR")

# 2. 255 deep mixed HTML / custom names: the scanner limit.
html = ["b", "i", "em", "span", "div", "a", "code", "strong"]
src = nest(255, lambda k: html[k % len(html)] if k % 2 else f"w{k}")
tree = parser.parse(src.encode())
check(not tree.root_node.has_error, "255 deep mixed parses cleanly")
check(max_pair_depth(tree.root_node) == 255, "255 deep: nesting depth 255")
print("255 deep mixed: ok" if not tree.root_node.has_error else "255 deep: ERROR")

# 3. 300 deep: openers past 255 are refused -> errors, but no crash.
src = nest(300, lambda k: f"n{k}")
tree = parser.parse(src.encode())
# The 45 refused openers fall back to plain text, so their closers no longer
# match anything and come out as stray_close nodes. At least those 45: a closer
# that matches no opener may stand for one of the entries the serialized state
# had to forget (they match any closer), which closes the pairs inside it, so
# there are more. No ERROR, no crash.
strays = count(tree.root_node, "stray_close")
check(max_pair_depth(tree.root_node) == 255, "300 deep: capped at 255")
check(strays >= 45, f"300 deep: at least 45 stray closers, got {strays}")
check(not tree.root_node.has_error, "300 deep: no error")
print(f"300 deep: capped at 255, the refused openers became text + {strays} stray_close")

# 4. Incremental reparse equals a full parse after random edits.
random.seed(7)
base = (
    "Intro [ b #]bold [ i #]both[# i ][# b ] text.\n"
    + nest(60, lambda k: ["em", "span", f"cu-{k}"][k % 3])
    + "[ div, card #]\nInside [ q #]quote[# q ].\n| a | b |\n[# div ]\n"
    + "  1. Heading [ em #]t[# em ]\nbody [ x-y #]z[# x-y ]\n"
)
edits = 0
for trial in range(300):
    old_src = base.encode()
    old_tree = parser.parse(old_src)
    pos = random.randrange(len(old_src))
    kind = random.choice(["ins", "del", "rep"])
    piece = random.choice([b"x", b" ", b"[", b"#", b"]", b"\n", b"[ b #]", b"[# b ]", b"q"])
    if kind == "ins":
        new_src = old_src[:pos] + piece + old_src[pos:]
        old_end, new_end = pos, pos + len(piece)
    elif kind == "del":
        n = random.randint(1, 4)
        new_src = old_src[:pos] + old_src[pos + n:]
        old_end, new_end = min(pos + n, len(old_src)), pos
    else:
        n = random.randint(1, 3)
        new_src = old_src[:pos] + piece + old_src[pos + n:]
        old_end, new_end = min(pos + n, len(old_src)), pos + len(piece)

    def point(b, off):
        line = b.count(b"\n", 0, off)
        col = off - (b.rfind(b"\n", 0, off) + 1)
        return (line, col)

    old_tree.edit(
        start_byte=pos, old_end_byte=old_end, new_end_byte=new_end,
        start_point=point(old_src, pos), old_end_point=point(old_src, old_end),
        new_end_point=point(new_src, new_end),
    )
    inc = parser.parse(new_src, old_tree)
    full = parser.parse(new_src)
    if sexp(inc.root_node) != sexp(full.root_node):
        check(False, f"incremental != full after {kind} {piece!r} at {pos}")
        break
    edits += 1
    if trial % 3 == 0:  # keep the document drifting
        base = new_src.decode(errors="replace")
print(f"incremental reparse: {edits} random edits matched full parses")

print("FAILURES:", failures) if failures else print("all stress tests passed")
sys.exit(1 if failures else 0)
