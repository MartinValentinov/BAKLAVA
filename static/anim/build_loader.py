#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_loader.py - build static/anim/loader.svg from the frames in static/img.

USAGE
-----
    python3 static/anim/build_loader.py

Re-run it after redrawing any frame. Tuning knobs are CYCLE_S and FADE_FRAC
below; BBOX is the union of all frames' content bounds, measured in a browser.
"""

from __future__ import annotations

import math
import os
import re
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.path.join(os.path.dirname(HERE), "img")
OUT = os.path.join(HERE, "loader.svg")
SVG_NS = "{http://www.w3.org/2000/svg}"

FRAMES = ["frame_0_8", "frame_1", "frame_2", "frame_3",
          "frame_5", "frame_6", "frame_7"]
GEOMETRY_SOURCE = "frame_0_8"

BBOX = (54.05, 108.02, 86.69, 124.67)
PAD = 1.5

CYCLE_S = 1.8
FADE_FRAC = 0.55

INHERIT = ("fill", "fill-opacity", "fill-rule", "stroke", "stroke-opacity",
           "stroke-width", "stroke-linecap", "stroke-linejoin",
           "stroke-miterlimit", "stroke-dasharray")

def mat_mul(m, n):
    a1, b1, c1, d1, e1, f1 = m
    a2, b2, c2, d2, e2, f2 = n
    return (a1 * a2 + c1 * b2, b1 * a2 + d1 * b2,
            a1 * c2 + c1 * d2, b1 * c2 + d1 * d2,
            a1 * e2 + c1 * f2 + e1, b1 * e2 + d1 * f2 + f1)

def parse_transform(text):
    m = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
    if not text:
        return m
    for fn, args in re.findall(r"([a-zA-Z]+)\s*\(([^)]*)\)", text):
        v = [float(x) for x in re.split(r"[\s,]+", args.strip()) if x]
        if fn == "translate":
            m = mat_mul(m, (1, 0, 0, 1, v[0], v[1] if len(v) > 1 else 0))
        elif fn == "scale":
            sx = v[0]
            sy = v[1] if len(v) > 1 else sx
            m = mat_mul(m, (sx, 0, 0, sy, 0, 0))
        elif fn == "rotate":
            a = math.radians(v[0])
            cos, sin = math.cos(a), math.sin(a)
            r = (cos, sin, -sin, cos, 0.0, 0.0)
            if len(v) == 3:
                cx, cy = v[1], v[2]
                r = mat_mul((1, 0, 0, 1, cx, cy), mat_mul(r, (1, 0, 0, 1, -cx, -cy)))
            m = mat_mul(m, r)
        elif fn == "matrix":
            m = mat_mul(m, tuple(v[:6]))
        elif fn == "skewX":
            m = mat_mul(m, (1, 0, math.tan(math.radians(v[0])), 1, 0, 0))
        elif fn == "skewY":
            m = mat_mul(m, (1, math.tan(math.radians(v[0])), 0, 1, 0, 0))
        else:
            raise SystemExit(f"unhandled transform function '{fn}' - add it here")
    return m

def fmt_matrix(m):
    return "matrix(%s)" % ",".join(("%.6f" % v).rstrip("0").rstrip(".") or "0" for v in m)

def parse_style(text):
    out = {}
    for bit in (text or "").split(";"):
        if ":" in bit:
            k, v = bit.split(":", 1)
            out[k.strip()] = v.strip()
    return out

def resolved_style(chain, el):
    style = {}
    for node in list(chain) + [el]:
        merged = parse_style(node.get("style"))
        for k in INHERIT:
            if node.get(k) is not None:
                merged.setdefault(k, node.get(k))
        for k, v in merged.items():
            if node is el or k in INHERIT:
                style[k] = v
    style.pop("display", None)
    return style

def norm_d(d):
    return re.sub(r"\s+", " ", d or "").strip()

def frame_shapes(name):
    with open(os.path.join(IMG, name + ".svg"), encoding="utf-8") as fh:
        return {norm_d(d) for d in re.findall(r'\bd="([^"]+)"', fh.read())}

def collect_paths(name):
    root = ET.parse(os.path.join(IMG, name + ".svg")).getroot()
    found = []

    def walk(el, chain, mat):
        for ch in el:
            tag = ch.tag.replace(SVG_NS, "")
            m = mat_mul(mat, parse_transform(ch.get("transform")))
            if tag == "g":
                walk(ch, chain + [ch], m)
            elif tag == "path":
                found.append((norm_d(ch.get("d")), m, resolved_style(chain, ch)))
            elif tag in ("rect", "circle", "ellipse", "polygon", "polyline",
                         "line", "text", "image", "use"):
                raise SystemExit(
                    f"{name}.svg contains <{tag}>; this builder only flattens "
                    "<path>. Convert it in Inkscape (Path > Object to Path).")

    walk(root, [], (1.0, 0.0, 0.0, 1.0, 0.0, 0.0))
    return found

def main():
    present = {f: frame_shapes(f) for f in FRAMES}
    n = len(FRAMES)

    paths = collect_paths(GEOMETRY_SOURCE)
    missing = [d for d, _, _ in paths if not any(d in present[f] for f in FRAMES)]
    if missing:
        raise SystemExit(f"{len(missing)} shapes in {GEOMETRY_SOURCE} appear in no frame")

    pieces = {}
    for d, mat, style in paths:
        key = tuple(1 if d in present[f] else 0 for f in FRAMES)
        pieces.setdefault(key, []).append((d, mat, style))

    windows = {}
    for key in pieces:
        on = [i for i, b in enumerate(key) if b]
        start = next(i for i in on if key[(i - 1) % n] == 0)
        length = len(on)
        if any(key[(start + k) % n] != 1 for k in range(length)):
            raise SystemExit(
                f"piece {''.join(map(str, key))} is not a contiguous run of frames; "
                "it needs more than one fade per cycle")
        windows[key] = (start, length)

    slot = 100.0 / n
    fade = slot * FADE_FRAC
    fade_frac = fade / 100.0
    order = sorted(pieces, key=lambda k: windows[k][0])

    css = ["  .bk-p{opacity:0;animation-timing-function:linear;"
           "animation-iteration-count:infinite;animation-duration:%.2fs}" % CYCLE_S]
    for length in sorted({w[1] for w in windows.values()}):
        css.append(
            "  @keyframes bk-hold%d{0%%{opacity:0}%.3f%%{opacity:1}%.3f%%{opacity:1}"
            "%.3f%%{opacity:0}100%%{opacity:0}}"
            % (length, fade, length * slot, length * slot + fade))
    for idx, key in enumerate(order):
        start, length = windows[key]
        phase = (((start - 0.5) / n) - fade_frac / 2.0) % 1.0
        css.append("  .bk-p%d{animation-name:bk-hold%d;animation-delay:%.4fs}"
                   % (idx, length, CYCLE_S * (phase - 1.0)))
    css += [
        "  @media(prefers-reduced-motion:reduce){",
        "    .bk-p{animation-name:bk-breathe;animation-delay:0s;"
        "animation-duration:2s;animation-timing-function:ease-in-out}",
        "    @keyframes bk-breathe{0%,100%{opacity:1}50%{opacity:.5}}",
        "  }",
    ]

    x, y, w, h = BBOX
    vb = f"{x - PAD:.2f} {y - PAD:.2f} {w + 2 * PAD:.2f} {h + 2 * PAD:.2f}"
    out = [
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="%s" role="img">' % vb,
        "<title>BAKLAVA — processing</title>",
        "<desc>The BAKLAVA logo coming apart piece by piece and rebuilding, "
        "looping while the scene is processed.</desc>",
        "<style>\n%s\n</style>" % "\n".join(css),
    ]
    for idx, key in enumerate(order):
        out.append('<g class="bk-p bk-p%d">' % idx)
        for d, mat, style in pieces[key]:
            st = ";".join(f"{k}:{v}" for k, v in style.items())
            out.append('<path d="%s" transform="%s" style="%s"/>'
                       % (d, fmt_matrix(mat), st))
        out.append("</g>")
    out.append("</svg>")

    text = "\n".join(out) + "\n"
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write(text)

    print(f"wrote {OUT}  ({len(text)/1024:.1f} KB)")
    print(f"  {n} frames -> {len(order)} pieces, {len(paths)} paths emitted once each")
    for idx, key in enumerate(order):
        start, length = windows[key]
        frames = [FRAMES[(start + k) % n] for k in range(length)]
        print(f"  piece {idx}: {len(pieces[key])} paths, frames {frames}")
    print(f"  {CYCLE_S}s loop, fade {fade:.2f}% of cycle, viewBox '{vb}'")

if __name__ == "__main__":
    main()
