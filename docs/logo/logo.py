#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Generates docs/logo/tinycc-os-logo.svg: python3 docs/logo/logo.py > docs/logo/tinycc-os-logo.svg
#
# A horned viper (Cerastes) curled into a "C", seen from the side: the back is the outer edge of
# the ring, the belly with its ventral scales the inner edge.  The head is in profile with the
# jaws open toward the tail, a fang, a slit pupil and the horn above the eye.  One colour,
# outlines only.
import math

C = "#00e5ff"
cx, cy, R = 256.0, 236.0, 150.0
TH0, SPAN = -40.0, 318.0          # neck angle (deg) and how far the body runs (counterclockwise)


def center(s):
    th = math.radians(TH0 - s * SPAN)
    r = R + 4.0 * math.sin(2 * math.pi * s * 2.0)          # a slight ripple along the body
    return cx + r * math.cos(th), cy + r * math.sin(th), th


def width(s):
    """Thin neck, heavy body, a viper's short tail tapering to a point."""
    if s < 0.14:
        return 22 + 16 * math.sin(s / 0.14 * math.pi / 2)
    if s < 0.80:
        return 38 + 3 * math.sin((s - 0.14) / 0.66 * math.pi)
    return 38 * (1 - ((s - 0.80) / 0.20)) ** 1.25 + 0.8


N = 400
spine = []                                                 # (x, y, nx, ny, w, s)
for i in range(N + 1):
    s = i / N
    x, y, th = center(s)
    spine.append((x, y, math.cos(th), math.sin(th), width(s), s))
outer = [(x + nx * w / 2, y + ny * w / 2) for x, y, nx, ny, w, s in spine]
inner = [(x - nx * w / 2, y - ny * w / 2) for x, y, nx, ny, w, s in spine]

# The head is drawn in its own frame: u forward from the neck (toward the tail tip), v up
# (outward, the back).  The frame bends a little with the ring so the head follows the curve.
x0, y0, th0 = center(0.0)
vx, vy = math.cos(th0), math.sin(th0)
REFF = 320.0
bx, by = x0 - vx * REFF, y0 - vy * REFF


def H(u, v):
    a = u / REFF
    dx, dy = vx * math.cos(a) - vy * math.sin(a), vy * math.cos(a) + vx * math.sin(a)
    return bx + dx * (REFF + v), by + dy * (REFF + v)


def path(cmds):
    """cmds: [('M', u, v), ('L', u, v), ('C', u1, v1, u2, v2, u, v)] in head coordinates."""
    out = []
    for c in cmds:
        p = c[1:]
        xy = [H(p[i], p[i + 1]) for i in range(0, len(p), 2)]
        out.append(c[0] + " ".join("%.1f %.1f" % q for q in xy))
    return " ".join(out)


w0 = width(0.0) / 2
# one closed contour: neck top -> crown -> snout -> upper lip back to the gape -> lower jaw out
# to the chin -> under the jaw -> throat -> neck bottom
head = [
    ('M', 0, w0),
    ('C', 8, w0 + 1, 14, 18, 24, 19.5),                    # the jowls swell behind the eye
    ('C', 30, 20.5, 36, 19.5, 40, 19),                     # crown up to the brow
    ('C', 46, 18.5, 50, 18, 54, 15.5),                     # brow ridge over the eye
    ('C', 59, 12.5, 63, 9.5, 66, 6),                       # canthus down to the snout
    ('C', 68.5, 3, 68, -0.5, 65, -1.5),                    # rounded snout tip
    ('C', 52, -2.5, 36, -3, 22, -4.5),                     # upper lip back to the corner of the mouth
    ('L', 56, -25),                                        # lower jaw, open
    ('C', 60, -27.5, 60.5, -31, 56.5, -31.5),              # chin
    ('C', 44, -31, 30, -26, 20, -21),                      # under the jaw
    ('C', 12, -17, 6, -w0 - 1, 0, -w0),                    # throat into the neck
]
# the horn: one enlarged supraocular scale, a spike curving back; the second one, over the far
# eye, shows just behind it
horn_near = [('M', 37, 19.2), ('C', 38, 27, 36.5, 33, 32, 39.5), ('C', 37.5, 34, 42, 27, 45, 18.8)]
horn_far = [('M', 31, 20), ('C', 31, 25, 29.5, 29.5, 26.5, 33.5), ('C', 30.5, 30, 33.5, 26, 35.5, 19.8)]
brow = [('M', 35, 14), ('C', 39, 17.5, 47, 17.5, 51, 13)]
fang = [('M', 57, -2.0), ('C', 58, -8, 56.5, -13, 52, -16.5), ('C', 54, -12, 53.5, -7, 52, -2.4)]
fang2 = [('M', 50.5, -2.6), ('C', 51, -6, 50, -9, 47.5, -11), ('C', 48.5, -8, 48, -5.5, 46.8, -2.8)]
stripe = [('M', 37.5, 5.5), ('C', 33, 3, 28, 0, 23.5, -3)]   # the dark stripe from eye to gape
lip = [('M', 26, -6.5), ('C', 30, -10, 34, -13, 38, -15)]     # inside of the lower jaw
nostril = H(61.5, 5.5)
eye = H(43.5, 9.0)
eye_r = 6.2
pa, pb = H(43.5, 13.2), H(43.5, 4.8)                     # the slit pupil, along the head's v axis

