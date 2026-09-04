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
generate_pdf_map.py — Render a DragonGL floor dump as a faithful color PDF.

Used by the DM commands `dm_pdf <floor>` and `dm_mapfloor`: the server dumps
the floor as a text grid (see server_commands.c), then invokes this script to
turn it into a readable, color-coded PDF with title, coordinates and legend.

Dump formats (auto-detected):

  v2 (current, written by the server):
      # DragonGL map dump v2
      floor 3
      width 300
      height 300
      player 142 97          <- optional, only if the DM is on that floor
      <300 lines, each line = width * 2 hex chars>
  Each tile is encoded as a 2-digit hex VoxelType (00 = VOXEL_ROCK ...
  1B = VOXEL_CRYSTAL_WHITE, see include/map.h), so every tile type is
  rendered with its exact in-game color.

  v1 (legacy, 1 ASCII char per tile, no header):
      ' ' rock   '#' wall/obsidian   '+' door   '~' water (or lava,
      'L' lava   '>' stairs down     '<' stairs up   '$' gold vein
      'B' blue crystal  'P' purple crystal  'M' glow mushroom
      ',' grass   '.' floor/other
  v1 dumps lose information ('.' lumps ~12 tile types together); the PDF is
  still generated, with a footnote explaining the lumps.

Tile colors are kept in sync with the in-game radar minimap
(src/client/client_minimap.c, tile_to_color()), so the PDF matches what the
player sees on the 2D top-down view.

Usage:
    generate_pdf_map.py <dump> <output_pdf> [floor_number] [options]

Options:
    --cell N    points per tile (default 4.0)
    --px N      internal PNG pixels per tile (default 3)
    --cols N    legend columns (default 4)
    --no-coords omit the coordinate ticks around the map

If [floor_number] is omitted it is taken from the dump header (v2) or
inferred from the dump filename (e.g. "map_dump_floor_7.txt" -> floor 7).

Exit codes:
    0  success
    1  error (details on stderr, visible on the server console)
    2  usage error
"""

import argparse
import io
import re
import sys
from collections import Counter
from datetime import datetime
from pathlib import Path

# ---------------------------------------------------------------------------
# In-game tile palette — MUST stay in sync with
# src/client/client_minimap.c :: tile_to_color() (the 2D top-down reference).
# Order = VoxelType enum values from include/map.h.
# ---------------------------------------------------------------------------
VOXEL_NAMES = [
    "Rock (unexcavated)",     # 00 VOXEL_ROCK
    "Floor (stone)",          # 01 VOXEL_FLOOR
    "Wall",                   # 02 VOXEL_WALL
    "Door",                   # 03 VOXEL_DOOR
    "Stairs Up",              # 04 VOXEL_STAIRS_UP
    "Stairs Down",            # 05 VOXEL_STAIRS_DOWN
    "Grass",                  # 06 VOXEL_GRASS
    "Wood",                   # 07 VOXEL_WOOD
    "Water",                  # 08 VOXEL_WATER
    "Cobble",                 # 09 VOXEL_COBBLE
    "Trap",                   # 0A VOXEL_TRAP
    "Lava",                   # 0B VOXEL_LAVA
    "Ice",                    # 0C VOXEL_ICE
    "Sand",                   # 0D VOXEL_SAND
    "Ash",                    # 0E VOXEL_ASH
    "Mud",                    # 0F VOXEL_MUD
    "Marble",                 # 10 VOXEL_MARBLE
    "Glowing Mushroom",       # 11 VOXEL_MUSHROOM_GLOW
    "Crystal (Blue)",         # 12 VOXEL_CRYSTAL_BLUE
    "Crystal (Purple)",       # 13 VOXEL_CRYSTAL_PURPLE
    "Gold Vein",              # 14 VOXEL_GOLD_VEIN
    "Obsidian",               # 15 VOXEL_OBSIDIAN
    "Crystal (Red)",          # 16 VOXEL_CRYSTAL_RED
    "Crystal (Green)",        # 17 VOXEL_CRYSTAL_GREEN
    "Crystal (Yellow)",       # 18 VOXEL_CRYSTAL_YELLOW
    "Crystal (Orange)",       # 19 VOXEL_CRYSTAL_ORANGE
    "Crystal (Cyan)",         # 1A VOXEL_CRYSTAL_CYAN
    "Crystal (White)",        # 1B VOXEL_CRYSTAL_WHITE
]
VOXEL_COLORS = [
    (0, 0, 0),          # ROCK
    (40, 40, 50),       # FLOOR
    (120, 120, 130),    # WALL
    (130, 80, 30),      # DOOR
    (0, 230, 230),      # STAIRS_UP
    (230, 230, 0),      # STAIRS_DOWN
    (20, 100, 20),      # GRASS
    (100, 75, 50),      # WOOD
    (20, 80, 200),      # WATER
    (75, 75, 75),       # COBBLE
    (200, 50, 30),      # TRAP
    (255, 60, 0),       # LAVA
    (150, 200, 255),    # ICE
    (200, 180, 100),    # SAND
    (65, 65, 65),       # ASH
    (75, 50, 25),       # MUD
    (230, 230, 230),    # MARBLE
    (50, 255, 130),     # MUSHROOM_GLOW
    (80, 180, 255),     # CRYSTAL_BLUE
    (200, 50, 255),     # CRYSTAL_PURPLE
    (200, 180, 20),     # GOLD_VEIN
    (30, 15, 60),       # OBSIDIAN
    (255, 26, 26),      # CRYSTAL_RED
    (26, 255, 51),      # CRYSTAL_GREEN
    (255, 230, 26),     # CRYSTAL_YELLOW
    (255, 128, 0),      # CRYSTAL_ORANGE
    (0, 230, 255),      # CRYSTAL_CYAN
    (230, 242, 255),    # CRYSTAL_WHITE
]

# Legacy (v1) char -> (voxel index, label). In v1 several VoxelTypes collapse
# into one char; the color used is the one of the most common member.
V1_MAP = {
    " ": (0, "Rock (unexcavated)"),
    ".": (1, "Floor (stone / other)"),
    "#": (2, "Wall"),
    "+": (3, "Door"),
    "<": (4, "Stairs Up"),
    ">": (5, "Stairs Down"),
    ",": (6, "Grass"),
    "~": (8, "Water"),
    "L": (11, "Lava"),
    "$": (20, "Gold Vein"),
    "B": (18, "Crystal (Blue)"),
    "P": (19, "Crystal (Purple)"),
    "M": (17, "Glowing Mushroom"),
}
# v1 legend order (fixed, filtered to entries actually present)
V1_LEGEND_ORDER = [" ", ".", "#", "+", "<", ">", ",", "~", "L", "$", "B", "P", "M"]

PLAYER_COLOR = (51, 204, 51)   # in-game player entity color (render_vk.c)

# Staircase VoxelTypes (include/map.h) and the legend keys that carry their
# tile coordinates (v2 decimal hex keys + v1 char keys).
VOXEL_STAIRS_UP = 4
VOXEL_STAIRS_DOWN = 5
STAIRS_LEGEND_KEYS = {
    str(VOXEL_STAIRS_UP): "stairs_up",
    str(VOXEL_STAIRS_DOWN): "stairs_down",
    "v1:<": "stairs_up",
    "v1:>": "stairs_down",
}

# ---------------------------------------------------------------------------
# Layout (PDF points)
# ---------------------------------------------------------------------------
MARGIN = 48.0
CELL = 4.0           # points per tile (override with --cell)
TITLE_H = 52.0
LEGEND_ROW_H = 16.0
SW = 10.0            # legend swatch size
GRID_GAP = 14.0      # space between map and legend
TICK_STEP = 25       # coordinate tick every N tiles


def die(msg):
    sys.stderr.write("generate_pdf_map: %s\n" % msg)
    sys.exit(1)


def c255(rgb):
    return tuple(min(255, max(0, v)) for v in rgb)


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------
class FloorData:
    """Parsed dump: rows of tile keys + metadata for rendering/legend."""
    def __init__(self):
        self.version = 1
        self.width = 0
        self.height = 0
        self.rows = []          # list[str], each of length width
        self.meta = {}          # key -> (label, rgb)
        self.order = []         # legend order (only keys present, in order)
        self.counts = Counter()
        self.floor_label = None
        self.player = None      # (x, y) or None
        self.stairs_up = []     # list of (x, y) staircase tiles
        self.stairs_down = []   # list of (x, y) staircase tiles
        self.footnote = None    # optional legend footnote


def parse_v1(lines, path):
    f = FloorData()
    f.version = 1
    # Note: never strip spaces — in v1 a space IS a tile (rock).
    rows = [line.rstrip("\r") for line in lines]
    if not rows or all(not row for row in rows):
        die("dump file '%s' is empty" % path)
    width = max(len(line) for line in rows)
    rows = [line.ljust(width, " ") for line in rows]
    f.width, f.height = width, len(rows)

    counts = Counter(ch for row in rows for ch in row)

    for ch in counts:
        if ch in V1_MAP:
            vix, label = V1_MAP[ch]
            f.meta["v1:%s" % ch] = (label, VOXEL_COLORS[vix])
        else:
            f.meta["v1:%s" % ch] = ("Unknown '%s'" % ch, VOXEL_COLORS[1])
    # Legacy dm_pdf dumps use '~' for both water and lava; if the dump has
    # an explicit 'L', '~' unambiguously means water.
    if counts.get("~") and not counts.get("L"):
        f.meta["v1:~"] = ("Water / Lava (legacy)", VOXEL_COLORS[8])

    for ch in V1_LEGEND_ORDER:
        if counts.get(ch):
            f.order.append("v1:%s" % ch)
    for ch in sorted(counts):
        if ch not in V1_LEGEND_ORDER:
            f.order.append("v1:%s" % ch)

    # rows as per-tile key strings ("v1:<char>") so color_of() can be shared;
    # staircase positions are collected for the legend
    f.rows = []
    for y, row in enumerate(rows):
        keys = []
        for x, ch in enumerate(row):
            if ch == "<":
                f.stairs_up.append((x, y))
            elif ch == ">":
                f.stairs_down.append((x, y))
            keys.append("v1:%s" % ch)
        f.rows.append(keys)
    f.counts = {("v1:%s" % ch): n for ch, n in counts.items()}
    f.footnote = (
        "Legacy v1 dump: '.' lumps floor, wood, cobble, ice, sand, ash, mud, "
        "marble, traps and the red/green/yellow/orange/cyan/white crystals "
        "(shown with the floor color); '#' lumps wall and obsidian. "
        "Update the server for the full-color v2 dump.")
    return f


def parse_v2(lines, path):
    f = FloorData()
    f.version = 2
    hdr = {}
    idx = 0
    for idx, line in enumerate(lines):
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split()
        if parts[0] in ("floor", "width", "height", "player") and len(parts) in (2, 3):
            hdr[parts[0]] = parts[1:]
        else:
            break
    grid_lines = [ln.strip() for ln in lines[idx:] if ln.strip()]

    def need_int(key, what):
        if key not in hdr:
            die("v2 dump '%s' is missing the '%s' header line" % (path, key))
        try:
            v = int(hdr[key][0])
        except ValueError:
            die("v2 dump '%s': bad value '%s' for '%s'" % (path, hdr[key][0], key))
        if v <= 0:
            die("v2 dump '%s': '%s' must be positive" % (path, key))
        return v

    width, height = need_int("width", "width"), need_int("height", "height")
    if len(grid_lines) < height:
        die("v2 dump '%s': expected %d grid lines, found %d"
            % (path, height, len(grid_lines)))

    hex_re = re.compile(r"^[0-9a-fA-F]+$")
    rows, counts = [], Counter()
    for i, line in enumerate(grid_lines[:height]):
        if len(line) != width * 2 or not hex_re.match(line):
            die("v2 dump '%s': grid line %d must be %d hex chars (found %d)"
                % (path, i + 1, width * 2, len(line)))
        keys = []
        for j in range(0, len(line), 2):
            code = int(line[j:j + 2], 16)
            if code == VOXEL_STAIRS_UP:
                f.stairs_up.append((j // 2, i))
            elif code == VOXEL_STAIRS_DOWN:
                f.stairs_down.append((j // 2, i))
            if code < len(VOXEL_NAMES):
                key = str(code)
            else:
                key = "u:%d" % code
            keys.append(key)
            counts[key] += 1
        rows.append(keys)

    for key in counts:
        if key.isdigit():
            code = int(key)
            f.meta[key] = (VOXEL_NAMES[code], VOXEL_COLORS[code])
        else:
            code = int(key.split(":")[1])
            f.meta[key] = ("Unknown (0x%02X)" % code, VOXEL_COLORS[1])

    f.order = [k for k in sorted(counts, key=lambda k: int(k) if k.isdigit() else 999)
               if k in counts]
    f.rows = rows
    f.width, f.height = width, height
    f.counts = counts

    if "floor" in hdr:
        f.floor_label = hdr["floor"][0]
    if "player" in hdr and len(hdr["player"]) == 2:
        try:
            px, py = int(hdr["player"][0]), int(hdr["player"][1])
            if 0 <= px < width and 0 <= py < height:
                f.player = (px, py)
        except ValueError:
            pass
    return f


def parse_dump(path):
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        die("cannot read dump file '%s': %s" % (path, e))
    lines = text.splitlines()
    if not lines:
        die("dump file '%s' is empty" % path)
    first = next((ln for ln in lines if ln.strip()), None)
    if first and first.startswith("#") and re.search(r"\bv2\b", first):
        return parse_v2(lines, path)
    return parse_v1(lines, path)


def derive_floor(dump_path, floor_data, explicit):
    if explicit not in (None, ""):
        return explicit
    if floor_data.floor_label is not None:
        return floor_data.floor_label
    m = re.search(r"floor[_-]?(\d+)", str(dump_path), re.IGNORECASE)
    return m.group(1) if m else "Unknown"


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------
def color_of(floor, key):
    return floor.meta[key][1]


def render_png_bytes(floor, px_per_tile=3):
    """Render the grid as a PNG (px_per_tile px per tile, NEAREST upscale)."""
    from PIL import Image

    width, height = floor.width, floor.height
    pixels = [color_of(floor, k) for row in floor.rows for k in row]
    img = Image.new("RGB", (width, height))
    img.putdata(pixels)
    if px_per_tile != 1:
        img = img.resize((width * px_per_tile, height * px_per_tile),
                         Image.NEAREST)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    buf.seek(0)
    return buf


def draw_grid_vector(c, floor, grid_x, grid_y, cell):
    """PIL-free fallback: draw tiles as vector rects, merging same-color
    horizontal runs (far fewer objects than one rect per tile)."""
    from reportlab.lib.colors import Color

    height = floor.height
    for y, row in enumerate(floor.rows):
        y0 = grid_y + (height - 1 - y) * cell
        x = 0
        n = floor.width
        while x < n:
            k = row[x]
            x2 = x + 1
            while x2 < n and row[x2] == k:
                x2 += 1
            r, g, b = color_of(floor, k)
            c.setFillColor(Color(r / 255.0, g / 255.0, b / 255.0))
            c.rect(grid_x + x * cell, y0, (x2 - x) * cell, cell,
                   stroke=0, fill=1)
            x = x2


def draw_player_marker(c, cx, cy, r):
    from reportlab.lib.colors import Color

    # White outer ring + green ring + white cross. The radius is chosen by
    # the caller (about half a tile) so the symbol stays inside the player
    # tile and the tile color underneath remains readable.
    c.setStrokeColor(Color(1, 1, 1))
    c.setLineWidth(1.2)
    c.circle(cx, cy, r, stroke=1, fill=0)
    c.setStrokeColor(Color(PLAYER_COLOR[0] / 255.0,
                           PLAYER_COLOR[1] / 255.0,
                           PLAYER_COLOR[2] / 255.0))
    c.setLineWidth(1.0)
    c.circle(cx, cy, r * 0.62, stroke=1, fill=0)
    c.setStrokeColor(Color(1, 1, 1))
    c.setLineWidth(0.8)
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        c.line(cx + dx * r, cy + dy * r, cx + dx * r * 1.2, cy + dy * r * 1.2)
    c.setFillColor(Color(1, 1, 1))
    c.circle(cx, cy, r * 0.22, stroke=0, fill=1)


def draw_coords(c, floor, grid_x, grid_y, grid_w, grid_h, cell):
    from reportlab.lib.colors import Color

    c.setStrokeColor(Color(0.4, 0.4, 0.4))
    c.setLineWidth(0.75)
    c.setFont("Helvetica", 6)
    c.setFillColor(Color(0.35, 0.35, 0.35))
    top = grid_y + grid_h
    # top ticks
    for x in range(0, floor.width, TICK_STEP):
        tx = grid_x + x * cell
        c.line(tx, top, tx, top + 4)
        c.drawCentredString(tx, top + 6, str(x))
    # left ticks
    for y in range(0, floor.height, TICK_STEP):
        ty = grid_y + (floor.height - 1 - y) * cell + cell / 2
        c.line(grid_x, ty, grid_x - 4, ty)
        c.drawRightString(grid_x - 6, ty - 2, str(y))


def wrap_text(text, font, size, max_w):
    from reportlab.pdfbase.pdfmetrics import stringWidth

    lines, cur = [], ""
    for word in text.split():
        trial = (cur + " " + word).strip()
        if stringWidth(trial, font, size) <= max_w:
            cur = trial
        else:
            if cur:
                lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    return lines


def build_pdf(floor, out_path, floor_label, cell=CELL, px=3, cols=4,
              coords=True):
    from reportlab.pdfgen import canvas as pdfcanvas
    from reportlab.lib.colors import Color
    from reportlab.pdfbase.pdfmetrics import stringWidth

    # reportlab.lib.utils (ImageReader) imports PIL at module level, so it
    # must only be imported when PIL is actually available.
    try:
        import PIL  # noqa: F401
        pil_ok = True
    except ImportError:
        pil_ok = False

    width, height = floor.width, floor.height
    grid_w, grid_h = width * cell, height * cell

    legend_keys = [k for k in floor.order if floor.counts.get(k)]
    if floor.player is not None:
        legend_keys = legend_keys + ["__player__"]

    def legend_text(key):
        if key == "__player__":
            return "Player position %s" % (floor.player,)
        text = "%s  (%s)" % (floor.meta[key][0],
                             format(floor.counts.get(key, 0), ","))
        # Stairs Up/Down entries list the coordinates of every staircase
        attr = STAIRS_LEGEND_KEYS.get(key)
        if attr:
            text += "  @ " + "  ".join("%d,%d" % p
                                       for p in getattr(floor, attr))
        return text

    # Choose as many columns as fit without text overflow, so labels never
    # collide on small maps; if one column is still wider than the grid the
    # page widens instead.
    max_text_w = max([stringWidth(legend_text(k), "Helvetica", 8)
                      for k in legend_keys] or [0.0])
    col_w_needed = SW + 5.0 + max_text_w
    n_cols = max(1, min(cols, int(grid_w // col_w_needed),
                        len(legend_keys) or 1))
    legend_w = n_cols * col_w_needed
    legend_rows = (len(legend_keys) + n_cols - 1) // n_cols
    legend_h = legend_rows * LEGEND_ROW_H

    has_footnote = floor.footnote is not None
    footnote_h = 26.0 if has_footnote else 0.0
    page_w = MARGIN * 2 + max(grid_w, legend_w)
    page_h = (MARGIN + TITLE_H + grid_h + GRID_GAP + legend_h +
              footnote_h + MARGIN)

    c = pdfcanvas.Canvas(str(out_path), pagesize=(page_w, page_h))
    c.setTitle("DragonGL Floor %s Map" % floor_label)
    c.setAuthor("DragonGL DM Tools")
    c.setCreator("tools/generate_pdf_map.py (v%d dump)" % floor.version)

    # Title + subtitle
    total = width * height
    rock = floor.counts.get("0", 0) + floor.counts.get("v1: ", 0)
    carved = 100.0 * (total - rock) / total if total else 0.0
    now = datetime.now().strftime("%Y-%m-%d %H:%M")
    c.setFont("Helvetica-Bold", 18)
    c.setFillColor(Color(0.12, 0.12, 0.12))
    c.drawCentredString(page_w / 2, page_h - MARGIN - 14,
                        "DragonGL - Floor %s Map (%d x %d tiles)"
                        % (floor_label, width, height))
    fmt = "v2 full-color" if floor.version == 2 else "v1 legacy"
    n_types = len(legend_keys) - (1 if floor.player is not None else 0)
    c.setFont("Helvetica", 9)
    c.setFillColor(Color(0.4, 0.4, 0.4))
    c.drawCentredString(page_w / 2, page_h - MARGIN - 30,
                        "%d%% carved - %d tile types - dump %s - %s"
                        % (carved, n_types, fmt, now))

    # Grid
    grid_x = (page_w - grid_w) / 2
    grid_y = MARGIN + GRID_GAP + legend_h + footnote_h
    try:
        if pil_ok:
            from reportlab.lib.utils import ImageReader
            c.drawImage(ImageReader(render_png_bytes(floor, px)),
                        grid_x, grid_y, width=grid_w, height=grid_h)
        else:
            draw_grid_vector(c, floor, grid_x, grid_y, cell)
    except ImportError:
        draw_grid_vector(c, floor, grid_x, grid_y, cell)

    # Frame + coordinates
    c.setStrokeColor(Color(0.2, 0.2, 0.2))
    c.setLineWidth(1.0)
    c.rect(grid_x, grid_y, grid_w, grid_h, stroke=1, fill=0)
    if coords:
        draw_coords(c, floor, grid_x, grid_y, grid_w, grid_h, cell)

    # Player marker
    if floor.player is not None:
        px_, py_ = floor.player
        cx = grid_x + (px_ + 0.5) * cell
        cy = grid_y + (height - 1 - py_ + 0.5) * cell
        draw_player_marker(c, cx, cy, max(cell * 0.42, 2.0))

    # Legend
    c.setFont("Helvetica", 8)
    legend_x0 = (page_w - legend_w) / 2
    for i, key in enumerate(legend_keys):
        r, col = divmod(i, n_cols)
        lx = legend_x0 + col * col_w_needed
        ly = grid_y - GRID_GAP - (r + 1) * LEGEND_ROW_H + (LEGEND_ROW_H - SW) / 2
        if key == "__player__":
            rgb = PLAYER_COLOR
            text = legend_text(key)
        else:
            rgb = floor.meta[key][1]
            text = legend_text(key)
        c.setFillColor(Color(rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0))
        c.setStrokeColor(Color(0.55, 0.55, 0.55))
        c.setLineWidth(0.5)
        c.rect(lx, ly, SW, SW, stroke=1, fill=1)
        c.setFillColor(Color(0.2, 0.2, 0.2))
        c.drawString(lx + SW + 5, ly + 2.5, text)

    # Footnote (legacy format)
    if has_footnote:
        c.setFont("Helvetica", 7.5)
        c.setFillColor(Color(0.5, 0.5, 0.5))
        wrapped = wrap_text(floor.footnote, "Helvetica", 7.5,
                            page_w - 2 * MARGIN)
        fy = grid_y - GRID_GAP - legend_h - 12
        for line in wrapped[:2]:
            c.drawString(MARGIN, fy, line)
            fy -= 10

    c.showPage()
    c.save()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main(argv):
    ap = argparse.ArgumentParser(
        description="Render a DragonGL floor dump as a color PDF map.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="examples:\n"
               "  generate_pdf_map.py map_dump_floor_0.txt map_floor_0.pdf\n"
               "  generate_pdf_map.py map_dump.txt map_floor_3.pdf 3\n"
               "  generate_pdf_map.py map_dump.txt out.pdf --cell 3 --px 2\n")
    ap.add_argument("dump", help="floor dump file (v1 ASCII grid or v2 hex grid)")
    ap.add_argument("output", help="output PDF path")
    ap.add_argument("floor", nargs="?", default=None,
                    help="floor number (default: dump header / filename)")
    ap.add_argument("--cell", type=float, default=CELL,
                    help="points per tile (default %(default)s)")
    ap.add_argument("--px", type=int, default=3,
                    help="internal PNG pixels per tile (default %(default)s)")
    ap.add_argument("--cols", type=int, default=4,
                    help="legend columns (default %(default)s)")
    ap.add_argument("--no-coords", action="store_true",
                    help="omit coordinate ticks")
    args = ap.parse_args(argv[1:])

    if args.cell <= 0 or args.px < 1 or args.cols < 1:
        die("invalid --cell/--px/--cols values")

    floor = parse_dump(args.dump)
    floor_label = derive_floor(args.dump, floor, args.floor)
    try:
        build_pdf(floor, args.output, floor_label, cell=args.cell,
                  px=args.px, cols=args.cols, coords=not args.no_coords)
    except ImportError as e:
        die("missing Python dependency '%s' (pip install reportlab Pillow)"
            % (e.name or "reportlab/Pillow"))
    except Exception as e:
        die("failed to write PDF '%s': %s" % (args.output, e))
    print("PDF map written to %s" % args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
