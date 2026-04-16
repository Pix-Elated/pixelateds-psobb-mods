# PSOBB Entity Class Descriptor Table

Entity type identification uses the **class metadata pointer at entity+0x04** (NOT unitxt_id).
These are NOT true C++ class descriptors — they're asset table entries in sect_3 (.bss). Multiple entries exist per enemy (base body, variants, hitboxes, projectiles). The real OOP class identity is the vtable at entity+0x00 (in the 0xB0xxxx range).

**What we use:** the +0x04 value is a unique per-type identifier (verified locale-independent and stable between builds), so it works as an identity key even though it's not the "class descriptor" in a strict sense. For disambiguation between variants that share a +0x04, use the battleparams HP at (entity+0x2B4)+0x06 plus the `+0x378 enemy_name_unitxt_index` field.

Ghidra struct: `enemy` size 0x448, `+0x004 pointer type_id` (what we call `cls_meta`).

## How to identify an entity type
```
uint32_t cls = SafeRead<uint32_t>(entity_addr + 0x04);
// cls is the class descriptor — unique per entity type, immutable after spawn
```

## De Rol Le / Dal Ra Lie (Barba Ray) Boss System

| Class (entity+0x04) | VTable (entity+0x00) | Type | HP (Normal) | Source |
|---|---|---|---|---|
| 0x00A43D2C | 0x00AF4440 | De Rol Le body controller | 6000 (real at +0x6B4) | r2 |
| 0x00A43DD8 | 0x00AF4BE0 (deployed) | De Rol Le static mine (spike) | 330 | r2 ctor |
| 0x00A43DD8 | 0x00AF4DC0 (reserve) | De Rol Le static mine (reserve pool) | 330 | r2 ctor |
| 0x00A72D1C | 0x00AFF860 | De Rol Le tracker projectile | 140 | live dumps |
| 0x00A47AF8 | — | Barba Ray body + skull | varies | solylib |
| 0x00A47B0C | — | Barba Ray shell segment | varies | solylib |

### Mine lifecycle
- Static mines: vtable swaps between 0xAF4BE0 (deployed) and 0xAF4DC0 (reserve)
- Fuse countdown at +0x3D8 (frames at 30fps), starts ~180, ticks to 0
- State enum at +0x3D4: 0=winding, 1=active, 2=exploding
- Class descriptor 0xA43DD8 loaded from global [0x9A482C]

### Boss data pointer: 0x00A43CC8
- Points to 48-byte stat struct in global pool
- +0x18 = body max HP, +0x1C = shell max HP per piece, +0x20 = skull HP

### Boss entity singleton: 0x00A43CE0
- Points to the actual De Rol Le entity object

## Vol Opt Phase 1 (uid=46)

| Class (entity+0x04) | Type | HP (Normal) | Source |
|---|---|---|---|
| 0x00A447D4 | Unknown (attached at table row 0x9a4de8) | ? | r2 |
| 0x00A44804 | Phase 1 body | 3000 | r2, live dumps |
| 0x00A44814 | Unknown (standalone at 0x9a4ee0) | ? | r2 |
| 0x00A44844 | Chandelier (hanging center) | 450 | r2, live dumps |
| 0x00A449D0 | Monitor/Panel (wall screens, small targets) | 350-420 | r2, live dumps |
| 0x00A44A18 | Pillar (red pop-up turrets) | 500 | r2, live dumps |

### Monitor vs Panel distinction (both cls 0xA449D0)
- Same class descriptor, different battle params HP
- Compare entity max_hp against battle params template HP at *(entity+0x2B4)+0x06
- Boosted HP = Monitor (420), template HP = Panel (350)

## Vol Opt Phase 2

