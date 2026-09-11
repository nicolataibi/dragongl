# Dragongl · Data Studio

Professional, **self-contained** CRUD workbenches for the three JSON
databases that power the world:

| Page            | Database               | Scope                                                        |
|-----------------|------------------------|--------------------------------------------------------------|
| `menu.html`     | —                      | Main menu, live record counts, workflow guide                |
| `bestiary.html` | `../../data/bestiary.json` | Monsters: archetypes, stat blocks, damage profiles, gold  |
| `items.html`    | `../../data/items.json`    | Items: categories, rarity tiers, properties, cost           |
| `spells.html`   | `../../data/spells.json`   | Spells: schools, components, rituals, VFX color profiles    |

Every HTML file is standalone (inline CSS + JS, zero dependencies) and can be
copied anywhere.

## Usage

**Option A — recommended (automatic loading):**

```sh
cd dragongl-dev
python3 -m http.server 8080
# open http://localhost:8080/tools/web/menu.html
```

**Option B — open the files directly (`file://`):**
the browser blocks local reads, so each editor shows a load panel: drag &
drop the JSON file or use *Browse file…*. In Chrome/Edge, *Open with write
permission…* enables **direct write-back** to the original file via the File
System Access API. Otherwise **Export JSON** downloads a ready-to-drop-in
copy (same name, 2-space pretty format, trailing newline).

## Features

- Full CRUD: create, edit, duplicate, delete (with confirmation), detail
  inspector, reset-to-last-saved.
- Full-text search (press `/`), sortable columns, pagination (25–200 rows),
  per-module filters and clickable colored distribution charts.
- Live stat cards, colored badges for every domain value (rarity tiers use
  the standard D&D 5e palette; schools, archetypes and creature types have
  their own color language), VFX orb preview for spells.
- Unsaved-changes tracking with status chip and unload guard.
- **Schema-safe by construction:** editing an entry starts from a deep copy
  of the original object, so engine-only extra fields (`book_seq`,
  `cast_spell`, `max_charges`, `material`, `vfx`, …) are always preserved on
  export — the C++ loader (`src/shared/data_loader.c`) keeps working without
  any change.

## Building (only needed if you edit the source fragments)

The four HTML files are generated from shared fragments so the design system
and CRUD engine stay in one place:

```
_design.css         shared design system (colors, components)
_core.js            shared CRUD engine (load/save/search/table/modal/toast)
_editor-shell.html  common editor markup
_mod-bestiary.js    bestiary configuration (columns, filters, form, palettes)
_mod-items.js       items configuration
_mod-spells.js      spells configuration
_menu.html          menu template
build.py            assembler
```

```sh
python3 tools/web/build.py
```

Re-run this after touching any `_`-prefixed fragment; the generated
`menu.html`, `bestiary.html`, `items.html` and `spells.html` are committed
and work without the build step.
