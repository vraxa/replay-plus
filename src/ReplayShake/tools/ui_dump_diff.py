#!/usr/bin/env python3
"""Find the replay editor's playhead and keyframes in two of ReSkate's `ui dump` files.

Take one dump, move the replay editor's playhead (or a keyframe), take another, then:

    python ui_dump_diff.py ui-dump-0x1234abcd-1000.txt ui-dump-0x1234abcd-2000.txt

It lists every number that changed between the two, read as the formats `replayshake map` takes,
with its path written the way `replayshake map` wants it, and every list whose length changed.
`--find 2.5` instead lists every number in the first dump within 0.01 of 2.5 (a keyframe's time as
the editor shows it, say). Standard library only.
"""
import argparse
import math
import re
import struct
import sys

FIELD = re.compile(r"^(0x[0-9a-f]{8})@0x[0-9a-f]+ <0x[0-9a-f]{8} k(\d+) (\d+)>(.*)$")
ELEMENT = re.compile(r"^\[(\d+)\] <0x[0-9a-f]{8} k(\d+) (\d+)>(.*)$")
ROOT = re.compile(r"^root 0x[0-9a-f]+ <(0x[0-9a-f]{8}) \d+>$")
COUNT = re.compile(r"^ \[(\d+)\]$")
BYTES = re.compile(r"^ = ([0-9a-f]+)$")


def parse(path):
    """Every scalar (as bytes) and list (as its length) in a dump, by root and field path."""
    values, lists = {}, {}
    root = None
    seen_roots = {}
    stack = []  # (depth, path segment or None for a step the mapping follows by itself)
    with open(path, encoding="utf-8", errors="replace") as file:
        for raw in file:
            line = raw.rstrip("\n")
            match = ROOT.match(line)
            if match:
                schema = match.group(1)
                seen_roots[schema] = seen_roots.get(schema, 0) + 1
                root = schema if seen_roots[schema] == 1 else f"{schema}#{seen_roots[schema]}"
                stack = []
                continue
            if root is None or not line.startswith("  "):
                continue
            spaces = len(line) - len(line.lstrip(" "))
            depth = (spaces - 2) // 2
            text = line.strip()
            while stack and stack[-1][0] >= depth:
                stack.pop()
            field = FIELD.match(text)
            element = ELEMENT.match(text)
            if field:
                segment, rest = field.group(1), field.group(4)
            elif element:
                segment, rest = f"[{element.group(1)}]", element.group(4)
            else:
                # "(model) -> handle ..." and "(record data) ...": steps a path takes by itself.
                stack.append((depth, None))
                continue
            stack.append((depth, segment))
            key = (root, tuple(s for _, s in stack if s is not None))
            count = COUNT.match(rest)
            data = BYTES.match(rest)
            if count:
                lists[key] = int(count.group(1))
            elif data and len(data.group(1)) in (8, 16):
                values[key] = bytes.fromhex(data.group(1))
    return values, lists


def readings(data):
    """The value as each format `replayshake map` takes for its size."""
    if len(data) == 4:
        return {"f32": struct.unpack("<f", data)[0], "i32": struct.unpack("<i", data)[0], "u32": struct.unpack("<I", data)[0]}
    return {"f64": struct.unpack("<d", data)[0], "i64": struct.unpack("<q", data)[0]}


def shown(value):
    if isinstance(value, float):
        return "nan" if math.isnan(value) else f"{value:.6g}"
    return str(value)


def path_text(key):
    root, segments = key
    text, out = "", []
    for segment in segments:
        if segment.startswith("["):
            text += segment
        else:
            text += ("/" if text else "") + segment
    out.append(f"{root}: {text}")
    return out[0]


def mapping_hint(key):
    """How `replayshake map` would take this path: an element of a list maps as keyframes."""
    root, segments = key
    for index, segment in enumerate(segments):
        if segment.startswith("["):
            array = "/".join(segments[:index])
            inside = "/".join(s for s in segments[index + 1:] if not s.startswith("[")) or "."
            return f"map root {root.split('#')[0]}; map keyframes {array} <format> {inside}"
    return f"map root {root.split('#')[0]}; map playhead <format> {'/'.join(segments)}  (or map selected, for an index)"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("before")
    parser.add_argument("after", nargs="?")
    parser.add_argument("--find", type=float, help="list numbers in BEFORE near this value instead of diffing")
    parser.add_argument("--within", type=float, default=0.01, help="how near --find must be (default 0.01)")
    args = parser.parse_args()
    before_values, before_lists = parse(args.before)
    if args.find is not None:
        found = 0
        for key, data in sorted(before_values.items()):
            for name, value in readings(data).items():
                if isinstance(value, float) and not math.isfinite(value):
                    continue
                if abs(value - args.find) <= args.within:
                    print(f"{path_text(key)}  {name} {shown(value)}\n    {mapping_hint(key).replace('<format>', name)}")
                    found += 1
        print(f"{found} number(s) near {args.find}.")
        return 0
    if not args.after:
        parser.error("give two dumps to compare, or --find with one")
    after_values, after_lists = parse(args.after)
    changed = 0
    for key in sorted(set(before_values) & set(after_values)):
        old, new = before_values[key], after_values[key]
        if old == new:
            continue
        old_read, new_read = readings(old), readings(new)
        parts = ", ".join(f"{name} {shown(old_read[name])} -> {shown(new_read[name])}" for name in old_read)
        print(f"{path_text(key)}\n    {parts}\n    {mapping_hint(key)}")
        changed += 1
    for key in sorted(set(before_lists) & set(after_lists)):
        if before_lists[key] != after_lists[key]:
            print(f"{path_text(key)}  list of {before_lists[key]} -> {after_lists[key]}")
            changed += 1
    only = len(set(before_values) ^ set(after_values))
    print(f"{changed} change(s); {only} value(s) in only one of the dumps (models that came or went).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
