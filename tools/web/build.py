#!/usr/bin/env python3
# ============================================================
# DRAGONGL · DATA STUDIO — assembler
#
# Builds the four self-contained HTML editors from the shared
# fragments in this directory:
#
#   _design.css        shared design system
#   _core.js           shared CRUD engine
#   _editor-shell.html common markup for the three editors
#   _mod-*.js          per-module configuration
#   _menu.html         menu template
#
# The _-prefixed files are source fragments; the final HTML files are
# fully self-contained and can be moved or copied independently.
#
# Output (committed, no build step needed to use them):
#   menu.html  bestiary.html  items.html  spells.html
#
# Usage:  python3 build.py
# ============================================================
from pathlib import Path

ROOT = Path(__file__).resolve().parent

CSS = (ROOT / "_design.css").read_text()
CORE = (ROOT / "_core.js").read_text()
SHELL = (ROOT / "_editor-shell.html").read_text()

ICONS = {
    "folder": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m6 14 1.5-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.54 6a2 2 0 0 1-1.95 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2"/></svg>',
    "refresh": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/></svg>',
    "save": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>',
    "plus": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14"/><path d="M12 5v14"/></svg>',
    "search": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/></svg>',
    "file": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z"/><path d="M14 2v4a2 2 0 0 0 2 2h4"/></svg>',
    "sword": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M14.5 17.5 3 6V3h3l11.5 11.5"/><path d="m13 19 6-6"/><path d="m16 16 4 4"/><path d="m19 21 2-2"/></svg>',
    "gem": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M6 3h12l4 6-10 13L2 9Z"/><path d="M11 3 8 9l4 13 4-13-3-6"/><path d="M2 9h20"/></svg>',
    "spark": '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3l1.9 5.8a2 2 0 0 0 1.3 1.3L21 12l-5.8 1.9a2 2 0 0 0-1.3 1.3L12 21l-1.9-5.8a2 2 0 0 0-1.3-1.3L3 12l5.8-1.9a2 2 0 0 0 1.3-1.3Z"/></svg>',
}


def build_editor(out, accent, title, icon, placeholder, footnote, filecode, mod):
    html = SHELL
    html = html.replace("@@PAGE_NAME@@", out.split(".")[0].title())
    html = html.replace("@@ACCENT@@", accent)
    html = html.replace("@@TITLE@@", f'{ICONS[icon]} {title}')
    html = html.replace("@@PLACEHOLDER@@", placeholder)
    html = html.replace("@@FOOTNOTE@@", footnote)
    html = html.replace("@@FILECODE@@", filecode)
    html = html.replace("@@ICON_FOLDER@@", ICONS["folder"])
    html = html.replace("@@ICON_REFRESH@@", ICONS["refresh"])
    html = html.replace("@@ICON_SAVE@@", ICONS["save"])
    html = html.replace("@@ICON_PLUS@@", ICONS["plus"])
    html = html.replace("@@ICON_SEARCH@@", ICONS["search"])
    html = html.replace("@@ICON_FILE@@", ICONS["file"])
    html = html.replace("@@CSS@@", CSS)
    html = html.replace("@@CFG@@", (ROOT / mod).read_text())
    html = html.replace("@@CORE@@", CORE)
    assert "@@" not in html, f"unresolved placeholder in {out}"
    (ROOT / out).write_text(html)
    print(f"  built {out:<14} {len(html):>7,} bytes")


def build_menu():
    src = (ROOT / "_menu.html").read_text()
    assert "/* @CSS@ */" in src, "menu template missing CSS placeholder"
    out = src.replace("/* @CSS@ */", CSS)
    (ROOT / "menu.html").write_text(out)
    print(f"  built menu.html      {len(out):>7,} bytes")


# --- editors -----------------------------------------------------------
build_editor(
    out="bestiary.html",
    accent=(
        ":root{"
        "--accent:#f43f5e;--accent2:#be123c;"
        "--accent-soft:rgba(244,63,94,.13);--accent-glow:rgba(244,63,94,.35);"
        "--accent-ink:#ffffff;}"
    ),
    title="Bestiary <small>monsters &amp; creatures database</small>",
    icon="sword",
    placeholder="Search monsters — name, type, description…",
    footnote="Module 01 · 6 archetypes · damage profiles",
    filecode="data/bestiary.json",
    mod="_mod-bestiary.js",
)
build_editor(
    out="items.html",
    accent=(
        ":root{"
        "--accent:#f5b82e;--accent2:#d97706;"
        "--accent-soft:rgba(245,184,46,.13);--accent-glow:rgba(245,184,46,.32);"
        "--accent-ink:#1a1205;}"
    ),
    title="Items <small>loot, gear &amp; wondrous artifacts</small>",
    icon="gem",
    placeholder="Search items — name, category, description…",
    footnote="Module 02 · 11 categories · 7 rarity tiers",
    filecode="data/items.json",
    mod="_mod-items.js",
)
build_editor(
    out="spells.html",
    accent=(
        ":root{"
        "--accent:#9d7bff;--accent2:#7c3aed;"
        "--accent-soft:rgba(157,123,255,.14);--accent-glow:rgba(157,123,255,.35);"
        "--accent-ink:#ffffff;}"
    ),
    title="Spells <small>arcane compendium &amp; VFX profiles</small>",
    icon="spark",
    placeholder="Search spells — name, school, effect…",
    footnote="Module 03 · 9 schools · 12 caster classes",
    filecode="data/spells.json",
    mod="_mod-spells.js",
)
build_menu()
print("done.")
