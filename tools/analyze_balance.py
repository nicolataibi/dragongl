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
analyze_balance.py — Dragon GL Combat Log Analyzer
====================================================
Reads combat_log.json (produced by the dragongl_server in real-time)
and prints a detailed statistical report for game balance analysis.

Usage:
    python3 tools/analyze_balance.py [path/to/combat_log.json]

Default path: ./combat_log.json
"""

import json
import sys
import os
from collections import defaultdict


def load_log(filepath):
    if not os.path.exists(filepath):
        print(f"[ERROR] File not found: {filepath}")
        sys.exit(1)
    with open(filepath, "r") as f:
        try:
            data = json.load(f)
        except json.JSONDecodeError as e:
            print(f"[ERROR] Invalid JSON: {e}")
            sys.exit(1)
    return data


def analyze_attacks(events):
    attacks = [e for e in events if e.get("type") == "attack"]
    if not attacks:
        return
    total = len(attacks)
    hits = sum(1 for e in attacks if e.get("hit"))
    crits = sum(1 for e in attacks if e.get("crit"))
    total_damage = sum(e.get("damage", 0) for e in attacks if e.get("hit"))
    hit_damages = [e.get("damage", 0) for e in attacks if e.get("hit")]
    avg_damage = total_damage / hits if hits else 0.0

    print("=" * 60)
    print("  ATTACK REPORT")
    print("=" * 60)
    print(f"  Total attacks   : {total}")
    print(f"  Hits            : {hits}  ({hits/total*100:.1f}%)")
    print(f"  Misses          : {total - hits}  ({(total-hits)/total*100:.1f}%)")
    print(f"  Critical hits   : {crits}  ({crits/total*100:.1f}%)")
    print(f"  Total damage    : {total_damage}")
    print(f"  Avg damage/hit  : {avg_damage:.2f}")
    if hit_damages:
        print(f"  Max damage hit  : {max(hit_damages)}")
        print(f"  Min damage hit  : {min(hit_damages)}")

    attacker_stats = defaultdict(lambda: {"attacks": 0, "hits": 0, "damage": 0})
    for e in attacks:
        a = e.get("attacker", "?")
        attacker_stats[a]["attacks"] += 1
        if e.get("hit"):
            attacker_stats[a]["hits"] += 1
            attacker_stats[a]["damage"] += e.get("damage", 0)

    print()
    print("  Per-attacker breakdown:")
    print(f"  {'Name':<20} {'Attacks':>8} {'Hit%':>8} {'Total DMG':>10} {'Avg DMG':>9}")
    print(f"  {'-'*20} {'-'*8} {'-'*8} {'-'*10} {'-'*9}")
    for name, s in sorted(attacker_stats.items(), key=lambda x: -x[1]["damage"]):
        hit_pct = s["hits"] / s["attacks"] * 100 if s["attacks"] else 0
        avg = s["damage"] / s["hits"] if s["hits"] else 0
        print(f"  {name:<20} {s['attacks']:>8} {hit_pct:>7.1f}% {s['damage']:>10} {avg:>9.2f}")


def analyze_saves(events):
    saves = [e for e in events if e.get("type") == "save"]
    if not saves:
        return
    total = len(saves)
    passed = sum(1 for e in saves if e.get("passed"))

    print()
    print("=" * 60)
    print("  SAVING THROW REPORT")
    print("=" * 60)
    print(f"  Total saves     : {total}")
    print(f"  Passed          : {passed}  ({passed/total*100:.1f}%)")
    print(f"  Failed          : {total - passed}  ({(total-passed)/total*100:.1f}%)")

    effect_stats = defaultdict(lambda: {"total": 0, "passed": 0})
    for e in saves:
        ef = e.get("effect", "?")
        effect_stats[ef]["total"] += 1
        if e.get("passed"):
            effect_stats[ef]["passed"] += 1

    print()
    print("  Per-effect breakdown:")
    print(f"  {'Effect':<25} {'Total':>7} {'Pass%':>8}")
    print(f"  {'-'*25} {'-'*7} {'-'*8}")
    for ef, s in sorted(effect_stats.items(), key=lambda x: -x[1]["total"]):
        pct = s["passed"] / s["total"] * 100 if s["total"] else 0
        print(f"  {ef:<25} {s['total']:>7} {pct:>7.1f}%")


def analyze_spells(events):
    spells = [e for e in events if e.get("type") == "spell"]
    if not spells:
        return
    total = len(spells)
    total_value = sum(e.get("value", 0) for e in spells)
    saved = sum(1 for e in spells if e.get("saved"))

    print()
    print("=" * 60)
    print("  SPELL REPORT")
    print("=" * 60)
    print(f"  Total casts     : {total}")
    print(f"  Resisted (saved): {saved}  ({saved/total*100:.1f}%)")
    print(f"  Total value     : {total_value}  (damage + healing)")

    spell_stats = defaultdict(lambda: {"casts": 0, "value": 0, "saved": 0})
    for e in spells:
        sp = e.get("spell", "?")
        spell_stats[sp]["casts"] += 1
        spell_stats[sp]["value"] += e.get("value", 0)
        if e.get("saved"):
            spell_stats[sp]["saved"] += 1

    print()
    print("  Per-spell breakdown:")
    print(f"  {'Spell':<28} {'Casts':>6} {'Total Val':>10} {'Avg Val':>9} {'Save%':>7}")
    print(f"  {'-'*28} {'-'*6} {'-'*10} {'-'*9} {'-'*7}")
    for sp, s in sorted(spell_stats.items(), key=lambda x: -x[1]["value"]):
        avg = s["value"] / s["casts"] if s["casts"] else 0
        save_pct = s["saved"] / s["casts"] * 100 if s["casts"] else 0
        print(f"  {sp:<28} {s['casts']:>6} {s['value']:>10} {avg:>9.2f} {save_pct:>6.1f}%")


def analyze_deaths(events):
    deaths = [e for e in events if e.get("type") == "death"]
    if not deaths:
        return
    total = len(deaths)

    print()
    print("=" * 60)
    print("  DEATH REPORT")
    print("=" * 60)
    print(f"  Total deaths    : {total}")

    floor_deaths = defaultdict(int)
    killer_kills = defaultdict(int)
    for e in deaths:
        floor_deaths[e.get("floor", -1)] += 1
        killer_kills[e.get("killer", "?")] += 1

    print()
    print("  Deaths by floor:")
    for floor in sorted(floor_deaths):
        bar = "#" * min(floor_deaths[floor], 40)
        print(f"    Floor {floor:>3}  : {floor_deaths[floor]:>4}  {bar}")

    print()
    print("  Top killers:")
    print(f"  {'Killer':<25} {'Kills':>6}")
    print(f"  {'-'*25} {'-'*6}")
    for killer, kills in sorted(killer_kills.items(), key=lambda x: -x[1])[:15]:
        print(f"  {killer:<25} {kills:>6}")


def main():
    filepath = sys.argv[1] if len(sys.argv) > 1 else "combat_log.json"
    print(f"\nDragon GL — Combat Balance Analyzer")
    print(f"Reading: {filepath}\n")
    events = load_log(filepath)
    print(f"Total events loaded: {len(events)}")
    print()
    analyze_attacks(events)
    analyze_saves(events)
    analyze_spells(events)
    analyze_deaths(events)
    print()
    print("=" * 60)
    print("  Analysis complete.")
    print("=" * 60)
    print()


if __name__ == "__main__":
    main()
