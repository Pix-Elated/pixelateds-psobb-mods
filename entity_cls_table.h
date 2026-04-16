// PSOBB entity class descriptor table — single source of truth.
//
// Generated ONCE from a Ghidra analysis of PsoBB.exe .rdata. The source
// binary is frozen (2004 Sega build, Ephinea patches server-side only),
// so this table does not need to be regenerated. Manual edits are fine —
// see tools/extract_entity_table.py for the extraction logic if you need
// to re-derive it.
//
// Coverage: every cls value referenced in the binary's enemy+object
// constructor .rdata tables. Sub-entities with no model or no Ghidra-
// named constructor are auto-classified as BossSubpart (hidden) based
// on positional clustering with the nearest preceding named cls.
//
// Usage: #include inside LookupEntityType() body.

// Total entries: 460

    case 0x00A4366C: return { EntityRole::NormalMob, nullptr }; // rico_ring.bml, derived
    case 0x00A43680: return { EntityRole::NormalMob, nullptr }; // rico_ring.bml, derived
    case 0x00A436A0: return { EntityRole::NormalMob, nullptr }; // rico_ring.bml, derived
    case 0x00A436A4: return { EntityRole::NormalMob, nullptr }; // rico_ring.bml, derived
    case 0x00A436A8: return { EntityRole::NormalMob, nullptr }; // rico_ring.bml, derived
    case 0x00A437E4: return { EntityRole::NormalMob, nullptr }; // bm_boss1_dragon.bml, ctor
    case 0x00A437E8: return { EntityRole::NormalMob, nullptr }; // bm_boss1_dragon.bml, derived
    case 0x00A437EC: return { EntityRole::NormalMob, nullptr }; // bm_boss1_dragon.bml, derived
    case 0x00A437F0: return { EntityRole::NormalMob, nullptr }; // bm_boss1_dragon.bml, derived
    case 0x00A437F4: return { EntityRole::NormalMob, nullptr }; // bm_boss1_dragon.bml, derived
    case 0x00A437FC: return { EntityRole::NormalMob, nullptr }; // bm_boss1_dragon.bml, derived
    case 0x00A43808: return { EntityRole::NormalMob, nullptr }; // bm_boss1_dragon.bml, derived
    case 0x00A43818: return { EntityRole::NormalMob, nullptr }; // bm_boss1_dragon.bml, derived
    case 0x00A43824: return { EntityRole::NormalMob, &kSynthDragonSubpart }; // bm_obj_boss1_common.bml, derived
    case 0x00A4389C: return { EntityRole::NormalMob, &kSynthDragonSubpart }; // bm_obj_boss1_common.bml, derived
    case 0x00A438AC: return { EntityRole::NormalMob, &kSynthDragonSubpart }; // bm_obj_boss1_common.bml, derived
    case 0x00A4390C: return { EntityRole::NormalMob, &kSynthDragonSubpart }; // bm_obj_boss1_common.bml, derived
    case 0x00A43910: return { EntityRole::NormalMob, &kSynthDragonSubpart }; // bm_obj_boss1_common.bml, derived
    case 0x00A43914: return { EntityRole::NormalMob, &kSynthDragonSubpart }; // bm_obj_boss1_common.bml, derived
    case 0x00A43918: return { EntityRole::NormalMob, &kSynthDragonSubpart }; // bm_obj_boss1_common.bml, derived
    case 0x00A43928: return { EntityRole::NormalMob, &kSynthDragonSubpart }; // bm_obj_boss1_common.bml, derived
    case 0x00A4392C: return { EntityRole::NormalMob, &kSynthDragonSubpart }; // bm_obj_boss1_common.bml, derived
    case 0x00A43D2C: return { EntityRole::SegmentBossBody, nullptr }; // bm_boss2_de_rol_le.bml, override
    case 0x00A43D88: return { EntityRole::NormalMob, nullptr }; // bm_boss2_de_rol_le.bml, derived
    case 0x00A43DAC: return { EntityRole::NormalMob, nullptr }; // bm_boss2_de_rol_le.bml, derived
    case 0x00A43DB0: return { EntityRole::NormalMob, nullptr }; // bm_boss2_de_rol_le.bml, derived
    case 0x00A43DD0: return { EntityRole::NormalMob, nullptr }; // bm_boss2_de_rol_le.bml, derived
    case 0x00A43DD4: return { EntityRole::NormalMob, nullptr }; // bm_boss2_de_rol_le.bml, derived
    case 0x00A43DD8: return { EntityRole::BossProjectile, nullptr }; // bm_boss2_de_rol_le.bml, override
    case 0x00A43DE8: return { EntityRole::NormalMob, nullptr }; // bm_boss2_de_rol_le.bml, derived
    case 0x00A43DEC: return { EntityRole::NormalMob, nullptr }; // bm_boss2_de_rol_le.bml, derived
    case 0x00A44220: return { EntityRole::NormalMob, nullptr }; // bm_boss2_de_rol_le.bml, derived
    case 0x00A44268: return { EntityRole::NormalMob, nullptr }; // bm_boss2_de_rol_le.bml, derived
    case 0x00A44274: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss2_common.bml, unitxt
    case 0x00A44714: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss2_common.bml, unitxt
    case 0x00A44718: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss2_common.bml, unitxt
    case 0x00A44724: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss2_common.bml, unitxt
    case 0x00A44734: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss2_common.bml, unitxt
    case 0x00A447D4: return { EntityRole::BossSubpart, nullptr }; // bm_boss3_volopt_ap.bml, override
    // Vol Opt Phase 1 body — nullptr display_name lets the name
    // resolve through the unitxt path (uid 46 → locale-correct
    // "Vol Opt" / "Vol Opt ver.2" per the game's own text table).
    // Still CollapseByName so all Vol Opt sub-parts aggregate into
    // a single HP row.
    case 0x00A44804: return { EntityRole::CollapseByName, nullptr }; // bm_boss3_volopt_ap.bml, use unitxt
    case 0x00A44814: return { EntityRole::BossSubpart, nullptr }; // bm_boss3_volopt_ap.bml, override
    case 0x00A44844: return { EntityRole::CollapseByName, &kSynthVolOptChandelier }; // bm_boss3_volopt_ap.bml, override
    case 0x00A449D0: return { EntityRole::CollapseByName, &kSynthVolOptMonitor }; // bm_boss3_volopt_ap.bml, override
    case 0x00A44A18: return { EntityRole::CollapseByName, &kSynthVolOptSpire }; // bm_boss3_volopt_ap.bml, override
    // Vol Opt Phase 2 body — nullptr display_name, resolves
    // via unitxt (uid 47 → the game's own Phase 2 label per
    // difficulty: "Vol Opt" on Normal/Hard/V.Hard, "Vol Opt ver.2"
    // on Ultimate, matching PSO's native target reticle).
    case 0x00A44BF0: return { EntityRole::CollapseByName, nullptr }; // use unitxt
    case 0x00A44C00: return { EntityRole::BossSubpart, nullptr }; // bm_boss3_volopt_ap.bml, override
    case 0x00A44C18: return { EntityRole::BossSubpart, nullptr }; // bm_boss3_volopt_ap.bml, override
    case 0x00A44C1C: return { EntityRole::BossSubpart, nullptr }; // bm_boss3_volopt_ap.bml, override
    case 0x00A44C34: return { EntityRole::BossSubpart, nullptr }; // bm_boss3_volopt_ap.bml, override
    case 0x00A44C38: return { EntityRole::BossSubpart, nullptr }; // bm_boss3_volopt_ap.bml, override
    case 0x00A44C68: return { EntityRole::BossSubpart, nullptr }; // bm_boss3_volopt_ap.bml, override
    case 0x00A44C78: return { EntityRole::BossSubpart, nullptr }; // override
    case 0x00A44C8C: return { EntityRole::BossSubpart, nullptr }; // override
    case 0x00A44CA0: return { EntityRole::BossSubpart, nullptr }; // override
    case 0x00A44CB8: return { EntityRole::BossSubpart, nullptr }; // override
    case 0x00A44CBC: return { EntityRole::BossSubpart, nullptr }; // override
    // All Dark Falz cls values collapse into one "Dark Falz" row.
    // The one at 0x00A44D6C is the pre-fight obelisk/placeholder that
    // sits at fixed pos (-9989, 5, -161); the rest are the active
    // Phase 1/2 rendered bodies and subparts. CollapseByName lets
    // BuildHpRows pick min-distance across all instances so the row
    // tracks whichever copy the player is actually near.
    case 0x00A44D6C: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, placeholder/obelisk
    case 0x00A44F40: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A44F7C: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A44FAC: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A44FDC: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A45008: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A4504C: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A45084: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A45194: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A45198: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A4519C: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A451A0: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A451A4: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A451A8: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A451AC: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A451B0: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A451E4: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A45200: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A4570C: return { EntityRole::CollapseByName, nullptr }; // darkfalz_dat.bml, derived
    case 0x00A45804: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz
    case 0x00A45808: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz
    case 0x00A45960: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz
    case 0x00A45964: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz
    case 0x00A45A54: return { EntityRole::CollapseByName, &kSynthDarkFalzDarvant }; // override
    case 0x00A45A58: return { EntityRole::CollapseByName, &kSynthDarkFalzDarvant }; // override
    case 0x00A45A5C: return { EntityRole::CollapseByName, &kSynthDarkFalzDarvant }; // override
    case 0x00A46B70: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz Darvant
    case 0x00A46C3C: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz Darvant
    case 0x00A46C80: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz Darvant
    case 0x00A46C84: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz Darvant
    case 0x00A46C98: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz Darvant
    case 0x00A46C9C: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz Darvant
    case 0x00A46CA0: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz Darvant
    case 0x00A46CA4: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz Darvant
    case 0x00A46CB4: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz Darvant
    case 0x00A46CC4: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz Darvant
    case 0x00A46CD4: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz Darvant
    case 0x00A46CEC: return { EntityRole::BossSubpart, nullptr }; // inherit:Dark Falz Darvant
    case 0x00A46CF0: return { EntityRole::NormalMob, &kSynthOlgaFlowHitbox }; // ctor
    case 0x00A46D00: return { EntityRole::NormalMob, &kSynthOlgaFlowHitbox }; // ctor
    case 0x00A46D10: return { EntityRole::NormalMob, &kSynthOlgaFlowHitbox }; // ctor
    case 0x00A46D14: return { EntityRole::NormalMob, &kSynthOlgaFlowHitbox }; // ctor
    case 0x00A46D38: return { EntityRole::NormalMob, &kSynthOlgaFlowHitbox }; // ctor
    case 0x00A46D50: return { EntityRole::NormalMob, &kSynthOlgaFlowBall }; // ctor
    case 0x00A46DCC: return { EntityRole::NormalMob, &kSynthOlgaFlowBall }; // ctor
    case 0x00A46E04: return { EntityRole::BossSubpart, nullptr }; // inherit:Olga Flow Ball
    case 0x00A46E60: return { EntityRole::BossSubpart, nullptr }; // inherit:Olga Flow Ball
    case 0x00A46F3C: return { EntityRole::BossSubpart, nullptr }; // inherit:Olga Flow Ball
    case 0x00A46F44: return { EntityRole::BossSubpart, nullptr }; // inherit:Olga Flow Ball
    case 0x00A46FA4: return { EntityRole::NormalMob, nullptr }; // boss06_plotfalz_dat.bml, derived
    case 0x00A471E8: return { EntityRole::NormalMob, nullptr }; // boss06_rock_dat.bml, unitxt
    case 0x00A471EC: return { EntityRole::NormalMob, nullptr }; // boss06_rock_dat.bml, unitxt
    case 0x00A471F0: return { EntityRole::NormalMob, nullptr }; // boss06_rock_dat.bml, unitxt
    case 0x00A47214: return { EntityRole::NormalMob, nullptr }; // boss06_rock_dat.bml, unitxt
    case 0x00A47230: return { EntityRole::NormalMob, nullptr }; // boss06_rock_dat.bml, unitxt
    case 0x00A47234: return { EntityRole::NormalMob, nullptr }; // boss06_rock_dat.bml, unitxt
    case 0x00A47244: return { EntityRole::NormalMob, nullptr }; // boss06_rock_dat.bml, unitxt
    case 0x00A4729C: return { EntityRole::NormalMob, nullptr }; // boss06_tube_dat.bml, unitxt
    case 0x00A472A0: return { EntityRole::NormalMob, nullptr }; // boss06_tube_dat.bml, unitxt
    case 0x00A473A0: return { EntityRole::NormalMob, nullptr }; // boss06_tube_dat.bml, unitxt
    case 0x00A47454: return { EntityRole::BossSubpart, nullptr }; // inherit:Boss06 Tube Dat
    case 0x00A474A8: return { EntityRole::BossSubpart, nullptr }; // inherit:Boss06 Tube Dat
    case 0x00A476B0: return { EntityRole::NormalMob, nullptr }; // bm_boss5_gryphon.bml, ctor
    case 0x00A476C0: return { EntityRole::NormalMob, nullptr }; // bm_boss5_gryphon.bml, derived
    case 0x00A476C4: return { EntityRole::NormalMob, nullptr }; // bm_boss5_gryphon.bml, derived
    case 0x00A476C8: return { EntityRole::NormalMob, nullptr }; // bm_boss5_gryphon.bml, derived
    case 0x00A476CC: return { EntityRole::NormalMob, nullptr }; // bm_boss5_gryphon.bml, derived
    case 0x00A476D4: return { EntityRole::NormalMob, nullptr }; // bm_boss5_gryphon.bml, derived
    case 0x00A476E4: return { EntityRole::NormalMob, nullptr }; // bm_boss5_gryphon.bml, derived
    case 0x00A476E8: return { EntityRole::NormalMob, nullptr }; // bm_boss5_gryphon.bml, derived
    case 0x00A476F0: return { EntityRole::NormalMob, nullptr }; // bm_boss5_gryphon.bml, derived
    case 0x00A476F8: return { EntityRole::NormalMob, nullptr }; // bm_boss5_gryphon.bml, derived
    case 0x00A4770C: return { EntityRole::BossSubpart, nullptr }; // inherit:Gal Gryphon
    case 0x00A47728: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss5_common.bml, unitxt
    case 0x00A4772C: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss5_common.bml, unitxt
    case 0x00A47730: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss5_common.bml, unitxt
    case 0x00A47734: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss5_common.bml, unitxt
    case 0x00A47738: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss5_common.bml, unitxt
    case 0x00A4773C: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss5_common.bml, unitxt
    case 0x00A47748: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss5_common.bml, unitxt
    case 0x00A47750: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss5_common.bml, unitxt
    case 0x00A47754: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss5_common.bml, unitxt
    case 0x00A47768: return { EntityRole::NormalMob, &kSynthBarbaRayMinion }; // bm_boss7_crawfish.bml, ctor
    case 0x00A47AF8: return { EntityRole::SegmentBossBody, nullptr }; // bm_boss7_de_rol_le_c.bml, override
    case 0x00A47B0C: return { EntityRole::SegmentBossShell, nullptr }; // bm_boss7_de_rol_le_c.bml, override
    case 0x00A47B10: return { EntityRole::NormalMob, nullptr }; // bm_boss7_de_rol_le_c.bml, derived
    case 0x00A47B14: return { EntityRole::NormalMob, nullptr }; // bm_boss7_de_rol_le_c.bml, derived
    case 0x00A47B24: return { EntityRole::NormalMob, nullptr }; // bm_boss7_de_rol_le_c.bml, derived
    case 0x00A47B28: return { EntityRole::NormalMob, nullptr }; // bm_boss7_de_rol_le_c.bml, derived
    case 0x00A47F7C: return { EntityRole::NormalMob, nullptr }; // bm_boss7_de_rol_le_c.bml, derived
    case 0x00A47F80: return { EntityRole::NormalMob, nullptr }; // bm_boss7_de_rol_le_c.bml, derived
    case 0x00A48474: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss7_common.bml, unitxt
    case 0x00A48478: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss7_common.bml, unitxt
    case 0x00A48484: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss7_common.bml, unitxt
    case 0x00A4848C: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss7_common.bml, unitxt
    case 0x00A48490: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss7_common.bml, unitxt
    case 0x00A48660: return { EntityRole::NormalMob, nullptr }; // bm_boss8_dragon.bml, unitxt
    case 0x00A48680: return { EntityRole::NormalMob, nullptr }; // bm_boss8_dragon.bml, unitxt
    case 0x00A48684: return { EntityRole::NormalMob, nullptr }; // bm_boss8_dragon.bml, unitxt
    case 0x00A4868C: return { EntityRole::NormalMob, nullptr }; // bm_boss8_dragon.bml, unitxt
    case 0x00A48690: return { EntityRole::NormalMob, nullptr }; // bm_boss8_dragon.bml, unitxt
    case 0x00A48698: return { EntityRole::NormalMob, nullptr }; // bm_boss8_dragon.bml, unitxt
    case 0x00A486A0: return { EntityRole::NormalMob, nullptr }; // bm_boss8_dragon.bml, unitxt
    case 0x00A486B0: return { EntityRole::NormalMob, nullptr }; // bm_boss8_dragon.bml, unitxt
    case 0x00A486BC: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss1_common_j.bml, unitxt
    case 0x00A486D0: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_demoroom.bml, unitxt
    case 0x00A486D4: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_demoroom.bml, unitxt
    case 0x00A486E0: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_piller.bml, unitxt
    case 0x00A486F4: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_piller.bml, unitxt
    case 0x00A4871C: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_monitor.bml, unitxt
    case 0x00A48720: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_monitor.bml, unitxt
    case 0x00A48724: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_monitor.bml, unitxt
    case 0x00A48728: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_monitor.bml, unitxt
    case 0x00A4872C: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_monitor.bml, unitxt
    case 0x00A48778: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_monitor.bml, unitxt
    case 0x00A48A50: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_monitor.bml, unitxt
    case 0x00A48A78: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_monitor.bml, unitxt
    case 0x00A48A7C: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_monitor.bml, unitxt
    case 0x00A48B90: return { EntityRole::NormalMob, nullptr }; // bm_obj_boss8_monitor.bml, unitxt
    case 0x00A49514: return { EntityRole::BossSubpart, nullptr }; // inherit:Obj Boss8 Monitor
    case 0x00A49590: return { EntityRole::BossSubpart, nullptr }; // inherit:Obj Boss8 Monitor
    case 0x00A495C4: return { EntityRole::BossSubpart, nullptr }; // inherit:Obj Boss8 Monitor
    case 0x00A49818: return { EntityRole::BossSubpart, nullptr }; // inherit:Obj Boss8 Monitor
    case 0x00A4982C: return { EntityRole::BossSubpart, nullptr }; // inherit:Obj Boss8 Monitor
    case 0x00A49830: return { EntityRole::BossSubpart, nullptr }; // inherit:Obj Boss8 Monitor
    case 0x00A4984C: return { EntityRole::BossSubpart, nullptr }; // inherit:Obj Boss8 Monitor
    case 0x00A49868: return { EntityRole::BossSubpart, nullptr }; // inherit:Obj Boss8 Monitor
    case 0x00A4986C: return { EntityRole::BossSubpart, nullptr }; // inherit:Obj Boss8 Monitor
    case 0x00A49898: return { EntityRole::BossSubpart, nullptr }; // inherit:Obj Boss8 Monitor
    case 0x00A498A0: return { EntityRole::BossSubpart, nullptr }; // inherit:Obj Boss8 Monitor
    case 0x00A49914: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A49918: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A49928: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A49E40: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A49F80: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4A0C0: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4A200: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4A340: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4A480: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4A5C0: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4A700: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4A840: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4A980: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4AAC0: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4AC00: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4AFC0: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4B360: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4B780: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4BCA0: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4C080: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4C440: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4C860: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4CC00: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4CEC0: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4D220: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4D5C0: return { EntityRole::NormalMob, nullptr }; // re_b_mark_base.bml, unitxt
    case 0x00A4D858: return { EntityRole::NormalMob, nullptr }; // charamakingmot.bml, unitxt
    case 0x00A4D85C: return { EntityRole::NormalMob, nullptr }; // charamakingmot.bml, unitxt
    case 0x00A5009C: return { EntityRole::NormalMob, nullptr }; // plYhed00.nj, unitxt
    case 0x00A500A4: return { EntityRole::NormalMob, nullptr }; // plYhed00.nj, unitxt
    case 0x00A500B4: return { EntityRole::NormalMob, nullptr }; // plYhed00.nj, unitxt
    case 0x00A500BE: return { EntityRole::NormalMob, nullptr }; // plYhed00.nj, unitxt
    case 0x00A500D6: return { EntityRole::NormalMob, nullptr }; // plYhed00.nj, unitxt
    case 0x00A5DC52: return { EntityRole::BossSubpart, nullptr }; // inherit:Plyhed00
    case 0x00A600B9: return { EntityRole::BossSubpart, nullptr }; // inherit:Plyhed00
    case 0x00A700A6: return { EntityRole::BossSubpart, nullptr }; // inherit:Plyhed00
    case 0x00A72150: return { EntityRole::NormalMob, nullptr }; // plynj.bml, unitxt
    case 0x00A72154: return { EntityRole::NormalMob, nullptr }; // plynj.bml, unitxt
    case 0x00A72158: return { EntityRole::NormalMob, nullptr }; // plxnj.bml, unitxt
    case 0x00A7215C: return { EntityRole::NormalMob, nullptr }; // plwnj.bml, unitxt
    case 0x00A72160: return { EntityRole::NormalMob, nullptr }; // plvnj.bml, unitxt
    case 0x00A72164: return { EntityRole::NormalMob, nullptr }; // plunj.bml, unitxt
    case 0x00A72168: return { EntityRole::NormalMob, nullptr }; // pltnj.bml, unitxt
    case 0x00A7216C: return { EntityRole::NormalMob, nullptr }; // plsnj.bml, unitxt
    case 0x00A72170: return { EntityRole::NormalMob, nullptr }; // plrnj.bml, unitxt
    case 0x00A72174: return { EntityRole::NormalMob, nullptr }; // plqnj.bml, unitxt
    case 0x00A72178: return { EntityRole::NormalMob, nullptr }; // plpnj.bml, unitxt
    case 0x00A7217C: return { EntityRole::NormalMob, nullptr }; // plonj.bml, unitxt
    case 0x00A72180: return { EntityRole::NormalMob, nullptr }; // pllnj.bml, unitxt
    case 0x00A72184: return { EntityRole::NormalMob, nullptr }; // plknj.bml, unitxt
    case 0x00A72188: return { EntityRole::NormalMob, nullptr }; // pljnj.bml, unitxt
    case 0x00A7218C: return { EntityRole::NormalMob, nullptr }; // plinj.bml, unitxt
    case 0x00A72190: return { EntityRole::NormalMob, nullptr }; // plhnj.bml, unitxt
    case 0x00A72194: return { EntityRole::NormalMob, nullptr }; // plgnj.bml, unitxt
    case 0x00A72198: return { EntityRole::NormalMob, nullptr }; // plfnj.bml, unitxt
    case 0x00A7219C: return { EntityRole::NormalMob, nullptr }; // plenj.bml, unitxt
    case 0x00A721A0: return { EntityRole::NormalMob, nullptr }; // pldnj.bml, unitxt
    case 0x00A721A4: return { EntityRole::NormalMob, nullptr }; // plcnj.bml, unitxt
    case 0x00A721A8: return { EntityRole::NormalMob, nullptr }; // plbnj.bml, unitxt
    case 0x00A721AC: return { EntityRole::NormalMob, nullptr }; // planj.bml, unitxt
    case 0x00A721D8: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72240: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72260: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A722B0: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A722D0: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A722D4: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A722D8: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72300: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72308: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72310: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72318: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72334: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72510: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72520: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72578: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A7257C: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72580: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72584: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72588: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A7258C: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72620: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72664: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72668: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72680: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72684: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A726B4: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A726C4: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A726D8: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A726DC: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A726F4: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A726F8: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72708: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72770: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A7278C: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72790: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A727AC: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A727C8: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A727D0: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72804: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72818: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A7281C: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A7282C: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A72830: return { EntityRole::BossSubpart, nullptr }; // inherit:Planj
    case 0x00A7283C: return { EntityRole::NormalMob, nullptr }; // bm_eff_ice.bml, unitxt
    case 0x00A72858: return { EntityRole::NormalMob, nullptr }; // bm_eff_ice.bml, unitxt
    case 0x00A72868: return { EntityRole::NormalMob, nullptr }; // bm_eff_ice.bml, unitxt
    case 0x00A7286C: return { EntityRole::NormalMob, nullptr }; // bm_eff_ice.bml, unitxt
    case 0x00A7295C: return { EntityRole::NormalMob, nullptr }; // bm_eff_ice.bml, unitxt
    case 0x00A72960: return { EntityRole::NormalMob, nullptr }; // bm_eff_ice.bml, unitxt
    case 0x00A72964: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72AAC: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72AE8: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72AF4: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72B00: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72B04: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72B08: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72B64: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72B68: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72B94: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72BA0: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72BBC: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72BD0: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72BE0: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72BE4: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72BF4: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72BF8: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72C18: return { EntityRole::BossSubpart, nullptr }; // inherit:Eff Ice
    case 0x00A72CD4: return { EntityRole::NormalMob, nullptr }; // bm_tec_regurar.bml, unitxt
    case 0x00A72CD8: return { EntityRole::NormalMob, nullptr }; // bm_tec_regurar.bml, unitxt
    case 0x00A72CE8: return { EntityRole::NormalMob, nullptr }; // bm_tec_regurar.bml, unitxt
    case 0x00A72D1C: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm1_shark.bml, ctor
    case 0x00A72D50: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm2_moja_a.bml, ctor
    case 0x00A72D7C: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm2_moja_a.bml, ctor
    case 0x00A72DF0: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm3_fly.bml, model_hint
    case 0x00A72DF4: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm3_fly.bml, ctor
    case 0x00A72E68: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm5_wolf.bml, ctor
    case 0x00A72E80: return { EntityRole::NormalMob, nullptr }; // bm_ene_cgrass.bml, unitxt
    case 0x00A72EB8: return { EntityRole::NormalMob, nullptr }; // bm_ene_cgrass.bml, unitxt
    case 0x00A72EBC: return { EntityRole::NormalMob, nullptr }; // bm_ene_cgrass.bml, unitxt
    case 0x00A72ECC: return { EntityRole::NormalMob, nullptr }; // bm_ene_cgrass.bml, unitxt
    case 0x00A72F20: return { EntityRole::NormalMob, nullptr }; // bm_ene_grass.bml, model_hint
    case 0x00A72F24: return { EntityRole::NormalMob, nullptr }; // bm_ene_grass.bml, ctor
    case 0x00A72F98: return { EntityRole::NormalMob, nullptr }; // bm_ene_sandlappy.bml, ctor
    case 0x00A72FD4: return { EntityRole::NormalMob, nullptr }; // bm_ene_re2_flower.bml, ctor
    case 0x00A72FEC: return { EntityRole::NormalMob, nullptr }; // bm_ene_re2_flower.bml, model_hint
    case 0x00A73048: return { EntityRole::NormalMob, nullptr }; // bm_ene_re2_flower.bml, model_hint
    case 0x00A7304C: return { EntityRole::NormalMob, nullptr }; // bm_ene_re2_flower.bml, model_hint
    case 0x00A73050: return { EntityRole::NormalMob, nullptr }; // bm_ene_re2_flower.bml, model_hint
    case 0x00A73054: return { EntityRole::NormalMob, nullptr }; // bm_ene_re2_flower.bml, model_hint
    case 0x00A73058: return { EntityRole::NormalMob, nullptr }; // bm_ene_re2_flower.bml, model_hint
    case 0x00A73084: return { EntityRole::NormalMob, nullptr }; // bm_ene_re2_flower.bml, model_hint
    case 0x00A730AC: return { EntityRole::NormalMob, nullptr }; // bm_ene_re2_flower.bml, model_hint
    case 0x00A732C0: return { EntityRole::NormalMob, nullptr }; // bm_ene_re2_flower.bml, model_hint
    case 0x00A732E0: return { EntityRole::NormalMob, nullptr }; // bm_ene_re2_flower.bml, model_hint
    case 0x00A733B4: return { EntityRole::NormalMob, nullptr }; // bm_ene_ill_gill.bml, model_hint
    case 0x00A733B8: return { EntityRole::NormalMob, nullptr }; // bm_ene_ill_gill.bml, ctor
    case 0x00A73488: return { EntityRole::NormalMob, nullptr }; // bm_ene_balclaw.bml, model_hint
    case 0x00A734CC: return { EntityRole::NormalMob, nullptr }; // bm_ene_balclaw.bml, ctor
    case 0x00A734EC: return { EntityRole::NormalMob, nullptr }; // bm_ene_balclaw.bml, ctor
    case 0x00A73518: return { EntityRole::NormalMob, nullptr }; // bm_ene_re8_b_beast.bml, ctor
    case 0x00A735FC: return { EntityRole::NormalMob, nullptr }; // bm_ene_re7_berura.bml, ctor
    case 0x00A73600: return { EntityRole::NormalMob, nullptr }; // bm_ene_re7_berura.bml, ctor
    case 0x00A73650: return { EntityRole::NormalMob, nullptr }; // bm_ene_df2_bringer.bml, unitxt
    case 0x00A736D8: return { EntityRole::NormalMob, nullptr }; // bm_ene_df2_bringer.bml, unitxt
    case 0x00A736DC: return { EntityRole::NormalMob, nullptr }; // bm_ene_df2_bringer.bml, unitxt
    case 0x00A736E0: return { EntityRole::NormalMob, nullptr }; // bm_ene_df2_bringer.bml, unitxt
    case 0x00A736E4: return { EntityRole::NormalMob, nullptr }; // bm_ene_df2_bringer.bml, unitxt
    case 0x00A736E8: return { EntityRole::NormalMob, nullptr }; // bm_ene_df2_bringer.bml, unitxt
    case 0x00A73778: return { EntityRole::NormalMob, nullptr }; // bm_ene_me1_mb.bml, model_hint
    case 0x00A7377C: return { EntityRole::NormalMob, nullptr }; // bm_ene_me1_mb.bml, ctor
    case 0x00A7378C: return { EntityRole::NormalMob, nullptr }; // bm_ene_me1_mb.bml, model_hint
    case 0x00A73790: return { EntityRole::NormalMob, nullptr }; // bm_ene_me1_mb.bml, model_hint
    case 0x00A73794: return { EntityRole::NormalMob, nullptr }; // bm_ene_me1_mb.bml, model_hint
    case 0x00A73798: return { EntityRole::NormalMob, nullptr }; // bm_ene_me1_mb.bml, model_hint
    case 0x00A7379C: return { EntityRole::NormalMob, nullptr }; // bm_ene_me1_mb.bml, model_hint
    case 0x00A737A0: return { EntityRole::NormalMob, nullptr }; // bm_ene_me1_mb.bml, model_hint
    case 0x00A737B0: return { EntityRole::NormalMob, nullptr }; // bm_ene_me1_mb.bml, ctor
    case 0x00A7383C: return { EntityRole::NormalMob, nullptr }; // bm_ene_darkgunner.bml, ctor
    case 0x00A7384C: return { EntityRole::NormalMob, nullptr }; // bm_ene_darkgunner.bml, model_hint
    case 0x00A73858: return { EntityRole::NormalMob, nullptr }; // bm_ene_darkgunner.bml, model_hint
    case 0x00A73868: return { EntityRole::NormalMob, nullptr }; // bm_ene_darkgunner.bml, model_hint
    case 0x00A738D0: return { EntityRole::NormalMob, nullptr }; // bm_ene_del_depth_low.bml, model_hint
    case 0x00A738D4: return { EntityRole::NormalMob, nullptr }; // bm_ene_del_depth_low.bml, ctor
    case 0x00A738E8: return { EntityRole::NormalMob, nullptr }; // bm_ene_del_depth_low.bml, model_hint
    case 0x00A73934: return { EntityRole::NormalMob, nullptr }; // bm_ene_biter_body_low.bml, ctor
    case 0x00A73938: return { EntityRole::NormalMob, nullptr }; // bm_ene_biter_body_low.bml, model_hint
    case 0x00A7393C: return { EntityRole::NormalMob, nullptr }; // bm_ene_biter_body_low.bml, model_hint
    case 0x00A73940: return { EntityRole::NormalMob, nullptr }; // bm_ene_biter_body_low.bml, model_hint
    case 0x00A73944: return { EntityRole::NormalMob, nullptr }; // bm_ene_biter_body_low.bml, model_hint
    case 0x00A73948: return { EntityRole::NormalMob, nullptr }; // bm_ene_biter_body_low.bml, model_hint
    case 0x00A739A8: return { EntityRole::NormalMob, nullptr }; // bm_ene_df1_saver.bml, ctor
    case 0x00A739EC: return { EntityRole::NormalMob, nullptr }; // bm_ene_df3_dimedian.bml, ctor
    case 0x00A73A20: return { EntityRole::NormalMob, nullptr }; // bm_ene_abgc01_wola_body_low.bml, unitxt
    case 0x00A73A30: return { EntityRole::NormalMob, nullptr }; // bm_ene_abgc01_wola_body_low.bml, unitxt
    case 0x00A73B24: return { EntityRole::NormalMob, nullptr }; // bm_ene_dubchik.bml, ctor
    case 0x00A73B38: return { EntityRole::NormalMob, nullptr }; // bm_ene_dubchik.bml, model_hint
    case 0x00A73B48: return { EntityRole::NormalMob, nullptr }; // bm_ene_dubchik.bml, model_hint
    case 0x00A73B94: return { EntityRole::NormalMob, nullptr }; // bm_ene_epsilon.bml, model_hint
    case 0x00A73B98: return { EntityRole::NormalMob, nullptr }; // bm_ene_epsilon.bml, model_hint
    case 0x00A73BEC: return { EntityRole::NormalMob, nullptr }; // bm_ene_epsilon.bml, ctor
    case 0x00A73C20: return { EntityRole::NormalMob, nullptr }; // bm_ene_epsilon.bml, model_hint
    case 0x00A73C94: return { EntityRole::NormalMob, nullptr }; // bm_ene_me1_gee_low.bml, unitxt
    case 0x00A73C98: return { EntityRole::NormalMob, nullptr }; // bm_ene_me1_gee_low.bml, unitxt
    case 0x00A73CAC: return { EntityRole::NormalMob, nullptr }; // bm_ene_me1_gee_low.bml, unitxt
    case 0x00A73CFC: return { EntityRole::NormalMob, nullptr }; // bm_ene_gibbles_low.bml, ctor
    case 0x00A73D70: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm5_gibon_u_low.bml, ctor
    case 0x00A73DB4: return { EntityRole::NormalMob, nullptr }; // bm_ene_gi_gue_low.bml, ctor
    case 0x00A73DBC: return { EntityRole::NormalMob, nullptr }; // bm_ene_gi_gue_low.bml, model_hint
    case 0x00A73DD0: return { EntityRole::NormalMob, nullptr }; // bm_ene_gi_gue_low.bml, model_hint
    case 0x00A73DE8: return { EntityRole::NormalMob, nullptr }; // bm_ene_gi_gue_low.bml, model_hint
    case 0x00A73EF0: return { EntityRole::NormalMob, nullptr }; // bm_ene_gyaranzo.bml, ctor
    case 0x00A73F0C: return { EntityRole::NormalMob, nullptr }; // bm_ene_gyaranzo.bml, model_hint
    case 0x00A73F1C: return { EntityRole::NormalMob, nullptr }; // bm_ene_gyaranzo.bml, model_hint
    case 0x00A73F9C: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm9_s_mericarol_low.bml, ctor
    case 0x00A73FB0: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm9_s_mericarol_low.bml, model_hint
    case 0x00A7400C: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm9_s_mericarol_low.bml, model_hint
    case 0x00A74010: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm9_s_mericarol_low.bml, model_hint
    case 0x00A74014: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm9_s_mericarol_low.bml, model_hint
    case 0x00A74018: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm9_s_mericarol_low.bml, model_hint
    case 0x00A7401C: return { EntityRole::NormalMob, nullptr }; // bm_ene_bm9_s_mericarol_low.bml, model_hint
    case 0x00A74040: return { EntityRole::NormalMob, nullptr }; // bm_ene_re8_merill_lia_low.bml, ctor
    case 0x00A74050: return { EntityRole::NormalMob, nullptr }; // bm_ene_re8_merill_lia_low.bml, model_hint
    case 0x00A74054: return { EntityRole::NormalMob, nullptr }; // bm_ene_re8_merill_lia_low.bml, model_hint
    case 0x00A74058: return { EntityRole::NormalMob, nullptr }; // bm_ene_re8_merill_lia_low.bml, model_hint
    case 0x00A7405C: return { EntityRole::NormalMob, nullptr }; // bm_ene_re8_merill_lia_low.bml, model_hint
    case 0x00A74064: return { EntityRole::NormalMob, nullptr }; // bm_ene_re8_merill_lia_low.bml, model_hint
    case 0x00A74068: return { EntityRole::NormalMob, nullptr }; // bm_ene_re8_merill_lia_low.bml, model_hint
    case 0x00A7406C: return { EntityRole::NormalMob, nullptr }; // bm_ene_re8_merill_lia_low.bml, model_hint
    case 0x00A74070: return { EntityRole::NormalMob, nullptr }; // bm_ene_re8_merill_lia_low.bml, model_hint
    case 0x00A74080: return { EntityRole::NormalMob, nullptr }; // bm_ene_common_all.bml, unitxt
    case 0x00A74090: return { EntityRole::NormalMob, nullptr }; // bm_ene_common_all.bml, unitxt
    case 0x00A74094: return { EntityRole::NormalMob, nullptr }; // bm_ene_common_all.bml, unitxt
    case 0x00A740D8: return { EntityRole::NormalMob, nullptr }; // bm_ene_common_all.bml, unitxt
    case 0x00A74138: return { EntityRole::NormalMob, nullptr }; // bm_ene_morfos_low.bml, ctor
    case 0x00A74140: return { EntityRole::NormalMob, nullptr }; // bm_ene_morfos_low.bml, model_hint
    case 0x00A74154: return { EntityRole::NormalMob, nullptr }; // bm_ene_morfos_low.bml, model_hint
    case 0x00A74198: return { EntityRole::NormalMob, nullptr }; // bm_ene_nanodrago.bml, ctor
    case 0x00A741AC: return { EntityRole::NormalMob, nullptr }; // bm_ene_nanodrago.bml, model_hint
    case 0x00A741C0: return { EntityRole::NormalMob, nullptr }; // bm_ene_nanodrago.bml, model_hint
    case 0x00A741C8: return { EntityRole::NormalMob, nullptr }; // bm_ene_npc_chao.bml, unitxt
    case 0x00A741D0: return { EntityRole::NormalMob, nullptr }; // bm_ene_npc_nights.bml, unitxt
    case 0x00A742A8: return { EntityRole::NormalMob, nullptr }; // bm7_s_paa_body.bml, ctor
    case 0x00A74370: return { EntityRole::NormalMob, nullptr }; // bm7_s_paa_body.bml, model_hint
    case 0x00A7439C: return { EntityRole::NormalMob, nullptr }; // bm7_s_paa_body.bml, model_hint
    case 0x00A743CC: return { EntityRole::NormalMob, nullptr }; // bm_ene_recobox_low.bml, ctor
    case 0x00A74400: return { EntityRole::NormalMob, nullptr }; // bm_ene_recobox_low.bml, model_hint
    case 0x00A7441C: return { EntityRole::NormalMob, nullptr }; // bm_ene_recobox_low.bml, model_hint
    case 0x00A744A4: return { EntityRole::NormalMob, nullptr }; // bm_ene_me3_shinowa.bml, unitxt
    case 0x00A744A8: return { EntityRole::NormalMob, nullptr }; // bm_ene_me3_shinowa.bml, unitxt
    case 0x00A744AC: return { EntityRole::NormalMob, nullptr }; // bm_ene_me3_shinowa.bml, unitxt
    case 0x00A74520: return { EntityRole::NormalMob, nullptr }; // bm_ene_me3_zoa_low.bml, ctor
    case 0x00A74524: return { EntityRole::NormalMob, nullptr }; // bm_ene_me3_zoa_low.bml, ctor
    case 0x00A74658: return { EntityRole::NormalMob, nullptr }; // bm4_ps_ma_body.bml, unitxt
    case 0x00A7465C: return { EntityRole::NormalMob, nullptr }; // bm4_ps_ma_body.bml, unitxt
    case 0x00A74660: return { EntityRole::NormalMob, nullptr }; // bm4_ps_ma_body.bml, unitxt
    case 0x00A74664: return { EntityRole::NormalMob, nullptr }; // bm4_ps_ma_body.bml, unitxt
    case 0x00A74678: return { EntityRole::NormalMob, nullptr }; // bm4_ps_ma_body.bml, unitxt
    case 0x00A746B0: return { EntityRole::NormalMob, nullptr }; // bm4_ps_ma_body.bml, unitxt
    case 0x00A74710: return { EntityRole::NormalMob, nullptr }; // bm_ene_re4_sorcerer.bml, unitxt
    case 0x00A7478C: return { EntityRole::NormalMob, nullptr }; // bm_ene_me3_beril_low.bml, model_hint
    case 0x00A74790: return { EntityRole::NormalMob, nullptr }; // bm_ene_me3_beril_low.bml, ctor
    case 0x00A747C8: return { EntityRole::NormalMob, nullptr }; // bm_ene_astark.bml, ctor
    case 0x00A747F4: return { EntityRole::NormalMob, nullptr }; // bm_ene_boota.bml, ctor
    case 0x00A74828: return { EntityRole::NormalMob, nullptr }; // bm_ene_dolphon.bml, ctor
    case 0x00A74890: return { EntityRole::NormalMob, nullptr }; // bm_ene_girtablulu.bml, unitxt
    case 0x00A7490C: return { EntityRole::NormalMob, nullptr }; // bm_ene_golan.bml, ctor
    case 0x00A74988: return { EntityRole::NormalMob, nullptr }; // bm_ene_yowie.bml, unitxt
    case 0x00A749EC: return { EntityRole::NormalMob, nullptr }; // bm_ene_zu.bml, model_hint
    case 0x00A74A14: return { EntityRole::NormalMob, nullptr }; // wait_bm4_ps_mb_body.njm, ctor
    case 0x00A74A18: return { EntityRole::NormalMob, nullptr }; // bm_ene_melissa.bml, unitxt
    case 0x00A74A68: return { EntityRole::NormalMob, nullptr }; // bm_ene_melissa.bml, unitxt
    case 0x00A74A84: return { EntityRole::NormalMob, nullptr }; // bm_ene_melissa.bml, unitxt
    case 0x00A74B78: return { EntityRole::NormalMob, nullptr }; // _zakoa.bml, unitxt
    case 0x00A75380: return { EntityRole::NormalMob, nullptr }; // _zakoa.bml, unitxt
