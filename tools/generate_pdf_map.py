#!/usr/bin/env python3
#
# DRAGON GL - 3D ARCANE ENGINE
# Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#

"""
generate_pdf_map.py — Render an ASCII floor dump as a color PDF map.

Used by the DM commands `dm_pdf <floor>` and `dm_mapfloor`: the server dumps
the whole floor as an ASCII grid (see server_commands.c), then invokes this
script to turn it into a readable, color-coded PDF with title and legend.

Usage:
    generate_pdf_map.py <ascii_dump> <output_pdf> [floor_number]

If [floor_number] is omitted it is inferred from the dump filename
(e.g. "map_dump_floor_7.txt" -> floor 7).

Exit codes:
    0  success
    1  error (details on stderr, visible on the server console)
"""

import re
import sys
from pathlib import Path

# Character -> (RGB, label). Keep in sync with the char mapping used by
# dm_pdf / dm_mapfloor in src/server/server_commands.c.
LEGEND = [
    ("#", (108, 117, 125), "Wall / Obsidian"),
    ("+", (140, 86, 39), "Door"),
    ("~", (52, 152, 219), "Water"),
    ("L", (214, 69, 29), "Lava"),
    (">", (39, 174, 96), "Stairs Down"),
    ("<", (0, 196, 200), "Stairs Up"),
    ("$", (241, 196, 15), "Gold Vein"),
    ("B", (133, 193, 233), "Crystal (Blue)"),
    ("P", (155, 89, 182), "Crystal (Purple)"),
    ("M", (164, 222, 126), "Glowing Mushroom"),
    (",", (88, 166, 74), "Grass"),
    (".", (70, 70, 78), "Other / carved"),
]
COLOR_BY_CHAR = {ch: rgb for ch, rgb, _ in LEGEND}
ROCK_COLOR = (16, 16, 20)  # ' ' -> unexcavated rock (background)

MARGIN = 48.0
CELL = 4.0          # points per tile
TITLE_H = 46.0
LEGEND_ROW_H = 20.0
LEGEND_COLS = 7


def die(msg):
    sys.stderr.write("generate_pdf_map: %s\n" % msg)
    sys.exit(1)


def load_grid(path):
    """Read the ASCII dump and return a list of equal-length rows."""
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        die("cannot read dump file '%s': %s" % (path, e))
    lines = text.splitlines()
    if not lines:
        die("dump file '%s' is empty" % path)
    width = max(len(line) for line in lines)
    return [line.ljust(width) for line in lines]


def color_for(ch):
    if ch == " ":
        return ROCK_COLOR
    return COLOR_BY_CHAR.get(ch, COLOR_BY_CHAR["."])


def derive_floor(dump_path, explicit):
    if explicit not in (None, ""):
        return explicit
    m = re.search(r"floor[_-]?(\d+)", str(dump_path), re.IGNORECASE)
    return m.group(1) if m else "Unknown"


def render_png_bytes(grid, scale=4):
    """Render the grid as a PNG (1 px per tile, scaled up with NEAREST)."""
    from PIL import Image
    import io

    height, width = len(grid), len(grid[0])
    img = Image.new("RGB", (width, height))
    img.putdata([color_for(ch) for row in grid for ch in row])
    if scale != 1:
        img = img.resize((width * scale, height * scale), Image.NEAREST)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf


def build_pdf(grid, out_path, floor_label):
    from reportlab.pdfgen import canvas as pdfcanvas
    from reportlab.lib.colors import Color
    from reportlab.lib.utils import ImageReader

    height, width = len(grid), len(grid[0])
    grid_w, grid_h = width * CELL, height * CELL

    legend_rows = (len(LEGEND) + LEGEND_COLS - 1) // LEGEND_COLS
    legend_h = legend_rows * LEGEND_ROW_H
    page_w = MARGIN * 2 + grid_w
    page_h = MARGIN + TITLE_H + grid_h + 12 + legend_h + MARGIN

    c = pdfcanvas.Canvas(str(out_path), pagesize=(page_w, page_h))
    c.setTitle("DragonGL Floor %s Map" % floor_label)
    c.setAuthor("DragonGL DM Tools")

    # Title
    c.setFont("Helvetica-Bold", 18)
    c.setFillColor(Color(1, 1, 1))
    c.drawCentredString(
        page_w / 2, page_h - MARGIN - 14,
        "DragonGL - Floor %s Map (%dx%d)" % (floor_label, width, height))

    # Grid (bottom-left origin in PDF coordinates)
    grid_x = (page_w - grid_w) / 2
    grid_y = MARGIN + legend_h + 12
    try:
        c.drawImage(ImageReader(render_png_bytes(grid)),
                    grid_x, grid_y, width=grid_w, height=grid_h)
    except ImportError:
        # No PIL available: draw the tiles directly (slower, bigger file).
        for y, row in enumerate(grid):
            for x, ch in enumerate(row):
                r, g, b = color_for(ch)
                c.setFillColor(Color(r / 255.0, g / 255.0, b / 255.0))
                c.rect(grid_x + x * CELL, grid_y + (height - 1 - y) * CELL,
                       CELL, CELL, stroke=0, fill=1)

    # Legend
    c.setFont("Helvetica", 9)
    for i, (ch, rgb, label) in enumerate(LEGEND):
        row, col = divmod(i, LEGEND_COLS)
        lx = MARGIN + col * (grid_w / LEGEND_COLS)
        ly = grid_y - 14 - row * LEGEND_ROW_H
        c.setFillColor(Color(rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0))
        c.rect(lx, ly, 12, 12, stroke=0, fill=1)
        c.setFillColor(Color(0.85, 0.85, 0.85))
        c.drawString(lx + 18, ly + 3, "%s  %s" % (ch, label))

    c.showPage()
    c.save()


def main(argv):
    if len(argv) < 3 or len(argv) > 4:
        die("usage: generate_pdf_map.py <ascii_dump> <output_pdf> [floor_number]")
    dump_path, out_path = argv[1], argv[2]
    floor_label = derive_floor(dump_path, argv[3] if len(argv) == 4 else None)
    grid = load_grid(dump_path)
    try:
        build_pdf(grid, out_path, floor_label)
    except ImportError as e:
        die("missing Python dependency '%s' (pip install reportlab)" % e.name)
    except Exception as e:
        die("failed to write PDF '%s': %s" % (out_path, e))
    print("PDF map written to %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
