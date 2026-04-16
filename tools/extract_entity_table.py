#!/usr/bin/env python3
"""
One-shot extraction of the complete PSOBB entity class table.

Input sources (from the Ghidra export, which is NOT shipped):
  - ghidra_export/entity_table.txt    (cls | ref_va | model | derived_name)
  - ghidra_export/all_constructors.json (Ghidra-identified constructors)

Output:
  - entity_cls_table.h at the repo root — the single source of truth.

Design:
  - PsoBB.exe is a frozen 2004-era binary. Every cls value is baked into
    .rdata at constructor addresses and will never change. This script
    runs once, writes a static header, and the Ghidra scaffolding gets
    deleted. Re-run only if Ghidra analysis improves upstream.

  - The entity_table.txt rows are in .rdata order, which is source-file
    order. "(no model)" sub-entities cluster immediately after their
    owning named cls. We use positional inheritance to label them as
    "<owner> Subpart" (role = BossSubpart, hidden) instead of dropping
    them, so nothing falls through to a generic "Sub-entity" fallback.

  - Manual overrides for special roles (SegmentBossBody for De Rol Le /
    Barba Ray, CollapseByName for Vol Opt sub-parts and Dark Falz
    Darvants, explicit hides for hitboxes) live in OVERRIDES below.
    They take precedence over the auto-classified rows.
"""

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GHIDRA = ROOT / "ghidra_export"
OUT_H = ROOT / "entity_cls_table.h"

# ---------------------------------------------------------------------------
# Manual overrides — take precedence over auto-classified rows.
#
# Tuple = (role, display_name_or_None)
#   NormalMob        individual row, normal handling
#   SegmentBossBody  De Rol Le / Barba Ray body (uses boss HP pointer)
#   SegmentBossShell shell segments that collapse into (xN) row
#   BossSubpart      shares HP pool, hide from list
#   BossProjectile   mines/trackers, hide
#   CollapseByName   aggregate multiple cls values into a single named row
# ---------------------------------------------------------------------------
OVERRIDES = {
    # De Rol Le
    0x00A43D2C: ("SegmentBossBody",  None),          # body
    0x00A43DD8: ("BossProjectile",   None),          # mine

    # Barba Ray
    0x00A47AF8: ("SegmentBossBody",  None),          # body
    0x00A47B0C: ("SegmentBossShell", None),          # shell

    # Vol Opt Phase 1 — ground-truth from in-game HUD observation.
    0x00A447D4: ("BossSubpart",      None),          # Control (hidden)
    0x00A44804: ("CollapseByName",   "Vol Opt"),     # Body / Core
    0x00A44814: ("BossSubpart",      None),          # Dead Checker
    0x00A44844: ("CollapseByName",   "Vol Opt Chandelier"),  # hanging pyramid
    0x00A449D0: ("CollapseByName",   "Vol Opt Monitor"),
    0x00A44A18: ("CollapseByName",   "Vol Opt Spire"),       # floor-mounted red turrets

    # Vol Opt Phase 2 — body visible, subparts hidden (share body HP)
    0x00A44BF0: ("CollapseByName",   "Vol Opt ver.2"),
    0x00A44C00: ("BossSubpart",      None),
    0x00A44C18: ("BossSubpart",      None),
    0x00A44C1C: ("BossSubpart",      None),
    0x00A44C34: ("BossSubpart",      None),
    0x00A44C38: ("BossSubpart",      None),
    0x00A44C68: ("BossSubpart",      None),
    0x00A44C78: ("BossSubpart",      None),
    0x00A44C8C: ("BossSubpart",      None),
    0x00A44CA0: ("BossSubpart",      None),
    0x00A44CB8: ("BossSubpart",      None),
    0x00A44CBC: ("BossSubpart",      None),

    # Dark Falz Phase 2 body. 6500 HP. cls 0x00A44D6C. Verified by
    # the user (multiple times). Always shows up with uid=0 because
    # the Phase 2 form doesn't carry a unitxt name — Sega assigned
    # the unitxt to the Phase 1 entity (uid=47, ~5900 HP) instead.
    # We give it the explicit "Dark Falz" name here so it shows in
    # the enemies panel during the actual fight.
    #
    # Pre-fight visibility (the dormant copy that exists before the
    # cutscene plays) is filtered at runtime via the entity_flags
    # 0x8000 "active in world" bit — see the main collection loop
    # in pixelated_mods.cpp.
    0x00A44D6C: ("CollapseByName",   "Dark Falz"),

    # Dark Falz Phase 3 Darvants — 80 small crystal spinners, 125 HP each.
    # Three cls values (one per anchor position). Collapse into one row.
    0x00A45A54: ("CollapseByName",   "Dark Falz Darvant"),
    0x00A45A58: ("CollapseByName",   "Dark Falz Darvant"),
    0x00A45A5C: ("CollapseByName",   "Dark Falz Darvant"),
}