# the contour: tail tip -> along the back to the neck -> head -> along the belly back to the tail
headd = path(head)
body = ("M%.1f %.1f " % outer[-1] + " ".join("L%.1f %.1f" % q for q in outer[-2::-1]) + " " +
        headd.replace("M", "L", 1) + " " + " ".join("L%.1f %.1f" % q for q in inner[1:]) + " Z")

# ventral scales: a belly line at about a third of the width, with cross lines down to the edge
belly_line, ticks = [], []
step, acc = 7.5, 0.0
for i, (x, y, nx, ny, w, s) in enumerate(spine):
    if s < 0.035 or s > 0.975:
        continue
    k = 0.30 * w
    p_in = (x - nx * w / 2, y - ny * w / 2)
    p_bl = (p_in[0] + nx * k, p_in[1] + ny * k)
    belly_line.append(p_bl)
    px, py = spine[i - 1][0], spine[i - 1][1]
    acc += math.hypot(x - px, y - py)
    if acc >= step * (0.55 + 0.45 * w / 40):
        acc = 0.0
        ticks.append((p_in, p_bl))
bellyd = "M" + " L".join("%.1f %.1f" % q for q in belly_line)
ticksd = " ".join("M%.1f %.1f L%.1f %.1f" % (a[0], a[1], b[0], b[1]) for a, b in ticks)

# the back: a row of the rounded saddle blotches of a Cerastes, smaller toward the tail
blotches = []
s = 0.05
while s < 0.93:
    x, y, th = center(s)
    w = width(s)
    nx, ny = math.cos(th), math.sin(th)
    px, py = x + nx * w * 0.14, y + ny * w * 0.14
    blotches.append((px, py, w * 0.26, w * 0.17, math.degrees(th) + 90))
    s += w * 1.15 / (R * math.radians(SPAN))
blotchd = "\n".join('    <ellipse cx="%.1f" cy="%.1f" rx="%.1f" ry="%.1f" transform="rotate(%.1f %.1f %.1f)"/>'
                    % (px, py, rx, ry, a, px, py) for px, py, rx, ry, a in blotches)

print(f'''<?xml version="1.0" encoding="UTF-8"?>
<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Autarkeia OS logo: an ouroboros "C", a horned viper reaching for its own tail (the system
     builds its own compiler and kernel), drawn as one-colour outlines. Generated by docs/logo/logo.py. -->
<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512">
  <title>Autarkeia OS</title>
  <defs>
    <filter id="glow" x="-20%" y="-20%" width="140%" height="140%">
      <feGaussianBlur stdDeviation="3" result="b"/>
      <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
  </defs>
  <rect x="8" y="8" width="496" height="496" rx="72" fill="#060a13"/>
  <rect x="8" y="8" width="496" height="496" rx="72" fill="none" stroke="{C}" stroke-opacity="0.25" stroke-width="2"/>
  <g stroke="{C}" stroke-opacity="0.07" stroke-width="1">
    <path d="M40 120 H472 M40 200 H472 M40 280 H472 M40 360 H472 M120 40 V472 M200 40 V472 M280 40 V472 M360 40 V472"/>
  </g>
  <circle cx="{cx:.0f}" cy="{cy:.0f}" r="196" fill="none" stroke="{C}" stroke-opacity="0.35" stroke-width="5" stroke-dasharray="2 10"/>
  <g fill="none" stroke="{C}" stroke-linejoin="round" stroke-linecap="round" filter="url(#glow)">
    <path d="{body}" stroke-width="4"/>
    <path d="{path(horn_near)}" stroke-width="3"/>
    <path d="{path(horn_far)}" stroke-width="2.2" stroke-opacity="0.7"/>
    <path d="{path(brow)}" stroke-width="2"/>
    <path d="{path(fang)}" stroke-width="2"/>
    <path d="{path(fang2)}" stroke-width="1.6" stroke-opacity="0.7"/>
    <path d="{path(stripe)}" stroke-width="2"/>
    <path d="{path(lip)}" stroke-width="1.6" stroke-opacity="0.7"/>
    <circle cx="{eye[0]:.1f}" cy="{eye[1]:.1f}" r="{eye_r}" stroke-width="2.4"/>
    <path d="M{pa[0]:.1f} {pa[1]:.1f} L{pb[0]:.1f} {pb[1]:.1f}" stroke-width="2.4"/>
    <circle cx="{nostril[0]:.1f}" cy="{nostril[1]:.1f}" r="1.4" stroke-width="1.6"/>
  </g>
  <g fill="none" stroke="{C}" stroke-linecap="round" stroke-opacity="0.75" filter="url(#glow)">
    <path d="{bellyd}" stroke-width="1.8"/>
    <path d="{ticksd}" stroke-width="1.4"/>
  </g>
  <g fill="none" stroke="{C}" stroke-opacity="0.6" stroke-width="1.6" filter="url(#glow)">
{blotchd}
  </g>
  <g font-family="'DejaVu Sans Mono', 'Liberation Mono', monospace" text-anchor="middle" fill="{C}">
    <text x="256" y="261" font-size="74" font-weight="bold" filter="url(#glow)">tcc</text>
    <text x="256" y="452" font-size="27" letter-spacing="4">AUTARKEIA OS</text>
    <text x="256" y="482" font-size="15" letter-spacing="6" fill-opacity="0.75">1.1 TELEIOS</text>
  </g>
</svg>''')