| Class (entity+0x04) | Type | Source |
|---|---|---|
| 0x00A44BF0 | Phase 2 main body | r2, live dumps |
| 0x00A44C00 | Phase 2 subpart #1 | r2 sect_3 |
| 0x00A44C18 | Phase 2 subpart #2 | r2 sect_3 |
| 0x00A44C1C | Phase 2 subpart #2b | r2 sect_3 |
| 0x00A44C34 | Phase 2 subpart #3 | r2 sect_3 |
| 0x00A44C38 | Phase 2 subpart #3b | r2 sect_3 |
| 0x00A44C68 | Phase 2 subpart #4 | r2 sect_3 |
| 0x00A44C78 | Phase 2 subpart #5 | r2 sect_3 |
| 0x00A44C8C | Phase 2 subpart #6 | r2 sect_3 |
| 0x00A44CA0 | Phase 2 subpart #7 | r2 sect_3 |
| 0x00A44CB8 | Phase 2 subpart #8 | r2 sect_3 |
| 0x00A44CBC | Phase 2 subpart #8b | r2 sect_3 |

Phase 2 subparts share the same HP pool as the main body — not separate entities with independent HP.

## Key Global Addresses

| Address | Type | Purpose |
|---|---|---|
| 0x9A482C | u32 ptr | Mine class metadata global → 0xA43DD8 |
| 0xA43CC8 | u32 ptr | Boss data pointer (De Rol Le / Barba Ray stats) |
| 0xA43CE0 | u32 ptr | Boss entity singleton pointer |
| 0x9A4DC0-0x9A56A0 | struct array | Vol Opt descriptor table |
| 0x00B5F800 | u32 ptr | Ephinea monster HP array (runtime-allocated) |
| 0x00B5F804 | f64 | Ephinea HP scaling factor |

## Full extracted table (460 cls ptrs)

Extracted via `ghidra_export/extract_entity_table.py` — scans the
Ephinea PsoBB.exe for all 4-byte values in the static global region
(sect_3/sect_4, 0x9A2000..0xAF2000) that reference the class
descriptor region (0xA40000..0xA80000). Each reference is matched
against the nearest preceding model filename string (.bml/.nj/.njm)
to derive a display name.

Results:
- **460 distinct cls ptrs** referenced from globals
- **347 mapped to model files** (named enemies, bosses, NPCs)
- **113 unnamed** (boss sub-parts / internal entities)

See `ghidra_export/entity_table.csv` for the full table and
`entity_cls_table.h` for the generated C++ lookup code that
gets `#include`'d into `LookupEntityType()`.

### Notable clusters

| Cls range | Model | Meaning |
|---|---|---|
| 0xA4366C..0xA436A8 | rico_ring.bml | Rico's ring (quest NPC) |
| 0xA437E4..0xA4392C | bm_boss1_dragon.bml | Dragon boss + hitboxes |
| 0xA43D2C..0xA44274 | bm_boss2_de_rol_le.bml | De Rol Le boss + mines + shells |
| 0xA44804..0xA44A18 | bm_boss3_volopt_ap.bml | Vol Opt Phase 1 body + sub-parts |
| 0xA44BF0..0xA44CBC | (Vol Opt P2) | Vol Opt Phase 2 body + 11 subparts |
| 0xA44D6C..0xA45200 | darkfalz_dat.bml | Dark Falz + Darvants + phases |
| 0xA46FA4..0xA473A0 | boss06_*.bml | Barba Ray (plot, rock, tube parts) |
| 0xA47AF8..0xA47B0C | (no model) | Barba Ray body + shell |
| 0xA72D1C | (no model) | De Rol Le tracker projectile |

### Known Darvant ranges

Dark Falz cls range 0xA44D6C..0xA45200 is entirely mapped to
`darkfalz_dat.bml`. The Darvant spinners (125 HP, 76 simultaneous)
are somewhere in this range — the live dumps will tell us which
specific cls when the user next fights Falz (see the unknown-cls
logging in `GetAliveMonsters`). All entries in this range are
currently treated as `BossSubpart` (hidden) except the first which
shows as "Dark Falz".

### Limitations of the heuristic

The "first cls per model = main body" rule sometimes misses when
multiple distinct enemies share the same model (e.g. Booma + Go-Booma
+ Gi-Booma all use `bm_ene_bm2_moja_a.bml`). For those cases we need
per-cls disambiguation by HP or position. The live debugger logs
unknown cls values so we can refine the table over time.
