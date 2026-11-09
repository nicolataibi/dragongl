#!/usr/bin/env python3
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Check the game data in data/: schema, cross references, canonical form.

    tools/check_data.py            check (exit 1 on any error)
    tools/check_data.py --fix      rewrite the files in canonical form

Checks, in order:
  1. every file parses and validates against data/schema/<name>.schema.json
     (JSON Schema 2020-12, python3-jsonschema);
  2. references the engine resolves by name: the spells of every book and
     the cast_spell of every item exist in spells.json (case-insensitive,
     as data_loader.c and cmd_magic.c compare them);
  3. consistency: item and spell names are unique (case-insensitive), no
     two monster records are identical, floor_min <= floor_max, every book
     is for classes that may cast each of its spells, every spell is in at
     least one book (spells are learned and cast only from a book);
     spell effects: the condition and handler names of the schema are the
     engine's (src/shared/rules.c, src/shared/spell_effect.c), only cantrips
     scale, only martial maneuvers have a fixed DC, a save rider has a
     condition to resist;
  4. canonical form, the one the Data Studio (tools/web) writes:
     2-space indentation, UTF-8 text (no \\u escapes), integral numbers
     without a decimal point, one trailing newline. --fix applies it.
"""

import argparse
import json
import os
import re
import sys

try:
    import jsonschema
except ImportError:  # pragma: no cover - reported to the user
    sys.exit("check_data.py: the jsonschema module is missing "
             "(dnf install python3-jsonschema / apt install python3-jsonschema)")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
FILES = ("bestiary", "items", "spells")
# ClassType order of include/classes.h: bit N of book_class_mask.
CLASSES = ("barbarian", "bard", "cleric", "druid", "fighter", "monk", "paladin",
           "ranger", "rogue", "sorcerer", "warlock", "wizard")


def integral(v):
    """Integral floats as integers, as JSON.stringify writes them."""
    if isinstance(v, float) and v.is_integer():
        return int(v)
    if isinstance(v, dict):
        return {k: integral(x) for k, x in v.items()}
    if isinstance(v, list):
        return [integral(x) for x in v]
    return v


def canonical(doc):
    """The form the Data Studio (tools/web, JSON.stringify(doc, null, 2))
    writes, so that editing a file there leaves the other lines alone."""
    return json.dumps(integral(doc), indent=2, ensure_ascii=False) + "\n"


class Report:
    def __init__(self):
        self.errors = 0

    def error(self, where, msg):
        self.errors += 1
        print("%s: %s" % (where, msg))


def load(report):
    docs, raw = {}, {}
    for name in FILES:
        path = os.path.join(DATA, name + ".json")
        with open(path, encoding="utf-8") as f:
            raw[name] = f.read()
        try:
            docs[name] = json.loads(raw[name])
        except json.JSONDecodeError as e:
            report.error("data/%s.json" % name, "not valid JSON: %s" % e)
    return docs, raw


def validate(report, docs):
    for name, doc in docs.items():
        with open(os.path.join(DATA, "schema", name + ".schema.json"), encoding="utf-8") as f:
            schema = json.load(f)
        validator = jsonschema.Draft202012Validator(
            schema, format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER)
        for err in sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path)):
            path = "/".join(str(p) for p in err.absolute_path)
            report.error("data/%s.json:/%s" % (name, path), err.message)


def unique(report, where, entries, key):
    seen = {}
    for i, e in enumerate(entries):
        k = key(e)
        if k in seen:
            report.error("%s[%d]" % (where, i), "duplicate of entry %d (%r)" % (seen[k], k))
        else:
            seen[k] = i


def cross_check(report, docs):
    monsters = docs["bestiary"]["monsters"]
    items = docs["items"]["items"]
    spells = docs["spells"]["spells"]
    unique(report, "items", items, lambda e: e["name"].lower())
    unique(report, "spells", spells, lambda e: e["name"].lower())
    unique(report, "monsters", monsters, lambda e: json.dumps(e, sort_keys=True))
    for i, m in enumerate(monsters):
        if m["floor_min"] > m["floor_max"]:
            report.error("monsters[%d]" % i, "floor_min %d > floor_max %d (%s)"
                         % (m["floor_min"], m["floor_max"], m["name"]))

    by_name = {s["name"].lower(): s for s in spells}
    in_book = set()
    for i, it in enumerate(items):
        where = "items[%d] %s" % (i, it["name"])
        if "cast_spell" in it and it["cast_spell"].lower() not in by_name:
            report.error(where, "cast_spell %r is not in spells.json" % it["cast_spell"])
        if "spells" not in it:
            continue
        book_classes = {c for n, c in enumerate(CLASSES) if it["book_class_mask"] >> n & 1}
        for sname in it["spells"]:
            sp = by_name.get(sname.lower())
            if sp is None:
                report.error(where, "spell %r is not in spells.json" % sname)
                continue
            in_book.add(sname.lower())
            casters = set(sp["classes"].split(","))
            if not book_classes & casters:
                report.error(where, "spell %r is for %s, the book for %s"
                             % (sname, sp["classes"], ",".join(sorted(book_classes))))
    for i, sp in enumerate(spells):
        if sp["name"].lower() not in in_book:
            report.error("spells[%d] %s" % (i, sp["name"]), "in no book: it cannot be learned")


def c_names(path, table):
    """The string values of one designated-initializer table of a C file."""
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"%s\[[^]]*\] = \{(.*?)\};" % re.escape(table), text, re.S)
    return set(re.findall(r'= "([^"]+)"', m.group(1))) if m else set()


def effect_check(report, spells):
    with open(os.path.join(DATA, "schema", "spells.schema.json"), encoding="utf-8") as f:
        props = json.load(f)["properties"]["spells"]["items"]["properties"]["effect"]["properties"]
    engine = {
        "condition": {n.lower() for n in c_names("src/shared/rules.c", "g_condition_names")},
        "handler": c_names("src/shared/spell_effect.c", "k_handlers"),
    }
    for field, names in engine.items():
        if set(props[field]["enum"]) != names:
            report.error("data/schema/spells.schema.json", "effect.%s names %s, the engine %s"
                         % (field, sorted(props[field]["enum"]), sorted(names)))
    for i, sp in enumerate(spells):
        e, where = sp["effect"], "spells[%d] %s" % (i, sp["name"])
        if e.get("cantrip_scaling") and sp["level"] != "cantrip":
            report.error(where, "cantrip_scaling on a level %s spell" % sp["level"])
        if "dc" in e and sp["school"] != "martial":
            report.error(where, "a fixed dc on a spell (only martial maneuvers have one)")
        if "save" in e and e.get("roll") != "save" and "condition" not in e:
            report.error(where, "a rider save without a condition to resist")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fix", action="store_true", help="rewrite the files in canonical form")
    args = ap.parse_args()

    report = Report()
    docs, raw = load(report)
    if len(docs) == len(FILES):
        validate(report, docs)
        if report.errors == 0:
            cross_check(report, docs)
            effect_check(report, docs["spells"]["spells"])
    for name, doc in docs.items():
        text = canonical(doc)
        if text == raw[name]:
            continue
        if args.fix:
            with open(os.path.join(DATA, name + ".json"), "w", encoding="utf-8") as f:
                f.write(text)
            print("data/%s.json: rewritten in canonical form" % name)
        else:
            report.error("data/%s.json" % name,
                         "not in canonical form (tools/check_data.py --fix)")
    if report.errors:
        print("check_data: %d error(s)" % report.errors)
        return 1
    print("check_data: %d monsters, %d items, %d spells: OK"
          % (len(docs["bestiary"]["monsters"]), len(docs["items"]["items"]),
             len(docs["spells"]["spells"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