# ---------------------------------------------------------------------------
# Name fixups applied to Ghidra constructor names and derived model names
# ("Boss1 Dragon" -> "Dragon", "Darkfalz" -> "Dark Falz", etc).
# ---------------------------------------------------------------------------
NAME_FIXUPS = {
    # Ghidra constructor display names
    "Derolle":            "De Rol Le",
    "Barbaray":           "Barba Ray",
    "Barbaray Minion":    "Barba Ray Minion",
    "Voloptcontrol":      "Vol Opt Control",
    "Darkfalz":            "Dark Falz",
    "Galgryphon":         "Gal Gryphon",
    "Grassassassin":      "Grass Assassin",
    "Illgill":            "Ill Gill",
    "Ulgibbon":           "Ul Gibbon",
    "Sinowberill":        "Sinow Berill",
    "Sinowzoa":           "Sinow Zoa",
    "Sinowbeat":          "Sinow Beat",
    "Gilchic":            "Gillchich",
    "Gigue":              "Gi Gue",
    "Nanodragon":         "Nano Dragon",
    "Panarms":            "Pan Arms",
    "Merissa":            "Merissa A",
    "Single Canadine":    "Canadine",
    "Ragrappy":           "Rag Rappy",
    "Poisonlily":         "Poison Lily",
    "Dragon Boss":        "Dragon",
    "Olga Dp Object":     "Olga Flow Hitbox",
    "Olga1 Dm Ball":      "Olga Flow Ball",

    # Derived-from-model cleanups
    "Boss1 Dragon":       "Dragon",
    "Obj Boss1 Common":   "Dragon Subpart",
    "Boss2 De Rol Le":    "De Rol Le",
    "Boss3 Volopt Ap":    "Vol Opt",
    "Boss5 Gryphon":      "Gal Gryphon",
    "Boss7 Crawfish":     "Barba Ray Minion",
    "Boss7 De Rol Le C":  "Barba Ray",
    "Boss06 Plotfalz Dat": "Barba Ray",
    "Darkfalz Dat":       "Dark Falz",
    "Rico Ring":          "Rico Tyrell",
}

# Heuristic model→name mappings. These kick in for rows where the derived
# name is too generic (e.g. a wrapper class whose model is bm_ene_X.bml
# should display as X, not as the raw model stem).
MODEL_HINTS = {
    "bm_ene_bm1_shark.bml":       "Evil Shark",
    "bm_ene_bm2_moja_a.bml":      "Hildebear",
    "bm_ene_bm3_fly.bml":         "Monest",
    "bm_ene_bm5_wolf.bml":        "Savage Wolf",
    "bm_ene_grass.bml":           "Grass Assassin",
    "bm_ene_sandlappy.bml":       "Rag Rappy",
    "bm_ene_re2_flower.bml":      "Poison Lily",
    "bm_ene_ill_gill.bml":        "Ill Gill",
    "bm_ene_balclaw.bml":         "Bulclaw",
    "bm_ene_re8_b_beast.bml":     "Booma",
    "bm_ene_re7_berura.bml":      "Dark Belra",
    "bm_ene_me1_mb.bml":          "Canadine",
    "bm_ene_darkgunner.bml":      "Dark Gunner",
    "bm_ene_del_depth_low.bml":   "Delsaber",
    "bm_ene_biter_body_low.bml":  "Delbiter",
    "bm_ene_df1_saver.bml":       "Delsaber",
    "bm_ene_df3_dimedian.bml":    "Dimenian",
    "bm_ene_dubchik.bml":         "Gillchich",
    "bm_ene_epsilon.bml":         "Epsilon",
    "bm_ene_gibbles_low.bml":     "Gibbles",
    "bm_ene_bm5_gibon_u_low.bml": "Ul Gibbon",
    "bm_ene_gi_gue_low.bml":      "Gi Gue",
    "bm_ene_gyaranzo.bml":        "Garanz",
    "bm_ene_bm9_s_mericarol_low.bml": "Mericarol",
    "bm_ene_re8_merill_lia_low.bml":  "Merillia",
    "bm_ene_morfos_low.bml":      "Morfos",
    "bm_ene_nanodrago.bml":       "Nano Dragon",
    "bm7_s_paa_body.bml":         "Pan Arms",
    "bm_ene_recobox_low.bml":     "Recobox",
    "bm_ene_me3_zoa_low.bml":     "Sinow Zoa",
    "bm_ene_me3_beril_low.bml":   "Sinow Berill",
    "bm_ene_astark.bml":          "Astark",
    "bm_ene_boota.bml":           "Boota",
    "bm_ene_dolphon.bml":         "Dorphon",
    "bm_ene_golan.bml":           "Goran",
    "bm_ene_zu.bml":              "Zu",
    "wait_bm4_ps_mb_body.njm":    "Merissa A",
}


def load_entity_table():
    """Read ghidra_export/entity_table.txt -> list of (cls, model, name)."""
    rows = []
    path = GHIDRA / "entity_table.txt"
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)\s+(.+?)\s*$", line)
        if not m:
            continue
        cls = int(m.group(1), 16)
        rest = m.group(3).rstrip()
        # Either "(no model)" alone or "model.bml    Derived Name"
        if rest.startswith("(no model)"):
            rows.append((cls, None, None))
        else:
            parts = re.split(r"\s{2,}", rest, maxsplit=1)
            model = parts[0].strip() if parts else None
            name = parts[1].strip() if len(parts) > 1 else None
            rows.append((cls, model, name))
    return rows


