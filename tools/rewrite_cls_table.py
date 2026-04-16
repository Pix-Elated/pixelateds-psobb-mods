#!/usr/bin/env python3
"""Rewrite entity_cls_table.h: game-named entries become nullptr,
synthetic entries become references to LocalizedString instances
declared in pixelated_mods.cpp before LookupEntityType."""

import re
import sys
from pathlib import Path

# Synthetic name -> LocalizedString variable name
SYNTHETIC = {
    "Dragon Subpart":     "kSynthDragonSubpart",
    "Vol Opt Chandelier": "kSynthVolOptChandelier",
    "Vol Opt Monitor":    "kSynthVolOptMonitor",
    "Vol Opt Panel":      "kSynthVolOptPanel",
    "Vol Opt Spire":      "kSynthVolOptSpire",
    "Barba Ray Minion":   "kSynthBarbaRayMinion",
    "Olga Flow Hitbox":   "kSynthOlgaFlowHitbox",
    "Olga Flow Ball":     "kSynthOlgaFlowBall",
    "Dark Falz Darvant":  "kSynthDarkFalzDarvant",
}

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")

# Match:  { EntityRole::Xxx, "Name" }
pat = re.compile(r'\{\s*(EntityRole::\w+)\s*,\s*"([^"]+)"\s*\}')

def repl(m):
    role = m.group(1)
    name = m.group(2)
    if name in SYNTHETIC:
        return f"{{ {role}, &{SYNTHETIC[name]} }}"
    return f"{{ {role}, nullptr }}"

new_text, n = pat.subn(repl, text)
path.write_text(new_text, encoding="utf-8")
print(f"Rewrote {n} entries in {path}")
