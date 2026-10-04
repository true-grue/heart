#!/usr/bin/env python3
"""Bake a TrueType font into the flat edge-soup format read by src/text/font.c.

Curves are flattened here so the runtime rasteriser never learns about Beziers.
This is a development tool: fontTools and the TTF never reach the game build.

    py3 tools/py/bake_font.py DejaVuSans.ttf assets/font/sans.font
"""

import math
import sys

from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.ttLib import TTFont

# U+0020..U+007E printable ASCII
ASCII = range(0x20, 0x7F)
# U+0400..U+045F Russian, including Yo
CYRILLIC = range(0x0400, 0x0460)
# Quotation marks, dashes, ellipsis, non-breaking space
EXTRA = [0x00A0, 0x00AB, 0x00BB, 0x2013, 0x2014, 0x2026]

MAX_DEPTH = 8

# Codepoints that are legitimately blank.
BLANK = {0x20, 0xA0}


def wanted_codepoints():
    out = set(ASCII) | set(CYRILLIC) | set(EXTRA)
    return sorted(out)


def flat_enough(p0, c, p1, tol):
    """True when the control point sits within tol of the chord p0-p1."""
    dx = p1[0] - p0[0]
    dy = p1[1] - p0[1]
    d2 = dx * dx + dy * dy
    if d2 == 0:
        return True
    d = abs(dx * (p0[1] - c[1]) - dy * (p0[0] - c[0])) / math.sqrt(d2)
    return d <= tol


def flatten_quad(p0, c, p1, tol, out):
    """Append the line segments approximating one quadratic Bezier."""
    if flat_enough(p0, c, p1, tol):
        out.append((p0, p1))
        return
    if MAX_DEPTH <= 0:
        out.append((p0, p1))
        return
    p01 = ((p0[0] + c[0]) / 2.0, (p0[1] + c[1]) / 2.0)
    p12 = ((c[0] + p1[0]) / 2.0, (c[1] + p1[1]) / 2.0)
    mid = ((p01[0] + p12[0]) / 2.0, (p01[1] + p12[1]) / 2.0)
    flatten_quad(p0, p01, mid, tol, out)
    flatten_quad(mid, p12, p1, tol, out)


def split_contours(ops):
    """RecordingPen gives a flat op list; a contour starts at every moveTo."""
    contours = []
    cur = []
    for op, args in ops:
        if op == "moveTo":
            if cur:
                contours.append(cur)
            cur = [(op, args)]
        elif cur:
            cur.append((op, args))
    if cur:
        contours.append(cur)
    return contours


def contour_edges(contour, tol):
    """Turn one recorded contour into line segments.

    qCurveTo carries a run of off-curve points closed by a single on-curve
    point, which is TrueType's implied-midpoint convention.
    """
    out = []
    cur = contour[0][1][0]
    start = cur
    for op, args in contour[1:]:
        if op == "lineTo":
            out.append((cur, args[0]))
            cur = args[0]
        elif op == "qCurveTo":
            pts = args
            last = pts[-1]
            for i in range(len(pts) - 1):
                ctrl = pts[i]
                if i + 1 < len(pts) - 1:
                    nxt = ((pts[i][0] + pts[i + 1][0]) / 2.0,
                           (pts[i][1] + pts[i + 1][1]) / 2.0)
                else:
                    nxt = last
                flatten_quad(cur, ctrl, nxt, tol, out)
                cur = nxt
            cur = last
        elif op == "curveTo":
            raise SystemExit("cubic segments are not supported by this format")
    if cur != start:
        out.append((cur, start))
    return out


def bake(path, bake_px, tol_px):
    font = TTFont(path)
    upem = font["head"].unitsPerEm
    hhea = font["hhea"]
    hmtx = font["hmtx"].metrics
    glyph_set = font.getGlyphSet()
    cmap = font.getBestCmap()
    tol = tol_px * upem / bake_px

    # DejaVu builds many Cyrillic letters as composite glyphs that reference the
    # Latin ones, so a plain RecordingPen yields a single addComponent and no
    # outline at all. DecomposingRecordingPen resolves those references.
    pen = DecomposingRecordingPen(glyph_set)
    glyphs = []
    missing = 0
    empty = []

    for cp in wanted_codepoints():
        name = cmap.get(cp)
        if name is None:
            missing += 1
            continue
        pen.value = []
        glyph_set[name].draw(pen)
        contours = split_contours(pen.value)

        segs = []
        for contour in contours:
            if len(contour) >= 2:
                segs.extend(contour_edges(contour, tol))

        if not segs and cp not in BLANK:
            # Blanks are legitimately empty; anything else means the pen did
            # not hand over an outline, which once shipped 43 invisible glyphs.
            empty.append(cp)
        advance = hmtx[name][0]
        glyphs.append((cp, advance, segs))

    glyphs.sort(key=lambda g: g[0])
    return upem, hhea.ascender, hhea.descender, hhea.lineGap, glyphs, missing, empty


def emit(out, upem, asc, desc, gap, glyphs, bake_px, tol_px, source):
    total = sum(len(g[2]) for g in glyphs)
    out.write("# generated from %s by tools/py/bake_font.py -- do not edit\n" % source)
    out.write("font 1\n")
    out.write("upem %d\n" % upem)
    out.write("bake %d %d\n" % (bake_px, int(round(tol_px * 100))))
    out.write("asc %d\n" % asc)
    out.write("desc %d\n" % desc)
    out.write("line %d\n" % (asc - desc + gap))
    out.write("glyphs %d\n" % len(glyphs))
    for cp, advance, segs in glyphs:
        out.write("glyph %d %d %d\n" % (cp, advance, len(segs)))
        for (x0, y0), (x1, y1) in segs:
            out.write("  %d %d %d %d\n" % (round(x0), round(y0), round(x1), round(y1)))
    return total


def main(argv):
    if len(argv) != 3:
        raise SystemExit("usage: bake_font.py <input.ttf> <output.font>")
    bake_px = 64
    tol_px = 0.5
    upem, asc, desc, gap, glyphs, missing, empty = bake(argv[1], bake_px, tol_px)
    with open(argv[2], "w", encoding="utf-8") as out:
        total = emit(out, upem, asc, desc, gap, glyphs, bake_px, tol_px, argv[1])
    sys.stderr.write(
        "%s: %d glyphs, %d segments, %d not in font\n"
        % (argv[2], len(glyphs), total, missing))
    if empty:
        sys.stderr.write(
            "ERROR: %d glyphs produced no outline: %s\n"
            % (len(empty), " ".join("U+%04X" % c for c in empty[:8])))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