def load_constructors():
    """Read ghidra_export/all_constructors.json -> {cls: display_name}."""
    path = GHIDRA / "all_constructors.json"
    data = json.loads(path.read_text())
    result = {}
    for cls_hex, entries in data.get("cls_groups", {}).items():
        cls = int(cls_hex, 16)
        if not entries:
            continue
        # Prefer the shortest display name (usually the cleanest). If the
        # prefix is "Unknown ", strip it.
        names = [e.get("display", "").strip() for e in entries if e.get("display")]
        names = [n for n in names if n]
        if not names:
            continue
        names.sort(key=lambda n: (len(n), n))
        pick = names[0]
        if pick.startswith("Unknown "):
            pick = pick[len("Unknown "):]
        result[cls] = pick
    return result


def clean_name(name):
    """Run NAME_FIXUPS, strip stock suffixes, titlecase where safe."""
    if not name:
        return None
    name = name.strip()
    if name in NAME_FIXUPS:
        return NAME_FIXUPS[name]
    return name


def classify(rows, constructors):
    """
    For each cls row, produce a (role, name) pair.

    Resolution order:
      1. OVERRIDES (hand-tuned)
      2. Ghidra constructor name (cleaned)
      3. MODEL_HINTS (model filename -> canonical enemy name)
      4. Derived name from the entity_table row (cleaned)
      5. Inherited "<owner> Subpart" for (no model) rows, from the
         nearest preceding row that produced a name
      6. BossSubpart with None as an absolute fallback (hidden)
    """
    classified = []
    last_family = None  # name of the nearest preceding named cls

    for cls, model, derived in rows:
        # 1. manual override
        if cls in OVERRIDES:
            role, name = OVERRIDES[cls]
            classified.append((cls, role, name, model, "override"))
            if name:
                last_family = name
            continue

        # 2. constructor name
        name = None
        source = None
        if cls in constructors:
            name = clean_name(constructors[cls])
            source = "ctor"

        # 3. model hint
        if not name and model and model in MODEL_HINTS:
            name = MODEL_HINTS[model]
            source = "model_hint"

        # 4. derived name
        if not name and derived:
            name = clean_name(derived)
            source = "derived"

        if name:
            last_family = name
            classified.append((cls, "NormalMob", name, model, source))
            continue

        # 5. inherit from nearest named owner
        if last_family:
            classified.append((cls, "BossSubpart", None, model, f"inherit:{last_family}"))
            continue

        # 6. absolute fallback: hide
        classified.append((cls, "BossSubpart", None, model, "fallback"))

    return classified


def emit_header(classified):
    lines = []
    lines.append("// PSOBB entity class descriptor table — single source of truth.")
    lines.append("//")
    lines.append("// Generated ONCE from a Ghidra analysis of PsoBB.exe .rdata. The source")
    lines.append("// binary is frozen (2004 Sega build, Ephinea patches server-side only),")
    lines.append("// so this table does not need to be regenerated. Manual edits are fine —")
    lines.append("// see tools/extract_entity_table.py for the extraction logic if you need")
    lines.append("// to re-derive it.")
    lines.append("//")
    lines.append("// Coverage: every cls value referenced in the binary's enemy+object")
    lines.append("// constructor .rdata tables. Sub-entities with no model or no Ghidra-")
    lines.append("// named constructor are auto-classified as BossSubpart (hidden) based")
    lines.append("// on positional clustering with the nearest preceding named cls.")
    lines.append("//")
    lines.append("// Usage: #include inside LookupEntityType() body.")
    lines.append("")
    lines.append(f"// Total entries: {len(classified)}")
    lines.append("")

    for cls, role, name, model, source in classified:
        if name is not None:
            name_str = f'"{name}"'
        else:
            name_str = "nullptr"
        comment_bits = []
        if model:
            comment_bits.append(model)
        comment_bits.append(source)
        comment = " // " + ", ".join(comment_bits)
        lines.append(
            f"    case 0x{cls:08X}: return {{ EntityRole::{role}, {name_str} }};{comment}"
        )
    lines.append("")
    return "\n".join(lines)


def main():
    rows = load_entity_table()
    constructors = load_constructors()
    print(f"Loaded {len(rows)} rows, {len(constructors)} named constructors")

    classified = classify(rows, constructors)

    stats = {}
    for _, role, name, _, source in classified:
        key = source
        stats[key] = stats.get(key, 0) + 1
    print("Classification source counts:")
    for k, v in sorted(stats.items(), key=lambda kv: -kv[1]):
        print(f"  {k:20s} {v:4d}")

    hidden = sum(1 for _, _, name, _, _ in classified if name is None)
    print(f"Hidden (BossSubpart nullptr): {hidden}")
    print(f"Visible:                      {len(classified) - hidden}")

    header = emit_header(classified)
    OUT_H.write_text(header, encoding="utf-8")
    print(f"Wrote {OUT_H}")


if __name__ == "__main__":
    main()
