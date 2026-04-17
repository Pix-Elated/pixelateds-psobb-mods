// Pixelated's PSOBB Mods — ReShade add-on for Ephinea PSO Blue Burst.
//
// Features:
//   - Live monster HP overlay (name / current / max / bar)
//   - Filtered floor items list (weapons, armor, notable tools)
//   - Controller chord remapper: LT/RT + face button -> palette slots 1-0
//
// Memory is READ only, via VirtualQuery-guarded pointer chains, using
// offsets shared with the Ephinea-sanctioned solylib Lua addons and
// Jake's Drop Checker.

#include <Windows.h>
#include <mmsystem.h>   // PlaySoundA — low-HP alert WAV playback

#include <imgui.h>

// reshade.hpp forwards every addon event through a GetProcAddress call
// and reinterpret_casts the resulting FARPROC to the concrete callback
// signature. MSVC flags that as C4191 ("unsafe conversion from FARPROC
// to ...") under /W4. The cast is intentional and inherent to the
// ReShade plugin model; there is no safer form. Silence C4191 only
// across this single vendored header include so our own code still
// gets the full warning set.
#pragma warning(push)
#pragma warning(disable : 4191)
#include <reshade.hpp>
#pragma warning(pop)

#include "controller_chords.hpp"
#include "pso_addresses.hpp"
#include "pso_filters.hpp"
#include "pso_log.hpp"
#include "pso_sigscan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Monster;
struct FloorItem;

static void LoadConfig();
static void SaveConfig();
static void LoadCuratedIdLists();
static void AppendUserFilter(uint32_t id24, const char *name);
static void RemoveUserFilter(uint32_t id24);
static uintptr_t GetLocalPlayerPtr();
static void WheelFilterTryInstall();
static void WheelFilterUninstall();

// ============================================================================
// PSO memory layout (taken from solylib + Drop Checker; verbatim for parity)
// ============================================================================

namespace pso_offsets {
    // Static globals in PsoBB.exe — PlayerArray, PlayerIndex,
    // PlayerCount, EntityCount, Difficulty, UnitxtPointer,
    // EntityArrayRef, FloorItemsArrayPtr, FloorItemsCountPtr,
    // EpisodeByte — are now declared in pso_addresses.hpp and
    // resolved at DllMain attach (currently: rebase-delta against
    // the PE ImageBase; future: instruction-pattern scan in .text).
    // All call sites still write `pso_offsets::Name`; the storage
    // has just moved out of this header-only namespace into
    // pso_addresses.cpp so it can be rewritten at runtime.
    //
    // The remaining entries in this namespace are STRUCT FIELD
    // offsets, not absolute addresses. They describe the layout of
    // the monster/player/item structs the game uses internally;
    // those layouts haven't changed in 15+ years of Blue Burst
    // history and would not be affected by any plausible Ephinea
    // rebuild.

    constexpr uintptr_t MonsterEntityFlags = 0x30;   // u32, bit 0x0800 = dead
    constexpr uintptr_t MonsterUnitxtID    = 0x378;
    constexpr uintptr_t MonsterHP          = 0x334;
    constexpr uintptr_t MonsterHPMax       = 0x2BC;
    // (MonsterBpPtr/MonsterBpHp removed — they were only used for the
    // unreliable Monitor/Panel HP comparison, which fails across
    // difficulties because HP scales. Identity decisions use cls only.)
    constexpr uintptr_t EntityTypeID       = 0x04;   // u32 class type pointer
    constexpr uintptr_t EntityPrevSibling  = 0x0C;   // base_game_object *
    constexpr uintptr_t EntityNextSibling  = 0x10;   // base_game_object *
    constexpr uintptr_t EntityParentObject = 0x14;   // base_game_object *  (verified: shell parent → body)
    constexpr uintptr_t EntityChildObject  = 0x18;   // base_game_object *
    constexpr uintptr_t EntityActiveFlag   = 0x1E;   // u16
    constexpr uintptr_t EntityDormant      = 0xB0;   // u16 Pan Arms state machine

    // These three are extern and rebase-resolved at startup via
    // pso_addresses::InitializeAndLog(), same as PlayerArray et al.
    extern uintptr_t BossDataPtr;
    extern uintptr_t EphineaMonsterArray;
    extern uintptr_t EphineaHPScale;

    // De Rol Le HP offsets (on entity struct, NOT boss pointer)
    constexpr uintptr_t DRL_BodyMaxHP      = 0x6B0;  // i32
    constexpr uintptr_t DRL_BodyHP         = 0x6B4;  // i32
    constexpr uintptr_t DRL_SkullHP        = 0x6B8;  // i32
    constexpr uintptr_t SegmentShellHP     = 0x39C;  // i32 per-segment shell HP

    // De Rol Le max HP from dereferenced BossDataPtr
    constexpr uintptr_t DRL_SkullMaxHP_Ptr = 0x20;   // i32 at *(BossDataPtr) + 0x20
    constexpr uintptr_t DRL_ShellMaxHP_Ptr = 0x1C;   // i32 at *(BossDataPtr) + 0x1C

    // Barba Ray / Dal Ra Lie HP offsets (on entity struct)
    constexpr uintptr_t BR_BodyMaxHP       = 0x700;  // i32
    constexpr uintptr_t BR_BodyHP          = 0x704;  // i32
    constexpr uintptr_t BR_SkullHP         = 0x708;  // i32
    constexpr uintptr_t BR_ShellHP         = 0x7AC;  // i32 per-segment shell HP

    // Barba Ray animations BP pointer (NOT the static BossDataPtr)
    constexpr uintptr_t BR_AnimBPPtr       = 0x628;  // on entity struct
    constexpr uintptr_t BR_SkullMaxHP_Ptr  = 0x20;   // i32 at *(animBPPtr) + 0x20
    constexpr uintptr_t BR_ShellMaxHP_Ptr  = 0x1C;   // i32 at *(animBPPtr) + 0x1C

    // Barba Ray class type IDs (entity+0x04)
    constexpr uint32_t  BR_BodyTypeID      = 0xA47AF8;
    constexpr uint32_t  BR_ShellTypeID     = 0xA47B0C;

    // Monster unitxt IDs for multi-form entities.
    constexpr uint32_t  UidPanArmsCombined = 21;
    constexpr uint32_t  UidMigium          = 22;
    constexpr uint32_t  UidHidoom          = 23;
    constexpr uint32_t  UidDeRolLe         = 45;
    constexpr uint32_t  UidBarbaRay        = 73;

    // ---- Class metadata pointers (entity+0x04) ----
    // Immutable per entity type, locale-independent, the engine's
    // own type identity. Use these for ALL entity identification.

    // Segment-boss body cls values — kept as named constants because
    // several call sites (IsSegmentBoss, dispatchers) need them by
    // symbolic name. Everything else lives in entity_cls_table.h,
    // which is the single source of truth for cls → role / display.
    constexpr uintptr_t ClsDeRolLeBody     = 0x00A43D2C;
    constexpr uintptr_t ClsBarbaRayBody    = 0x00A47AF8;

    // Room membership. Each entity carries a u16 room ID at +0x28; the
    // local player also has a second room slot at +0x2E for transitions.
    // Monster Reader filters by "monster is in one of the player's two
    // rooms" to hide entities the player can't see — this also happens
    // to catch the Olga Flow dead-body-after-cutscene problem where the
    // corpse persists at full HP in its original room while the player
    // gets teleported elsewhere.
    constexpr uintptr_t EntityRoom         = 0x28;
    constexpr uintptr_t PlayerRoom2        = 0x2E;

    // Per-entity unique ID. i16 at +0x1C. Used to match against the
    // local player's current-target ID (see below). Not an array
    // index — IDs are assigned by the server, so a scan + match is
    // the only way to resolve "which entity am I targeting."
    constexpr uintptr_t EntityId           = 0x1C;

    // Player world position (XZ plane). +0x38 = X float, +0x3C = Y
    // float (unused — PSO movement is planar), +0x40 = Z float.
    // Documented in solylib's characters.lua.
    constexpr uintptr_t PlayerPosX         = 0x38;
    constexpr uintptr_t PlayerPosZ         = 0x40;

    // Player / entity facing direction. u16 at +0x60, empirically
    // verified via an in-client diagnostic dump — NOT documented in
    // solylib, which never needed yaw. Full revolution is 0x10000,
    // value DECREASES when the player rotates right (clockwise seen
    // on screen). +0x62 is a separate unrelated field and is NOT
    // read as part of the rotation value. Used by the PlayerRelative
    // direction-arrow mode only.
    constexpr uintptr_t PlayerRotationY    = 0x60;

    // Player current floor / area index. u32. Solylib's same offset.
    constexpr uintptr_t PlayerCurrentFloor = 0x3F0;

    // Player inventory list head. `player + 0xDF4` points at an
    // inventory struct; its +0x1C4 is the list address; that list's
    // +0x18 is the first node; each node's +0x10 is the next node
    // and +0x1C is the item pointer. Item objects have a type byte
    // at +0xF2 (0x02 = mag), an equipped flag byte at +0x190
    // (bit 0 = equipped), and the mag sync-timer f32 at +0x1B4.
    constexpr uintptr_t PlayerInventory    = 0xDF4;
    constexpr uintptr_t InventoryListPtr   = 0x1C4;
    constexpr uintptr_t InventoryFirstNode = 0x18;
    constexpr uintptr_t InventoryNextNode  = 0x10;
    constexpr uintptr_t InventoryNodeItem  = 0x1C;
    constexpr uintptr_t ItemTypeByte       = 0xF2;
    constexpr uintptr_t ItemEquippedFlag   = 0x190;
    constexpr uintptr_t MagSyncTimer       = 0x1B4;

    // Player level (stored as level-1, add 1 for display) and total
    // cumulative EXP. From solylib (level) and an empirical scan
    // (exp — solylib doesn't document it, newserv's serialized
    // struct implies it, and an in-game menu comparison confirmed
    // the layout is level at 0xE44 immediately followed by total
    // EXP at 0xE48).
    constexpr uintptr_t PlayerLevel        = 0xE44;
    constexpr uintptr_t PlayerExp          = 0xE48;

    // Current-target lookup. The local player's target isn't stored
    // directly on the player struct; it lives on a "sibling" struct
    // pointed to by `player + 0x18`, at offset 0x108C inside the
    // sibling, as an i16 target entity ID (-1 = no target). From
    // Solybum's Monster Reader addon (lines 208-209 / 447-488 of
    // Monster Reader/init.lua), which is the canonical source of
    // this offset (solylib itself never exposed it).
    //
    // The sibling pointer can be briefly null on map transitions,
    // so the read has to be guarded — never dereference without a
    // null check.
    constexpr uintptr_t PlayerTargetSibling = 0x18;
    constexpr uintptr_t TargetIdInSibling   = 0x108C;

    // Floor item enumeration. Verified against Solybum's solylib
    // (`solylib/items/items.lua`, _MultiFloorItem* constants).
    //
    // PSO maintains TWO parallel static arrays, one for floor item
    // pointers and one for floor item counts:
    //
    //   FloorItemsArrayPtr  -> u32* -> array[18] of floor-item-array ptrs
    //   FloorItemsCountPtr  -> u32* -> array[18] of u32 counts
    //
    // Each floor's item array is a contiguous run of 0x24-byte records:
    //
    //   itemAddr = thisFloorsItems + j * 0x24
    //
    // Inside one record:
    //
    //   +0x10..+0x1B  12 bytes of item data (the canonical "drop checker"
    //                 12-byte item code; first 3 bytes are the type ID)
    //   +0x1C..+0x1F  u32 unique instance ID (per-drop handle, NOT type)
    //   +0x20..+0x23  4 more data bytes (stack count, mag stats, etc.)
    //
    // The earlier comment in this file claimed `*0xA8D8A4 + 16` was the
    // single contiguous floor table. That was wrong on every dimension
    // and explains why GetFloorItems was reading garbage and finding
    // raw_slots=0 even when items were on the ground.
    //
    // FloorItemsArrayPtr / FloorItemsCountPtr are declared in
    // pso_addresses.hpp — they're absolute addresses in .data/.bss
    // that get rebased at startup.
    constexpr uint32_t  FloorItemRecordSize = 0x24;
    constexpr uint32_t  FloorItemPosXOffset = 0x04;  // f32 world X
    constexpr uint32_t  FloorItemPosZOffset = 0x08;  // f32 world Z (no Y)
    constexpr uint32_t  FloorItemDataOffset = 0x10;  // 12-byte item data
    constexpr uint32_t  FloorItemIdOffset   = 0x1C;  // u32 instance handle
    constexpr uint32_t  AreaCount           = 18;
    constexpr uint32_t  MaxItemsPerFloorSanityCap = 256;
}


// ============================================================================
// Entity type system — cls-based identification
// ============================================================================
// Every decision about how to display, hide, rename, or collapse an entity
// is driven by its class metadata pointer at entity+0x04, NOT unitxt_id.
//
// Each known cls maps to an EntityType that tells the rendering code what
// to do. Unknown cls values fall through to "normal mob" behavior.

enum class EntityRole {
    NormalMob,          // individual row, no special handling
    SegmentBossBody,    // De Rol Le / Barba Ray: use boss HP pointer
    SegmentBossShell,   // shell segments: aggregate into (×N) row
    BossSubpart,        // boss sub-entity sharing HP pool: hide
    BossProjectile,     // mines, trackers: hide from HP bar
    CollapseByName,     // aggregate into (×N) row by name + max_hp (Vol Opt parts)
    Hidden,             // surgical cls-keyed hide (e.g. Dark Falz obelisk)
};

// Localized string pair. `en` is required, `jp` is optional (fall back
// to `en` when nullptr).
struct LocalizedString {
    const char *en;
    const char *jp;
};

enum class Locale { English = 0, Japanese = 1 };

struct EntityType {
    EntityRole role;
    // nullptr = resolve via the game's unitxt table. Non-null only for
    // entities the game itself doesn't name (Vol Opt sub-parts, Olga
    // Flow hitboxes, etc.) — those carry a LocalizedString so they
    // still follow the user's EN/JP locale.
    const LocalizedString *synth_name;
};

static EntityType LookupEntityType(uintptr_t cls)
{
    // entity_cls_table.h is the single source of truth — a static table
    // of every cls value in PsoBB.exe's enemy+object .rdata ranges with
    // its role (NormalMob / BossSubpart / CollapseByName / ...) and
    // display name. Extracted once from a Ghidra analysis of the frozen
    // 2004 binary via tools/extract_entity_table.py. Edit that script
    // and re-run only if the extraction heuristics need updating; the
    // output file itself can also be hand-tweaked safely.
    // Synthetic names for entities the game itself can't name via unitxt.
    // Every other entry in entity_cls_table.h carries nullptr so name
    // resolution flows through the game's own localized unitxt table.
    static const LocalizedString kSynthDragonSubpart     = { "Dragon Subpart",     "ドラゴンパーツ" };
    static const LocalizedString kSynthVolOptChandelier  = { "Vol Opt Chandelier", "ヴォル・オプト シャンデリア" };
    static const LocalizedString kSynthVolOptMonitor     = { "Vol Opt Monitor",    "ヴォル・オプト モニター" };
    static const LocalizedString kSynthVolOptPanel       = { "Vol Opt Panel",      "ヴォル・オプト パネル" };
    static const LocalizedString kSynthVolOptPillar      = { "Vol Opt Pillar",     "ヴォル・オプト ピラー" };
    static const LocalizedString kSynthBarbaRayMinion    = { "Barba Ray Minion",   "バルバ・レイの手下" };
    static const LocalizedString kSynthOlgaFlowHitbox    = { "Olga Flow Hitbox",   "オルガ・フロウ ヒットボックス" };
    static const LocalizedString kSynthOlgaFlowBall      = { "Olga Flow Ball",     "オルガ・フロウ ボール" };
    static const LocalizedString kSynthDarkFalzDarvant   = { "Dark Falz Darvant",  "ダーク・ファルス ダーヴァント" };
    static const LocalizedString kSynthDarkFalzPhase2    = { "Dark Falz (Phase 2)","ダーク・ファルス (第二形態)" };
    static const LocalizedString kSynthDarkFalzPhase3    = { "Dark Falz (Final Form)","ダーク・ファルス (最終形態)" };
    static const LocalizedString kSynthDubswitch         = { "Dubswitch",          "ドブスイッチ" };

    switch (cls)
    {
    // Manual overrides for object-range cls values (0xA8xxxx+) not
    // in the auto-generated entity_cls_table.h. These must come
    // BEFORE the #include so the case matches first.
    case 0x00A8EB8C:
        return { EntityRole::CollapseByName, &kSynthDubswitch };

#include "entity_cls_table.h"
    default:
        return { EntityRole::NormalMob, nullptr };
    }
}

// " Controller" suffix appended to parent-name when we label a spawner
// entity like "Poison Lily Controller" / "ポイズンリリー コントローラー".
static const LocalizedString kSynthControllerSuffix = { " Controller", " コントローラー" };

// ============================================================================
// Safe memory reader
// ============================================================================

static bool IsValidReadPtr(const void *ptr, size_t size)
{
    if (ptr == nullptr) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

    constexpr DWORD kReadable =
        PAGE_READONLY | PAGE_READWRITE |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
        PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & kReadable) == 0) return false;

    const uint8_t *region_end =
        static_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
    const uint8_t *requested_end = static_cast<const uint8_t *>(ptr) + size;
    return requested_end <= region_end;
}

// SEH-guarded memcpy. IsValidReadPtr alone is not race-safe: another
// thread (the game) can free the queried region between the VirtualQuery
// check and the dereference. Entity dumps during boss fights hit this
// when subparts transition — SafeRead crashed with 0xC0000005 even
// though VirtualQuery had just reported the page as MEM_COMMIT.
//
// This helper MUST NOT contain any C++ objects with destructors,
// otherwise MSVC refuses to compile __try inside it. Inline the tiny
// body here and keep it a plain C-style function.
static bool TryMemcpy(void *dst, const void *src, size_t n)
{
    __try
    {
        std::memcpy(dst, src, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template <typename T>
static T SafeRead(uintptr_t addr, T fallback = T{})
{
    if (!IsValidReadPtr(reinterpret_cast<const void *>(addr), sizeof(T)))
        return fallback;
    T val{};
    if (!TryMemcpy(&val, reinterpret_cast<const void *>(addr), sizeof(T)))
        return fallback;
    return val;
}

// Bulk byte read. One range check, then a memcpy. Use this when
// reading a contiguous struct-like block (a floor item record, a
// monster's field group, etc.) so the scan path doesn't pay a
// VirtualQuery syscall for every byte. Zero-fills `out` on failure
// so the caller can proceed with a known-empty buffer.
static bool SafeReadBytes(uintptr_t addr, void *out, size_t size)
{
    if (!IsValidReadPtr(reinterpret_cast<const void *>(addr), size))
    {
        std::memset(out, 0, size);
        return false;
    }
    if (!TryMemcpy(out, reinterpret_cast<const void *>(addr), size))
    {
        std::memset(out, 0, size);
        return false;
    }
    return true;
}

static std::string SafeReadWideStringUtf8(uintptr_t addr, size_t max_chars = 64)
{
    if (addr == 0) return {};
    if (!IsValidReadPtr(reinterpret_cast<const void *>(addr),
                        max_chars * sizeof(wchar_t)))
        return {};

    const wchar_t *p = reinterpret_cast<const wchar_t *>(addr);
    size_t len = 0;
    while (len < max_chars && p[len] != L'\0') ++len;
    if (len == 0) return {};

    const int utf8_size = WideCharToMultiByte(
        CP_UTF8, 0, p, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 0) return {};

    std::string out(static_cast<size_t>(utf8_size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, p, static_cast<int>(len), out.data(), utf8_size, nullptr, nullptr);
    return out;
}

// ============================================================================
// Unitxt name lookup: base -> group -> index -> wide string
//   group 2 = monster name (Normal/Hard/Very Hard)
//   group 4 = monster name (Ultimate)
// ============================================================================

static std::string UnitxtRead(uint32_t group, uint32_t index)
{
    const uintptr_t base_addr = SafeRead<uintptr_t>(pso_offsets::UnitxtPointer);
    if (base_addr == 0) return {};
    const uintptr_t group_addr = SafeRead<uintptr_t>(base_addr + group * 4);
    if (group_addr == 0) return {};
    const uintptr_t str_addr = SafeRead<uintptr_t>(group_addr + index * 4);
    if (str_addr == 0) return {};
    return SafeReadWideStringUtf8(str_addr);
}

static std::string GetMonsterName(uint32_t unitxt_id, bool ultimate)
{
    return UnitxtRead(ultimate ? 4 : 2, unitxt_id);
}

// ============================================================================
// Locale detection + synthetic-name localization
// ============================================================================
//
// The game's own unitxt table returns strings in the player's locale
// directly, so any entity the game itself can name needs no help from us.
//
// A handful of entities are OUR synthetics — sub-parts the game lumps
// under one unitxt entry (Vol Opt Chandelier/Monitor/Panel/Spire), Olga
// Flow hitboxes, Dubswitch, etc. Those need their own EN/JP strings so
// they render correctly regardless of the user's locale.
//
// We detect locale by sniffing what the game's own unitxt table returns
// for a known uid. ASCII-only result → Latin-script locale (treat as
// English); high-byte result → CJK locale (treat as Japanese). This
// avoids hunting for an ambiguous raw flag address and piggy-backs on
// the unitxt pipeline we already know works.

static Locale DetectGameLocale()
{
    static Locale s_cached = Locale::English;
    static bool   s_probed = false;
    if (s_probed) return s_cached;

    // "Booma" (uid 2) is one of the earliest, most reliable names in
    // the unitxt table — guaranteed present on every locale/difficulty.
    // Fall back to a couple of nearby uids if the read comes back empty
    // (unitxt may not be populated yet during the first frame or two).
    std::string sample;
    for (uint32_t uid : {2u, 1u, 3u, 10u})
    {
        sample = UnitxtRead(2, uid);
        if (!sample.empty()) break;
    }
    if (sample.empty()) return s_cached;  // not ready, try again next call

    s_probed = true;
    s_cached = (static_cast<uint8_t>(sample[0]) >= 0x80)
        ? Locale::Japanese
        : Locale::English;
    return s_cached;
}

static const char *Localize(const LocalizedString &s)
{
    const Locale loc = DetectGameLocale();
    if (loc == Locale::Japanese && s.jp != nullptr) return s.jp;
    return s.en;  // default (also the fallback when jp is null)
}


// Apply Ultimate-difficulty name swaps for the Vol Opt sub-part
// synthetic-name path. Phase 1 sub-parts (Chandelier/Monitor/Panel/
// Spire) are OUR labels rather than unitxt entries, so the difficulty-
// based name swap the unitxt path gets for free doesn't apply.
// Match exact strings to avoid double-rewriting.
static std::string ApplyUltimateNameSwap(std::string name, bool ultimate)
{
    if (!ultimate) return name;
    if (name == "Vol Opt Chandelier") return "Vol Opt ver.2 Chandelier";
    if (name == "Vol Opt Monitor")    return "Vol Opt ver.2 Monitor";
    if (name == "Vol Opt Panel")      return "Vol Opt ver.2 Panel";
    if (name == "Vol Opt Pillar")     return "Vol Opt ver.2 Pillar";
    return name;
}

// ============================================================================
// Monster list
// ============================================================================

struct Monster
{
    uintptr_t   address;
    uint32_t    unitxt_id;
    int32_t     hp;
    int32_t     max_hp;
    uint32_t    entity_flags;  // u32, bit 0x0800 = dead
    uint16_t    room;
    uint16_t    active_flag;  // entity+0x1E: 0=dormant, 1=active (Pan Arms split state)
    int32_t     shell_hp;    // entity+0x39C: per-segment shell HP (De Rol Le / Barba Ray)
    int16_t     entity_id;   // entity+0x1C, for Ephinea array lookup
    float       world_x;    // entity+0x38: raw world X (for mob↔mob distance)
    float       world_z;    // entity+0x40: raw world Z
    float       dist_xz;    // 2D distance to local player
    float       world_dx;   // normalized direction player→monster (X)
    float       world_dz;   // normalized direction player→monster (Z)
    bool        is_targeted;  // local player's current target
    uintptr_t   cls_meta;     // entity+0x04: class metadata pointer (immutable per type)
    uintptr_t   boss_root;    // address of the segment boss body this entity belongs to, or 0
    std::string name;
};

// Boss HP from the global boss data pointer. De Rol Le (uid 45) and
// Barba Ray / Dal Ra Lie (uid 44 in Ultimate, but we match by name)
// store their actual HP at dedicated offsets from a global pointer
// rather than in the per-entity MonsterHP field. This function reads
// the real body + skull HP for display, replacing the dummy values.
//
// Returns true if boss HP was successfully read, in which case the
// out parameters are filled. Returns false if the pointer is null
// or the boss isn't one we handle.
struct BossHP
{
    int32_t  body_hp;
    int32_t  body_max_hp;
    int32_t  skull_hp;
    int32_t  skull_max_hp;   // from boss data pointer +0x20
    int32_t  shell_max_hp;   // from boss data pointer +0x1C (per piece)
    bool     valid;
};

// De Rol Le / Barba Ray HP is stored on the ENTITY struct itself at
// offsets 0x6B0 (body max), 0x6B4 (body current), 0x6B8 (skull).
// NOT on the dereferenced boss data pointer — that's a stat template.
// The standard MonsterHP at 0x334 reads a dummy value (20000) for
// these bosses; the real HP lives further into the struct.
//
// Shell HP is on the dereferenced pointer at +0x39C (De Rol Le) or
// +0x7AC (Barba Ray), with max from pointer+0x1C.

// Segment bosses: multi-segment entities whose real HP lives at
// boss-specific offsets, not the standard MonsterHP field.
static bool IsSegmentBossByCls(uintptr_t cls)
{
    return cls == pso_offsets::ClsDeRolLeBody ||
           cls == pso_offsets::ClsBarbaRayBody;
}

// Walk the parent_object chain (enemy+0x14) upward looking for a segment
// boss body (De Rol Le / Barba Ray). If found, return the body's address.
// If the entity itself has a segment-boss cls, return its own address.
// Otherwise return 0.
//
// The game's enemy struct links segments to their body via parent_object.
// This is verified against live dumps: shell entity +0x14 = body address.
// Walking this chain is locale-independent and uses the engine's own
// data structures — no uid / cls enumeration needed.
//
// Safety: we only walk if the entity's OWN cls is in the same "neighborhood"
// as a segment boss (De Rol Le / Barba Ray range). This prevents false
// positives where an unrelated entity's parent_object happens to lead
// somewhere whose +0x04 reads as a value in the segment boss cls range
// (e.g. through a stale pointer).
static uintptr_t FindSegmentBossBody(uintptr_t entity_addr)
{
    const uintptr_t self_cls = SafeRead<uintptr_t>(entity_addr + pso_offsets::EntityTypeID);

    // Quick gate: only entities whose own cls is in the De Rol Le or
    // Barba Ray cluster ranges are candidates for segment walking.
    // The De Rol Le cluster is 0xA43D00..0xA44300; Barba Ray cluster
    // is 0xA47A00..0xA47C00. Anything outside these ranges is not a
    // segment boss part regardless of what its parent_object points at.
    const bool in_derolle_range  = (0xA43D00 <= self_cls && self_cls < 0xA44300);
    const bool in_barbaray_range = (0xA47A00 <= self_cls && self_cls < 0xA47C00);
    if (!in_derolle_range && !in_barbaray_range)
        return 0;

    uintptr_t cur = entity_addr;
    for (int depth = 0; depth < 8 && cur != 0; ++depth)
    {
        const uintptr_t cls = SafeRead<uintptr_t>(cur + pso_offsets::EntityTypeID);
        if (IsSegmentBossByCls(cls))
            return cur;
        const uintptr_t parent = SafeRead<uintptr_t>(cur + pso_offsets::EntityParentObject);
        if (parent == cur || parent == 0) break;  // self-loop or end of chain
        cur = parent;
    }
    return 0;
}

// Read Ephinea HP scaling factor (f64 at 0x00B5F804).
// Returns 1.0 if Ephinea monster array is not active.
static double GetEphineaHPScale()
{
    const uintptr_t eph = SafeRead<uintptr_t>(pso_offsets::EphineaMonsterArray);
    if (eph == 0) return 1.0;
    return SafeRead<double>(pso_offsets::EphineaHPScale);
}

static void ApplyEphineaScale(int32_t &skull_max, int32_t &shell_max)
{
    const double scale = GetEphineaHPScale();
    if (scale != 1.0)
    {
        skull_max = static_cast<int32_t>(skull_max * scale);
        shell_max = static_cast<int32_t>(shell_max * scale);
    }
}

static BossHP ReadDeRolLeHP(uintptr_t entity_addr)
{
    BossHP out{};
    if (entity_addr == 0) return out;
    out.body_max_hp = SafeRead<int32_t>(entity_addr + pso_offsets::DRL_BodyMaxHP);
    out.body_hp     = SafeRead<int32_t>(entity_addr + pso_offsets::DRL_BodyHP);
    out.skull_hp    = SafeRead<int32_t>(entity_addr + pso_offsets::DRL_SkullHP);

    // Skull and shell MAX from dereferenced boss data pointer.
    const uintptr_t ptr = SafeRead<uintptr_t>(pso_offsets::BossDataPtr);
    if (ptr != 0)
    {
        out.skull_max_hp = SafeRead<int32_t>(ptr + pso_offsets::DRL_SkullMaxHP_Ptr);
        out.shell_max_hp = SafeRead<int32_t>(ptr + pso_offsets::DRL_ShellMaxHP_Ptr);
        ApplyEphineaScale(out.skull_max_hp, out.shell_max_hp);
    }

    out.valid = out.body_max_hp > 0 && out.body_hp >= 0 &&
                out.body_hp <= out.body_max_hp;
    return out;
}

static BossHP ReadBarbaRayHP(uintptr_t entity_addr)
{
    BossHP out{};
    if (entity_addr == 0) return out;

    // Barba Ray uses entity+0x628 for the animations BP pointer,
    // NOT the static BossDataPtr. Differentiate body vs shell by
    // the class type ID at entity+0x04.
    // Source: solylib Monster Reader init.lua (GetMonsterDataBarbaRay).
    const uint32_t type_id = SafeRead<uint32_t>(entity_addr + 0x04);
    uintptr_t anim_bp = 0;

    if (type_id == pso_offsets::BR_BodyTypeID)
    {
        // Main body + skull
        anim_bp = SafeRead<uintptr_t>(entity_addr + pso_offsets::BR_AnimBPPtr);
        out.body_max_hp = SafeRead<int32_t>(entity_addr + pso_offsets::BR_BodyMaxHP);
        out.body_hp     = SafeRead<int32_t>(entity_addr + pso_offsets::BR_BodyHP);
        out.skull_hp    = SafeRead<int32_t>(entity_addr + pso_offsets::BR_SkullHP);
    }
    else
    {
        // Not body type — not valid for body HP read
        return out;
    }

    if (anim_bp != 0)
    {
        out.skull_max_hp = SafeRead<int32_t>(anim_bp + pso_offsets::BR_SkullMaxHP_Ptr);
        out.shell_max_hp = SafeRead<int32_t>(anim_bp + pso_offsets::BR_ShellMaxHP_Ptr);
        ApplyEphineaScale(out.skull_max_hp, out.shell_max_hp);
    }

    out.valid = out.body_max_hp > 0 && out.body_hp >= 0 &&
                out.body_hp <= out.body_max_hp;
    return out;
}

// Set of monster unitxt_ids the user wants hidden from the HP panel,
// loaded from pixelated_mods_monster_hidden.txt on startup. Same file format
// as pixelated_mods_hidden.txt / pixelated_mods_rares.txt: one hex or decimal ID
// per line. Used for boss sub-entities and decorative hitboxes that
// show up as "Unknown" alongside the real boss form. Users populate
// surgically by reading diagnostic log lines emitted by
// GetAliveMonsters and copying the unitxt_id into the file.
static std::unordered_set<uint32_t> g_hidden_monster_ids;

// Blanket fallback: hide every monster whose name comes back empty
// or literally "Unknown" from PSO's unitxt table. Off by default so
// the surgical hide list above is the primary mechanism.
static bool g_hide_unnamed_monsters = false;

static uintptr_t g_entity_array_addr = 0;

// Return the i16 entity ID of the local player's currently targeted
// enemy, or a sentinel "no target" value when:
//   - The player isn't in-game (no local player ptr)
//   - The "sibling" struct pointer at player+0x18 is null (happens
//     briefly on map transitions)
//   - The target ID read back as -1 (PSO's in-band "nothing targeted")
//
// We use INT32_MIN as the sentinel so it can't collide with any real
// 16-bit entity ID. Callers that want to check "is anything targeted"
// should compare against INT32_MIN, not -1.
//
// Path (from Solybum's Monster Reader/init.lua):
//   local player ptr -> +0x18 -> sibling ptr -> +0x108C -> i16 target ID
static int GetCurrentTargetId()
{
    const uintptr_t p = GetLocalPlayerPtr();
    if (p == 0) return INT32_MIN;

    const uintptr_t sibling =
        SafeRead<uintptr_t>(p + pso_offsets::PlayerTargetSibling);
    if (sibling == 0) return INT32_MIN;

    const int16_t raw =
        SafeRead<int16_t>(sibling + pso_offsets::TargetIdInSibling,
                          static_cast<int16_t>(-1));
    if (raw == -1) return INT32_MIN;
    return static_cast<int>(raw);
}

static uintptr_t GetEntityArrayAddr()
{
    if (g_entity_array_addr == 0)
        g_entity_array_addr = SafeRead<uintptr_t>(pso_offsets::EntityArrayRef);
    return g_entity_array_addr;
}

static std::vector<Monster> GetAliveMonsters()
{
    std::vector<Monster> out;

    const uintptr_t entity_array = GetEntityArrayAddr();
    if (entity_array == 0) return out;

    const uint32_t entity_count = SafeRead<uint32_t>(pso_offsets::EntityCount);
    const uint32_t player_count = SafeRead<uint32_t>(pso_offsets::PlayerCount);
    if (entity_count == 0 || entity_count > 1024) return out;

    // One-shot per-session diagnostic: log our entity-array base
    // against the two Ghidra-known list globals so we can tell
    // whether anything we're missing (like Dark Falz Phase 3) lives
    // in a different list than we're iterating.
    //
    //   0x00AB0210  g_p_object_list          (pointer-to-array)
    //   0x00AB0214  g_p_monster_structure_list (pointer-to-array)
    //
    // If either resolves to a non-null pointer that differs from our
    // entity_array base, we have a second array to consider.
    {
        static bool s_lists_probed = false;
        if (!s_lists_probed)
        {
            s_lists_probed = true;
            const uintptr_t g_p_object_list  = SafeRead<uintptr_t>(0x00AB0210);
            const uintptr_t g_p_monster_list = SafeRead<uintptr_t>(0x00AB0214);
            PSO_LOG("entity-lists our_base=0x%08X "
                    "g_p_object_list=0x%08X g_p_monster_list=0x%08X "
                    "entity_count=%u player_count=%u",
                    (unsigned)entity_array,
                    (unsigned)g_p_object_list,
                    (unsigned)g_p_monster_list,
                    entity_count, player_count);
        }
    }

    const bool ultimate = SafeRead<uint32_t>(pso_offsets::Difficulty) == 3;

    // Snapshot the local player's current-target entity ID once per
    // scan so we can mark the matching monster as `is_targeted` for
    // the highlight render path. Returns INT32_MIN when nothing is
    // targeted (no player, sibling ptr null, or raw value -1), which
    // can't collide with any real i16 entity ID — so the per-monster
    // comparison below safely evaluates to false for everyone when
    // no target exists.
    const int target_id = GetCurrentTargetId();

    // Room filter: a monster is only "in the same room as the player"
    // if its 0x28 room ID matches either of the player's two rooms
    // (primary at +0x28, secondary at +0x2E used during transitions).
    // This catches the Olga Flow dead-body-after-cutscene case: the
    // corpse persists at full HP in its original room while the
    // player gets teleported elsewhere by the cutscene, so the rooms
    // no longer match and the filter hides it.
    const uintptr_t local_player = GetLocalPlayerPtr();
    uint16_t player_room1 = 0xFFFF;
    uint16_t player_room2 = 0xFFFE;  // different sentinels so the "no
                                     // player" case doesn't accidentally
                                     // match a valid 0 room on a monster
    float player_x = 0.f, player_z = 0.f;
    bool have_player_pos = false;
    if (local_player != 0)
    {
        player_room1 = SafeRead<uint16_t>(local_player + pso_offsets::EntityRoom);
        player_room2 = SafeRead<uint16_t>(local_player + pso_offsets::PlayerRoom2);
        player_x = SafeRead<float>(local_player + 0x38);
        player_z = SafeRead<float>(local_player + 0x40);
        have_player_pos = true;
    }

    // Pre-scan: for each Pan Arms combined form (uid=21), record its
    // entity address and whether it's dormant (d >= 48 = halves are
    // split out). Halves (uid=22/23) are paired to the nearest
    // preceding uid=21 in the entity array by address proximity.
    //
    // Hysteresis: once a combined form is marked active (not dormant),
    // it stays active for several frames even if d briefly spikes
    // during transition animations. This prevents one-frame flicker
    // where halves appear alongside the combined form.
    struct PaCombined { uintptr_t addr; int16_t eid; bool dormant; };
    std::vector<PaCombined> pa_combined;

    // Per-entity cooldown: maps entity address to frames remaining
    // where the entity is forced "active" (not dormant).
    static std::unordered_map<uintptr_t, int> s_pa_active_cooldown;
    constexpr int kCooldownFrames = 10;

    for (uint32_t i = 0; i < entity_count; ++i)
    {
        const uintptr_t slot = entity_array + 4 * (i + player_count);
        const uintptr_t a = SafeRead<uintptr_t>(slot);
        if (a == 0) continue;
        if (SafeRead<uint32_t>(a + pso_offsets::MonsterUnitxtID) == pso_offsets::UidPanArmsCombined)
        {
            const int16_t eid = SafeRead<int16_t>(a + pso_offsets::EntityId,
                                                   static_cast<int16_t>(-1));
            const uint16_t d = SafeRead<uint16_t>(a + pso_offsets::EntityDormant);
            bool dormant = (d >= 48);

            if (!dormant)
            {
                // Combined is active — set/refresh cooldown
                s_pa_active_cooldown[a] = kCooldownFrames;
            }
            else
            {
                // d >= 48, but check cooldown
                auto it = s_pa_active_cooldown.find(a);
                if (it != s_pa_active_cooldown.end() && it->second > 0)
                {
                    // Still in cooldown — force active
                    it->second--;
                    dormant = false;
                }
            }

            pa_combined.push_back({a, eid, dormant});
        }
    }

    // Clean up stale cooldown entries (entities no longer present)
    for (auto it = s_pa_active_cooldown.begin(); it != s_pa_active_cooldown.end(); )
    {
        bool found = false;
        for (const auto &pc : pa_combined)
            if (pc.addr == it->first) { found = true; break; }
        if (!found) it = s_pa_active_cooldown.erase(it);
        else ++it;
    }

    out.reserve(entity_count);

    for (uint32_t i = 0; i < entity_count; ++i)
    {
        const uintptr_t slot = entity_array + 4 * (i + player_count);
        const uintptr_t mon_addr = SafeRead<uintptr_t>(slot);
        if (mon_addr == 0) continue;

        Monster m{};
        m.address      = mon_addr;
        m.unitxt_id    = SafeRead<uint32_t>(mon_addr + pso_offsets::MonsterUnitxtID);
        m.entity_flags = SafeRead<uint32_t>(mon_addr + pso_offsets::MonsterEntityFlags);
        m.room         = SafeRead<uint16_t>(mon_addr + pso_offsets::EntityRoom);
        m.active_flag  = SafeRead<uint16_t>(mon_addr + pso_offsets::EntityActiveFlag);
        m.entity_id    = SafeRead<int16_t>(mon_addr + pso_offsets::EntityId,
                                           static_cast<int16_t>(-1));
        m.cls_meta     = SafeRead<uintptr_t>(mon_addr + 0x04);
        m.boss_root    = FindSegmentBossBody(mon_addr);

        // Read HP: try Ephinea i32 array first, fall back to entity struct
        // if Ephinea returns 0 (boss sub-parts, mines, etc. aren't tracked).
        {
            const uintptr_t eph = SafeRead<uintptr_t>(pso_offsets::EphineaMonsterArray);
            bool got_hp = false;
            if (eph != 0 && m.entity_id >= 0)
            {
                const int32_t eh = SafeRead<int32_t>(eph + (m.entity_id * 32) + 0x04);
                const int32_t em = SafeRead<int32_t>(eph + (m.entity_id * 32));
                if (eh > 0 || em > 0)
                {
                    m.hp     = eh;
                    m.max_hp = em;
                    got_hp = true;
                }
            }
            if (!got_hp)
            {
                m.hp     = SafeRead<int16_t>(mon_addr + pso_offsets::MonsterHP);
                m.max_hp = SafeRead<int16_t>(mon_addr + pso_offsets::MonsterHPMax);
            }
        }

        // Entity type lookup via class metadata — the engine's own type
        // identity. All hide/rename/collapse decisions are driven by this.
        const EntityType etype = LookupEntityType(m.cls_meta);

        // Per-session unique-entity dump. Keyed on (cls_meta, uid,
        // max_hp) so difficulty variants of the same entity log as
        // separate rows. Fires once per unique signature; capped at
        // 512 rows so a pathological session can't spam the log.
        //
        // Fires BEFORE every drop gate below (role filter, hp/flags
        // checks, collapse distance, room mismatch, etc.) so every
        // entity the game places in the array shows up here — even
        // ones we'd normally filter. User curates hide lists from
        // real observed data rather than speculative cls lists.
        {
            static std::unordered_set<uint64_t> s_entity_dump_seen;
            const uint64_t sig =
                (static_cast<uint64_t>(m.cls_meta) << 32) |
                (static_cast<uint64_t>(m.unitxt_id) << 16) |
                static_cast<uint64_t>(m.max_hp & 0xFFFF);
            if (s_entity_dump_seen.size() < 2048 &&
                s_entity_dump_seen.insert(sig).second)
            {
                const std::string name =
                    GetMonsterName(m.unitxt_id, ultimate);
                const uint16_t dormant = SafeRead<uint16_t>(
                    mon_addr + pso_offsets::EntityDormant);
                const char *role_str = "?";
                switch (etype.role) {
                case EntityRole::NormalMob:        role_str = "NormalMob"; break;
                case EntityRole::SegmentBossBody:  role_str = "SegBossBody"; break;
                case EntityRole::SegmentBossShell: role_str = "SegBossShell"; break;
                case EntityRole::BossSubpart:      role_str = "BossSubpart"; break;
                case EntityRole::BossProjectile:   role_str = "BossProjectile"; break;
                case EntityRole::CollapseByName:   role_str = "CollapseByName"; break;
                }
                PSO_LOG("entity-dump cls=0x%08X uid=%u name='%s' "
                        "hp=%d/%d room=%u flags=0x%04X dormant=0x%04X "
                        "role=%s addr=0x%08X",
                        (unsigned)m.cls_meta, m.unitxt_id,
                        name.c_str(), m.hp, m.max_hp, m.room,
                        m.entity_flags, dormant, role_str,
                        (unsigned)mon_addr);
            }
        }

        // Projectiles (mines, trackers): always hidden from HP bar.
        if (etype.role == EntityRole::BossProjectile)
            continue;

        // Boss subparts sharing HP pool: hidden (they don't have independent HP).
        if (etype.role == EntityRole::BossSubpart)
            continue;

        // Surgical cls-keyed hide — decorative placeholders like the
        // Dark Falz peace-area obelisk that the game treats as a full
        // entity (with real HP even) but which we don't want in the
        // HP panel because the player can't interact with them.
        if (etype.role == EntityRole::Hidden)
            continue;

        // Shell HP for De Rol Le / Barba Ray segments. Uses boss_root
        // (derived from walking the parent_object chain) so the check
        // is locale-independent and captures every segment entity
        // regardless of its individual cls_meta.
        if (m.boss_root != 0 &&
            SafeRead<uint16_t>(mon_addr + pso_offsets::EntityDormant) != 0)
            m.shell_hp = SafeRead<int32_t>(mon_addr + pso_offsets::SegmentShellHP);

        m.is_targeted =
            (target_id != INT32_MIN) &&
            (static_cast<int>(m.entity_id) == target_id);

        if (m.hp <= 0 || m.max_hp <= 0) continue;   // signed check: catches negative HP
        if ((m.entity_flags & 0x0800) != 0) continue;

        // De Rol Le (uid=45) and Dal Ra Lie/Barba Ray (uid=73) have
        // dummy 20000 HP at the standard +0x334/+0x2BC offsets. The
        // real body HP lives at entity-specific offsets (+0x6B4 etc.)
        // and is read by BuildHpRows → ReadDeRolLeHP/ReadBarbaRayHP.

        // Vol Opt: all parts share room 0 across both phases. Distance
        // gate hides the inactive phase (>1000u away).
        if (etype.role == EntityRole::CollapseByName && have_player_pos)
        {
            const float mx = SafeRead<float>(mon_addr + 0x38);
            const float mz = SafeRead<float>(mon_addr + 0x40);
            const float dx = mx - player_x;
            const float dz = mz - player_z;
            if (dx * dx + dz * dz > 1000.f * 1000.f) continue;
        }

        // Pan Arms: hide combined form (uid=21) when d >= 48.
        // Hide halves (uid=22/23) when combined form is active
        // (pa_combined_active, from pre-scan above).
        if (m.unitxt_id == pso_offsets::UidPanArmsCombined)
        {
            // Use the pre-scanned d value to avoid a race between
            // the pre-scan and the main loop (d can change mid-frame).
            bool is_dormant = true; // default: hide if not found
            for (const auto &pc : pa_combined)
                if (pc.addr == mon_addr) { is_dormant = pc.dormant; break; }
            if (is_dormant) continue;
        }
        // Pan Arms halves: find the owning combined form by entity ID.
        // Pan Arms spawns as 3 consecutive eids: combined=N, hidoom=N+1,
        // migium=N+2. Match to the combined form with the closest eid
        // that is less than this half's eid. Address proximity fails
        // when multiple Pan Arms groups are allocated consecutively.
        if ((m.unitxt_id == pso_offsets::UidMigium || m.unitxt_id == pso_offsets::UidHidoom) && !pa_combined.empty())
        {
            int best_eid_dist = INT_MAX;
            bool owner_dormant = true;
            for (const auto &pc : pa_combined)
            {
                // Half's eid should be 1 or 2 above the combined's eid
                int eid_dist = static_cast<int>(m.entity_id) - static_cast<int>(pc.eid);
                if (eid_dist > 0 && eid_dist < best_eid_dist)
                {
                    best_eid_dist = eid_dist;
                    owner_dormant = pc.dormant;
                }
            }
            if (!owner_dormant) continue;  // combined is active, hide half
        }

        // Unknown entity types with unitxt_id == 0: these are controllers,
        // hitboxes, and summon-spawner objects (usually in the 0xA8xxxx
        // object range which our cls table only partially covers).
        //
        // Name resolution for these happens in two phases:
        //   Phase 1 (here): walk entity+0x14 parent_object chain looking
        //     for a named ancestor. Works for shell-style subparts that
        //     actually carry a parent pointer (verified for De Rol Le /
        //     Barba Ray shells, sometimes Dark Falz subparts).
        //   Phase 2 (post-pass below): for any monster that came out of
        //     this block with no name, find the closest named monster
        //     in the same room and call it "<that name> Controller".
        //     This is the fallback when parent_object is null.
        if (m.unitxt_id == 0 && etype.role == EntityRole::NormalMob)
        {
            // Walk the parent chain up to 6 levels looking for a parent
            // with a known cls+display_name, or a named unitxt.
            std::string parent_name;
            uintptr_t parent_chain[6] = {0};
            uintptr_t parent_cls_chain[6] = {0};
            int       chain_len = 0;
            uintptr_t cur = mon_addr;
            for (int depth = 0; depth < 6; ++depth)
            {
                const uintptr_t parent = SafeRead<uintptr_t>(
                    cur + pso_offsets::EntityParentObject);
                if (parent == 0 || parent == cur) break;
                parent_chain[chain_len] = parent;
                const uintptr_t pcls = SafeRead<uintptr_t>(
                    parent + pso_offsets::EntityTypeID);
                parent_cls_chain[chain_len] = pcls;
                chain_len++;
                if (pcls != 0)
                {
                    // Prefer the game's unitxt (locale-correct) over
                    // the synthetic name — fall back to synth only if
                    // unitxt has nothing or returns "Unknown".
                    const uint32_t puid = SafeRead<uint32_t>(
                        parent + pso_offsets::MonsterUnitxtID);
                    if (puid != 0)
                    {
                        std::string pn = GetMonsterName(puid, ultimate);
                        if (!pn.empty() && pn != "Unknown")
                        {
                            parent_name = std::move(pn);
                            break;
                        }
                    }
                    const EntityType pet = LookupEntityType(pcls);
                    if (pet.synth_name)
                    {
                        parent_name = ApplyUltimateNameSwap(
                            Localize(*pet.synth_name), ultimate);
                        break;
                    }
                }
                cur = parent;
            }

            // Log each unique unknown cls once, with its parent chain
            // and the resolved parent name (if any). This is the signal
            // we use to decide which cls values need to be promoted
            // into entity_cls_table.h with an explicit name.
            static std::unordered_set<uintptr_t> s_logged_cls;
            if (s_logged_cls.find(m.cls_meta) == s_logged_cls.end() &&
                s_logged_cls.size() < 32)
            {
                s_logged_cls.insert(m.cls_meta);
                char chain_buf[192];
                int off = 0;
                for (int ci = 0; ci < chain_len && off < (int)sizeof(chain_buf) - 24; ++ci)
                {
                    off += std::snprintf(chain_buf + off, sizeof(chain_buf) - off,
                                         " [%d]@0x%08X cls=0x%08X",
                                         ci, parent_chain[ci],
                                         (unsigned)parent_cls_chain[ci]);
                }
                if (chain_len == 0)
                    std::snprintf(chain_buf, sizeof(chain_buf), " (no parent)");
                PSO_LOG("Unknown uid=0 cls=0x%08X hp=%d/%d room=%u parent='%s' chain=%s",
                        (unsigned)m.cls_meta, m.hp, m.max_hp, m.room,
                        parent_name.c_str(), chain_buf);
            }

            if (!parent_name.empty())
                m.name = parent_name + Localize(kSynthControllerSuffix);
            else if (etype.synth_name)
                m.name = ApplyUltimateNameSwap(
                    Localize(*etype.synth_name), ultimate);
            // else: leave m.name empty, Phase 2 post-pass will try to
            // resolve it via same-room proximity. If that also fails,
            // the unnamed-monster handling further down sets a default.
        }

        // Room match required. A monster in a different room than the
        // player is either pre-spawned in a room the player hasn't
        // reached yet, or — the case we care about — a stale corpse
        // left behind after a cutscene transition (Olga Flow, etc).
        //
        // EXCEPTION: boss entities live in their own sub-room within
        // the arena and don't follow the player room as the player
        // walks/cutscenes between sub-rooms. Verified for Dark Falz
        // via the 19:13:17 boss dump: player in room 0, Dark Falz
        // entity in room 1, both inside the same fight arena. Without
        // this bypass the room filter hides the boss and the panel
        // shows "No enemies in room" mid-fight.
        //
        // Boss uids that need a room-filter bypass because their
        // entity lives in a sub-room different from the player's:
        //   44  Dal Ra Lie / Barba Ray
        //   45  De Rol Le
        //   46  Vol Opt
        //   47  Vol Opt ver.2 (and Dark Falz on stock PSOBB)
        //   78  Olga Flow
        static const std::unordered_set<uint32_t> kBossRoomBypass = {
            44, 45, 46, 47, 78,
        };
        if (local_player != 0 &&
            m.room != player_room1 && m.room != player_room2 &&
            kBossRoomBypass.find(m.unitxt_id) == kBossRoomBypass.end())
        {
            continue;
        }


        // Vol Opt Monitor vs Panel disambiguation. Both share cls
        // 0x00A449D0 — the static table can only label one of them.
        // The two are distinguished by battle-params HP: the Monitor
        // is the boosted variant, the Panel is the template variant.
        // Read the battle-params template HP at *(entity+0x2B4)+0x06
        // and compare against the live max_hp. This is
        // difficulty-independent because the template is the BASE,
        // not a hardcoded value.
        if (m.cls_meta == 0x00A449D0)
        {
            const uintptr_t bp = SafeRead<uintptr_t>(mon_addr + 0x2B4);
            const uint16_t template_hp = (bp != 0)
                ? SafeRead<uint16_t>(bp + 0x06)
                : 0;

            // One-shot diagnostic per unique (max_hp, template_hp) pair
            // so we can verify the disambig is actually firing and see
            // what values we're comparing.
            {
                static std::unordered_set<uint32_t> s_volopt_seen;
                const uint32_t key = (static_cast<uint32_t>(m.max_hp) << 16) |
                                     static_cast<uint32_t>(template_hp);
                if (s_volopt_seen.size() < 16 &&
                    s_volopt_seen.insert(key).second)
                {
                    PSO_LOG("volopt-disambig cls=0x00A449D0 max_hp=%d "
                            "template_hp=%u bp=0x%08X → %s",
                            m.max_hp, template_hp, (unsigned)bp,
                            (template_hp == 0)
                                ? "SKIP (no bp/template)"
                                : (m.max_hp > template_hp
                                    ? "Monitor (boosted)"
                                    : "Panel (template)"));
                }
            }

            if (template_hp > 0)
            {
                // Mirror the synthetic name strings here — the cls
                // table file-scope statics are not reachable from
                // outside LookupEntityType, and we're post-collection.
                static const LocalizedString kPanel   = { "Vol Opt Panel",   "ヴォル・オプト パネル" };
                static const LocalizedString kMonitor = { "Vol Opt Monitor", "ヴォル・オプト モニター" };
                const LocalizedString &pick = (m.max_hp > template_hp)
                    ? kMonitor : kPanel;
                m.name = ApplyUltimateNameSwap(Localize(pick), ultimate);
            }
        }

        // Name resolution priority:
        //   1. Existing m.name (already set above by Vol Opt Monitor/
        //      Panel disambig or the uid=0 parent-chain branch).
        //   2. Synthetic name for entities the cls table explicitly
        //      labels (Vol Opt Chandelier/Spire, Olga Flow Hitbox,
        //      Dragon Subpart, etc.). A synth override is declared
        //      precisely because the generic unitxt name isn't
        //      specific enough, so it must win over the game name.
        //   3. Game's unitxt name (locale-correct + Ult-aware).
        //   4. Empty string, handled by the unnamed-monster branch below.
        if (m.name.empty())
        {
            if (etype.synth_name)
            {
                m.name = ApplyUltimateNameSwap(
                    Localize(*etype.synth_name), ultimate);
            }
            else
            {
                std::string game_name = GetMonsterName(m.unitxt_id, ultimate);
                if (!game_name.empty() && game_name != "Unknown")
                    m.name = std::move(game_name);
                // else: leave empty, the unnamed-monster branch below
                //       either applies the "Hide unnamed" toggle or
                //       fills in a "Monster #N" fallback.
            }
        }

        // Surgical hide: drop any monster whose unitxt_id is in the
        // user-curated pixelated_mods_monster_hidden.txt list. This is how
        // boss sub-entities like the Olga Flow anchor hitbox get
        // filtered without blanket-hiding every unnamed monster.
        if (g_hidden_monster_ids.find(m.unitxt_id) !=
            g_hidden_monster_ids.end())
        {
            continue;
        }

        // "Unnamed" means the name came back empty OR it came back
        // as the literal string "Unknown" from PSO's own unitxt
        // table. PSO tags certain boss sub-entities and decorative
        // hitboxes (e.g. Olga Flow's anchored body hitbox) with
        // "Unknown" in its internal string table. The entity-dump
        // log earlier in this function already records every unique
        // (cls, uid, max_hp) signature the session encounters, so
        // the user can correlate from pixelated_mods.log and add
        // IDs to pixelated_mods_monster_hidden.txt from real data.
        const bool unnamed = m.name.empty() || m.name == "Unknown";

        if (unnamed)
        {
            // Blanket fallback: if the user has explicitly enabled
            // "Hide unnamed monsters" they get the immediate effect
            // of every unnamed entity disappearing. Default is off.
            if (g_hide_unnamed_monsters)
                continue;

            if (m.name.empty())
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "Monster #%u", m.unitxt_id);
                m.name = buf;
            }
        }

        // Raw world position — used both for player-distance and for
        // the mob-to-mob "Sub-entity" → "<parent> Controller" fallback
        // in the post-collection pass.
        m.world_x = SafeRead<float>(mon_addr + 0x38);
        m.world_z = SafeRead<float>(mon_addr + 0x40);

        // Distance and direction from player to monster
        m.dist_xz = 0.f;
        m.world_dx = 0.f;
        m.world_dz = 0.f;
        if (have_player_pos)
        {
            const float dx = m.world_x - player_x;
            const float dz = m.world_z - player_z;
            m.dist_xz = std::sqrt(dx * dx + dz * dz);
            if (m.dist_xz > 0.001f)
            {
                m.world_dx = dx / m.dist_xz;
                m.world_dz = dz / m.dist_xz;
            }
        }

        out.push_back(std::move(m));
    }

    // Target-miss diagnostic: if the game has a live target ID but
    // nothing in `out` carries that entity ID, the entity is either
    // outside the array we iterate (wrong list?), has been dropped
    // by a filter gate, or simply wasn't in the entity_array scan
    // range. Rate-limited to distinct target IDs so a single missed
    // target doesn't spam the log every frame.
    //
    // This is the smoking gun for the Dark Falz Phase 3 problem:
    // the bottom-left HUD shows a name (so the game resolves target
    // → name just fine), but nothing in our HP panel. If that line
    // fires for Phase 3, our array iteration is the wrong place to
    // look.
    if (target_id != INT32_MIN)
    {
        bool found = false;
        for (const auto &mm : out)
        {
            if (static_cast<int>(mm.entity_id) == target_id)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            static std::unordered_set<int> s_target_miss_seen;
            if (s_target_miss_seen.size() < 64 &&
                s_target_miss_seen.insert(target_id).second)
            {
                PSO_LOG("target-miss eid=%d — game has target but no "
                        "entity with that id is in our collected list "
                        "(out.size=%zu, entity_count=%u)",
                        target_id, out.size(), entity_count);
            }
        }
    }

    return out;
}

// ============================================================================
// Floor items
// ============================================================================

// Why a floor item is NOT showing in the main visible list. Populated
// by ClassifyItem + the area/distance gates in GetFloorItems so the
// "Hidden in view" sidebar can tell the user exactly which filter is
// suppressing each entry. `Visible` means the item shows normally.
enum class HideReason : int {
    Visible = 0,
    OtherFloor,           // "only current floor" is on, item is on a different floor
    TooFar,               // distance cap exceeded
    UserHideList,         // 24-bit ID is in pixelated_mods_hidden.txt
    GearOnlyMode,         // filter mode Gear-only, item isn't weapon/armor/shield/unit/mag
    LowHitWeapon,         // BBMod: hide weapons below minimum hit %
    LowSocketArmor,       // BBMod: hide non-4-slot armor
    AllArmorHidden,       // BBMod: HideLowSocket + HideFourSocket combination
    UnitsHidden,          // BBMod: hide all units
    MesetaHidden,         // BBMod: hide meseta
    ToolCategoryOff,      // per-sub-type tool toggle is off (mates, fluids, etc.)
    TechDiskBelowMin,     // tech disk level < min-level slider
};

struct FloorItem
{
    uintptr_t   entity_ptr;  // record+0x00: base_game_object *, points to
                             // the full item entity in the game's object
                             // tree. +0x378 on that entity is the unitxt
                             // name index (same offset every game object
                             // uses, incl. monsters and players).
    uint8_t     bytes[12];   // 12-byte item data block from record+0x10
    uint8_t     bytes2[4];   // 4-byte data2 block from record+0x20
                             //   for meseta (type 0x04) this is the amount
                             //   as a little-endian u32
    uint32_t    id24;        // (bytes[0]<<16) | (bytes[1]<<8) | bytes[2]
    float       pos_x;       // world X from record+0x04
    float       pos_z;       // world Z from record+0x08 (no Y stored)
    float       dist_xz;     // 2D distance to local player, computed
                             // in GetFloorItems after the read.
                             // Meaningful only when is_on_player_floor
                             // is true; cross-floor distances are in
                             // different coordinate spaces and are
                             // not computed.
    float       world_dx;    // Normalized direction from player to
                             // item in world space. Zero when the
                             // direction couldn't be computed (player
                             // not resolvable, item on another floor,
                             // or item at the player's exact position).
    float       world_dz;    // See world_dx.
    std::string display;
    int         area;
    DWORD       first_seen_tick;  // GetTickCount() when this drop first appeared
    bool        is_rare;
    bool        is_on_player_floor;  // item.area == player's current area
    HideReason  hide_reason;         // populated for items in the hidden sidebar
};

// Set of 24-bit item IDs considered "rare red box" drops, loaded from
// pixelated_mods_rares.txt on startup. Items in this set pulse red in the floor
// items list when g_blink_rare_items is on. Photon drops / spheres /
// crystals / mag cells / Add Slot are NOT in this set — they're flagged
// by the sub-type heuristic in IsRareItem instead.
static std::unordered_set<uint32_t> g_rare_ids;

// Set of 24-bit item IDs the user wants hidden from the floor list,
// loaded from pixelated_mods_hidden.txt on startup. Equivalent to setting an
// item's flag to `false` in solylib's items_list.lua. Rares always
// bypass this set — hiding red-pulsing rares is bad UX. Format: one
// 6-digit hex ID per line, blank lines and `#` comments allowed.
static std::unordered_set<uint32_t> g_hidden_ids;

// Floor-item filter settings (user-configurable via the overlay panel,
// persisted to pixelated_mods.ini).
//
// Notable  — default. Weapons/armor/shields/units/mags always, tools
//            filtered via the per-sub-type show flags below.
// All      — show everything on the floor, no filtering at all.
// Gear     — only weapons, armor, shields, units, and mags. Hides ALL
//            tools, including grinders / materials / mag cells. Useful
//            when rare-hunting and you want a clean list of only gear.
enum class FilterMode : int {
    Notable = 0,
    All     = 1,
    Gear    = 2,
};

static FilterMode g_filter_mode            = FilterMode::Notable;
static int        g_tech_disk_min_level    = 15;  // 1..30, Notable mode only
static bool       g_filter_current_area    = true;  // only show items on player's current floor
static bool       g_flash_new_drops        = true;  // yellow flash for newly appeared items
static bool       g_blink_rare_items       = true;  // pulsing red for rare drops

// BBMod / Item Reader compatible opt-in filters. All default OFF to
// match the upstream Solybum Item Reader defaults — users enable the
// ones they want from the config panel. Faithful semantics, with one
// deliberate deviation: rares ALWAYS pass these filters in our build,
// because hiding pulsing red rare items is uniformly worse UX than
// the alternative.
//
//   HideLowHitWeapons : hide weapons whose hit% is below HitMin
//   HitMin            : threshold for HideLowHitWeapons (0..100)
//   HideLowSocketArmor: hide non-4-slot armor (paired with HideFourSocketArmor)
//   HideFourSocketArmor: when HideLowSocketArmor is on, also hide 4-slot armor
//                       so the combination hides ALL armor
//   HideUnits         : hide all units regardless of type (Item Reader's
//                       "HideUselessUnits" is misnamed — it has no per-unit
//                       logic, just blanket-hides every unit)
//   HideMeseta        : hide all meseta drops (Item Reader's ignoreMeseta)
static bool g_filter_hide_low_hit_weapons   = false;
static int  g_filter_hit_min                = 40;
static bool g_filter_hide_low_socket_armor  = false;
static bool g_filter_hide_four_socket_armor = false;
static bool g_filter_hide_units             = false;
static bool g_filter_hide_meseta            = false;

// Distance feature. Each floor item record carries an (X, Z) position
// in world units at record+0x04/+0x08. We compute 2D distance from
// the local player in GetFloorItems. Two knobs:
//
//   g_show_item_distance    - print "Nu" after the area name
//   g_item_max_distance     - hide items beyond this many world units
//                             (0 means unlimited)
//
// World units are floats; PSO rooms are typically 200-2000 units
// across so a cap around 1000-1500 is a natural "the rooms around
// me" radius.
static bool g_show_item_distance = true;
// Distance cap is gated by an explicit checkbox so the slider value
// is preserved when toggled off (the user can flip Nearby mode on
// and off without losing their preferred radius). The slider value
// is the cap; the checkbox decides whether the cap is applied.
// Rares bypass the cap regardless.
static bool g_item_distance_cap_enabled = true;
// Default 1500 world units acts as a natural auto-declutter: items
// you've walked past drop off the list as you move forward, and
// come BACK when you walk backward through the same area.
static int  g_item_max_distance  = 1500;
// Show a small arrow next to each item in the floor items list
// pointing toward that item. Two modes (toggle: g_arrow_mode):
//
//   World         - arrow is a fixed "minimap-style" compass in
//                   world coordinates. +X_world -> screen right,
//                   +Z_world -> screen down. Verified working; the
//                   arrow doesn't rotate when the character turns.
//
//   PlayerRelative - arrow rotates with the character, so "up on the
//                    arrow" = "character forward." If the character
//                    is facing the item, the arrow points straight
//                    up; if the item is 90° to the character's right,
//                    the arrow points right; etc. Uses player yaw
//                    from player+0x60 to compute the forward vector.
static bool g_show_item_arrow    = true;
enum class ArrowMode : int
{
    World          = 0,
    PlayerRelative = 1,
};
static ArrowMode g_arrow_mode    = ArrowMode::PlayerRelative;

// Per-frame player forward snapshot, populated by GetFloorItems and
// consumed by DrawFloorItemArrow. Decouples the memory-read pass
// from the render pass so the renderer doesn't need to re-read yaw
// (stays consistent with the item positions sampled in the same
// scan). Only used by ArrowMode::PlayerRelative.
struct ArrowSnapshot
{
    bool  have_forward;
    float forward_x;    // world XZ forward, unit vector
    float forward_z;
};
static ArrowSnapshot g_arrow_snapshot{};

// Low-HP safety alert. When the player's HP drops below
// g_hp_alert_threshold_pct of max HP, the HUD window flashes red and
// (optionally) the system beeps. Turn off if you find it annoying.
static bool g_hp_alert_enabled     = true;
static int  g_hp_alert_threshold_pct = 25;   // 5..90
static bool g_hp_alert_beep        = true;   // audio beep on threshold crossing

// Per-sub-type "show this tool kind" flags (Notable mode only; indexed by
// the tool sub-type byte 0x00..0x1F). Defaults reproduce the original
// Notable-mode skip list: hide mates, fluids, sol/moon atomizers,
// antidotes, telepipes, trap vision, event badges (0x12-0x15).
static bool g_show_tool_sub[32] = {
    false, // 0x00 Mates
    false, // 0x01 Fluids
    true,  // 0x02 Tech disks (further filtered by g_tech_disk_min_level)
    false, // 0x03 Sol Atomizer
    false, // 0x04 Moon Atomizer
    true,  // 0x05 Star Atomizer
    false, // 0x06 Antidote / Antiparalysis
    false, // 0x07 Telepipe
    false, // 0x08 Trap Vision
    true,  // 0x09 Scape Doll
    true,  // 0x0A Grinders
    true,  // 0x0B Materials
    true,  // 0x0C Mag Cells
    true,  // 0x0D Enemy parts
    true,  // 0x0E Special items
    true,  // 0x0F Add Slot
    true,  // 0x10 Photon drops / spheres / crystals / tickets
    true,  // 0x11 Book of Katana
    false, // 0x12 Event badges
    false, // 0x13 Event badges
    false, // 0x14 Event badges
    false, // 0x15 Event badges
    true,  // 0x16 Music disks
    true,  // 0x17 Hunter's Report
    true,  // 0x18 Quest items
    true,  // 0x19 Team Points
    true, true, true, true, true, true, // 0x1A-0x1F reserved / show by default
};

// Tool sub-type labels for the config panel, indexed same as g_show_tool_sub.
// nullptr means "not exposed as a user-toggleable category" (tech disks are
// controlled separately via the level slider).
static const char *kToolSubLabels[32] = {
    "Mates",            // 0x00
    "Fluids",           // 0x01
    nullptr,            // 0x02 tech disks - use the level slider instead
    "Sol Atomizers",    // 0x03
    "Moon Atomizers",   // 0x04
    "Star Atomizers",   // 0x05
    "Antidote/paralys", // 0x06
    "Telepipes",        // 0x07
    "Trap Vision",      // 0x08
    "Scape Dolls",      // 0x09
    "Grinders",         // 0x0A
    "Materials",        // 0x0B
    "Mag Cells",        // 0x0C
    "Enemy parts",      // 0x0D
    "Special items",    // 0x0E
    "Add Slot",         // 0x0F
    "Photon drops/spheres", // 0x10
    "Book of Katana",   // 0x11
    "Event badges",     // 0x12
    nullptr, nullptr, nullptr, // 0x13-0x15 share the event badge toggle
    "Music disks",      // 0x16
    "Hunter's Report",  // 0x17
    "Quest items",      // 0x18
    "Team Points",      // 0x19
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

// Rare-box detection. Two-pronged:
//
//   1. Hardcoded tool sub-type heuristic — photon drops / spheres /
//      crystals (0x10), mag cells (0x0C), and Add Slot (0x0F) are
//      always rare red boxes in any real play scenario, so they get
//      caught without needing a lookup.
//
//   2. Lookup against g_rare_ids, populated from pixelated_mods_rares.txt at
//      startup. This is how weapon / armor / shield / unit rares are
//      detected. PSO's server-side "rare flag" isn't documented in the
//      12-byte drop record we read, so we fall back to a community-
//      curated ID list. Users can edit the .txt file to add or remove
//      entries without recompiling.
static bool IsRareItem(const uint8_t b[12], uint32_t id24)
{
    const uint8_t type = b[0];
    const uint8_t sub  = b[1];
    if (type == 0x03 && (sub == 0x10 || sub == 0x0C || sub == 0x0F))
        return true;
    return g_rare_ids.find(id24) != g_rare_ids.end();
}

// ============================================================================
// Per-frame cache.
//
// A handful of PSO globals (local player struct pointer, current
// episode byte) are read from multiple render-thread callers on
// every frame. Each SafeRead does a VirtualQuery syscall, so five
// calls to GetLocalPlayerPtr() per frame is 10 VirtualQuery calls
// per frame for the same value. These caches collapse the redundant
// work down to one resolve per frame, invalidated at the top of
// on_reshade_overlay_event. Everything here is render-thread-only;
// the input thread (DirectInput hook) never touches them.
// ============================================================================

static bool      g_fc_player_resolved  = false;
static uintptr_t g_fc_player           = 0;
static bool      g_fc_episode_resolved = false;
// g_fc_episode is typed as int here because the Episode enum is
// declared below this point in the file. Promoted to Episode at
// the call site.
static int       g_fc_episode          = 0;

static void FrameCacheReset()
{
    g_fc_player_resolved  = false;
    g_fc_player           = 0;
    g_fc_episode_resolved = false;
    g_fc_episode          = 0;
}

// Resolve the local player's struct pointer via PlayerArray[PlayerIndex].
// Returns 0 if the chain is invalid (character select screen, warping,
// etc.). Cached for the rest of the frame after the first call.
static uintptr_t GetLocalPlayerPtr()
{
    if (g_fc_player_resolved) return g_fc_player;
    const uint32_t idx = SafeRead<uint32_t>(pso_offsets::PlayerIndex);
    uintptr_t p = 0;
    if (idx < 12)
        p = SafeRead<uintptr_t>(pso_offsets::PlayerArray + 4 * idx);
    g_fc_player           = p;
    g_fc_player_resolved  = true;
    return p;
}

// Current player's floor / area index. Returns 0xFFFFFFFF on any
// read failure (no player ptr, page unmapped, etc.).
static uint32_t GetPlayerCurrentFloor()
{
    const uintptr_t p = GetLocalPlayerPtr();
    if (p == 0) return 0xFFFFFFFF;
    return SafeRead<uint32_t>(p + pso_offsets::PlayerCurrentFloor);
}

// Player HP / max HP. Same struct offsets monsters use (0x334 / 0x2BC),
// because the player is just another entity in PSO's entity array. Pair
// is (hp, max_hp); either field is 0 on any read failure.
struct PlayerHP { uint16_t hp; uint16_t max_hp; };
static PlayerHP GetLocalPlayerHP()
{
    PlayerHP out{};
    const uintptr_t p = GetLocalPlayerPtr();
    if (p == 0) return out;
    out.hp     = SafeRead<uint16_t>(p + pso_offsets::MonsterHP);
    out.max_hp = SafeRead<uint16_t>(p + pso_offsets::MonsterHPMax);
    return out;
}

// Blue Burst level-up experience table. `kLevelCumExp[N]` is the total
// cumulative EXP required to REACH level N+1 from a standing start.
// Index 0 = 0 EXP (level 1, everyone starts here). Index 1 = 50 EXP
// (the threshold at which you become level 2). Index 200 is the level
// 200 cap; no further level-ups after that.
//
// Values extracted from the official BB level-up table published on
// johndellarosa.github.io/projects/pso (which sources from Ephinea's
// community research on PlyLevelTbl.prs). Verified against the user's
// live in-game character menu: at level 141 with 11425485 EXP the
// in-game "To Next Lv" reads 375310, which matches
// kLevelCumExp[141] (11800795) minus current EXP exactly.
//
// A scan of the player struct in earlier sessions found no u32 field
// holding the computed "To Next Lv" delta directly — PSO computes it
// on the fly from a similar static table inside PsoBB.exe whenever the
// character menu is opened. We replicate that computation here so we
// don't have to locate the in-binary table address.
static const uint32_t kLevelCumExp[200] = {
    /* level 1   */ 0,
    /* level 2   */ 50,         200,        450,        800,
    /* level 6   */ 1250,       1800,       2450,       3200,
    /* level 10  */ 4050,       5000,       6050,       7200,
    /* level 14  */ 8450,       9848,       11427,      13252,
    /* level 18  */ 15364,      17785,      20539,      23644,
    /* level 22  */ 27118,      30979,      35245,      39924,
    /* level 26  */ 45022,      50548,      56515,      62935,
    /* level 30  */ 69816,      77165,      84993,      93309,
    /* level 34  */ 102126,     111455,     121310,     131698,
    /* level 38  */ 142625,     154103,     166145,     178762,
    /* level 42  */ 191962,     205756,     220153,     235166,
    /* level 46  */ 250807,     267086,     284012,     301595,
    /* level 50  */ 319847,     338779,     358404,     378730,
    /* level 54  */ 399766,     421524,     444016,     467249,
    /* level 58  */ 491238,     515991,     541513,     567815,
    /* level 62  */ 594908,     622800,     651502,     681023,
    /* level 66  */ 711375,     742566,     774604,     807502,
    /* level 70  */ 841271,     875920,     911460,     947904,
    /* level 74  */ 985259,     1023533,    1062737,    1102882,
    /* level 78  */ 1143980,    1186040,    1229071,    1273081,
    /* level 82  */ 1318092,    1364125,    1411201,    1459348,
    /* level 86  */ 1508599,    1558983,    1610536,    1663300,
    /* level 90  */ 1717321,    1772647,    1829330,    1887428,
    /* level 94  */ 1947004,    2008126,    2070867,    2135310,
    /* level 98  */ 2201539,    2269648,    2339735,    2411903,
    /* level 102 */ 2488164,    2568714,    2653755,    2743485,
    /* level 106 */ 2838105,    2937820,    3042926,    3153725,
    /* level 110 */ 3270523,    3393621,    3523317,    3659910,
    /* level 114 */ 3803697,    3954983,    4114067,    4281251,
    /* level 118 */ 4456936,    4641520,    4835405,    5038992,
    /* level 122 */ 5252677,    5476560,    5710739,    5955316,
    /* level 126 */ 6210392,    6476064,    6752432,    7039597,
    /* level 130 */ 7337658,    7646717,    7966876,    8298232,
    /* level 134 */ 8640881,    8994927,    9360472,    9737616,
    /* level 138 */ 10126458,   10527095,   10939630,   11364163,
    /* level 142 */ 11800795,   12249627,   12710761,   13184299,
    /* level 146 */ 13670344,   14168994,   14680346,   15204498,
    /* level 150 */ 15741551,   16291604,   16854759,   17431119,
    /* level 154 */ 18020783,   18623854,   19240432,   19870618,
    /* level 158 */ 20514614,   21172626,   21844850,   22531485,
    /* level 162 */ 23232732,   23948794,   24679868,   25426153,
    /* level 166 */ 26187854,   26965277,   27758726,   28568599,
    /* level 170 */ 29395299,   30239347,   31101333,   31981946,
    /* level 174 */ 32881910,   33802211,   34743869,   35708107,
    /* level 178 */ 36696348,   37710175,   38751371,   39821758,
    /* level 182 */ 40925323,   42068048,   43257823,   44504634,
    /* level 186 */ 45820378,   47219013,   48716731,   50331355,
    /* level 190 */ 52082887,   53992896,   56085071,   58386099,
    /* level 194 */ 60923945,   63728621,   66837114,   70289303,
    /* level 198 */ 74132060,   78415384,   83227800,
};

struct PlayerXp {
    uint32_t exp;
    uint32_t level;         // 1-indexed display level
    uint32_t to_next_level; // EXP remaining until level+1 (or 0 at cap)
    bool     valid;
};

static PlayerXp GetLocalPlayerXp()
{
    PlayerXp out{};
    const uintptr_t p = GetLocalPlayerPtr();
    if (p == 0) return out;
    out.exp   = SafeRead<uint32_t>(p + pso_offsets::PlayerExp);
    out.level = SafeRead<uint32_t>(p + pso_offsets::PlayerLevel) + 1;
    out.valid = true;

    // "To Next Lv" = kLevelCumExp[level] - current total EXP.
    if (out.level >= 1 && out.level < 200)
    {
        const uint32_t next = kLevelCumExp[out.level];
        out.to_next_level = (out.exp < next) ? (next - out.exp) : 0;
    }
    return out;
}

// Mag feed timer. PSO mags have a sync cycle of 210 seconds (3m30s):
// once per cycle the mag ticks +1 sync and the player can feed it.
// The "time remaining in the current cycle" is stored as a raw float
// tick count at `mag_item + pso_offsets::MagSyncTimer`. Solylib's
// items.lua reads it as `(29 + raw) / 30` — PSO runs at 30 ticks/sec
// internally, the +29 rounds partial ticks up. Near 210 = just ticked,
// near 0 = feedable.
//
// The equipped mag is found by walking the player's inventory linked
// list via the offsets in pso_offsets. All reads are SafeRead-guarded;
// returns 0 if any link is invalid (character select, corrupted ptr,
// etc). The walk is hard-capped to 64 nodes so a corrupted `next`
// pointer can't send us into an infinite loop.
static uintptr_t GetEquippedMagAddr()
{
    const uintptr_t p = GetLocalPlayerPtr();
    if (p == 0) return 0;
    const uintptr_t inv_ptr = SafeRead<uintptr_t>(p + pso_offsets::PlayerInventory);
    if (inv_ptr == 0) return 0;
    const uintptr_t list_addr = SafeRead<uintptr_t>(inv_ptr + pso_offsets::InventoryListPtr);
    if (list_addr == 0) return 0;
    uintptr_t node = SafeRead<uintptr_t>(list_addr + pso_offsets::InventoryFirstNode);

    for (int i = 0; i < 64 && node != 0; ++i)
    {
        const uintptr_t item_addr =
            SafeRead<uintptr_t>(node + pso_offsets::InventoryNodeItem);
        if (item_addr != 0)
        {
            const uint8_t type     = SafeRead<uint8_t>(item_addr + pso_offsets::ItemTypeByte);
            const uint8_t equipped = SafeRead<uint8_t>(item_addr + pso_offsets::ItemEquippedFlag);
            if (type == 0x02 && (equipped & 1) != 0)
                return item_addr;
        }
        node = SafeRead<uintptr_t>(node + pso_offsets::InventoryNextNode);
    }
    return 0;
}

// Mag feed timer state for a single frame.
//   valid:    true if an equipped mag was found
//   secs:     seconds until the next sync tick / feed opportunity
//   ready:    true if secs is effectively 0 (feedable right now)
struct MagTimer { bool valid; float secs; bool ready; };
static MagTimer GetMagFeedTimer()
{
    MagTimer out{};
    const uintptr_t mag = GetEquippedMagAddr();
    if (mag == 0) return out;
    const float raw = SafeRead<float>(mag + pso_offsets::MagSyncTimer);
    // Sanity-check the raw value — if it's NaN or wildly out of range
    // the pointer is probably bad.
    if (!(raw >= -100.0f && raw <= 20000.0f)) return out;
    out.secs  = (29.0f + raw) / 30.0f;
    out.valid = true;
    out.ready = out.secs <= 1.0f;
    return out;
}

// Two rolling windows of EXP samples for rate computation.
//
//   SHORT ring — 128 slots at 500ms = 64-second history. Used for
//   "Current", the responsive rate that tells you what you're doing
//   right now.
//
//   LONG ring — 720 slots at 5s = 3600-second (one hour) history.
//   Used for "Hour", a sustained trailing 60-minute rate, and for
//   "Peak" (highest Hour rate observed since the session had enough
//   data to compute a full 60 minutes).
//
// The long ring is memory-cheap (720 × 12 bytes = ~8.6 KB) and the
// sample cadence is gated separately from the short ring's cadence.
//
// xp.exp is the player's *cumulative* EXP from level 1 (verified
// against the kLevelCumExp[] table: at level 141 with EXP 11425804 and
// NXT 374991, 11425804 + 374991 = 11800795 = kLevelCumExp[141]). So
// each ring stores cumulative values directly and the rate / gained
// math works across level-ups without any per-level reset.
static bool     g_xp_track_enabled       = true;
static bool     g_mag_timer_enabled      = true;

// Rolling EXP/hour ring. 500 ms sample cadence over 64 s covers the
// "what am I earning right now" window — longer averaging adds lag
// without more useful information.
static constexpr int   kXpSampleCount    = 128;
static constexpr DWORD kXpSampleInterval = 500;
struct XpSample { DWORD tick; uint32_t exp; };
static XpSample g_xp_samples[kXpSampleCount] = {};
static int      g_xp_head             = 0;
static int      g_xp_count             = 0;
static DWORD    g_xp_last_sample_tick = 0;

static uint32_t g_xp_session_baseline     = 0;
static bool     g_xp_session_baseline_set = false;
static DWORD    g_xp_session_start_tick   = 0;

// Feed the rolling EXP buffer. EXP decrease means character switch — reset.
static void UpdateXpTracker(const PlayerXp &xp)
{
    if (!xp.valid) return;
    const DWORD now = GetTickCount();

    if (g_xp_session_baseline_set && xp.exp < g_xp_session_baseline)
    {
        g_xp_head                 = 0;
        g_xp_count                = 0;
        g_xp_session_baseline_set = false;
    }

    if (!g_xp_session_baseline_set)
    {
        g_xp_session_baseline     = xp.exp;
        g_xp_session_baseline_set = true;
        g_xp_session_start_tick   = now;
    }

    if (g_xp_count == 0 || now - g_xp_last_sample_tick >= kXpSampleInterval)
    {
        g_xp_last_sample_tick           = now;
        g_xp_samples[g_xp_head].tick    = now;
        g_xp_samples[g_xp_head].exp     = xp.exp;
        g_xp_head = (g_xp_head + 1) % kXpSampleCount;
        if (g_xp_count < kXpSampleCount) ++g_xp_count;
    }
}

static uint32_t ComputeXpGained(const PlayerXp &xp)
{
    if (!g_xp_session_baseline_set || !xp.valid) return 0;
    if (xp.exp < g_xp_session_baseline) return 0;
    return xp.exp - g_xp_session_baseline;
}

// EXP/hour rate across the rolling ring. Returns 0.0 before the ring
// has at least one second of history or if samples go backwards
// (character switch mid-window).
static double ComputeXpPerHour()
{
    if (g_xp_count < 2) return 0.0;
    const int oldest_idx = (g_xp_count < kXpSampleCount) ? 0 : g_xp_head;
    const int newest_idx = (g_xp_head - 1 + kXpSampleCount) % kXpSampleCount;
    const XpSample &oldest = g_xp_samples[oldest_idx];
    const XpSample &newest = g_xp_samples[newest_idx];
    const DWORD dt_ms = newest.tick - oldest.tick;
    if (dt_ms < 1000)            return 0.0;
    if (newest.exp < oldest.exp) return 0.0;
    const double dexp = static_cast<double>(newest.exp - oldest.exp);
    const double dhr  = static_cast<double>(dt_ms) / 3600000.0;
    return dexp / dhr;
}

// Episode auto-detect. pso_offsets::EpisodeByte holds a u32 whose
// low byte is 0 for Episode 1, 1 for Episode 2, and 2 for Episode 4.
// Verified empirically in pixelated_mods.log by walking through all three
// episodes and watching this address transition in lock-step with
// the area change (Forest 1/Cave 1 -> 0, Jungle/Seabed -> 1, Crater
// -> 2). solylib's Monster Reader never needed this because their
// monster tables use flat area indices, but floor area names
// DO collide across episodes — floor index 10 is Ruins 3 in Ep1,
// Seabed Upper in Ep2, and Desert 2 in Ep4 — so we have to resolve
// the episode to pick the right name table.
enum class Episode : int {
    Ep1 = 0,
    Ep2 = 1,
    Ep4 = 2,
};

static Episode GetCurrentEpisode()
{
    if (g_fc_episode_resolved)
        return static_cast<Episode>(g_fc_episode);
    const uint32_t raw = SafeRead<uint32_t>(pso_offsets::EpisodeByte);
    Episode ep = Episode::Ep1;
    if      (raw == 1) ep = Episode::Ep2;
    else if (raw == 2) ep = Episode::Ep4;
    g_fc_episode          = static_cast<int>(ep);
    g_fc_episode_resolved = true;
    return ep;
}

// KNOWN LOCALE ISSUE: the area name strings below are hardcoded
// English. PSO's own UI shows localized names from unitxt_j.prs (the
// same file that provides monster / item / tech names), but solylib
// doesn't document the area-name unitxt group and we haven't traced
// it from PsoBB.exe yet. Everything else in the name resolution
// pipeline goes through unitxt (monster names, item names, specials,
// techs, buffs where enum allows). The short-term cost of leaving
// this hardcoded is the "(Forest 1, 320u)" suffix on each floor item
// renders in English for non-English players; we lose nothing else.
// TODO: find the area-name unitxt group (candidates: groups 3, 6, 7,
// or 12 from the r2-traced accessor list) and replace this with
// UnitxtRead(group, area_index_per_episode).
static const char *AreaName(int area)
{
    switch (GetCurrentEpisode())
    {
    case Episode::Ep2:
        switch (area)
        {
        case 0:  return "Pioneer 2 (Lab)";
        case 1:  return "VR Temple Alpha";
        case 2:  return "VR Temple Beta";
        case 3:  return "VR Spaceship Alpha";
        case 4:  return "VR Spaceship Beta";
        case 5:  return "Central Control Area";
        case 6:  return "Jungle North";
        case 7:  return "Jungle East";
        case 8:  return "Mountain";
        case 9:  return "Seaside";
        case 10: return "Seabed Upper";
        case 11: return "Seabed Lower";
        case 12: return "Cliffs of Gal Da Val";
        case 13: return "Test Subject Disposal Area";
        case 14: return "VR Temple Final";
        case 15: return "VR Spaceship Final";
        case 16: return "Seaside at Night";
        case 17: return "Control Tower";
        default: return "Area ?";
        }

    case Episode::Ep4:
        switch (area)
        {
        case 0:  return "Pioneer 2 (Lab)";
        case 1:  return "Crater East";
        case 2:  return "Crater West";
        case 3:  return "Crater South";
        case 4:  return "Crater North";
        case 5:  return "Crater Interior";
        case 6:  return "Desert 1";
        case 7:  return "Desert 2";
        case 8:  return "Desert 3";
        case 9:  return "Saint-Milion / Shambertin / Kondrieu";
        default: return "Area ?";
        }

    case Episode::Ep1:
    default:
        switch (area)
        {
        case 0:  return "Pioneer 2";
        case 1:  return "Forest 1";
        case 2:  return "Forest 2";
        case 3:  return "Cave 1";
        case 4:  return "Cave 2";
        case 5:  return "Cave 3";
        case 6:  return "Mine 1";
        case 7:  return "Mine 2";
        case 8:  return "Ruins 1";
        case 9:  return "Ruins 2";
        case 10: return "Ruins 3";
        case 11: return "Dragon";
        case 12: return "De Rol Le";
        case 13: return "Vol Opt";
        case 14: return "Dark Falz";
        case 15: return "Lobby";
        case 16: return "Battle 1";
        case 17: return "Battle 2";
        default: return "Area ?";
        }
    }
}

// Stat-based override helpers used by ShouldShowItem's hidden-list
// check. An item on the curated hide list (pixelated_mods_hidden.txt) still
// shows if its stats exceed the thresholds below — the goal is to
// hide the 99% of trash drops while surfacing the rare lucky roll
// from the same ID that's worth grabbing.
//
// Weapon logic (tightened again after the first pass was still
// catching stuff like `Gungnir +8` via the grinder exceptional path
// — grinder 8 is trivially achievable on common weapons and nothing
// a drop filter should treat as "always show"):
//
//   1. Count "notable features". A feature is one of:
//        - grinder >= 15
//        - non-zero special
//        - any attribute >= 40%
//        - hit% >= 30
//   2. Show if two or more features are present.
//   3. Or show if any single feature crosses an "exceptional" bar:
//        - grinder >= 30
//        - any attribute >= 50%
//        - hit% >= 40
//
struct WeaponAttrs
{
    int native, abeast, machine, dark, hit;
    uint8_t max_elem;  // highest of native/abeast/machine/dark
};

static WeaponAttrs ExtractWeaponAttrs(const uint8_t b[12])
{
    WeaponAttrs a{};
    for (int i = 0; i < 3; ++i)
    {
        const uint8_t t = b[6 + i * 2];
        const uint8_t v = b[7 + i * 2];
        switch (t)
        {
        case 1: a.native  = v; break;
        case 2: a.abeast  = v; break;
        case 3: a.machine = v; break;
        case 4: a.dark    = v; break;
        case 5: a.hit     = v; break;
        default: break;
        }
    }
    a.max_elem = static_cast<uint8_t>(
        std::max({a.native, a.abeast, a.machine, a.dark}));
    return a;
}

// Grinder 15 / 30 are still below what fully-grinded rares reach
// (most rare weapons cap in the +30..+60 range, boss-gated rares
// higher), so a freshly-dropped rare at +0..+5 still has to earn
// its "show" via attributes or special, not via grinder inflation.
static bool WeaponHasNotableStats(const uint8_t b[12])
{
    const uint8_t grinder = b[3];
    const uint8_t special = b[4] & 0x3F;
    const auto a = ExtractWeaponAttrs(b);

    if (grinder  >= 30)    return true;
    if (a.max_elem >= 50)  return true;
    if (a.hit     >= 40)   return true;

    int features = 0;
    if (grinder    >= 15) ++features;
    if (special    >  0)  ++features;
    if (a.max_elem >= 40) ++features;
    if (a.hit      >= 30) ++features;
    return features >= 2;
}

// Armor threshold: slots >= 4 is the real "notable" line for frames.
// 2-slot frames are common drops from Caves/Forest and shouldn't
// surface through the hide list. DFP/EVP thresholds tightened too —
// the curated hide list applies to base-tier frames that max around
// DFP 10 / EVP 5 naturally, so >= 20 / >= 15 is the lucky-roll bar.
static bool ArmorHasNotableBonus(const uint8_t b[12])
{
    const uint8_t slots = b[5];
    const uint8_t dfp   = b[6];
    const uint8_t evp   = b[8];
    return slots >= 4 || dfp >= 20 || evp >= 15;
}

static bool ShieldHasNotableBonus(const uint8_t b[12])
{
    const uint8_t dfp = b[6];
    const uint8_t evp = b[8];
    return dfp >= 15 || evp >= 10;
}

// Floor item filter. Faithful implementation of BBMod / Solybum Item
// Reader's filter semantics, with two deliberate deviations:
//
//   - Item Reader has no universal rare bypass; aggressive filters can
//     hide rares too. We pass rares through every filter unconditionally
//     because hiding pulsing red rare drops is uniformly worse UX.
//
//   - Item Reader's per-item hide list is strict: an entry with
//     display=false is never shown. Ours uses stat-based overrides
//     so a hidden-by-default item (e.g. Hard Shield) still shows
//     when it rolls notable stats (e.g. Hard Shield [+20 DFP +10 EVP]).
//
// Filter precedence:
//   1. FilterMode::All  -> always show
//   2. Rare              -> always show (deviation from upstream)
//   3. g_hidden_ids hit  -> hide, unless stats are notable (deviation)
//   4. FilterMode::Gear  -> show only equipment types (weapons / armor /
//                           shields / units / mags), hide everything else
//   5. Per-type checks below
// Classify an item against all active filters. Returns HideReason::Visible
// if the item passes, otherwise the first filter it failed. Order matches
// the legacy ShouldShowItem flow — rare-override beats user-hide-list,
// user-hide beats category/mode filters, etc.
static HideReason ClassifyItem(const uint8_t b[12], const uint8_t /*b2*/[4],
                               uint32_t id24)
{
    const uint8_t type = b[0];
    const uint8_t sub  = b[1];

    if (g_filter_mode == FilterMode::All)
        return HideReason::Visible;

    // Rares always pass. Has to be checked BEFORE the hidden-list check
    // so that a rare item the user accidentally added to pixelated_mods_hidden
    // still surfaces.
    if (IsRareItem(b, id24))
        return HideReason::Visible;

    // User-curated hidden list with stat-based overrides.
    if (g_hidden_ids.find(id24) != g_hidden_ids.end())
    {
        if (type == 0x00 && WeaponHasNotableStats(b)) return HideReason::Visible;
        if (type == 0x01 && sub == 0x01 && ArmorHasNotableBonus(b))  return HideReason::Visible;
        if (type == 0x01 && sub == 0x02 && ShieldHasNotableBonus(b)) return HideReason::Visible;
        return HideReason::UserHideList;
    }

    if (g_filter_mode == FilterMode::Gear)
    {
        if (type == 0x00 || type == 0x01 || type == 0x02)
            return HideReason::Visible;
        return HideReason::GearOnlyMode;
    }

    // ===== Notable mode (the default) =====

    // Weapons.
    if (type == 0x00)
    {
        if (g_filter_hide_low_hit_weapons)
        {
            if (ExtractWeaponAttrs(b).hit < g_filter_hit_min)
                return HideReason::LowHitWeapon;
        }
        return HideReason::Visible;
    }

    // Frames / armor.
    if (type == 0x01 && sub == 0x01)
    {
        if (g_filter_hide_low_socket_armor)
        {
            const uint8_t slots = b[5];
            if (slots != 4) return HideReason::LowSocketArmor;
            if (g_filter_hide_four_socket_armor) return HideReason::AllArmorHidden;
        }
        return HideReason::Visible;
    }

    // Shields / barriers.
    if (type == 0x01 && sub == 0x02)
        return HideReason::Visible;

    // Units.
    if (type == 0x01 && sub == 0x03)
    {
        if (g_filter_hide_units) return HideReason::UnitsHidden;
        return HideReason::Visible;
    }

    // Mags.
    if (type == 0x02)
        return HideReason::Visible;

    // Tools.
    if (type == 0x03)
    {
        // Tech disks: sub-type toggle plus level slider.
        if (sub == 0x02)
        {
            if (!g_show_tool_sub[0x02]) return HideReason::ToolCategoryOff;
            const int min_idx = g_tech_disk_min_level - 1;
            if (static_cast<int>(b[2]) < min_idx) return HideReason::TechDiskBelowMin;
            return HideReason::Visible;
        }
        // Event badges 0x12..0x15 share one toggle.
        if (sub >= 0x12 && sub <= 0x15)
            return g_show_tool_sub[0x12] ? HideReason::Visible : HideReason::ToolCategoryOff;
        if (sub < 32)
            return g_show_tool_sub[sub] ? HideReason::Visible : HideReason::ToolCategoryOff;
        return HideReason::Visible;
    }

    // Meseta.
    if (type == 0x04)
        return g_filter_hide_meseta ? HideReason::MesetaHidden : HideReason::Visible;

    return HideReason::Visible;
}

// Short human-readable label for the "Hidden in view" sidebar.
static const char *HideReasonLabel(HideReason r)
{
    switch (r)
    {
    case HideReason::Visible:          return "";
    case HideReason::OtherFloor:       return "other floor";
    case HideReason::TooFar:           return "out of range";
    case HideReason::UserHideList:     return "user hide list";
    case HideReason::GearOnlyMode:     return "Gear-only mode";
    case HideReason::LowHitWeapon:     return "low hit %";
    case HideReason::LowSocketArmor:   return "not 4-slot";
    case HideReason::AllArmorHidden:   return "BBMod: all armor hidden";
    case HideReason::UnitsHidden:      return "units hidden";
    case HideReason::MesetaHidden:     return "meseta hidden";
    case HideReason::ToolCategoryOff:  return "tool category off";
    case HideReason::TechDiskBelowMin: return "below tech-disk min level";
    }
    return "";
}

// Locale-aware item name via the game's own ItemPMT table + unitxt
// group 3. Reverse-engineered from PsoBB.exe:
//
//   * `ItemPMT.prs` global pointer lives at 0x00A8DC94 and resolves
//     (after PRS decompress + vtable init) to a struct whose first
//     fields are pointers into five type-specific record tables:
//       *(pmt + 0x00)  weapon header array  { u32 count; Rec *recs }[237]
//                      records 44 bytes each
//       *(pmt + 0x04)  armor/shield header  { u32 count; Rec *recs }[2]
//                      records 32 bytes each
//       *(pmt + 0x08)  unit table           { u32 count; Rec *recs }
//                      records 20 bytes each
//       *(pmt + 0x0C)  tool header array    { u32 count; Rec *recs }[26]
//                      records 24 bytes each
//       *(pmt + 0x10)  mag table            { u32 count; Rec *recs }
//                      records 28 bytes each
//
//   * Every record type stores its display-name unitxt index as the
//     first u32 (offset 0x00). PSO's in-game item description window
//     calls `unitxt[3][name_idx]` via the accessor at 0x007930A0
//     (`*[UnitxtBase + 0x0C]`), so our UnitxtRead(group=3, ...) hits
//     the same table.
//
//   * Dispatch by the 3-byte (type, subtype, variant) tuple at the
//     start of the 12-byte item data block. Rules exactly match the
//     game's fcn.005d2890:
//       type 0           → weapon
//       type 1, sub 1|2  → armor/shield
//       type 1, sub 3    → unit
//       type 2           → mag
//       type 3           → tool / tech disk
//
// Fails gracefully to the hardcoded text file if any step returns
// null/empty so users with broken ItemPMT pointers (pre-init, wrong
// binary version) still see something.
// Unitxt groups used by the PSO item description builder:
//   group 1 — item base names + weapon special prefixes (the canonical
//             table; this is what fcn.005d2aac in PsoBB.exe reads when
//             rendering an item's in-world tooltip / pickup dialog).
//             Weapon special prefix index = 0x256 + (byte[4] & 0x3F).
//   group 5 — technique names, indexed by the tech_id byte on a tech
//             disk (byte[4]). Ryuker=0x0E, Reverser=0x11 are the only
//             IDs that skip the "Lv.%d" suffix formatting.
//
// Source: r2 analysis of PsoBB.exe — fcn.005d2aac (item name builder),
// fcn.005e4ca0 (special lookup), and the tech-disk format branch at
// 0x5d2de7 that calls 0x793000 (group 5 accessor).
static constexpr uint32_t kItemUnitxtGroup    = 1;
static constexpr uint32_t kTechUnitxtGroup    = 5;
static constexpr uint32_t kSpecialUnitxtBase  = 0x256;   // offset in group 1

static uintptr_t FindItemPmtRecord(const uint8_t b[12])
{
    const uintptr_t pmt = SafeRead<uintptr_t>(pso_offsets::ItemPmtPtr);
    if (pmt == 0) return 0;

    const uint8_t type    = b[0];
    const uint8_t subtype = b[1];
    const uint8_t variant = b[2];

    // Small helper: `header` is one of the per-type arrays whose
    // entries are {u32 variant_count; ItemRec *variants}. Look up
    // the subtype slot, bounds-check the variant, and return the
    // record pointer with the requested stride.
    auto walk = [&](uintptr_t header_base, uint32_t max_sub,
                    uint32_t sub, uint32_t var, uint32_t stride) -> uintptr_t
    {
        if (sub >= max_sub) return 0;
        const uintptr_t slot = header_base + sub * 8;
        const uint32_t  count = SafeRead<uint32_t>(slot);
        if (var >= count) return 0;
        const uintptr_t recs = SafeRead<uintptr_t>(slot + 4);
        if (recs == 0) return 0;
        return recs + var * stride;
    };

    switch (type)
    {
    case 0x00: { // weapons: pmt+0x00, 44-byte records
        const uintptr_t weapons = SafeRead<uintptr_t>(pmt + 0x00);
        if (weapons == 0) return 0;
        return walk(weapons, 237, subtype, variant, 44);
    }

    case 0x01: { // armor / shield / unit
        if (subtype == 0x01 || subtype == 0x02) {
            // Armor (sub=1) and shield (sub=2): pmt+0x04, 32-byte.
            // Array is 2-wide indexed by (sub-1).
            const uintptr_t ax = SafeRead<uintptr_t>(pmt + 0x04);
            if (ax == 0) return 0;
            return walk(ax, 3, subtype, variant, 32);
        }
        if (subtype == 0x03) {
            // Unit: pmt+0x08, 20-byte, flat table.
            const uintptr_t units = SafeRead<uintptr_t>(pmt + 0x08);
            if (units == 0) return 0;
            const uint32_t count = SafeRead<uint32_t>(units);
            if (variant >= count) return 0;
            const uintptr_t recs = SafeRead<uintptr_t>(units + 4);
            if (recs == 0) return 0;
            return recs + variant * 20;
        }
        return 0;
    }

    case 0x02: { // mag: pmt+0x10, 28-byte, flat table indexed by subtype
        const uintptr_t mags = SafeRead<uintptr_t>(pmt + 0x10);
        if (mags == 0) return 0;
        const uint32_t count = SafeRead<uint32_t>(mags);
        if (subtype >= count) return 0;
        const uintptr_t recs = SafeRead<uintptr_t>(mags + 4);
        if (recs == 0) return 0;
        return recs + subtype * 28;
    }

    case 0x03: { // tools / tech disks: pmt+0x0C, 24-byte records
        const uintptr_t tools = SafeRead<uintptr_t>(pmt + 0x0C);
        if (tools == 0) return 0;
        // Tech disks (subtype=0x02) store the TECH LEVEL in byte[2],
        // NOT a variant index. The PMT has a single "Disk" record at
        // variant 0; the level + tech name are appended separately in
        // FormatItem. Without this override, a Lv21 Zonde (b[2]=20)
        // tries to look up variant 20 in the tool table, overshoots
        // the 1-entry "Disk" bucket, and falls back to the raw id24
        // hex display.
        //
        // Non-disk tools (mates, fluids, atomizers, etc.) carry flag
        // bits in the high nibble of b[2] on stack drops — observed
        // b[2] == 0x80 for a plain Monomate which would otherwise be
        // b[2] == 0x00. Mask to the low 4 bits so "variant" is really
        // the mono/di/tri / mini/normal/max distinction.
        const uint32_t var = (subtype == 0x02) ? 0 : (variant & 0x0F);
        return walk(tools, 26, subtype, var, 24);
    }
    }
    return 0;
}

static std::string GetItemUnitxtName(const uint8_t b[12])
{
    const uintptr_t rec = FindItemPmtRecord(b);
    if (rec == 0)
    {
        // One-shot diagnostic: log each distinct (type, sub, var)
        // combination that fails the PMT walk so the user can share
        // the log and we can see whether count==0, recs==null, or
        // var overshoots the record count.
        static std::unordered_set<uint32_t> s_rec_fail_seen;
        const uint32_t key = (uint32_t(b[0]) << 16) |
                             (uint32_t(b[1]) << 8)  |
                              uint32_t(b[2]);
        if (s_rec_fail_seen.size() < 32 &&
            s_rec_fail_seen.insert(key).second)
        {
            PSO_LOG("pmt-miss rec=null type=0x%02X sub=0x%02X "
                    "var=0x%02X (id24=%06X) b[5]=%u",
                    b[0], b[1], b[2], key, b[5]);
        }
        return {};
    }
    const uint32_t name_idx = SafeRead<uint32_t>(rec);
    if (name_idx == 0)
    {
        static std::unordered_set<uint32_t> s_nidx_fail_seen;
        const uint32_t key = (uint32_t(b[0]) << 16) |
                             (uint32_t(b[1]) << 8)  |
                              uint32_t(b[2]);
        if (s_nidx_fail_seen.size() < 32 &&
            s_nidx_fail_seen.insert(key).second)
        {
            PSO_LOG("pmt-miss name_idx=0 type=0x%02X sub=0x%02X "
                    "var=0x%02X (id24=%06X) rec=0x%08X",
                    b[0], b[1], b[2], key, (unsigned)rec);
        }
        return {};
    }
    std::string s = UnitxtRead(kItemUnitxtGroup, name_idx);
    if (s.empty() || s == "Unknown")
    {
        static std::unordered_set<uint32_t> s_unitxt_fail_seen;
        const uint32_t key = (uint32_t(b[0]) << 16) |
                             (uint32_t(b[1]) << 8)  |
                              uint32_t(b[2]);
        if (s_unitxt_fail_seen.size() < 32 &&
            s_unitxt_fail_seen.insert(key).second)
        {
            PSO_LOG("pmt-miss unitxt-empty type=0x%02X sub=0x%02X "
                    "var=0x%02X (id24=%06X) rec=0x%08X name_idx=%u",
                    b[0], b[1], b[2], key, (unsigned)rec, name_idx);
        }
        return {};
    }
    return s;
}

// Weapon special attribute name (Hell, Berserk, Charge, etc.).
// Mirrors PsoBB's fcn.005e4ca0: raw special id lives in the low 6
// bits of byte[4], bit 0x80 is the "untradeable" marker (no display),
// valid special ids are 1..40, and the unitxt index is biased by
// +0x256 inside group 1. Returns empty string when the special slot
// is zero or outside the valid range.
static std::string GetSpecialName(uint8_t special_byte)
{
    if (special_byte & 0x80) return {};         // untradeable: hide
    const uint32_t raw = special_byte & 0x3F;
    if (raw == 0 || raw >= 41) return {};
    std::string s = UnitxtRead(kItemUnitxtGroup, kSpecialUnitxtBase + raw);
    if (s.empty() || s == "Unknown") return {};
    return s;
}

// Tech disk name for the given tech_id byte (item byte[4] on a tech
// disk). Direct index into unitxt group 5; PSO only defines 19 tech
// ids (0..18 = Foie..Megid) so anything else bails out.
static std::string GetTechName(uint8_t tech_id)
{
    if (tech_id >= 19) return {};
    std::string s = UnitxtRead(kTechUnitxtGroup, tech_id);
    if (s.empty() || s == "Unknown") return {};
    return s;
}

static std::string FormatItem(uintptr_t entity_ptr,
                              const uint8_t b[12], const uint8_t b2[4],
                              uint32_t id24)
{
    const uint8_t type = b[0];
    const uint8_t sub  = b[1];

    // Name resolution: ItemPMT walk → unitxt group 3. The game
    // always has PMT loaded by the time a floor item exists, so
    // there is no text-file fallback — if the lookup fails we
    // render the raw id24 hex as a diagnostic so a broken name
    // shows up immediately instead of masquerading as "English
    // text that happened to be in a side file."
    (void)entity_ptr;
    std::string unitxt_name = GetItemUnitxtName(b);

    char id_buf[16];
    std::snprintf(id_buf, sizeof(id_buf), "[%06X]", id24);

    const char *display_name =
          !unitxt_name.empty() ? unitxt_name.c_str()
                               : id_buf;

    char out[160];

    // Meseta is a special case: type byte 0x04, amount stored as a
    // little-endian u32 in the data2 block (record+0x20). We don't
    // bother with thousands separators — keeps the panel narrow.
    if (type == 0x04)
    {
        const uint32_t amount = (uint32_t(b2[0]))      |
                                (uint32_t(b2[1]) << 8) |
                                (uint32_t(b2[2]) << 16)|
                                (uint32_t(b2[3]) << 24);
        std::snprintf(out, sizeof(out), "Meseta %u", amount);
        return std::string(out);
    }

    if (type == 0x00)
    {
        // Weapon: <Special> Name +grinder native/abeast/machine/dark|hit
        const auto a = ExtractWeaponAttrs(b);
        const uint8_t grinder = b[3];

        const std::string special = GetSpecialName(b[4]);
        const char *special_name = special.c_str();

        if (grinder > 0 || a.native || a.abeast || a.machine || a.dark || a.hit)
        {
            std::snprintf(out, sizeof(out), "%s%s%s +%u  %d/%d/%d/%d|%d",
                          special_name, (*special_name ? " " : ""),
                          display_name, grinder,
                          a.native, a.abeast, a.machine, a.dark, a.hit);
        }
        else
        {
            std::snprintf(out, sizeof(out), "%s%s%s",
                          special_name, (*special_name ? " " : ""),
                          display_name);
        }
    }
    else if (type == 0x01 && sub == 0x01)
    {
        // Armor: Name [N slots, +DFP, +EVP]
        std::snprintf(out, sizeof(out), "%s [%u slots +%u DFP +%u EVP]",
                      display_name, b[5], b[6], b[8]);
    }
    else if (type == 0x01 && sub == 0x02)
    {
        // Shield: Name [+DFP, +EVP]
        std::snprintf(out, sizeof(out), "%s [+%u DFP +%u EVP]",
                      display_name, b[6], b[8]);
    }
    else if (type == 0x03 && sub == 0x02)
    {
        // Tech disk — base name ("Disk") from unitxt_item path,
        // tech name ("Foie") from unitxt group 5, level from byte[2].
        const uint8_t level   = b[2];
        const uint8_t tech_id = b[4];
        const std::string tech = GetTechName(tech_id);
        if (!tech.empty())
            std::snprintf(out, sizeof(out), "%s [%s] Lv%u",
                          display_name, tech.c_str(),
                          static_cast<unsigned>(level) + 1u);
        else
            std::snprintf(out, sizeof(out), "%s Lv%u", display_name,
                          static_cast<unsigned>(level) + 1u);
    }
    else if (type == 0x03 && b[5] > 0)
    {
        // Stackable tool with count
        std::snprintf(out, sizeof(out), "%s x%u", display_name, b[5]);
    }
    else
    {
        std::snprintf(out, sizeof(out), "%s", display_name);
    }

    return std::string(out);
}

// Stable hash of an item for "first seen" tracking. Combining area with
// all 12 bytes means two otherwise-identical items on different floors
// get distinct entries, and picking up one copy doesn't affect the flash
// timer on a duplicate in a different slot.
static uint64_t HashFloorItem(int area, const uint8_t bytes[12])
{
    uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a 64-bit offset basis
    h ^= static_cast<uint64_t>(area);
    h *= 0x100000001b3ULL;
    for (int i = 0; i < 12; ++i)
    {
        h ^= static_cast<uint64_t>(bytes[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

// "First seen" tick per known floor item. Populated by GetFloorItems on
// every poll; entries for items no longer on the floor are pruned at the
// end of each call so that if the same item drops again later it flashes
// fresh.
static std::unordered_map<uint64_t, DWORD> g_floor_item_first_seen;

// Items rejected by the user-curated hide list on this frame — separate
// from the main visible list. Rendered as a collapsible "Hidden in view"
// section at the bottom of the floor-items panel so the user can browse
// and unhide individual IDs without editing pixelated_mods_hidden.txt
// by hand. Cleared + repopulated every call to GetFloorItems().
static std::vector<FloorItem> g_hidden_floor_items;

static std::vector<FloorItem> GetFloorItems()
{
    std::vector<FloorItem> out;
    g_hidden_floor_items.clear();

    const uintptr_t ptrs_array =
        SafeRead<uintptr_t>(pso_offsets::FloorItemsArrayPtr);
    const uintptr_t counts_array =
        SafeRead<uintptr_t>(pso_offsets::FloorItemsCountPtr);
    if (ptrs_array == 0 || counts_array == 0) return out;

    // Always read the player's current floor — we need it both for
    // the optional "only current area" filter (when g_filter_current_area
    // is on) and for per-item `is_on_player_floor` annotation so the
    // arrow renderer can skip items that aren't on the same floor.
    // Cross-floor item positions live in a different world coordinate
    // space and the direction vector between them is meaningless.
    const uint32_t player_floor_actual = GetPlayerCurrentFloor();

    // Local player world position for the distance computation and
    // distance filter. If the local player isn't resolvable we skip
    // distance work (leave pos at 0, distances at 0, filter disabled).
    const uintptr_t local_player = GetLocalPlayerPtr();
    float player_x = 0.f, player_z = 0.f;
    bool  have_player_pos = false;
    if (local_player != 0)
    {
        player_x = SafeRead<float>(local_player + pso_offsets::PlayerPosX);
        player_z = SafeRead<float>(local_player + pso_offsets::PlayerPosZ);
        have_player_pos = true;
    }

    // Player facing direction, used by the PlayerRelative arrow mode.
    // Derivation (see comments at g_arrow_mode):
    //   yaw_u16       = read player+0x60 (low 16 bits only)
    //   yaw_rad       = yaw_u16 / 65536 * 2π
    //   forward_world = (sin(yaw_rad), cos(yaw_rad))
    // At yaw=0 this gives (0, 1) = +Z world, which with the verified
    // identity world→screen mapping is "facing screen down."
    // Right-turn observation (yaw decreases on right-turn input)
    // matches: right-turn is CW on screen, and with this formula
    // decreasing yaw rotates the forward vector CW on screen.
    g_arrow_snapshot = ArrowSnapshot{};
    if (have_player_pos)
    {
        const uint16_t yaw_u16 = SafeRead<uint16_t>(
            local_player + pso_offsets::PlayerRotationY);
        const float yaw_rad =
            (static_cast<float>(yaw_u16) / 65536.0f) * 6.2831853f;
        g_arrow_snapshot.have_forward = true;
        g_arrow_snapshot.forward_x    = std::sin(yaw_rad);
        g_arrow_snapshot.forward_z    = std::cos(yaw_rad);
    }

    const DWORD now = GetTickCount();
    std::unordered_map<uint64_t, DWORD> seen_this_call;

    // Whether we're restricting visible items to the player's floor.
    // When true we still PROCESS cross-floor items — they get tagged
    // with HideReason::OtherFloor and surfaced in the hidden sidebar so
    // the user can see at-a-glance what exists elsewhere on the ship.
    const bool restrict_to_current_floor =
        g_filter_current_area && player_floor_actual != 0xFFFFFFFF;

    for (uint32_t area = 0; area < pso_offsets::AreaCount; ++area)
    {
        const uintptr_t floor_items_base =
            SafeRead<uintptr_t>(ptrs_array + area * 4);
        const uint32_t  floor_item_count =
            SafeRead<uint32_t>(counts_array + area * 4);
        if (floor_items_base == 0 || floor_item_count == 0) continue;

        // Cap corrupted count reads.
        const uint32_t safe_count = (floor_item_count >
                                     pso_offsets::MaxItemsPerFloorSanityCap)
            ? pso_offsets::MaxItemsPerFloorSanityCap
            : floor_item_count;

        for (uint32_t j = 0; j < safe_count; ++j)
        {
            const uintptr_t record_base =
                floor_items_base + j * pso_offsets::FloorItemRecordSize;

            // One range check + memcpy for the whole 0x24-byte record,
            // then all the field extraction is in-process. Skips the
            // item entirely on unmapped memory (which happens during
            // area transitions when the arrays briefly point at stale
            // pages).
            uint8_t rec[pso_offsets::FloorItemRecordSize];
            if (!SafeReadBytes(record_base, rec, sizeof(rec))) continue;

            FloorItem fi{};
            fi.area = static_cast<int>(area);
            std::memcpy(&fi.entity_ptr, &rec[0x00], sizeof(uintptr_t));
            std::memcpy(fi.bytes,  &rec[pso_offsets::FloorItemDataOffset], 12);
            std::memcpy(fi.bytes2, &rec[0x20],                              4);
            std::memcpy(&fi.pos_x, &rec[pso_offsets::FloorItemPosXOffset], sizeof(float));
            std::memcpy(&fi.pos_z, &rec[pso_offsets::FloorItemPosZOffset], sizeof(float));

            fi.id24 = (uint32_t(fi.bytes[0]) << 16) |
                      (uint32_t(fi.bytes[1]) << 8)  |
                       uint32_t(fi.bytes[2]);
            if (fi.id24 == 0) continue;

            // Annotate each item with "is the player on the same
            // floor as this item right now". Distance and direction
            // only make sense for same-floor items.
            fi.is_on_player_floor =
                (player_floor_actual != 0xFFFFFFFF) &&
                (static_cast<uint32_t>(fi.area) == player_floor_actual);

            // 2D distance to the local player (PSO movement is
            // effectively XZ-planar, item records only carry X/Z).
            // Also store the normalized (dx, dz) direction for the
            // world-space compass arrow. No yaw math — the direction
            // is a fixed function of the two world positions within
            // a single floor's coordinate space.
            fi.world_dx = 0.f;
            fi.world_dz = 0.f;
            if (have_player_pos && fi.is_on_player_floor)
            {
                const float dx = fi.pos_x - player_x;
                const float dz = fi.pos_z - player_z;
                fi.dist_xz = std::sqrt(dx * dx + dz * dz);
                if (fi.dist_xz > 0.001f)
                {
                    fi.world_dx = dx / fi.dist_xz;
                    fi.world_dz = dz / fi.dist_xz;
                }
            }

            // First-seen tracking. Look up the stamp for this slot
            // (or create a new one now) and record it in the
            // seen_this_call map BEFORE filters run — that way the
            // yellow "new drop" flash is consistent across frames
            // even when filters change mid-session, and the map
            // still prunes items that left the floor at the end.
            const uint64_t h = HashFloorItem(fi.area, fi.bytes);
            auto prev_it = g_floor_item_first_seen.find(h);
            const DWORD item_first_seen =
                (prev_it != g_floor_item_first_seen.end())
                    ? prev_it->second : now;
            seen_this_call[h] = item_first_seen;
            const bool is_rare_item = IsRareItem(fi.bytes, fi.id24);

            // Classify against every filter (cross-floor, distance
            // cap, mode, user hide list, BBMod toggles, tech-disk
            // level, tool category) and stash the first failing
            // reason. Visible items go into the main `out` list;
            // everything else goes into `g_hidden_floor_items` so
            // the sidebar can show the user what's being suppressed
            // and why — at a glance, including items elsewhere on
            // the ship and items out of range.
            HideReason reason = HideReason::Visible;
            if (restrict_to_current_floor && !fi.is_on_player_floor)
            {
                reason = HideReason::OtherFloor;
            }
            else if (g_item_distance_cap_enabled && g_item_max_distance > 0 &&
                     have_player_pos && fi.is_on_player_floor &&
                     fi.dist_xz > static_cast<float>(g_item_max_distance) &&
                     !is_rare_item)
            {
                reason = HideReason::TooFar;
            }
            else
            {
                reason = ClassifyItem(fi.bytes, fi.bytes2, fi.id24);
            }

            fi.is_rare         = is_rare_item;
            fi.display         = FormatItem(fi.entity_ptr, fi.bytes, fi.bytes2, fi.id24);
            fi.first_seen_tick = item_first_seen;
            fi.hide_reason     = reason;

            if (reason == HideReason::Visible)
                out.push_back(std::move(fi));
            else
                g_hidden_floor_items.push_back(std::move(fi));
        }
    }

    // Swap in the pruned map so entries for items no longer on the floor
    // disappear (if the same item drops again later it flashes fresh).
    g_floor_item_first_seen = std::move(seen_this_call);

    // Sort closest-first. Uses a stable sort so items at identical
    // distances keep their original floor / slot ordering, which
    // means multiple drops in the same spot stay grouped consistently
    // across frames instead of shuffling. When the local player
    // position wasn't available dist_xz stays 0 for everything, and
    // the stable sort is effectively a no-op — we still hand back
    // the items in their reader-discovery order.
    if (have_player_pos)
    {
        std::stable_sort(out.begin(), out.end(),
            [](const FloorItem &a, const FloorItem &b) {
                return a.dist_xz < b.dist_xz;
            });
    }

    return out;
}

// ============================================================================
// Overlay rendering
// ============================================================================

static HMODULE g_module = nullptr;

namespace {

enum class Anchor : int {
    TopLeft      = 0,
    TopRight     = 1,
    BottomLeft   = 2,
    BottomRight  = 3,
    TopCenter    = 4,
    BottomCenter = 5,
};

// Tunables — persisted to pixelated_mods.ini next to the DLL.
//
// Two independently-anchored HUD windows since the v1.0 combined panel
// was split: the monster-HP ("reader") panel defaults to the left side
// dropped below the ally party list, and the floor-items panel stays
// where the original combined panel lived (below PSO's in-game minimap
// in the top-right corner).
bool   g_enabled          = true;
bool   g_always_visible   = true;
bool   g_show_monster_hp  = true;
bool   g_show_floor_items = true;
bool   g_show_hp_numbers  = true;
bool   g_show_hp_bar      = true;
float  g_bar_width        = 200.0f;
bool   g_show_count_header = true;

// Monster HP panel position (top-left by default, dropped ~400px to
// clear the stacked ally party portraits in the upper-left corner).
Anchor g_monsters_anchor       = Anchor::TopLeft;
float  g_monsters_window_x     = 10.0f;
float  g_monsters_window_y     = 400.0f;
float  g_monsters_window_alpha = 0.60f;

// Floor items panel position (top-right, below the minimap — where
// the v1.0 combined panel lived).
Anchor g_items_anchor          = Anchor::TopRight;
float  g_items_window_x        = 10.0f;
float  g_items_window_y        = 280.0f;
float  g_items_window_alpha    = 0.60f;

// Boss-parts collapse. When enabled, untargeted entities whose
// display name matches one of the known multi-part-boss names in
// kCollapseBossNames below are folded into a single aggregate row
// with a "(×N)" count suffix and summed HP/MaxHP. Normal enemies
// (Boomas, Canadines, etc.) are never touched — the collapse only
// fires on names in the hardcoded boss set. The currently-targeted
// entity always gets its own row regardless so you can still see
// the HP of whichever boss part you're hitting.
bool g_collapse_boss_parts = true;

// Names whose same-name entities should collapse into aggregate
// rows. PSOBB's multi-part boss list has been frozen for years, so
// this is a small static set rather than a config file.
//
// IMPORTANT: these must be the UNITXT names the game's own string
// table returns, not the community nicknames. The game calls them
// things like "Dal Ra Lie" (community: "Barba Ray"), "Vol Opt"
// and "Vol Opt ver.2" (same), "Gol Dragon" (same), etc. The names
// here must match GetMonsterName() output exactly.
//
// Both Normal/Hard/VH (unitxt group 2) and Ultimate (unitxt group 4)
// Bosses whose parts should collapse into aggregate (×N) rows.
// Keyed by unitxt_id — locale-independent.
static const std::unordered_set<uint32_t> &CollapseBossIds()
{
    static const std::unordered_set<uint32_t> s = {
        pso_offsets::UidDeRolLe,   // 45
        pso_offsets::UidBarbaRay,  // 73
        46,                         // Vol Opt (all sub-types)
        47,                         // Vol Opt ver.2
    };
    return s;
}
// g_hidden_monster_ids and g_hide_unnamed_monsters live at file
// scope (above, near g_hidden_ids) because GetAliveMonsters needs
// them and is defined earlier in the file than this namespace.
bool   g_config_dirty      = false;

// ---- EXP tracker window (separate from monster HP / floor items HUD) ----
// Dedicated to the EXP/hour tracker. Low HP visual feedback moved to the
// status vignette; buffs moved to their own window (g_buff_*). The negative
// X offset keeps it inside the screen edge by ~12 px.
bool   g_xp_window_enabled  = true;
Anchor g_xp_window_anchor   = Anchor::TopRight;
float  g_xp_window_x        = -12.0f;
float  g_xp_window_y        = 30.0f;
float  g_xp_window_alpha    = 0.70f;

// ---- Buff/debuff panel (independent window) ----
// Anchored just to the right of the mag timer (which lives at the
// fixed top-left position 10, 5). Default x=210 leaves enough room
// for the typical mag timer width. Users can drag both reminder
// toggles on for Shifta and Deband — when a reminder is enabled and
// the corresponding buff is NOT active, the window background pulses
// the buff's color (Shifta red, Deband blue). With both enabled and
// both missing, the pulse alternates between red and blue at ~1s
// each so the player sees both colors.
bool   g_buff_panel_enabled  = true;
float  g_buff_panel_x        = 210.0f;
float  g_buff_panel_y        = 5.0f;
float  g_buff_panel_alpha    = 0.70f;
bool   g_buff_shifta_reminder = false;
bool   g_buff_deband_reminder = false;

// ---- Status vignette ----
//
// Pulsing screen-edge tint that fires when any of the tracked
// negative-state conditions are active on the local player. One
// condition: solid color of that condition's hue. Multiple
// conditions: alternates between their colors at 1 Hz so the
// player sees every hue without needing to blend them, matching
// the pattern the Shifta/Deband reminder already uses.
//
// Conditions and hues:
//   Low HP       — red     (threshold reuses the HP alert setting)
//   Frozen       — ice blue
//   Paralyzed    — orange
//   Confused     — pink
//   Poison       — purple
//   Poison       — toxic purple
//   Shocked      — electric yellow-white
//   Slow         — muddy teal
//   Jellen       — mustard yellow
//   Zalure       — sickly green
//
// Detection state: Low HP from HP alert, Jellen/Zalure from the
// BuffState path, and all six ailments (poison/paralyzed/frozen/
// shocked/slow/confused) read directly from player+0x25C and
// player+0x268 — offsets + value map verified by r2 static analysis.
bool  g_vignette_enabled          = true;
bool  g_vignette_low_hp           = true;
bool  g_vignette_poison           = true;
bool  g_vignette_paralyzed        = true;
bool  g_vignette_frozen           = true;
bool  g_vignette_shocked          = true;
bool  g_vignette_slow             = true;
bool  g_vignette_confused         = true;
bool  g_vignette_jellen           = true;
bool  g_vignette_zalure           = true;
float g_vignette_intensity        = 0.55f;  // outer-edge max alpha
float g_vignette_pulse_hz         = 1.4f;   // pulse cycles per second
float g_vignette_thickness_frac   = 0.22f;  // edge band as fraction of half-dimension

// ---- Controller chord overlay ----
//
// Per-slot badges floating over each of PSO's 10 native palette
// cells. Each badge shows which face button triggers that slot under
// the currently-held modifier (A/B/X/Y + slot digit), with the
// trigger color-coded via the badge border:
//   LT = cyan, RT = amber, LT+RT = magenta, unbound = gray
//
// When a chord fires, that slot's badge flashes white and a border
// strobe lights up the underlying slot rect — a click-feedback cue
// PSO's native HUD doesn't provide.
//
// Geometry is calibrated in "1080p base units" and scaled to the
// current render resolution via w/1920 x h/1080. Every base-unit
// value below is a slider in the settings panel so the user can
// line the grid up with their own resolution / aspect.
bool   g_chord_overlay_enabled = true;
float  g_chord_overlay_alpha   = 0.75f;

// 1080p-base calibration. Slot row of the native palette bar at
// 1920x1080. PSO scales the HUD linearly with render size, so a
// single 1080p-base value set works at every resolution via the
// w/1920 × h/1080 scaling applied per frame.
//
// Values were derived by hand-tuning on a 1440p run, then refined
// against a PIL pixel scan of the digit-label row (y=1060) in a
// 1920×1080 ReShade screenshot: the 10 slot-digit glyphs landed
// at wide-digit centers 732, 792, 910, 968, 1028, 1086, 1146, 1205,
// which give a row center of (672.88 + 1205) / 2 ≈ 939 and a
// regressed pitch of 59.12. height / width / badge_gap came from
// live look-and-feel tuning during the same run.
//
// A resolution-preset dropdown was considered and rejected: all
// three presets (1080p, 1440p, 4K) would store identical base
// numbers because the scaling math already handles the cross-
// resolution conversion, so the dropdown would be UI clutter with
// no functional benefit.
float  g_chord_slot_center_x = 939.0f;   // horizontal center of the 10-slot row
float  g_chord_slot_bottom_y = 1072.0f;  // bottom edge of the palette bar
float  g_chord_slot_pitch    = 64.0f;    // stride between slot centers
float  g_chord_slot_width    = 50.0f;    // slot cell width (for border strobe)
float  g_chord_slot_height   = 44.0f;    // slot cell height (for border strobe)
float  g_chord_badge_gap     = 7.0f;     // vertical gap, badge bottom → slot top

// Slot-outline shape for the strobe + calibrate indicator. Glass HUD
// slots are rectangular (rounded corners). Stock PSO HUD slots are
// flat-top hexagons. The user picks the shape to match whichever HUD
// they're running under; value persists to INI and swaps in/out with
// the preset buttons.
//   0 = Rectangle (rounded corners)
//   1 = Hexagon   (flat-top, 6 vertices)
int    g_chord_slot_shape    = 0;

// Per-slot X nudges. Added to the pitch-computed X position for each
// slot so the user can line badges up against a custom HUD whose
// slot cells aren't at the pitch-grid positions vanilla PSO uses.
// All zero = standard pitch-based layout. After nudging, the user
// can hit the "Auto-fit from offsets" button which runs a least-
// squares regression of offset[i] vs slot index, bakes the slope
// and intercept back into g_chord_slot_pitch and g_chord_slot_center_x,
// and zeros the offsets.
float  g_chord_slot_x_offset[10] = {0,0,0,0,0,0,0,0,0,0};

// ---- Right-stick mapping (zoom + target cycle) ----
//
// PSOBB natively ignores the XInput right stick, so we synthesize
// input from it:
//
//   Stick Y (up/down)   → SendInput mouse wheel ±WHEEL_DELTA,
//                          which PSO interprets as camera zoom.
//                          Works in wheel filter mode 0 and 1;
//                          mode 2 (block all) eats the injection.
//
//   Stick X (left/right) → Tab / Shift+Tab via SendInput. Intended
//                          for target cycling; user can rebind to
//                          whatever fits if Tab isn't the right
//                          key for their PSO build.
//
// Axis dominance: the larger-magnitude axis must be >= 1.5x the
// smaller one to "win". Diagonal pushes where both axes are close
// get suppressed entirely so the stick doesn't fire both at once.
// Outer circular deadzone (8000 ≈ 25% of 32767) rejects resting
// drift. Each direction rate-limits independently so a held stick
// produces a steady stream of clicks at g_stick_rate_ms cadence.
bool g_stick_enabled         = true;
bool g_stick_zoom_enabled    = true;   // Y axis → mouse wheel
bool g_stick_invert_y        = false;  // true = flip zoom direction
int  g_stick_deadzone        = 8000;
int  g_stick_rate_ms         = 250;
// X-axis is hardcoded: left push sends PgDn, right push sends PgUp,
// both routed through SendInput with KEYEVENTF_EXTENDEDKEY so Windows
// doesn't confuse them with the NumPad digits that share the scan
// codes. R3 (right thumb click) sends Esc. See controller_chords.cpp
// PollRightStick for the send path.

bool   g_chord_overlay_show_hints    = true;   // dim badges when no trigger held
int    g_chord_overlay_flash_ms      = 220;    // flash animation length
bool   g_chord_overlay_calibrate_mode = false; // draw slot-border rect every frame
                                               // so the user can see the hitboxes
                                               // while dragging sliders

// ---- Mouse-wheel rate limiter (touchpad inertia fix) ----
//
// Windows Precision Touchpads emit a burst of wheel events with
// inertial momentum — one two-finger swipe can dispatch 20-30 small
// deltas over 1-2 seconds after your fingers leave the pad. PSO's
// camera zoom is clamped to 5 levels so it absorbs the burst fine,
// but the menu code auto-repeats while any non-zero wheel delta is
// flowing, and a single swipe runs the selection clean off the end
// of the list.
//
// Wheel filter mode.
//   0 = off       — raw DI passthrough.
//   1 = smart     — magnitude-agnostic quiet-gap. Pass the first
//                   wheel event after at least g_wheel_throttle_ms
//                   of silence; drop everything inside that window.
//                   Touchpad bursts (events every ~33 ms under PSO's
//                   mouse poll period) only get one event per
//                   gesture through. Physical wheel clicks under
//                   ~5 Hz all pass.
//   2 = block_all — drop every wheel event period. Nuclear option
//                   when even one event per gesture is too much,
//                   or for silencing the touchpad entirely in
//                   favour of keyboard navigation.
//
// The earlier magnitude-split theory (sub-WHEEL_DELTA means touchpad,
// >= WHEEL_DELTA means physical wheel click) turned out to be wrong:
// touchpad deltas accumulate across PSO's ~33 ms mouse poll period
// and routinely land at -306, -304, -230 etc., which the magnitude
// filter wrongly passed as "physical clicks". The diagnostic log
// caught the false negatives — see the `wheel-state:` lines in
// pixelated_mods.log around 15:09:54 on 2026-04-15. Pure timing-
// based detection is the right model.
bool g_wheel_filter_enabled = true;
int  g_wheel_filter_mode    = 1;     // smart mode is now magnitude-agnostic
int  g_wheel_throttle_ms    = 200;   // quiet gap (ms) for smart mode

// Debug: dump every wheel event our hooks see to pixelated_mods.log
// for analysis. Bounded to the first N events per process lifetime
// so the log doesn't flood. Defaults ON so we get diagnostics from
// every session without the user having to fiddle with a toggle;
// once the wheel filter is dialled in we can flip it back to off.
bool g_wheel_debug_log = false;

// One row in the monster HP table. Either a single monster (targeted
// or when collapse-by-name is off) or an aggregate of N monsters
// sharing a display name (untargeted, when collapse-by-name is on).
// Safe conversion from signed HP to unsigned display value.
// Negative HP (overkill) clamps to 0.
static inline uint32_t hp_u(int32_t v) { return v > 0 ? static_cast<uint32_t>(v) : 0u; }

struct HpRow
{
    std::string name;       // display name, no count suffix
    uint32_t    hp;         // sum across aggregated members
    uint32_t    max_hp;     // sum across aggregated members
    uint32_t    count;      // 1 for single rows, N for aggregates
    float       dist_xz;   // distance to player (nearest in group)
    float       world_dx;   // direction to nearest member
    float       world_dz;
    bool        is_targeted;
};

// Forward declaration — defined later in the floor items section.
static void DrawFloorItemArrow(float world_dx, float world_dz);

// Build an HpRow with distance/direction data.
static HpRow MakeRow(const std::string &name, uint32_t hp, uint32_t max_hp,
                     uint32_t count, bool targeted,
                     float dist = 0.f, float dx = 0.f, float dz = 0.f)
{
    return {name, hp, max_hp, count, dist, dx, dz, targeted};
}
static HpRow MonsterRow(const Monster &m, bool targeted)
{
    return {m.name, hp_u(m.hp), hp_u(m.max_hp), 1,
            m.dist_xz, m.world_dx, m.world_dz, targeted};
}

// Build the rendered row list from the raw monsters vector.
//
// Rules:
//   - Targeted entity always gets its own row (cyan highlight), never
//     folded or hidden.
//   - For entities in the CollapseBossNames() set, a two-pass approach:
//     1. Find the highest max_hp among all entities sharing that name.
//        If one entity's max_hp is >= 2× the next-highest, it's the
//        "primary body" — only that entity renders as a boss row,
//        and all the smaller sub-parts (monitors, pillars, segments)
//        are hidden. This handles Vol Opt phase 2: the 9500hp body
//        shows, the 350/420/500hp monitors disappear.
//     2. If no single entity dominates (all similar max_hp, like
//        De Rol Le's 11 segments at 20000 each), fall back to the
//        aggregate "(×N)" row with summed HP.
//   - Everything else (normal mobs) renders as individual rows.
//   - When g_collapse_boss_parts is false, skip all boss logic.
// Tracks boss names for which we've shown real body HP. Used to
// suppress the 20000 dummy aggregate during the death animation
// (the body controller entity has hp=0 and is filtered out, but
// the segments linger with dummy HP).
static std::unordered_set<uintptr_t> s_seen_boss_bodies;
// Keyed by the body's cls_meta so it's locale-independent and stable
// across respawns (fresh body entity gets a new address each fight).
static std::unordered_map<uintptr_t, uint32_t> s_pinned_shell_count;

// Pinned peak max_hp for collapse/aggregate rows (Vol Opt Monitor ×24,
// Pillar ×6, any other multi-instance group). Without this, the
// displayed max drains as instances die — Pillar ×6 = 18000/18000
// visibly shrinking to Pillar ×4 = 12000/12000 which reads as "bar at
// 100%" instead of "bar at 66% because a third of the pool is gone."
//
// Keyed on agg_key (cls-or-name composite). Updated to max(pinned,
// frame_total) each frame, then applied back to the row's max_hp.
// The `count` display stays CURRENT (how many are alive right now),
// matching the shell pattern — label reads "Pillar ×4" but max
// remains 18000, so the bar visibly drops to 12000/18000 = 66%.
static std::unordered_map<uint64_t, uint32_t> s_pinned_aggregate_max;

static std::vector<HpRow> BuildHpRows(const std::vector<Monster> &monsters)
{
    std::vector<HpRow> rows;
    if (monsters.empty())
    {
        s_seen_boss_bodies.clear();
        s_pinned_shell_count.clear();
        s_pinned_aggregate_max.clear();
        return rows;
    }

    if (!g_collapse_boss_parts)
    {
        rows.reserve(monsters.size());
        for (const auto &m : monsters)
            rows.push_back(MonsterRow(m, m.is_targeted));
        return rows;
    }

    const auto &collapse_ids = CollapseBossIds();

    // Pre-scan: check if any segment boss is targeted. Match by boss_root
    // (the segment boss body address from the parent_object chain) so we
    // catch any segment entity — body, head, or shell — regardless of
    // its individual cls_meta.
    std::unordered_set<uintptr_t> boss_targeted_roots;
    for (const auto &m : monsters)
        if (m.is_targeted && m.boss_root != 0)
            boss_targeted_roots.insert(m.boss_root);

    rows.reserve(monsters.size());

    // Aggregate index: row grouping key → row index.
    // Default: key on (cls_meta, max_hp) so same-type monsters with
    // the same HP template collapse into one (×N) row.
    // Exception: CollapseByName rows key on the NAME pointer instead
    // so entities with different cls values but the same display
    // name (e.g. Dark Falz Phase 1/Phase 2 across ~20 cls) merge
    // into a single row. Name pointers are stable static strings
    // from entity_cls_table.h, so pointer identity == string identity.
    std::unordered_map<uint64_t, size_t> agg_index;
    auto agg_key = [](const Monster &m) -> uint64_t {
        const EntityType et = LookupEntityType(m.cls_meta);
        if (et.role == EntityRole::CollapseByName && et.synth_name)
        {
            // Top bit set to keep name-keyed rows disjoint from
            // cls-keyed rows (cls values are all <0x01000000, so
            // bit 63 is always free for cls keys). Pointer identity
            // on the synth_name is stable: each is a file-scope static
            // const char*, so same name → same pointer.
            return (1ULL << 63) |
                   reinterpret_cast<uint64_t>(et.synth_name);
        }
        return (static_cast<uint64_t>(m.cls_meta) << 32) |
               static_cast<uint64_t>(hp_u(m.max_hp));
    };

    for (const auto &m : monsters)
    {
        const EntityType etype = LookupEntityType(m.cls_meta);
        const bool is_collapse = (etype.role == EntityRole::CollapseByName) ||
                                 collapse_ids.count(m.unitxt_id) != 0;
        const bool is_segment  = (m.boss_root != 0);

        // Targeted non-segment: own cyan row
        if (m.is_targeted && !is_segment)
        {
            rows.push_back(MonsterRow(m, true));
            continue;
        }

        // Sub-entities (uid=0): aggregate by cls+maxhp into (×N) rows.
        if (m.unitxt_id == 0 && !m.is_targeted)
        {
            const uint64_t key = static_cast<uint64_t>(hp_u(m.max_hp));
            auto it = agg_index.find(key);
            if (it == agg_index.end())
            {
                agg_index[key] = rows.size();
                rows.push_back(MonsterRow(m, false));
            }
            else if (it->second != SIZE_MAX)
            {
                HpRow &r = rows[it->second];
                r.hp     += hp_u(m.hp);
                r.max_hp += hp_u(m.max_hp);
                r.count  += 1;
                if (m.dist_xz < r.dist_xz)
                {
                    r.dist_xz  = m.dist_xz;
                    r.world_dx = m.world_dx;
                    r.world_dz = m.world_dz;
                }
            }
            continue;
        }

        // Normal mob: individual row
        if (!is_collapse)
        {
            rows.push_back(MonsterRow(m, m.is_targeted));
            continue;
        }

        // --- Segment boss (De Rol Le / Barba Ray): boss-pointer path ---
        if (is_segment)
        {
            // Key on boss_root so all segments of one boss share one
            // aggregation slot. First segment we see triggers the boss
            // HP read; subsequent segments with the same root skip.
            const uint64_t key = static_cast<uint64_t>(m.boss_root);
            if (agg_index.count(key))
                continue;  // already handled

            // Read boss HP from the BODY entity (m.boss_root).
            BossHP bhp{};
            const uintptr_t body_cls = SafeRead<uintptr_t>(m.boss_root + pso_offsets::EntityTypeID);
            if (body_cls == pso_offsets::ClsDeRolLeBody)
                bhp = ReadDeRolLeHP(m.boss_root);
            else if (body_cls == pso_offsets::ClsBarbaRayBody)
                bhp = ReadBarbaRayHP(m.boss_root);

            if (bhp.valid)
            {
                if (bhp.body_hp == 0)
                {
                    agg_index[key] = SIZE_MAX;
                }
                else
                {
                    const bool tgt =
                        boss_targeted_roots.count(m.boss_root) != 0;
                    agg_index[key] = rows.size();
                    rows.push_back(MakeRow(m.name + " (Body)",
                                    hp_u(bhp.body_hp), hp_u(bhp.body_max_hp),
                                    1, tgt, m.dist_xz, m.world_dx, m.world_dz));
                    if (bhp.skull_max_hp > 0 && bhp.skull_hp > 0)
                    {
                        rows.push_back(MakeRow(m.name + " (Head)",
                                        hp_u(bhp.skull_hp), hp_u(bhp.skull_max_hp),
                                        1, false, m.dist_xz, m.world_dx, m.world_dz));
                    }
                    if (bhp.shell_max_hp > 0)
                    {
                        uint32_t shell_total = 0;
                        uint32_t shell_count = 0;
                        for (const auto &mm : monsters)
                        {
                            if (mm.boss_root == m.boss_root && mm.shell_hp > 0)
                            {
                                shell_total += mm.shell_hp;
                                shell_count++;
                            }
                        }
                        if (shell_count > 0)
                        {
                            auto &pinned = s_pinned_shell_count[body_cls];
                            if (shell_count > pinned)
                                pinned = shell_count;
                            uint32_t shell_max_total =
                                static_cast<uint32_t>(bhp.shell_max_hp) * pinned;
                            char label[64];
                            std::snprintf(label, sizeof(label),
                                "%s (Shell) \xc3\x97%u",
                                m.name.c_str(), shell_count);
                            rows.push_back(MakeRow(label,
                                            shell_total, shell_max_total,
                                            1, false,
                                            m.dist_xz, m.world_dx, m.world_dz));
                        }
                    }
                    s_seen_boss_bodies.insert(m.boss_root);
                }
                continue;
            }
            // Boss pointer not valid — suppress if previously seen
            if (s_seen_boss_bodies.count(m.boss_root))
            {
                agg_index[key] = SIZE_MAX;
                continue;
            }
            // Fall through to aggregate
        }

        // --- Aggregation (Vol Opt sub-types, other bosses, segment fallback) ---
        const uint64_t key = agg_key(m);
        auto it = agg_index.find(key);
        if (it == agg_index.end())
        {
            agg_index[key] = rows.size();
            rows.push_back(MonsterRow(m, false));
        }
        else if (it->second != SIZE_MAX)
        {
            HpRow &r = rows[it->second];
            r.hp     += hp_u(m.hp);
            r.max_hp += hp_u(m.max_hp);
            r.count  += 1;
            if (m.dist_xz < r.dist_xz)
            {
                r.dist_xz  = m.dist_xz;
                r.world_dx = m.world_dx;
                r.world_dz = m.world_dz;
            }
        }
    }

    // Pin the peak max_hp for multi-instance aggregate rows. The first
    // frame of a fight, all members are alive and we see the full pool
    // — that's our peak. On subsequent frames some members have been
    // killed and dropped from `monsters`, so the summed max_hp shrinks.
    // Without pinning, the bar's denominator moves and visual damage
    // progress resets. Matches the shell-pinning pattern above.
    //
    // Only aggregates (count > 1) need pinning. Single-instance rows
    // carry a stable max_hp from the entity itself.
    for (const auto &kv : agg_index)
    {
        if (kv.second == SIZE_MAX) continue;
        HpRow &r = rows[kv.second];
        if (r.count < 2) continue;
        uint32_t &pinned = s_pinned_aggregate_max[kv.first];
        if (r.max_hp > pinned) pinned = r.max_hp;
        r.max_hp = pinned;
    }

    return rows;
}

void render_hp_table(const std::vector<Monster> &monsters)
{
    if (monsters.empty())
    {
        ImGui::TextDisabled("No enemies in room.");
        return;
    }

    std::vector<HpRow> rows = BuildHpRows(monsters);

    // Pin the currently-targeted row to the top of the list. Stable
    // partition preserves the relative order of everything else, so
    // this is just "move targeted row to front" without disturbing
    // the rest of the enemies panel.
    std::stable_partition(rows.begin(), rows.end(),
        [](const HpRow &r) { return r.is_targeted; });

    // Cap visible rows to prevent overflow off screen
    constexpr size_t kMaxVisibleRows = 18;
    const size_t total_rows = rows.size();
    const bool truncated = total_rows > kMaxVisibleRows;

    if (g_show_count_header)
        ImGui::Text("Enemies: %zu", rows.size());

    // Column widths: Dist / Dir / Bar are tight fixed sizes because
    // their content is bounded ("(9999u)", one arrow glyph, and the
    // worst-case HP string respectively). The Name column auto-sizes
    // to whatever the longest name needs this frame — the window's
    // AlwaysAutoResize + raised max-width constraint lets the whole
    // panel scale side-to-side as enemy names come and go. No more
    // 23-char hard truncation.
    const float char_w = 0.55f * ImGui::GetFontSize();
    const float dist_col_w = 7.0f * char_w;   // "(9999u)"
    const float arrow_col_w = ImGui::GetFontSize() + 4.0f; // single arrow glyph
    const float bar_col_w = ImGui::CalcTextSize("999999 / 999999 (100%)").x + 16.0f;

    if (ImGui::BeginTable("monsters", 4,
                          ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_RowBg))
    {
        // Name column: WidthFixed with init_width=0 → auto-size to the
        // widest row's content.
        ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthFixed, 0.0f);
        ImGui::TableSetupColumn("Dist",  ImGuiTableColumnFlags_WidthFixed, dist_col_w);
        ImGui::TableSetupColumn("Dir",   ImGuiTableColumnFlags_WidthFixed, arrow_col_w);
        ImGui::TableSetupColumn("Bar",   ImGuiTableColumnFlags_WidthFixed, bar_col_w);
        // No header row — layout is self-explanatory.

        size_t row_idx = 0;
        for (const auto &r : rows)
        {
            if (row_idx >= kMaxVisibleRows) break;
            ++row_idx;
            ImGui::TableNextRow();

            // Targeted-monster highlight: fills the whole row with a
            // saturated cyan background (drawn via RowBg1, which sits
            // above the alternating zebra background from
            // ImGuiTableFlags_RowBg) and tints the name text cyan so
            // the row is unmistakable from across the screen.
            if (r.is_targeted)
            {
                const ImU32 hl = IM_COL32(80, 200, 255, 70);
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, hl);
            }

            // Compose the display label — suffix "(xN)" when this is
            // an aggregate row of more than one monster. ASCII 'x'
            // instead of U+00D7 because ReShade's default font has no
            // glyph for it and it renders as '?'.
            char label[192];
            if (r.count > 1)
                std::snprintf(label, sizeof(label), "%s  (x%u)",
                              r.name.c_str(), r.count);
            else
                std::snprintf(label, sizeof(label), "%s", r.name.c_str());

            ImGui::TableSetColumnIndex(0);
            if (r.is_targeted)
            {
                const ImVec4 cyan(0.45f, 0.90f, 1.00f, 1.00f);
                ImGui::TextColored(cyan, "%s", label);
            }
            else
            {
                ImGui::TextUnformatted(label);
            }
            ImGui::TableSetColumnIndex(1);
            if (r.dist_xz > 0.001f)
                ImGui::TextDisabled("(%4.0fu)", r.dist_xz);

            ImGui::TableSetColumnIndex(2);
            if (r.count == 1 && r.dist_xz > 0.001f &&
                (r.world_dx != 0.f || r.world_dz != 0.f))
                DrawFloorItemArrow(r.world_dx, r.world_dz);

            ImGui::TableSetColumnIndex(3);
            if (g_show_hp_bar && r.max_hp > 0)
            {
                const float frac = static_cast<float>(r.hp) /
                                   static_cast<float>(r.max_hp);
                char overlay[64];
                if (g_show_hp_numbers)
                    std::snprintf(overlay, sizeof(overlay), "%u / %u (%.0f%%)",
                                  r.hp, r.max_hp, frac * 100.0f);
                else
                    std::snprintf(overlay, sizeof(overlay), "%.0f%%", frac * 100.0f);

                // Color the bar by HP percentage for at-a-glance
                // threat assessment. ImGui::ProgressBar pulls its
                // fill color from ImGuiCol_PlotHistogram; push a
                // temporary override scoped to this bar only so
                // other ProgressBar users (XP bar etc.) keep the
                // default style.
                ImVec4 bar_color;
                if      (frac > 0.66f) bar_color = ImVec4(0.15f, 0.55f, 0.15f, 1.0f); // dark green
                else if (frac > 0.33f) bar_color = ImVec4(0.65f, 0.55f, 0.10f, 1.0f); // dark yellow
                else                   bar_color = ImVec4(0.70f, 0.15f, 0.15f, 1.0f); // dark red

                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_color);
                // Draw the bar with no built-in overlay, then manually
                // draw the text right-aligned so it stays anchored as
                // the HP number shrinks.
                ImGui::ProgressBar(frac, ImVec2(bar_col_w - 4.0f, 0.0f), "");
                ImGui::PopStyleColor();
                {
                    const ImVec2 text_size = ImGui::CalcTextSize(overlay);
                    const ImVec2 bar_max   = ImGui::GetItemRectMax();
                    const ImVec2 bar_min   = ImGui::GetItemRectMin();
                    const float  text_x    = bar_max.x - text_size.x - 4.0f;
                    const float  text_y    = bar_min.y + (bar_max.y - bar_min.y - text_size.y) * 0.5f;
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(text_x, text_y),
                        IM_COL32(255, 255, 255, 255),
                        overlay);
                }
            }
        }

        ImGui::EndTable();
    }

    if (truncated)
        ImGui::TextDisabled("+%zu more", total_rows - kMaxVisibleRows);
}

// Compute the screen direction (fx, fy) for the compass arrow.
// Screen Y grows downward; the returned vector is unit-length on
// success (the world-space input is already normalized).
//
// In World mode: identity projection — world +X goes screen right,
// world +Z goes screen down. Verified against the in-game minimap.
//
// In PlayerRelative mode: rotate the world direction into the
// player's local frame using the forward vector, so that "ahead of
// the player" is always "up on screen" regardless of which way the
// character is actually facing in the world.
//
//   ahead = forward · delta_world       (component along forward)
//   right = forward_x*dz - forward_z*dx (component perpendicular,
//                                        positive = player's right)
//   screen_fx = right     (positive = right on screen)
//   screen_fy = -ahead    (negative = up on screen)
static void ArrowWorldDirToScreen(ArrowMode mode,
                                  float world_dx, float world_dz,
                                  float *out_fx, float *out_fy)
{
    if (mode == ArrowMode::PlayerRelative && g_arrow_snapshot.have_forward)
    {
        const float fx_w = g_arrow_snapshot.forward_x;
        const float fz_w = g_arrow_snapshot.forward_z;
        const float ahead = fx_w * world_dx + fz_w * world_dz;
        const float right = fx_w * world_dz - fz_w * world_dx;
        *out_fx =  right;
        *out_fy = -ahead;
        return;
    }
    // World mode (default / fallback when forward isn't available).
    *out_fx = world_dx;
    *out_fy = world_dz;
}

// Draw an elongated compass-style arrow next to the current cursor,
// pointing from the player toward an item. The input is the
// normalized 2D world direction (world_dx, world_dz); the arrow's
// screen orientation is determined by g_arrow_mode:
//   World          - fixed minimap-style compass
//   PlayerRelative - rotates with the character's facing
static void DrawFloorItemArrow(float world_dx, float world_dz)
{
    // Caller has already guaranteed finite, non-zero direction.
    ImDrawList *draw = ImGui::GetWindowDrawList();
    if (draw == nullptr) return;

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float line_h  = ImGui::GetTextLineHeight();

    const float pad_x    = 4.0f;
    const float length_2 = line_h * 0.70f;  // half-length tip-to-tail
    const float width_2  = line_h * 0.28f;  // half-width at the tail

    const ImVec2 center(cursor.x + pad_x + length_2,
                        cursor.y + line_h * 0.5f);

    float fx = 0.f, fy = 0.f;
    ArrowWorldDirToScreen(g_arrow_mode, world_dx, world_dz, &fx, &fy);
    const float px = -fy;
    const float py =  fx;

    auto P = [&](float along, float across) {
        return ImVec2(center.x + fx * along + px * across,
                      center.y + fy * along + py * across);
    };
    const ImVec2 tip        = P(+length_2,          0.0f);
    const ImVec2 shoulder_R = P(-length_2 * 0.35f, +width_2);
    const ImVec2 notch      = P(-length_2 * 0.10f,  0.0f);
    const ImVec2 shoulder_L = P(-length_2 * 0.35f, -width_2);

    const ImU32 fill_col    = IM_COL32(240, 240, 120, 235);  // warm yellow
    const ImU32 outline_col = IM_COL32( 40,  40,  40, 255);

    draw->AddTriangleFilled(tip, shoulder_R, notch, fill_col);
    draw->AddTriangleFilled(tip, notch,      shoulder_L, fill_col);

    draw->AddLine(tip,        shoulder_R, outline_col, 1.0f);
    draw->AddLine(shoulder_R, notch,      outline_col, 1.0f);
    draw->AddLine(notch,      shoulder_L, outline_col, 1.0f);
    draw->AddLine(shoulder_L, tip,        outline_col, 1.0f);

    ImGui::Dummy(ImVec2(pad_x + length_2 * 2.0f, line_h));
}

// Persistent disclosure state for the hidden-items list. Tied to the
// clickable "(N) hidden [+/-]" portion of the unified items-panel
// header. Defaults to collapsed so the HUD stays compact on launch —
// the user opens it when they want to browse what's being filtered.
static bool g_hidden_items_expanded = false;

// Forward declaration — render_items_table calls this after the main
// table so the hidden rows render whether or not the visible list is
// empty (fixed prior bug where empty-state early-return skipped it).
static void render_hidden_items_section();

// Draws the unified header at the top of the floor-items window.
// Format examples:
//   "Floor items: 3"                  — visible only, no hidden
//   "Floor items: 3 │ 12 (hidden) ▾"  — both, hidden disclosure open
//   "Floor items: 0 │ 12 (hidden) ▸"  — nothing visible but things hidden
//   (no header rendered at all if nothing visible AND nothing hidden)
//
// The "(N) hidden ▾/▸" portion is a subtle clickable control that
// flips `g_hidden_items_expanded`. Uses disabled-text styling so it
// reads as informational but still has a hover highlight to signal
// interactivity.
static void render_items_header(size_t visible_count, size_t hidden_count)
{
    const bool has_hidden = hidden_count > 0;
    if (visible_count == 0 && !has_hidden) return;

    // "Floor items: N" prefix (honours the existing g_show_count_header
    // toggle — user may have turned it off to free screen space, but
    // the hidden indicator still shows since the user needs that
    // information to know what they're missing).
    bool drew_prefix = false;
    if (g_show_count_header)
    {
        ImGui::Text("Floor items: %zu", visible_count);
        drew_prefix = true;
    }

    if (!has_hidden) return;

    if (drew_prefix)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
    }

    // ASCII-only disclosure markers — ReShade's default font has no
    // Unicode box-drawing or arrow glyphs, those render as '?'.
    char lbl[64];
    std::snprintf(lbl, sizeof(lbl), "%zu (hidden) %s###hidden_toggle",
                  hidden_count,
                  g_hidden_items_expanded ? "[-]" : "[+]");

    // Render the toggle as a button styled to look like TextDisabled —
    // zero-alpha background, faint hover tint for affordance, disabled
    // text colour. Matches the rest of the header's visual weight.
    const ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 25));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(255, 255, 255, 50));
    ImGui::PushStyleColor(ImGuiCol_Text,          dim);
    if (ImGui::SmallButton(lbl))
        g_hidden_items_expanded = !g_hidden_items_expanded;
    ImGui::PopStyleColor(4);
}

void render_items_table(const std::vector<FloorItem> &items)
{
    if (items.empty())
    {
        // Header still renders for the hidden count so the user can
        // see + browse what's being filtered even when nothing's
        // visible. When there's nothing hidden either, fall back to
        // the scope-aware "no items" text so the player gets a
        // meaningful state message rather than a blank window.
        if (g_hidden_floor_items.empty())
        {
            // Three scopes — pick the wording that matches the
            // active filters:
            //   Room    g_filter_current_area = true
            //   Nearby  current_area = false AND distance cap enabled
            //   Global  current_area = false AND distance cap disabled
            const char *empty_text;
            if (g_filter_current_area)
                empty_text = "No items on current floor.";
            else if (g_item_distance_cap_enabled && g_item_max_distance > 0)
                empty_text = "No items nearby.";
            else
                empty_text = "No items on any floor.";
            ImGui::TextDisabled("%s", empty_text);
        }
        else
        {
            render_items_header(0, g_hidden_floor_items.size());
        }
        render_hidden_items_section();
        return;
    }

    render_items_header(items.size(), g_hidden_floor_items.size());

    const DWORD now = GetTickCount();
    // "Show the floor each item is on" is useful whenever we're actually
    // looking at more than one floor's drops — i.e. the per-area filter
    // is off. In that case we append a subdued "(Forest 1)" next to each
    // row so the user can tell at a glance where the item actually is.
    const bool show_area_suffix = !g_filter_current_area;

    if (ImGui::BeginTable("floor_items", 1,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);

        for (const auto &it : items)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            // Colour selection per row: rare > fresh > normal.
            //
            // Rare items pulse red permanently to make red-box drops
            // impossible to miss. Fresh drops tint yellow and fade
            // back to white over a configurable window (currently 8s,
            // long enough to catch a drop while fighting through a
            // pack of enemies). A rare fresh drop is rare AND
            // yellow-tinted, but rare wins for colour because catching
            // a red box is more important than flagging newness.
            constexpr DWORD kFreshFadeMs = 8000;
            const DWORD age_ms  = now - it.first_seen_tick;
            const bool  is_fresh = g_flash_new_drops && age_ms < kFreshFadeMs;

            if (g_blink_rare_items && it.is_rare)
            {
                const float phase = static_cast<float>((now % 1000)) / 1000.0f;
                const float s = 0.5f + 0.5f * std::sin(phase * 6.2831853f);
                const ImVec4 red(1.0f, 0.25f + 0.15f * s, 0.25f + 0.15f * s, 1.0f);
                ImGui::TextColored(red, "%s", it.display.c_str());
            }
            else if (is_fresh)
            {
                const float t = static_cast<float>(age_ms) /
                                static_cast<float>(kFreshFadeMs);
                const ImVec4 yellow(1.0f, 1.0f, 0.3f + 0.7f * t, 1.0f);
                ImGui::TextColored(yellow, "%s", it.display.c_str());
            }
            else
            {
                ImGui::TextUnformatted(it.display.c_str());
            }

            // Hover tooltip: read-only info (id24, override reason, or
            // hint to right-click). ImGui tooltips are non-interactive,
            // so the actual hide/unhide actions live in the right-click
            // popup below.
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextDisabled("%06X", it.id24);
                if (g_hidden_ids.count(it.id24) != 0)
                {
                    const uint8_t type = it.bytes[0];
                    const uint8_t sub  = it.bytes[1];
                    if (IsRareItem(it.bytes, it.id24))
                        ImGui::TextDisabled("Hidden - shown: rare item");
                    else if (type == 0x00 && WeaponHasNotableStats(it.bytes))
                        ImGui::TextDisabled("Hidden - shown: notable weapon stats");
                    else if (type == 0x01 && sub == 0x01 && ArmorHasNotableBonus(it.bytes))
                        ImGui::TextDisabled("Hidden - shown: notable armor stats");
                    else if (type == 0x01 && sub == 0x02 && ShieldHasNotableBonus(it.bytes))
                        ImGui::TextDisabled("Hidden - shown: notable shield stats");
                    else
                        ImGui::TextDisabled("Hidden - shown: filter override");
                }
                else
                {
                    ImGui::TextDisabled("Right-click to hide");
                }
                ImGui::EndTooltip();
            }

            // Right-click context menu: actual hide/unhide action.
            char ctx_id[32];
            std::snprintf(ctx_id, sizeof(ctx_id), "##item_ctx_%06X", it.id24);
            if (ImGui::BeginPopupContextItem(ctx_id))
            {
                ImGui::TextDisabled("%s", it.display.c_str());
                ImGui::Separator();
                if (g_hidden_ids.count(it.id24) != 0)
                {
                    if (ImGui::MenuItem("Unhide this item"))
                    {
                        g_hidden_ids.erase(it.id24);
                        RemoveUserFilter(it.id24);
                    }
                }
                else
                {
                    if (ImGui::MenuItem("Hide this item"))
                    {
                        g_hidden_ids.insert(it.id24);
                        AppendUserFilter(it.id24, it.display.c_str());
                    }
                }
                ImGui::EndPopup();
            }

            if (show_area_suffix)
            {
                ImGui::SameLine();
                if (g_show_item_distance)
                {
                    ImGui::TextDisabled("(%s, %.0fu)",
                                        AreaName(it.area), it.dist_xz);
                }
                else
                {
                    ImGui::TextDisabled("(%s)", AreaName(it.area));
                }
            }
            else if (g_show_item_distance)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%.0fu)", it.dist_xz);
            }

            // Direction arrow, drawn last on the row via ImDrawList
            // so it sits to the right of the distance suffix.
            // Skipped when the direction couldn't be computed
            // (player not resolvable, item on another floor, or
            // item at the player's exact position).
            if (g_show_item_arrow && it.is_on_player_floor &&
                (it.world_dx != 0.f || it.world_dz != 0.f))
            {
                ImGui::SameLine();
                DrawFloorItemArrow(it.world_dx, it.world_dz);
            }
        }

        ImGui::EndTable();
    }

    render_hidden_items_section();
}

// Renders the list of hidden items. Disclosure is owned by the unified
// header's "(N) hidden ▾/▸" toggle (g_hidden_items_expanded), so this
// function just checks the flag and either draws the rows or bails.
// Called after the main visible items table in render_items_table.
static void render_hidden_items_section()
{
    if (g_hidden_floor_items.empty()) return;
    if (!g_hidden_items_expanded) return;

    // Sort: user-hides first (actionable — they get the Unhide
    // button), then by distance within each reason group.
    std::sort(g_hidden_floor_items.begin(), g_hidden_floor_items.end(),
              [](const FloorItem &a, const FloorItem &b) {
                  const int ra = (a.hide_reason == HideReason::UserHideList) ? 0 : 1;
                  const int rb = (b.hide_reason == HideReason::UserHideList) ? 0 : 1;
                  if (ra != rb) return ra < rb;
                  return a.dist_xz < b.dist_xz;
              });

    for (const auto &h : g_hidden_floor_items)
    {
        ImGui::PushID(static_cast<int>(h.id24));
        ImGui::TextDisabled("%s", h.display.c_str());

        // Reason tag — cross-floor shows the actual floor name rather
        // than the generic "other floor" label so the user can tell
        // at a glance where the item is.
        ImGui::SameLine();
        if (h.hide_reason == HideReason::OtherFloor)
            ImGui::TextDisabled("[%s]", AreaName(h.area));
        else
            ImGui::TextDisabled("[%s]", HideReasonLabel(h.hide_reason));

        // Distance + world-direction arrow for same-floor items so
        // the player can actually navigate to something they've
        // decided to unhide. Cross-floor items have no meaningful
        // distance/direction (different coordinate spaces) and get
        // just the area-name tag already rendered above.
        if (h.is_on_player_floor && h.dist_xz > 0.001f)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(%.0fu)", h.dist_xz);

            if (g_show_item_arrow &&
                (h.world_dx != 0.f || h.world_dz != 0.f))
            {
                ImGui::SameLine();
                DrawFloorItemArrow(h.world_dx, h.world_dz);
            }
        }

        // Unhide button for user-hide rows only. Previously tried to
        // right-align via SameLine(0, avail_x - btn_width) but that
        // formed a feedback loop with AlwaysAutoResize — the window
        // grew on every frame until it hit max constraint. Now just
        // flows naturally after the distance tag with default spacing.
        if (h.hide_reason == HideReason::UserHideList)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Unhide"))
            {
                g_hidden_ids.erase(h.id24);
                RemoveUserFilter(h.id24);
            }
        }

        ImGui::PopID();
    }
}

// Kept for the ReShade config-panel preview pane (shows both panels
// inline in the addon's settings screen regardless of in-game anchors).
void render_combined_body()
{
    const bool want_hp    = g_show_monster_hp;
    const bool want_items = g_show_floor_items;

    if (want_hp)
    {
        const auto monsters = GetAliveMonsters();
        render_hp_table(monsters);
    }

    if (want_hp && want_items)
        ImGui::Separator();

    if (want_items)
        render_items_table(GetFloorItems());

    if (!want_hp && !want_items)
        ImGui::TextDisabled("(nothing enabled)");
}

#define DIRTY_IF(x) do { if (x) g_config_dirty = true; } while(0)

void draw_pixelated_mods_overlay(reshade::api::effect_runtime *runtime)
{
    (void)runtime;

    DIRTY_IF(ImGui::Checkbox("Enabled", &g_enabled));
    ImGui::SameLine();
    DIRTY_IF(ImGui::Checkbox("Always visible", &g_always_visible));

    DIRTY_IF(ImGui::Checkbox("Monster HP", &g_show_monster_hp));
    ImGui::SameLine();
    DIRTY_IF(ImGui::Checkbox("Floor items", &g_show_floor_items));
    ImGui::SameLine();
    DIRTY_IF(ImGui::Checkbox("Show section counts", &g_show_count_header));

    if (ImGui::CollapsingHeader("Monster HP"))
    {
        DIRTY_IF(ImGui::Checkbox("HP numbers", &g_show_hp_numbers));
        ImGui::SameLine();
        DIRTY_IF(ImGui::Checkbox("HP bar", &g_show_hp_bar));
        DIRTY_IF(ImGui::Checkbox("Merge boss subparts into one row",
                                 &g_collapse_boss_parts));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Untargeted entities named Vol Opt, Vol Opt ver.2,\n"
                              "Barba Ray, Gol Dragon, Saint-Milion, Shambertin, or\n"
                              "Kondrieu fold into one aggregate row with a (xN) count\n"
                              "suffix and summed HP/MaxHP. Every other monster (Boomas,\n"
                              "Canadines, all normal mobs) renders individually. The\n"
                              "currently-targeted monster always gets its own row.");

        ImGui::TextDisabled("Monster hide list: %zu IDs (pixelated_mods_monster_hidden.txt)",
                            g_hidden_monster_ids.size());
        ImGui::TextDisabled(
            "Add a unitxt_id to that file to hide a specific entity (e.g.");
        ImGui::TextDisabled(
            "Olga Flow's anchored hitbox). Check pixelated_mods.log for 'unnamed");
        ImGui::TextDisabled(
            "monster unitxt_id=N' lines to find the right ID.");
        DIRTY_IF(ImGui::Checkbox(
            "Hide 'Unknown' monsters",
            &g_hide_unnamed_monsters));
        ImGui::TextDisabled(
            "Fallback if you don't want to curate IDs. Default off.");

        DIRTY_IF(ImGui::SliderFloat("Bar width", &g_bar_width, 40.0f, 500.0f, "%.0f"));
    }

    if (ImGui::CollapsingHeader("EXP Tracker"))
    {
        DIRTY_IF(ImGui::Checkbox("Show EXP window", &g_xp_window_enabled));
        if (g_xp_window_enabled)
        {
            int s_anchor_idx = static_cast<int>(g_xp_window_anchor);
            const char *anchor_labels[] = {
                "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right",
                "Top-Center", "Bottom-Center",
            };
            if (ImGui::Combo("EXP anchor", &s_anchor_idx, anchor_labels, 6))
            {
                g_xp_window_anchor = static_cast<Anchor>(s_anchor_idx);
                g_config_dirty = true;
            }
            DIRTY_IF(ImGui::SliderFloat(
                "EXP X offset", &g_xp_window_x, -2000.0f, 2000.0f, "%.0f px"));
            DIRTY_IF(ImGui::SliderFloat(
                "EXP Y offset", &g_xp_window_y, 0.0f, 2000.0f, "%.0f px"));
            DIRTY_IF(ImGui::SliderFloat(
                "EXP BG opacity", &g_xp_window_alpha, 0.0f, 1.0f, "%.2f"));
            DIRTY_IF(ImGui::Checkbox(
                "Track EXP/hour", &g_xp_track_enabled));
            ImGui::TextDisabled(
                "Current = trailing 60s. Session = total gained / wall time.");
        }
    }

    if (ImGui::CollapsingHeader("Low HP Alert"))
    {
        DIRTY_IF(ImGui::Checkbox("Enable low HP alert", &g_hp_alert_enabled));
        if (g_hp_alert_enabled)
        {
            DIRTY_IF(ImGui::SliderInt(
                "Alert threshold (%)", &g_hp_alert_threshold_pct, 5, 90));
            DIRTY_IF(ImGui::Checkbox(
                "Sound on entering danger zone", &g_hp_alert_beep));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Plays pixelated_mods_alert.wav from the add-on directory\n"
                    "if it exists. Fallback is the Windows warning chime\n"
                    "(MessageBeep). Drop your own .wav file with that exact\n"
                    "name next to pixelated_mods.addon32 to customize.");
            ImGui::TextDisabled(
                "Visual feedback is the red status vignette around the screen.");
        }
    }

    if (ImGui::CollapsingHeader("Mag Feed Timer"))
    {
        DIRTY_IF(ImGui::Checkbox(
            "Show mag feed countdown", &g_mag_timer_enabled));
        ImGui::TextDisabled(
            "Shows seconds until next feed; pulses green when ready.");
    }

    if (ImGui::CollapsingHeader("Chord Overlay"))
    {
        DIRTY_IF(ImGui::Checkbox(
            "Show chord badges over palette slots",
            &g_chord_overlay_enabled));
        if (g_chord_overlay_enabled)
        {
        DIRTY_IF(ImGui::SliderFloat(
            "Badge BG opacity",
            &g_chord_overlay_alpha, 0.0f, 1.0f, "%.2f"));
        DIRTY_IF(ImGui::Checkbox(
            "Show hint badges when no trigger held",
            &g_chord_overlay_show_hints));
        DIRTY_IF(ImGui::SliderInt(
            "Fire flash duration (ms)",
            &g_chord_overlay_flash_ms, 60, 600));

        if (ImGui::TreeNode("Palette grid calibration (1080p base)"))
        {
            ImGui::TextDisabled(
                "All values are measured at 1920x1080 and scaled to");
            ImGui::TextDisabled(
                "the actual render resolution automatically. Enable");
            ImGui::TextDisabled(
                "calibrate mode below to see the slot hitboxes.");

            // ---- Built-in presets ----
            // Each preset captures the full geometry stack (centre/
            // pitch/width/height/badge-gap + per-slot x-offsets) for
            // one specific HUD layout. Applying one overwrites the
            // current calibration values. Visual prefs (alpha, hint
            // badges, flash duration) are kept across preset swaps.
            struct ChordPreset {
                const char *name;
                float center_x, bottom_y, pitch, width, height, badge_gap;
                int   shape;   // 0 = Rectangle, 1 = Hexagon
                float offset[10];
            };
            static const ChordPreset kPresets[] = {
                {
                    "Glass HUD (Modern / PSO2-style)",
                    939.0f, 1069.0f, 69.0f, 58.0f, 41.0f, 8.0f,
                    /* shape = */ 0,  // rectangle
                    { 2.0f, -5.0f, -11.0f, -16.4f, -23.1f,
                     -28.6f, -34.0f, -40.1f, -46.3f, -52.3f },
                },
                {
                    // Vanilla PSO flat-top hexagonal action palette.
                    // Values dialed in against the live stock HUD
                    // in Forest 1 at 1440p (values are 1920x1080
                    // base coordinates; scale automatically).
                    "Stock HUD (vanilla PSO)",
                    914.0f, 1068.0f, 63.0f, 53.0f, 44.0f, 10.0f,
                    /* shape = */ 1,  // hexagon
                    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                },
            };
            ImGui::TextDisabled("Presets:");
            for (const auto &p : kPresets)
            {
                char lbl[96];
                std::snprintf(lbl, sizeof(lbl), "Apply: %s", p.name);
                if (ImGui::Button(lbl))
                {
                    g_chord_slot_center_x = p.center_x;
                    g_chord_slot_bottom_y = p.bottom_y;
                    g_chord_slot_pitch    = p.pitch;
                    g_chord_slot_width    = p.width;
                    g_chord_slot_height   = p.height;
                    g_chord_badge_gap     = p.badge_gap;
                    g_chord_slot_shape    = p.shape;
                    for (int i = 0; i < 10; ++i)
                        g_chord_slot_x_offset[i] = p.offset[i];
                    g_config_dirty = true;
                }
            }

            // Manual shape toggle so the user can flip shape without
            // blowing away their tuned dimensions.
            {
                static const char *kShapeLabels[] = { "Rectangle", "Hexagon" };
                if (ImGui::Combo("Slot outline shape",
                                 &g_chord_slot_shape,
                                 kShapeLabels,
                                 static_cast<int>(
                                     sizeof(kShapeLabels) /
                                     sizeof(kShapeLabels[0]))))
                {
                    g_config_dirty = true;
                }
            }
            ImGui::Separator();
            DIRTY_IF(ImGui::Checkbox(
                "Calibrate mode (draw slot hitboxes)",
                &g_chord_overlay_calibrate_mode));
            DIRTY_IF(ImGui::SliderFloat(
                "Slot row center X",
                &g_chord_slot_center_x, 0.0f, 1920.0f, "%.1f px"));
            DIRTY_IF(ImGui::SliderFloat(
                "Slot row bottom Y",
                &g_chord_slot_bottom_y, 0.0f, 1080.0f, "%.1f px"));
            DIRTY_IF(ImGui::SliderFloat(
                "Slot pitch (stride between cells)",
                &g_chord_slot_pitch, 30.0f, 80.0f, "%.1f px"));
            DIRTY_IF(ImGui::SliderFloat(
                "Slot width",
                &g_chord_slot_width, 30.0f, 80.0f, "%.1f px"));
            DIRTY_IF(ImGui::SliderFloat(
                "Slot height",
                &g_chord_slot_height, 30.0f, 80.0f, "%.1f px"));
            DIRTY_IF(ImGui::SliderFloat(
                "Badge -> slot gap",
                &g_chord_badge_gap, 0.0f, 40.0f, "%.1f px"));

            ImGui::Separator();
            if (ImGui::TreeNode("Per-slot X nudge (custom HUDs)"))
            {
                ImGui::TextDisabled(
                    "Shift each badge individually along the row so");
                ImGui::TextDisabled(
                    "you can line every slot up against a custom HUD");
                ImGui::TextDisabled(
                    "whose cells aren't at the vanilla pitch grid.");
                ImGui::TextDisabled(
                    "Once aligned, hit 'Bake offsets to center+pitch'");
                ImGui::TextDisabled(
                    "to regress the 10 positions back into clean");
                ImGui::TextDisabled(
                    "center_x + pitch values and zero the offsets.");

                static const char *kSlotLabels[10] = {
                    "Slot 1 (A1)", "Slot 2 (X2)",
                    "Slot 3 (Y3)", "Slot 4 (B4)",
                    "Slot 5 (A5)", "Slot 6 (X6)",
                    "Slot 7 (Y7)", "Slot 8 (B8)",
                    "Slot 9 (A9)", "Slot 0 (X0)",
                };
                for (int i = 0; i < 10; ++i)
                {
                    DIRTY_IF(ImGui::SliderFloat(
                        kSlotLabels[i],
                        &g_chord_slot_x_offset[i],
                        -100.0f, 100.0f, "%+.1f px"));
                }

                // Least-squares regression of (i, x_i) where
                // x_i = center_x + (i - 4.5) * pitch + offset[i].
                // We fit new_center + (i - 4.5) * new_pitch to those
                // observed positions and bake the result back into
                // the base values, zeroing every offset.
                //
                // With 10 evenly-spaced indices (0..9, i-4.5
                // centered at 0), sum(i-4.5) = 0 and
                // sum((i-4.5)^2) = 82.5, so the fit simplifies:
                //   new_center = mean(x_i)
                //   new_pitch  = sum((i-4.5) * x_i) / 82.5
                if (ImGui::Button("Bake offsets to center+pitch"))
                {
                    float sum_x = 0.0f, sum_ix = 0.0f;
                    for (int i = 0; i < 10; ++i)
                    {
                        const float x_i =
                            g_chord_slot_center_x
                            + (static_cast<float>(i) - 4.5f)
                              * g_chord_slot_pitch
                            + g_chord_slot_x_offset[i];
                        sum_x  += x_i;
                        sum_ix += (static_cast<float>(i) - 4.5f) * x_i;
                    }
                    g_chord_slot_center_x = sum_x / 10.0f;
                    g_chord_slot_pitch    = sum_ix / 82.5f;
                    for (int i = 0; i < 10; ++i)
                        g_chord_slot_x_offset[i] = 0.0f;
                    g_config_dirty = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset nudges to zero"))
                {
                    for (int i = 0; i < 10; ++i)
                        g_chord_slot_x_offset[i] = 0.0f;
                    g_config_dirty = true;
                }
                ImGui::TreePop();
            }

            ImGui::TreePop();
        }

        ImGui::TextDisabled(
            "Each badge shows face button + slot (A1, B2, ...).");
        ImGui::TextDisabled(
            "Border color = trigger: LT cyan, RT amber, LT+RT magenta.");
            ImGui::TextDisabled(
                "RB (palette set 2) is signalled by PSO's own HUD, not here.");
        }
    }

    if (ImGui::CollapsingHeader("Status Vignette"))
    {
        DIRTY_IF(ImGui::Checkbox(
            "Pulse screen edges on status conditions",
            &g_vignette_enabled));
        if (g_vignette_enabled)
        {
            DIRTY_IF(ImGui::SliderFloat(
                "Intensity",
                &g_vignette_intensity, 0.05f, 1.0f, "%.2f"));
            DIRTY_IF(ImGui::SliderFloat(
                "Pulse rate (Hz)",
                &g_vignette_pulse_hz, 0.2f, 4.0f, "%.2f"));
            DIRTY_IF(ImGui::SliderFloat(
                "Edge thickness (fraction)",
                &g_vignette_thickness_frac, 0.05f, 0.5f, "%.2f"));

            ImGui::Spacing();
            ImGui::TextDisabled("Tracked conditions:");
            DIRTY_IF(ImGui::Checkbox("Low HP (red)",           &g_vignette_low_hp));
            DIRTY_IF(ImGui::Checkbox("Poison (toxic purple)",  &g_vignette_poison));
            DIRTY_IF(ImGui::Checkbox("Paralyzed (orange)",     &g_vignette_paralyzed));
            DIRTY_IF(ImGui::Checkbox("Frozen (ice blue)",      &g_vignette_frozen));
            DIRTY_IF(ImGui::Checkbox("Shocked (electric yellow)", &g_vignette_shocked));
            DIRTY_IF(ImGui::Checkbox("Slow (muddy teal)",      &g_vignette_slow));
            DIRTY_IF(ImGui::Checkbox("Confused (pink)",        &g_vignette_confused));
            DIRTY_IF(ImGui::Checkbox("Jellen (mustard)",       &g_vignette_jellen));
            DIRTY_IF(ImGui::Checkbox("Zalure (sickly green)",  &g_vignette_zalure));
            ImGui::TextDisabled(
                "Multiple active conditions cycle one per second.");
        }
    }

    if (ImGui::CollapsingHeader("Right Stick"))
    {
        DIRTY_IF(ImGui::Checkbox(
            "Enable right stick mapping", &g_stick_enabled));
        if (g_stick_enabled)
        {
            DIRTY_IF(ImGui::Checkbox(
                "Right-stick Y zooms camera",
                &g_stick_zoom_enabled));
        if (g_stick_zoom_enabled)
        {
            DIRTY_IF(ImGui::Checkbox(
                "Invert Y (stick up = zoom out)",
                &g_stick_invert_y));
            ImGui::TextDisabled(
                "Requires wheel filter mode 0 or 1. Mode 2 (block all)");
            ImGui::TextDisabled(
                "eats the injected events and stick zoom won't work.");
        }
        ImGui::TextDisabled("Right stick X axis (hardcoded):");
        ImGui::TextDisabled("  push left  -> PgDn");
        ImGui::TextDisabled("  push right -> PgUp");
        ImGui::TextDisabled("R3 (stick press) -> Esc");
        DIRTY_IF(ImGui::SliderInt(
            "Deadzone",
            &g_stick_deadzone, 0, 20000));
        ImGui::TextDisabled(
            "Outer circular threshold the stick must cross before");
        ImGui::TextDisabled(
            "any axis registers. Default 8000 ~= 25%% of stick range.");
        DIRTY_IF(ImGui::SliderInt(
            "Rate (ms between events)",
            &g_stick_rate_ms, 50, 1000));
        ImGui::TextDisabled(
            "How often a held stick fires events. Each direction");
        ImGui::TextDisabled(
            "rate-limits independently. Diagonal pushes are ignored");
        ImGui::TextDisabled(
            "unless one axis is at least 1.5x the other.");
        }
    }

    if (ImGui::CollapsingHeader("Mouse Wheel"))
    {
        DIRTY_IF(ImGui::Checkbox(
            "Filter mouse wheel input",
            &g_wheel_filter_enabled));
        ImGui::TextDisabled(
            "PSOBB's menus scroll one line per wheel tick, but a single");
        ImGui::TextDisabled(
            "touchpad two-finger swipe emits 20-30 micro-ticks, sending");
        ImGui::TextDisabled(
            "the menu cursor flying off the end of the list. The filter");
        ImGui::TextDisabled(
            "drops or throttles those ticks before they reach the game.");
    if (g_wheel_filter_enabled)
    {
        const char *mode_labels[] = {
            "Off (passthrough)",
            "Smart (one event per gesture)",
            "Block all wheel input"
        };
        int mode = g_wheel_filter_mode;
        if (mode < 0 || mode > 2) mode = 1;
        if (ImGui::Combo("Wheel filter mode", &mode, mode_labels, 3))
        {
            g_wheel_filter_mode = mode;
            g_config_dirty = true;
        }

        if (g_wheel_filter_mode == 1)
        {
            DIRTY_IF(ImGui::SliderInt(
                "Quiet gap (ms)",
                &g_wheel_throttle_ms, 0, 1500));
            ImGui::TextDisabled(
                "Only the first wheel event after N ms of silence");
            ImGui::TextDisabled(
                "passes; everything within the window is blocked.");
            ImGui::TextDisabled(
                "Magnitude-agnostic - catches touchpad accumulated");
            ImGui::TextDisabled(
                "deltas that sum above 120 across a poll period.");
        }
        else if (g_wheel_filter_mode == 2)
        {
            ImGui::TextDisabled(
                "Every mouse-wheel event is dropped unconditionally.");
            ImGui::TextDisabled(
                "Touchpad scroll and physical mouse wheel both die;");
            ImGui::TextDisabled(
                "use keyboard for menu navigation and +/- for camera");
            ImGui::TextDisabled(
                "zoom. Nuclear fallback if smart mode isn't enough.");
        }

        DIRTY_IF(ImGui::Checkbox(
            "Log wheel events (diagnostic)",
            &g_wheel_debug_log));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Logs every wheel event our DI hooks observe, with\n"
                "the lZ value and filter decision. Toggle on, swipe\n"
                "the touchpad a few times, toggle off, then send the\n"
                "log. Useful for diagnosing why a given input path\n"
                "isn't being caught by the filter.");
    }
    }

    if (ImGui::CollapsingHeader("Buff Panel"))
    {
        DIRTY_IF(ImGui::Checkbox(
            "Show buff/debuff panel", &g_buff_panel_enabled));
        if (g_buff_panel_enabled)
        {
        DIRTY_IF(ImGui::SliderFloat(
            "Buff X offset", &g_buff_panel_x, 0.0f, 2000.0f, "%.0f px"));
        DIRTY_IF(ImGui::SliderFloat(
            "Buff Y offset", &g_buff_panel_y, 0.0f, 2000.0f, "%.0f px"));
        DIRTY_IF(ImGui::SliderFloat(
            "Buff BG opacity", &g_buff_panel_alpha, 0.0f, 1.0f, "%.2f"));

        DIRTY_IF(ImGui::Checkbox(
            "Shifta reminder (red pulse when missing)",
            &g_buff_shifta_reminder));
        DIRTY_IF(ImGui::Checkbox(
            "Deband reminder (blue pulse when missing)",
            &g_buff_deband_reminder));
        ImGui::TextDisabled(
            "When both reminders are on and both buffs are missing,");
        ImGui::TextDisabled(
            "the panel alternates between red and blue.");
        }
    }

    if (ImGui::CollapsingHeader("Floor Items"))
    {
        int mode_idx = static_cast<int>(g_filter_mode);
        const char *mode_labels[] = {
            "Notable (hide common consumables)",
            "All items (no filtering)",
            "Gear only (weapons, armor, mags)",
        };
        if (ImGui::Combo("Filter mode", &mode_idx, mode_labels, 3))
        {
            g_filter_mode = static_cast<FilterMode>(mode_idx);
            g_config_dirty = true;
        }

        DIRTY_IF(ImGui::Checkbox(
            "Only show items on my current floor", &g_filter_current_area));
        DIRTY_IF(ImGui::Checkbox(
            "Flash new drops (yellow for 8s)", &g_flash_new_drops));
        DIRTY_IF(ImGui::Checkbox(
            "Pulse rare item drops", &g_blink_rare_items));

        DIRTY_IF(ImGui::Checkbox(
            "Show distance to each item", &g_show_item_distance));
        DIRTY_IF(ImGui::Checkbox(
            "Auto-hide items past distance cap", &g_item_distance_cap_enabled));
        if (!g_item_distance_cap_enabled) ImGui::BeginDisabled();
        DIRTY_IF(ImGui::SliderInt(
            "Distance cap (world units)",
            &g_item_max_distance, 100, 3000));
        if (!g_item_distance_cap_enabled) ImGui::EndDisabled();
        ImGui::TextDisabled(
            "Self-declutters as you move forward; items reappear when you");
        ImGui::TextDisabled(
            "walk back into range. Rares always bypass the cap.");

        DIRTY_IF(ImGui::Checkbox(
            "Show direction arrow to each item", &g_show_item_arrow));
        if (g_show_item_arrow)
        {
            int arrow_idx = static_cast<int>(g_arrow_mode);
            const char *arrow_labels[] = {
                "World (fixed minimap-style compass)",
                "Player-relative (rotates with character)",
            };
            if (ImGui::Combo("Arrow mode", &arrow_idx, arrow_labels, 2))
            {
                g_arrow_mode = static_cast<ArrowMode>(arrow_idx);
                g_config_dirty = true;
            }
        }

        // BBMod / Item Reader compatible opt-in filters. All default OFF
        // to match Solybum Item Reader's defaults — flip on the ones you
        // want for stricter filtering. Rare items always pass through
        // these regardless (we deviate from Item Reader on this point so
        // pulsing red rares can never be hidden).
        if (ImGui::TreeNode("BBMod-style filters (opt-in, all default off)"))
        {
            ImGui::TextDisabled(
                "Matches Solybum Item Reader's filter toggles. Rares always");
            ImGui::TextDisabled(
                "pass these regardless. Use pixelated_mods_hidden.txt to hide");
            ImGui::TextDisabled(
                "specific item IDs by hand.");

            DIRTY_IF(ImGui::Checkbox(
                "Hide low-hit weapons", &g_filter_hide_low_hit_weapons));
            if (g_filter_hide_low_hit_weapons)
            {
                DIRTY_IF(ImGui::SliderInt(
                    "Min hit %", &g_filter_hit_min, 0, 100));
            }

            DIRTY_IF(ImGui::Checkbox(
                "Hide low-socket frames (only show 4-slot)",
                &g_filter_hide_low_socket_armor));
            if (g_filter_hide_low_socket_armor)
            {
                DIRTY_IF(ImGui::Checkbox(
                    "Also hide 4-slot frames (hides ALL frames)",
                    &g_filter_hide_four_socket_armor));
            }

            DIRTY_IF(ImGui::Checkbox(
                "Hide all units", &g_filter_hide_units));
            DIRTY_IF(ImGui::Checkbox(
                "Hide meseta drops", &g_filter_hide_meseta));

            ImGui::TextDisabled("Hidden by name list: %zu entries (pixelated_mods_hidden.txt)",
                                g_hidden_ids.size());
            ImGui::TreePop();
        }

        if (g_filter_mode == FilterMode::Notable)
        {
            DIRTY_IF(ImGui::SliderInt(
                "Technique disk min Lv", &g_tech_disk_min_level, 1, 30));
            ImGui::TextDisabled("Tech disks below this level are hidden.");

            // Collapsing header so the 13 checkboxes don't dominate the panel.
            // Only relevant in Notable mode — in All/Gear modes these flags
            // aren't consulted.
            if (ImGui::TreeNode("Tool categories (Notable mode)"))
            {
                ImGui::TextDisabled(
                    "Uncheck a category to hide it from the Notable list.");
                for (int sub = 0; sub < 32; ++sub)
                {
                    const char *label = kToolSubLabels[sub];
                    if (label == nullptr) continue;  // slot has no exposed toggle
                    if (ImGui::Checkbox(label, &g_show_tool_sub[sub]))
                        g_config_dirty = true;
                    // Two checkboxes per row to keep the list compact.
                    if ((sub & 1) == 0) ImGui::SameLine(180.0f);
                }
                ImGui::NewLine();
                ImGui::TreePop();
            }
        }
        else
        {
            ImGui::TextDisabled(
                "Tool category filters apply only in Notable mode.");
        }
    }

    static const char *kAnchorLabels[] = {
        "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right",
        "Top-Center", "Bottom-Center",
    };

    if (ImGui::CollapsingHeader("Monster HP panel position"))
    {
        int anchor_idx = static_cast<int>(g_monsters_anchor);
        if (ImGui::Combo("Anchor##monsters", &anchor_idx, kAnchorLabels, 6))
        {
            g_monsters_anchor = static_cast<Anchor>(anchor_idx);
            g_config_dirty = true;
        }

        if (ImGui::Button("Preset: Left, below allies (default)"))
        {
            g_monsters_anchor  = Anchor::TopLeft;
            g_monsters_window_x = 10.0f;
            g_monsters_window_y = 400.0f;
            g_config_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Preset: Top-Right##monsters"))
        {
            g_monsters_anchor  = Anchor::TopRight;
            g_monsters_window_x = 20.0f;
            g_monsters_window_y = 20.0f;
            g_config_dirty = true;
        }

        DIRTY_IF(ImGui::SliderFloat("X offset##monsters", &g_monsters_window_x,    0.0f, 2000.0f, "%.0f px"));
        DIRTY_IF(ImGui::SliderFloat("Y offset##monsters", &g_monsters_window_y,    0.0f, 2000.0f, "%.0f px"));
        DIRTY_IF(ImGui::SliderFloat("BG opacity##monsters", &g_monsters_window_alpha, 0.0f, 1.0f, "%.2f"));
    }

    if (ImGui::CollapsingHeader("Floor items panel position"))
    {
        int anchor_idx = static_cast<int>(g_items_anchor);
        if (ImGui::Combo("Anchor##items", &anchor_idx, kAnchorLabels, 6))
        {
            g_items_anchor = static_cast<Anchor>(anchor_idx);
            g_config_dirty = true;
        }

        if (ImGui::Button("Preset: Below Minimap (default)"))
        {
            g_items_anchor  = Anchor::TopRight;
            g_items_window_x = 10.0f;
            g_items_window_y = 280.0f;
            g_config_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Preset: Bottom-Right##items"))
        {
            g_items_anchor  = Anchor::BottomRight;
            g_items_window_x = 20.0f;
            g_items_window_y = 180.0f;
            g_config_dirty = true;
        }

        DIRTY_IF(ImGui::SliderFloat("X offset##items", &g_items_window_x,    0.0f, 2000.0f, "%.0f px"));
        DIRTY_IF(ImGui::SliderFloat("Y offset##items", &g_items_window_y,    0.0f, 2000.0f, "%.0f px"));
        DIRTY_IF(ImGui::SliderFloat("BG opacity##items", &g_items_window_alpha, 0.0f, 1.0f, "%.2f"));
    }

    ImGui::Separator();
    if (chords::RenderConfigPanelSection())
        g_config_dirty = true;

    if (g_config_dirty)
        SaveConfig();

    ImGui::Separator();
    ImGui::TextDisabled("Preview (live):");
    ImGui::Separator();

    if (!g_enabled)
    {
        ImGui::TextDisabled("(overlay disabled)");
        return;
    }

    render_combined_body();
}

#undef DIRTY_IF

// Compute a window's anchored position + pivot from its anchor enum and
// user-supplied X/Y offset. Pivots are chosen so the offset is measured
// from the nearest screen edge (left/right/top/bottom) for corner anchors
// and from the horizontal midpoint for top/bottom-center anchors.
static void compute_anchor_pos(Anchor anchor, float off_x, float off_y,
                               float display_w, float display_h,
                               ImVec2 *out_pos, ImVec2 *out_pivot)
{
    switch (anchor)
    {
    case Anchor::TopLeft:
        *out_pos   = ImVec2(off_x, off_y);
        *out_pivot = ImVec2(0.0f, 0.0f);
        break;
    case Anchor::TopRight:
        *out_pos   = ImVec2(display_w - off_x, off_y);
        *out_pivot = ImVec2(1.0f, 0.0f);
        break;
    case Anchor::BottomLeft:
        *out_pos   = ImVec2(off_x, display_h - off_y);
        *out_pivot = ImVec2(0.0f, 1.0f);
        break;
    case Anchor::BottomRight:
        *out_pos   = ImVec2(display_w - off_x, display_h - off_y);
        *out_pivot = ImVec2(1.0f, 1.0f);
        break;
    case Anchor::TopCenter:
        *out_pos   = ImVec2(display_w * 0.5f + off_x, off_y);
        *out_pivot = ImVec2(0.5f, 0.0f);
        break;
    case Anchor::BottomCenter:
        *out_pos   = ImVec2(display_w * 0.5f + off_x, display_h - off_y);
        *out_pivot = ImVec2(0.5f, 1.0f);
        break;
    }
}

// Directory the add-on DLL lives in (with trailing backslash) — used
// to resolve the config file, log file, data files, and the low-HP
// alert WAV. Populated by ResolveAddonPaths() during DllMain attach,
// cleared to a relative "./" fallback if GetModuleFileNameA fails.
// Defined up here so PlayLowHpAlertSound() below can reference it
// without a forward-declare dance.
static std::string g_addon_dir;

// Fire the audible low-HP alert. Plays pixelated_mods_alert.wav from
// next to the add-on DLL if the file exists, otherwise falls back to
// the system warning ding via MessageBeep. PlaySound runs async so
// the render thread doesn't block on audio I/O.
//
// Users can replace the WAV with any short clip they like by dropping
// a file of the same name next to pixelated_mods.addon32. If the file
// is missing the fallback keeps the alert functional.
static void PlayLowHpAlertSound()
{
    if (!g_addon_dir.empty())
    {
        const std::string wav = g_addon_dir + "pixelated_mods_alert.wav";
        const DWORD flags =
            SND_FILENAME | SND_ASYNC | SND_NODEFAULT | SND_NOWAIT;
        if (PlaySoundA(wav.c_str(), nullptr, flags))
            return;
    }
    MessageBeep(MB_ICONWARNING);
}

// Snapshot of the player HP alert state for a single frame. CheckLowHpAlert
// both reports whether the alert is currently firing AND edge-beeps on
// the safe→danger transition if audio alerts are enabled.
struct HpAlertState { bool firing; uint16_t hp; uint16_t max_hp; int pct; };

static HpAlertState CheckLowHpAlert()
{
    HpAlertState s{};
    if (!g_hp_alert_enabled) return s;
    const PlayerHP hp = GetLocalPlayerHP();
    s.hp     = hp.hp;
    s.max_hp = hp.max_hp;
    if (hp.max_hp == 0) return s;  // not in a game instance
    s.pct = (static_cast<int>(hp.hp) * 100) / hp.max_hp;
    s.firing = s.pct <= g_hp_alert_threshold_pct;

    static bool s_was_in_danger = false;
    if (s.firing && !s_was_in_danger && g_hp_alert_beep)
        PlayLowHpAlertSound();
    s_was_in_danger = s.firing;
    return s;
}

// Snapshot of the player's two technique-buff slots. Used by both the
// buff window renderer and the reminder pulse logic.
struct BuffState {
    bool shifta_active = false;
    bool deband_active = false;
    bool jellen_active = false;
    bool zalure_active = false;
    int  shifta_secs = 0;
    int  deband_secs = 0;
    int  jellen_secs = 0;
    int  zalure_secs = 0;
    int  shifta_level = 0;
    int  deband_level = 0;
    int  jellen_level = 0;
    int  zalure_level = 0;
    // Raw tech_type enum values captured from the buff slot — used
    // by render_buff_body to pull the localized name from unitxt
    // group 5 (technique names) instead of hardcoded English.
    uint32_t shifta_type = 0;
    uint32_t deband_type = 0;
    uint32_t jellen_type = 0;
    uint32_t zalure_type = 0;
};

static BuffState GetBuffState()
{
    BuffState s;
    const uintptr_t p = GetLocalPlayerPtr();
    if (p == 0) return s;

    // Two technique slots: 0 = attack (Shifta/Jellen), 1 = defense
    // (Deband/Zalure). Each slot is 12 bytes:
    //   +0x00  type   u32   (9=Shifta, 10=Deband, 11=Jellen, 12=Zalure)
    //   +0x04  mult   f32   (level multiplier; level = round((|mult|*100-10)/1.30) + 1)
    //   +0x08  frames u32   (remaining frames at 30 fps)
    for (int i = 0; i < 2; ++i)
    {
        const uint32_t type   = SafeRead<uint32_t>(p + 0x274 + i * 12);
        const float    mult   = SafeRead<float>(p + 0x278 + i * 12);
        const uint32_t frames = SafeRead<uint32_t>(p + 0x27C + i * 12);
        if (type == 0 || frames == 0) continue;
        const int secs  = static_cast<int>(frames / 30);
        const int level = static_cast<int>(
            (std::abs(mult) * 100.0f - 10.0f) / 1.30f + 0.5f) + 1;
        switch (type)
        {
        case  9: s.shifta_active = true; s.shifta_secs = secs; s.shifta_level = level; s.shifta_type = type; break;
        case 10: s.deband_active = true; s.deband_secs = secs; s.deband_level = level; s.deband_type = type; break;
        case 11: s.jellen_active = true; s.jellen_secs = secs; s.jellen_level = level; s.jellen_type = type; break;
        case 12: s.zalure_active = true; s.zalure_secs = secs; s.zalure_level = level; s.zalure_type = type; break;
        }
    }
    return s;
}

// Render the buff window body. Returns true if any buff text was drawn.
static bool render_buff_body(const BuffState &b)
{
    bool any = false;
    auto row = [&](const char *name, int level, int secs, ImVec4 color) {
        if (any) ImGui::SameLine();
        ImGui::TextColored(color, "%s%d:%ds", name, level, secs);
        any = true;
    };
    // The buff-slot type enum (9=Shifta, 10=Deband, 11=Jellen,
    // 12=Zalure) does NOT match unitxt group 5 tech_ids one-to-one.
    // Group 5 is the standard sequential PSO tech-name table:
    //
    //   tech_id  group 5 name
    //   ───────  ────────────
    //      6     Zonde                (confirmed via tech disc display)
    //      9     Grants
    //     10     Megid
    //     11     Shifta
    //     12     Deband
    //     13     Jellen
    //     14     Zalure
    //
    // So buff_type 9 (Shifta) must look up tech_id 11, not 9. Prior
    // code keyed UnitxtRead(5, buff_type) directly, which returned
    // "Grants" once the Ep2 tech table was populated (Ultimate).
    auto buff_name = [](uint32_t buff_type, const char *fallback) {
        uint32_t tech_id;
        switch (buff_type) {
        case  9: tech_id = 11; break;  // Shifta
        case 10: tech_id = 12; break;  // Deband
        case 11: tech_id = 13; break;  // Jellen
        case 12: tech_id = 14; break;  // Zalure
        default: return std::string(fallback);
        }
        std::string n = UnitxtRead(5, tech_id);

        // One-shot diagnostic per buff_type: log what we looked up
        // and what came back so a future mismatch surfaces in the log
        // instead of silently showing the wrong tech name.
        static std::unordered_set<uint32_t> s_buff_logged;
        if (s_buff_logged.size() < 8 &&
            s_buff_logged.insert(buff_type).second)
        {
            PSO_LOG("buff-name buff_type=%u → tech_id=%u → name='%s' "
                    "fallback='%s'",
                    buff_type, tech_id, n.c_str(), fallback);
        }

        if (!n.empty() && n != "Unknown") return n;
        return std::string(fallback);
    };

    if (b.shifta_active)
        row(buff_name(b.shifta_type, "Shifta").c_str(),
            b.shifta_level, b.shifta_secs, ImVec4(1.f, 0.4f, 0.4f, 1.f));
    if (b.deband_active)
        row(buff_name(b.deband_type, "Deband").c_str(),
            b.deband_level, b.deband_secs, ImVec4(0.4f, 0.4f, 1.f, 1.f));
    if (b.jellen_active)
        row(buff_name(b.jellen_type, "Jellen").c_str(),
            b.jellen_level, b.jellen_secs, ImVec4(0.4f, 1.f, 0.4f, 1.f));
    if (b.zalure_active)
        row(buff_name(b.zalure_type, "Zalure").c_str(),
            b.zalure_level, b.zalure_secs, ImVec4(1.f, 1.f, 0.4f, 1.f));
    return any;
}

// ---- Status vignette state + detection + rendering ----

struct StatusState {
    bool low_hp;
    bool poison;     // permanent ailment, +0x25C == 0x0F
    bool paralyzed;  // permanent ailment, +0x25C == 0x10
    bool frozen;     // temporary, +0x268 == 0x02
    bool shocked;    // temporary, +0x268 == 0x03
    bool slow;       // temporary, +0x268 == 0x11
    bool confused;   // temporary, +0x268 == 0x12
    bool jellen;     // from BuffState
    bool zalure;     // from BuffState
};

// Read the tracked negative-state conditions off the local player.
//
// Low HP, Jellen, Zalure come from existing HP alert + BuffState paths.
//
// Ailments come from two status-effect dwords on the player struct.
// Offsets + value map verified by r2 static analysis of PsoBB.exe
// (apply/cure fns in 0x00778000 - 0x00779C00 range; see also the
// multi-level cure cascade at 0x00779868 which reads every value
// we care about in sequence — that one is the ground truth).
//
// Solylib reads these as byte compares; the game itself writes them
// as dwords so we do the same and the high bytes are zero.
//
//   player + 0x25C (dword) : mutually-exclusive "permanent" slot
//                              0x0F = POISON      (cured by Antidote)
//                              0x10 = PARALYSIS   (cured by Antiparalysis)
//                              Paralysis wins if both would apply.
//
//   player + 0x268 (dword) : mutually-exclusive "temporary" slot
//                              0x02 = FROZEN     (~2.5 s)
//                              0x03 = SHOCKED    (~5 s)
//                              0x11 = SLOW       (~3.5 s)
//                              0x12 = CONFUSED   (~5 s)
static constexpr uintptr_t kStatusPermOff = 0x25C;
static constexpr uintptr_t kStatusTempOff = 0x268;

static constexpr uint32_t  kStatusPoison    = 0x0F;
static constexpr uint32_t  kStatusParalyzed = 0x10;

static constexpr uint32_t  kStatusFrozen    = 0x02;
static constexpr uint32_t  kStatusShocked   = 0x03;
static constexpr uint32_t  kStatusSlow      = 0x11;
static constexpr uint32_t  kStatusConfused  = 0x12;

static StatusState GetStatusState(const HpAlertState &hp_alert,
                                  const BuffState &buffs)
{
    StatusState s{};
    s.low_hp = hp_alert.firing;
    s.jellen = buffs.jellen_active;
    s.zalure = buffs.zalure_active;

    const uintptr_t p = GetLocalPlayerPtr();
    if (p != 0)
    {
        const uint32_t perm = SafeRead<uint32_t>(p + kStatusPermOff);
        const uint32_t temp = SafeRead<uint32_t>(p + kStatusTempOff);
        s.poison    = (perm == kStatusPoison);
        s.paralyzed = (perm == kStatusParalyzed);
        s.frozen    = (temp == kStatusFrozen);
        s.shocked   = (temp == kStatusShocked);
        s.slow      = (temp == kStatusSlow);
        s.confused  = (temp == kStatusConfused);
    }
    return s;
}

// Collect the set of currently-active vignette hues for the
// enabled conditions. Each entry is ImU32 premultiplied for the
// multi-color alpha blend. Returned vector is empty when nothing
// is firing — caller early-outs.
static constexpr int kVignetteMaxColors = 9;

static void CollectVignetteColors(const StatusState &ss,
                                  ImU32 *out, int *out_count)
{
    int n = 0;
    auto push = [&](ImU32 c, bool enabled, bool active) {
        if (enabled && active && n < kVignetteMaxColors) out[n++] = c;
    };
    // Intensity-agnostic base colors — alpha filled in at draw time.
    push(IM_COL32(255,  40,  40, 255), g_vignette_low_hp,     ss.low_hp);
    push(IM_COL32(170,  60, 220, 255), g_vignette_poison,     ss.poison);
    push(IM_COL32(255, 160,  40, 255), g_vignette_paralyzed,  ss.paralyzed);
    push(IM_COL32(120, 200, 255, 255), g_vignette_frozen,     ss.frozen);
    push(IM_COL32(255, 240, 120, 255), g_vignette_shocked,    ss.shocked);
    push(IM_COL32( 80, 150, 150, 255), g_vignette_slow,       ss.slow);
    push(IM_COL32(255, 100, 200, 255), g_vignette_confused,   ss.confused);
    push(IM_COL32(220, 200,  60, 255), g_vignette_jellen,     ss.jellen);
    push(IM_COL32(120, 200,  80, 255), g_vignette_zalure,     ss.zalure);
    *out_count = n;
}

// Draw a pulsing edge vignette across the entire screen. Uses four
// gradient rects (top/bottom/left/right) with outer-edge alpha ==
// the pulse level and inner alpha == 0, so the fade toward the
// center is smooth. Corner overlap is fine because the per-axis
// fades blend additively.
static void RenderStatusVignette(int screen_w, int screen_h,
                                 const StatusState &ss)
{
    if (!g_vignette_enabled) return;

    ImU32 colors[kVignetteMaxColors];
    int  color_count = 0;
    CollectVignetteColors(ss, colors, &color_count);
    if (color_count == 0) return;

    // Pick current color. Single condition = that color. Multiple =
    // cycle one per second in the order they were collected.
    const DWORD tick = GetTickCount();
    const ImU32 base_col =
        colors[(tick / 1000) % static_cast<DWORD>(color_count)];

    // Pulse envelope. sin wave at g_vignette_pulse_hz, mapped into
    // [0.45, 1.0] so even the valley is visible rather than
    // flashing to fully invisible.
    const float phase = static_cast<float>(tick) / 1000.0f
                      * g_vignette_pulse_hz * 6.2831853f;
    const float pulse = 0.725f + 0.275f * std::sin(phase);

    // Extract RGB from base color, then synthesize the outer/inner
    // ImU32 values with the pulse-modulated alpha.
    const uint8_t br = static_cast<uint8_t>((base_col >> IM_COL32_R_SHIFT) & 0xFF);
    const uint8_t bg = static_cast<uint8_t>((base_col >> IM_COL32_G_SHIFT) & 0xFF);
    const uint8_t bb = static_cast<uint8_t>((base_col >> IM_COL32_B_SHIFT) & 0xFF);
    const float a_outer_f =
        255.0f * g_vignette_intensity * pulse;
    int a_outer = static_cast<int>(a_outer_f);
    if (a_outer < 0)   a_outer = 0;
    if (a_outer > 255) a_outer = 255;
    const ImU32 col_outer = IM_COL32(br, bg, bb, a_outer);
    const ImU32 col_inner = IM_COL32(br, bg, bb, 0);

    // Full-screen transparent host window so ImDrawList draws into
    // the main screen space regardless of other HUD windows.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(static_cast<float>(screen_w),
               static_cast<float>(screen_h)),
        ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    constexpr ImGuiWindowFlags kVignetteFlags =
        ImGuiWindowFlags_NoDecoration       |
        ImGuiWindowFlags_NoInputs           |
        ImGuiWindowFlags_NoBackground       |
        ImGuiWindowFlags_NoSavedSettings    |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav              |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##PSO_HUD_Vignette", nullptr, kVignetteFlags))
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float w = static_cast<float>(screen_w);
        const float h = static_cast<float>(screen_h);
        const float tx = w * 0.5f * g_vignette_thickness_frac;
        const float ty = h * 0.5f * g_vignette_thickness_frac;

        // Top band: opaque at y=0, transparent at y=ty.
        dl->AddRectFilledMultiColor(
            ImVec2(0.0f, 0.0f), ImVec2(w, ty),
            col_outer, col_outer, col_inner, col_inner);
        // Bottom band: transparent at y=h-ty, opaque at y=h.
        dl->AddRectFilledMultiColor(
            ImVec2(0.0f, h - ty), ImVec2(w, h),
            col_inner, col_inner, col_outer, col_outer);
        // Left band: opaque at x=0, transparent at x=tx.
        dl->AddRectFilledMultiColor(
            ImVec2(0.0f, 0.0f), ImVec2(tx, h),
            col_outer, col_inner, col_inner, col_outer);
        // Right band: transparent at x=w-tx, opaque at x=w.
        dl->AddRectFilledMultiColor(
            ImVec2(w - tx, 0.0f), ImVec2(w, h),
            col_inner, col_outer, col_outer, col_inner);
    }
    ImGui::End();
}

static bool render_xp_body()
{
    bool had_any = false;

    if (g_xp_track_enabled)
    {
        const PlayerXp xp = GetLocalPlayerXp();
        if (xp.valid && xp.level > 0)
        {
            UpdateXpTracker(xp);
            const double   rate   = ComputeXpPerHour();
            const uint32_t gained = ComputeXpGained(xp);

            const DWORD session_ms = GetTickCount() - g_xp_session_start_tick;
            const int total_sec = static_cast<int>(session_ms / 1000);
            char timer_str[32];
            if (total_sec < 3600)
            {
                const int mm = total_sec / 60;
                const int ss = total_sec % 60;
                std::snprintf(timer_str, sizeof(timer_str), "%dm%02ds", mm, ss);
            }
            else
            {
                const int hh = total_sec / 3600;
                const int mm = (total_sec % 3600) / 60;
                std::snprintf(timer_str, sizeof(timer_str), "%dh%02dm", hh, mm);
            }

            char rate_str[32];
            if (g_xp_count < 2)
                std::snprintf(rate_str, sizeof(rate_str), "...");
            else
                std::snprintf(rate_str, sizeof(rate_str), "%.0f", rate);

            ImGui::Text("EXP %u / NXT %u", xp.exp, xp.to_next_level);
            ImGui::Text("EXP Gained: %u (%s) | EXP/hr: %s",
                        gained, timer_str, rate_str);

            had_any = true;
        }
    }

    return had_any;
}

void on_reshade_overlay_event(reshade::api::effect_runtime *runtime)
{
    if (!g_enabled || !g_always_visible) return;

    // Invalidate the per-frame read cache at the top of every
    // frame. Must happen BEFORE GetLocalPlayerPtr() below, otherwise
    // a cached 0 from the title screen never gets cleared (the early
    // return skips the reset, so the stale cache persists forever).
    FrameCacheReset();

    // Install the mouse-wheel rate limiter the first frame we have
    // a real game window. Running from DllMain ATTACH doesn't work
    // because the window sometimes isn't up yet at that point, and
    // we need ReShade's own WndProc hook to already be in place
    // below us in the chain. Running once per frame thereafter is a
    // cheap no-op.
    WheelFilterTryInstall();

    // Don't render anything until the player is in-game.
    // Prevents "(no alerts)" and "No enemies" on title/login screen.
    if (GetLocalPlayerPtr() == 0) return;

    uint32_t w = 0, h = 0;
    if (runtime != nullptr)
        runtime->get_screenshot_width_and_height(&w, &h);
    if (w == 0 || h == 0) { w = 1920; h = 1080; }

    // HP alert state (drives the vignette and the audible cue).
    const HpAlertState hp_alert = CheckLowHpAlert();

    // Status vignette gets its own snapshot of the buff state so it
    // can pulse an edge tint for low HP / Jellen / Zalure (and
    // eventually poison/frozen/paralyzed/confused once we locate the
    // status-effect byte). Drawn FIRST so the rest of the HUD sits
    // on top of the vignette instead of being obscured by it.
    {
        const BuffState vignette_buffs = GetBuffState();
        const StatusState ss = GetStatusState(hp_alert, vignette_buffs);
        RenderStatusVignette(static_cast<int>(w),
                             static_cast<int>(h), ss);
    }

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar         |
        ImGuiWindowFlags_NoResize           |
        ImGuiWindowFlags_NoMove             |
        ImGuiWindowFlags_NoScrollbar        |
        ImGuiWindowFlags_NoCollapse         |
        ImGuiWindowFlags_NoSavedSettings    |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav              |
        ImGuiWindowFlags_NoInputs           |
        ImGuiWindowFlags_AlwaysAutoResize;

    // Monster HP panel — left side, below allies by default.
    if (g_show_monster_hp)
    {
        ImVec2 pos, pivot;
        compute_anchor_pos(g_monsters_anchor,
                           g_monsters_window_x, g_monsters_window_y,
                           static_cast<float>(w), static_cast<float>(h),
                           &pos, &pivot);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
        ImGui::SetNextWindowBgAlpha(g_monsters_window_alpha);
        ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f, 0.0f),
                                            ImVec2(800.0f, FLT_MAX),
                                            nullptr, nullptr);
        if (ImGui::Begin("##PSO_HUD_Monsters", nullptr, kFlags))
        {
            const auto monsters = GetAliveMonsters();
            render_hp_table(monsters);
        }
        ImGui::End();
    }

    // Floor items panel — where the v1.0 combined panel lived. Uses a
    // separate flag set that drops NoInputs so hover tooltips and
    // right-click context menus (Hide / Unhide) can register. Cost:
    // during gameplay, left-clicks that happen to land inside this
    // panel get eaten by ImGui instead of passing through to PSO.
    // Acceptable for HUD-panel real estate and matches the design
    // intent that hide-list curation is a "pause + adjust" action
    // done with the ReShade overlay open.
    if (g_show_floor_items)
    {
        constexpr ImGuiWindowFlags kItemsFlags =
            kFlags & ~ImGuiWindowFlags_NoInputs;

        ImVec2 pos, pivot;
        compute_anchor_pos(g_items_anchor,
                           g_items_window_x, g_items_window_y,
                           static_cast<float>(w), static_cast<float>(h),
                           &pos, &pivot);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
        ImGui::SetNextWindowBgAlpha(g_items_window_alpha);
        ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f, 0.0f),
                                            ImVec2(800.0f, FLT_MAX),
                                            nullptr, nullptr);
        if (ImGui::Begin("##PSO_HUD_Items", nullptr, kItemsFlags))
            render_items_table(GetFloorItems());
        ImGui::End();
    }

    // ---- EXP tracker window (top-right by default) ----
    //
    // Width is locked to fit the worst-case
    // "EXP Gained: 999999999 (99h59m) | EXP/hr: 999999" string so
    // the window doesn't drift left as the gained number grows.
    if (g_xp_window_enabled)
    {
        ImVec2 s_pos, s_pivot;
        compute_anchor_pos(g_xp_window_anchor, g_xp_window_x, g_xp_window_y,
                           static_cast<float>(w), static_cast<float>(h),
                           &s_pos, &s_pivot);
        ImGui::SetNextWindowPos(s_pos, ImGuiCond_Always, s_pivot);
        // Pin the width: 380 px fits the longest EXP row at the
        // default font size, with headroom for ImGui's window padding
        // (~16 px total). Min == max means the window stays at this
        // exact width regardless of content. Height grows freely.
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(380.0f, 0.0f),
            ImVec2(380.0f, FLT_MAX));
        ImGui::SetNextWindowBgAlpha(g_xp_window_alpha);

        // Drop AlwaysAutoResize for this window only — the explicit
        // width constraint above does the sizing, and AAR would fight
        // it on the X axis.
        constexpr ImGuiWindowFlags kXpFlags =
            (kFlags & ~ImGuiWindowFlags_AlwaysAutoResize);
        if (ImGui::Begin("##PSO_HUD_Xp", nullptr, kXpFlags))
        {
            if (!render_xp_body())
            {
                // Nothing active right now; keep a faint placeholder so the
                // window doesn't collapse to zero size.
                ImGui::TextDisabled("(EXP tracker off)");
            }
        }
        ImGui::End();
    }

    // ---- Mag timer window (top-left, above HP/TP bar) ----
    if (g_mag_timer_enabled)
    {
        const MagTimer mag = GetMagFeedTimer();
        if (mag.valid)
        {
            // Fixed position: top-left with small margin, above the game's HP bar
            ImGui::SetNextWindowPos(ImVec2(10.0f, 5.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.5f);

            if (ImGui::Begin("##PSO_HUD_Mag", nullptr, kFlags))
            {
                if (mag.ready)
                {
                    const DWORD t = GetTickCount();
                    const float phase = static_cast<float>(t % 900) / 900.0f;
                    const float pulse = 0.7f + 0.3f * std::sin(phase * 6.2831853f);
                    const ImVec4 green(0.2f, 1.0f * pulse, 0.2f, 1.0f);
                    ImGui::TextColored(green, "Mag: READY TO FEED");
                }
                else
                {
                    const int total = static_cast<int>(mag.secs + 0.5f);
                    const int mm = total / 60;
                    const int ss = total % 60;
                    ImGui::Text("Mag: next feed in %d:%02d", mm, ss);
                }
            }
            ImGui::End();
        }
    }

    // ---- Buff/debuff panel (right of the mag timer) ----
    //
    // Independent window for Shifta/Deband/Jellen/Zalure timers,
    // with optional reminder pulses when Shifta and/or Deband are
    // missing. Position is fixed offset from the top-left so it
    // tracks alongside the mag timer rather than getting lost in
    // an anchor combo.
    //
    // Reminder pulse rules:
    //   - Shifta reminder ON  + shifta inactive → red pulse
    //   - Deband reminder ON  + deband inactive → blue pulse
    //   - Both reminders ON   + both inactive   → alternates red/blue
    //     every 1 second so the user sees both colors.
    //   - Otherwise           → normal opacity, no pulse
    if (g_buff_panel_enabled)
    {
        const BuffState buffs = GetBuffState();

        const bool need_shifta = g_buff_shifta_reminder && !buffs.shifta_active;
        const bool need_deband = g_buff_deband_reminder && !buffs.deband_active;
        const bool reminding   = need_shifta || need_deband;

        ImGui::SetNextWindowPos(ImVec2(g_buff_panel_x, g_buff_panel_y),
                                ImGuiCond_Always);

        bool pushed_bg = false;
        if (reminding)
        {
            const DWORD t = GetTickCount();
            // Pulse intensity (0.55..0.90) sine-wave at ~1.4 Hz so
            // the box visibly throbs. Independent of color choice.
            const float phase = static_cast<float>(t % 700) / 700.0f;
            const float pulse = 0.55f + 0.35f * std::sin(phase * 6.2831853f);

            // Color selection. If both reminders are firing, swap
            // between red and blue every 1 s. Otherwise use the
            // single missing-buff color.
            ImVec4 bg;
            if (need_shifta && need_deband)
            {
                const bool show_red = ((t / 1000) % 2) == 0;
                bg = show_red
                        ? ImVec4(0.65f * pulse, 0.05f, 0.05f, 0.92f)
                        : ImVec4(0.05f, 0.05f, 0.65f * pulse, 0.92f);
            }
            else if (need_shifta)
            {
                bg = ImVec4(0.65f * pulse, 0.05f, 0.05f, 0.92f);
            }
            else // need_deband
            {
                bg = ImVec4(0.05f, 0.05f, 0.65f * pulse, 0.92f);
            }
            ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
            pushed_bg = true;
        }
        else
        {
            ImGui::SetNextWindowBgAlpha(g_buff_panel_alpha);
        }

        if (ImGui::Begin("##PSO_HUD_Buffs", nullptr, kFlags))
        {
            if (!render_buff_body(buffs))
            {
                // No buffs active. If any reminder is enabled we
                // surface a textual hint inside the pulsing box;
                // otherwise show a faint placeholder so the window
                // doesn't collapse to nothing.
                if (need_shifta && need_deband)
                    ImGui::TextUnformatted("Cast Shifta + Deband!");
                else if (need_shifta)
                    ImGui::TextUnformatted("Cast Shifta!");
                else if (need_deband)
                    ImGui::TextUnformatted("Cast Deband!");
                else
                    ImGui::TextDisabled("(no buffs)");
            }
        }
        ImGui::End();

        if (pushed_bg) ImGui::PopStyleColor();
    }

    // Draw the slot outline (strobe + calibrate visualisation) as
    // either a rounded rectangle or a flat-top hexagon, depending on
    // the user's g_chord_slot_shape setting. `hw` and `hh` are HALF
    // dimensions (half-width, half-height) of the slot's bounding box.
    auto draw_slot_outline = [](ImDrawList *dl, ImVec2 c, float hw, float hh,
                                ImU32 color, float thickness)
    {
        if (g_chord_slot_shape == 1)
        {
            // Flat-top hexagon: top and bottom edges horizontal (at
            // half-width from center), left/right tapered to points.
            const ImVec2 v[6] = {
                ImVec2(c.x - hw * 0.5f, c.y - hh),
                ImVec2(c.x + hw * 0.5f, c.y - hh),
                ImVec2(c.x + hw,        c.y),
                ImVec2(c.x + hw * 0.5f, c.y + hh),
                ImVec2(c.x - hw * 0.5f, c.y + hh),
                ImVec2(c.x - hw,        c.y),
            };
            dl->AddPolyline(v, 6, color, ImDrawFlags_Closed, thickness);
        }
        else
        {
            // Rectangle with rounded corners.
            dl->AddRect(ImVec2(c.x - hw, c.y - hh),
                        ImVec2(c.x + hw, c.y + hh),
                        color, 4.0f, 0, thickness);
        }
    };

    // ---- Chord overlay: per-slot badges over the palette bar ----
    //
    // Ten rounded-rect badges, one per native palette slot (1..0).
    // Each badge carries "<face-letter><slot-digit>" text (A1, B2, X3,
    // Y4, A5, B6, X7, Y8, A9, B0), plus:
    //   - Border color = which trigger binds it (LT/RT/LT+RT)
    //   - Fill alpha = active/hint/dormant state
    //   - "+10" chip in the top-right corner when RB is held
    //     (indicates palette set 2, slots 11..20)
    //
    // When a chord fires the matching slot's badge does a 220 ms
    // snap-to-white pulse and strobes a border rectangle around the
    // underlying palette cell — the "your press registered" cue PSO
    // doesn't provide natively. Flash timing is counter-driven so a
    // rapid double-tap still re-triggers even if both fires land in
    // the same GetTickCount() tick.
    //
    // Layout is calibrated in 1080p base units then scaled by w/1920
    // x h/1080 so the grid tracks the game's render resolution.
    if (g_chord_overlay_enabled)
    {
        const chords::HeldState cs = chords::GetHeldState();
        if (cs.enabled)
        {
            const float sx = static_cast<float>(w) / 1920.0f;
            const float sy = static_cast<float>(h) / 1080.0f;

            // Per-slot view state: last seen counter + flash start
            // tick. Init-once statics so the renderer has continuity
            // across frames. Never touched by the input thread.
            static uint32_t s_render_last_counter[10]    = {};
            static uint32_t s_render_flash_start_tick[10] = {};

            const chords::FireEvents fe = chords::GetFireEvents();
            const uint32_t now = GetTickCount();
            for (int i = 0; i < 10; ++i)
            {
                if (fe.last_counter[i] != s_render_last_counter[i])
                {
                    s_render_last_counter[i]     = fe.last_counter[i];
                    s_render_flash_start_tick[i] = fe.last_tick[i];
                }
            }

            // Slot geometry helpers, both in screen pixel space.
            auto slot_center = [&](int i) -> ImVec2 {
                const float nudge =
                    (i >= 0 && i < 10) ? g_chord_slot_x_offset[i] : 0.0f;
                const float cx =
                    g_chord_slot_center_x * sx
                    + (static_cast<float>(i) - 4.5f) * g_chord_slot_pitch * sx
                    + nudge * sx;
                const float cy =
                    g_chord_slot_bottom_y * sy
                    - (g_chord_slot_height * 0.5f) * sy;
                return ImVec2(cx, cy);
            };
            auto slot_top_center = [&](int i) -> ImVec2 {
                ImVec2 c = slot_center(i);
                c.y -= (g_chord_slot_height * 0.5f) * sy;
                return c;
            };

            // Full-screen transparent host window for the ImDrawList
            // primitives. Cheaper than 10 ImGui windows and lets us
            // draw outside ImGui's normal content rect.
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(
                ImVec2(static_cast<float>(w), static_cast<float>(h)),
                ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.0f);
            constexpr ImGuiWindowFlags kChordFlags =
                ImGuiWindowFlags_NoDecoration       |
                ImGuiWindowFlags_NoInputs           |
                ImGuiWindowFlags_NoBackground       |
                ImGuiWindowFlags_NoSavedSettings    |
                ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav              |
                ImGuiWindowFlags_NoBringToFrontOnFocus;

            if (ImGui::Begin("##PSO_HUD_ChordGrid", nullptr, kChordFlags))
            {
                ImDrawList *dl = ImGui::GetWindowDrawList();

                // Slot index → trigger id (0 = LT, 1 = RT, 2 = LT+RT).
                // Face button order A/X/Y/B counter-clockwise around
                // the diamond — kept in lockstep with the chord
                // dispatch in controller_chords.cpp.
                static const int kSlotTrig[10] = {
                    0, 0, 0, 0,    // LT + A/X/Y/B → slots 1-4
                    1, 1, 1, 1,    // RT + A/X/Y/B → slots 5-8
                    2, 2,          // LT+RT + A/X  → slots 9-0
                };
                static const char *kLabel[10] = {
                    "A1", "X2", "Y3", "B4",
                    "A5", "X6", "Y7", "B8",
                    "A9", "X0",
                };

                const float bw = 28.0f * sx;
                const float bh = 18.0f * sy;
                const float br = 4.0f  * sx;

                for (int i = 0; i < 10; ++i)
                {
                    const int trig = kSlotTrig[i];
                    const bool trig_active =
                        (trig == 0 && cs.lt && !cs.rt) ||
                        (trig == 1 && cs.rt && !cs.lt) ||
                        (trig == 2 && cs.lt && cs.rt);

                    ImU32 border_col;
                    switch (trig)
                    {
                    case 0:  border_col = IM_COL32( 90, 200, 255, 255); break;
                    case 1:  border_col = IM_COL32(255, 170,  70, 255); break;
                    default: border_col = IM_COL32(230, 120, 255, 255); break;
                    }

                    // Alpha tier: active > hint > dormant.
                    float base_a;
                    if (trig_active)
                        base_a = 1.00f;
                    else if (g_chord_overlay_show_hints && !cs.lt && !cs.rt)
                        base_a = 0.35f;
                    else
                        base_a = 0.15f;
                    base_a *= g_chord_overlay_alpha;

                    // Flash phase in [0..1]. 0 = just fired, 1 = done.
                    float ft = 1.0f;
                    if (s_render_flash_start_tick[i] != 0)
                    {
                        const uint32_t elapsed =
                            now - s_render_flash_start_tick[i];
                        if (elapsed < static_cast<uint32_t>(g_chord_overlay_flash_ms))
                            ft = static_cast<float>(elapsed) /
                                 static_cast<float>(g_chord_overlay_flash_ms);
                    }
                    const bool flashing = ft < 1.0f;

                    // Badge rect, centered horizontally on the slot.
                    const ImVec2 top = slot_top_center(i);
                    const ImVec2 bmin(
                        top.x - bw * 0.5f,
                        top.y - g_chord_badge_gap * sy - bh);
                    const ImVec2 bmax(
                        top.x + bw * 0.5f,
                        top.y - g_chord_badge_gap * sy);

                    // Base fill color, then flash-compose a white pulse
                    // on top during the first ~18% of the flash window
                    // linearly fading back to the base.
                    int fill_r = 20, fill_g = 28, fill_b = 40;
                    int fill_a = static_cast<int>(210.0f * base_a);
                    if (flashing)
                    {
                        float k = ft < 0.18f
                            ? 1.0f
                            : (1.0f - (ft - 0.18f) / 0.82f);
                        fill_r = static_cast<int>(20  + (255 - 20)  * k);
                        fill_g = static_cast<int>(28  + (255 - 28)  * k);
                        fill_b = static_cast<int>(40  + (255 - 40)  * k);
                        fill_a = static_cast<int>(210 + (230 - 210) * k);
                    }
                    if (fill_a < 0)   fill_a = 0;
                    if (fill_a > 255) fill_a = 255;
                    const ImU32 fill_col =
                        IM_COL32(fill_r, fill_g, fill_b, fill_a);
                    dl->AddRectFilled(bmin, bmax, fill_col, br);

                    // Border: full trigger color when active, faded to
                    // 90 alpha in hint/dormant states.
                    ImU32 bcol = border_col;
                    if (!trig_active)
                        bcol = (bcol & 0x00FFFFFFu) | (static_cast<uint32_t>(90) << 24);
                    dl->AddRect(bmin, bmax, bcol, br, 0, 1.5f);

                    // (RB modifier intentionally has no overlay change.
                    // PSO's own HUD switches the palette art when RB is
                    // held, so adding a second indicator here is noise.
                    // The chord system still fires Ctrl+digit; we just
                    // don't redraw anything.)

                    // Centered label text. During the bright-pulse
                    // portion of a flash, invert text to dark so it
                    // stays readable on the white fill.
                    const char *label = kLabel[i];
                    const ImVec2 ts = ImGui::CalcTextSize(label);
                    const ImVec2 tp(
                        (bmin.x + bmax.x) * 0.5f - ts.x * 0.5f,
                        (bmin.y + bmax.y) * 0.5f - ts.y * 0.5f);
                    ImU32 text_col;
                    if (flashing && ft < 0.18f)
                    {
                        text_col = IM_COL32(20, 20, 30, 255);
                    }
                    else
                    {
                        int ta = static_cast<int>(255.0f * base_a);
                        if (ta < 60)  ta = 60;
                        if (ta > 255) ta = 255;
                        text_col = IM_COL32(255, 255, 255, ta);
                    }
                    dl->AddText(tp, text_col, label);

                    // Slot-border strobe: an unfilled outline drawn
                    // around the underlying palette slot. Shape is
                    // rectangle or flat-top hexagon depending on
                    // g_chord_slot_shape. Fades from opaque to
                    // transparent over the flash window. Also drawn
                    // every frame in calibrate mode so the user can
                    // see the grid while dragging sliders.
                    if (flashing || g_chord_overlay_calibrate_mode)
                    {
                        const ImVec2 sc = slot_center(i);
                        const float hw = g_chord_slot_width  * 0.5f * sx;
                        const float hh = g_chord_slot_height * 0.5f * sy;
                        int sa;
                        if (g_chord_overlay_calibrate_mode && !flashing)
                            sa = 120;
                        else
                            sa = static_cast<int>(255.0f * (1.0f - ft));
                        if (sa < 0)   sa = 0;
                        if (sa > 255) sa = 255;
                        const ImU32 strobe_col =
                            (border_col & 0x00FFFFFFu) |
                            (static_cast<uint32_t>(sa) << 24);
                        draw_slot_outline(dl, sc, hw, hh, strobe_col, 2.0f);
                    }
                }

            }
            ImGui::End();
        }
    }
}

} // namespace

// ----------------------------------------------------------------------------
// Wheel-filter accessors. g_wheel_filter_enabled and g_wheel_throttle_ms
// live inside the anonymous namespace above (internal linkage) so they
// can't be referenced directly from controller_chords.cpp. These file-
// scope thunks give the chord TU a clean read-only view of the state
// without leaking the globals themselves.
// ----------------------------------------------------------------------------

extern "C" bool WheelFilter_Enabled()     { return g_wheel_filter_enabled; }
extern "C" int  WheelFilter_ThrottleMs()  { return g_wheel_throttle_ms; }
extern "C" int  WheelFilter_Mode()        { return g_wheel_filter_mode; }
extern "C" bool WheelFilter_DebugLog()    { return g_wheel_debug_log; }

extern "C" bool StickMap_Enabled()         { return g_stick_enabled; }
extern "C" bool StickMap_ZoomEnabled()     { return g_stick_zoom_enabled; }
extern "C" bool StickMap_InvertY()         { return g_stick_invert_y; }
extern "C" int  StickMap_Deadzone()        { return g_stick_deadzone; }
extern "C" int  StickMap_RateMs()          { return g_stick_rate_ms; }

// ============================================================================
// Mouse-wheel rate limiter (defined at file scope, not in the anonymous
// namespace, so the forward declarations up top match these definitions).
//
// Lightweight WndProc subclass on PSO's top-level window that drops
// WM_MOUSEWHEEL / WM_MOUSEHWHEEL bursts tighter than g_wheel_throttle_ms.
// Installed lazily from on_reshade_overlay_event the first frame we
// have a window to work with, torn down on DllMain DETACH.
// ============================================================================

static HWND    s_wheel_hwnd         = nullptr;
static WNDPROC s_wheel_orig_proc    = nullptr;
static bool    s_wheel_proc_unicode = false;
static DWORD   s_wheel_last_tick    = 0;

static LRESULT CALLBACK WheelFilterWndProc(HWND hwnd, UINT msg,
                                           WPARAM wParam, LPARAM lParam)
{
    if ((msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) &&
        g_wheel_filter_enabled)
    {
        const DWORD now = GetTickCount();
        const DWORD limit = static_cast<DWORD>(g_wheel_throttle_ms);
        if (s_wheel_last_tick != 0 &&
            (now - s_wheel_last_tick) < limit)
        {
            // Inside the throttle window — swallow. Returning 0 from a
            // WM_MOUSEWHEEL handler tells Windows we processed it so
            // DefWindowProc's default forwarding behavior stays out of
            // the way, and the game never sees the message.
            return 0;
        }
        s_wheel_last_tick = now;
    }

    if (s_wheel_proc_unicode)
        return CallWindowProcW(s_wheel_orig_proc, hwnd, msg, wParam, lParam);
    return CallWindowProcA(s_wheel_orig_proc, hwnd, msg, wParam, lParam);
}

// EnumWindows callback that finds the first top-level visible window
// owned by the current process. That's the PSOBB game window.
static BOOL CALLBACK WheelFilterFindWindow(HWND hwnd, LPARAM lparam)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(hwnd))        return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    *reinterpret_cast<HWND *>(lparam) = hwnd;
    return FALSE;  // stop enumeration
}

static void WheelFilterTryInstall()
{
    if (s_wheel_orig_proc != nullptr) return;  // already installed
    HWND hwnd = nullptr;
    EnumWindows(WheelFilterFindWindow, reinterpret_cast<LPARAM>(&hwnd));
    if (hwnd == nullptr) return;

    // Pick the W or A variant of SetWindowLongPtr / CallWindowProc
    // that matches the target window. Using the wrong variant causes
    // string-carrying messages to be translated/corrupted, which
    // would break everything the game does in its normal WndProc.
    s_wheel_proc_unicode = IsWindowUnicode(hwnd) != FALSE;

    LONG_PTR prev;
    if (s_wheel_proc_unicode)
        prev = SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                 reinterpret_cast<LONG_PTR>(WheelFilterWndProc));
    else
        prev = SetWindowLongPtrA(hwnd, GWLP_WNDPROC,
                                 reinterpret_cast<LONG_PTR>(WheelFilterWndProc));

    if (prev == 0)
    {
        PSO_LOG("WheelFilter: SetWindowLongPtr failed err=%lu", GetLastError());
        return;
    }
    s_wheel_hwnd      = hwnd;
    s_wheel_orig_proc = reinterpret_cast<WNDPROC>(prev);
    PSO_LOG("WheelFilter: installed on hwnd=0x%p unicode=%d",
            hwnd, s_wheel_proc_unicode ? 1 : 0);
}

static void WheelFilterUninstall()
{
    if (s_wheel_orig_proc == nullptr || s_wheel_hwnd == nullptr) return;
    // Restore the original WndProc. If someone else has subclassed on
    // top of us in the meantime our restore will unchain them, but
    // that's an uncommon DETACH ordering issue and MinHook cleanup
    // paths elsewhere in the addon have the same limitation.
    if (s_wheel_proc_unicode)
        SetWindowLongPtrW(s_wheel_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(s_wheel_orig_proc));
    else
        SetWindowLongPtrA(s_wheel_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(s_wheel_orig_proc));
    PSO_LOG("WheelFilter: uninstalled from hwnd=0x%p", s_wheel_hwnd);
    s_wheel_hwnd      = nullptr;
    s_wheel_orig_proc = nullptr;
}

// ============================================================================
// Persistence — pixelated_mods.ini next to the DLL
// ============================================================================

// Resolved once in DllMain attach; avoids recomputing on every save.
static std::string g_config_path;

static void ResolveAddonPaths()
{
    char path[MAX_PATH] = {};
    DWORD len = g_module ? GetModuleFileNameA(g_module, path, MAX_PATH)
                         : GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len == 0)
    {
        g_addon_dir.clear();
        g_config_path = "pixelated_mods.ini";
        PSO_LOG("ResolveAddonPaths: GetModuleFileNameA failed, fallback cwd");
        return;
    }

    std::string p(path, len);
    const auto slash = p.find_last_of("\\/");
    if (slash != std::string::npos)
        p.erase(slash + 1);

    g_addon_dir   = p;
    g_config_path = g_addon_dir + "pixelated_mods.ini";
    PSO_LOG("ResolveAddonPaths: dir=%s config=%s",
            g_addon_dir.c_str(), g_config_path.c_str());
}

static std::string g_filter_error;

static void AppendUserFilter(uint32_t id24, const char *name)
{
    if (g_addon_dir.empty()) return;
    const std::string path = g_addon_dir + "pixelated_mods_hidden.txt";
    FILE *f = std::fopen(path.c_str(), "a");
    if (!f) return;
    std::fprintf(f, "%06X %s\n", id24, name ? name : "");
    std::fclose(f);
    PSO_LOG("AppendUserFilter: added %06X to %s", id24, path.c_str());
}

// Inverse of AppendUserFilter: rewrite pixelated_mods_hidden.txt with
// the line matching `id24` removed. Used when the user clicks Unhide
// in the "Hidden in view" sidebar. Parsing tolerates leading whitespace,
// `#` comments, and either hex (deduced from 6-digit format) or decimal
// IDs — matches LoadFilters' parsing style.
static void RemoveUserFilter(uint32_t id24)
{
    if (g_addon_dir.empty()) return;
    const std::string path = g_addon_dir + "pixelated_mods_hidden.txt";

    // Read all lines into memory, filter out the matching one, write back.
    std::vector<std::string> kept;
    {
        FILE *f = std::fopen(path.c_str(), "r");
        if (!f) return;
        char buf[512];
        while (std::fgets(buf, sizeof(buf), f))
        {
            // Find first non-whitespace char
            const char *p = buf;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            {
                kept.emplace_back(buf);
                continue;
            }
            // Parse leading ID token (hex if 6 chars else decimal).
            const char *tok_start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
            const size_t tok_len = static_cast<size_t>(p - tok_start);
            uint32_t parsed = 0;
            if (tok_len == 6)
                parsed = static_cast<uint32_t>(std::strtoul(tok_start, nullptr, 16));
            else
                parsed = static_cast<uint32_t>(std::strtoul(tok_start, nullptr, 10));
            if (parsed != id24)
                kept.emplace_back(buf);
        }
        std::fclose(f);
    }

    FILE *fw = std::fopen(path.c_str(), "w");
    if (!fw) return;
    for (const auto &ln : kept) std::fputs(ln.c_str(), fw);
    std::fclose(fw);
    PSO_LOG("RemoveUserFilter: removed %06X from %s", id24, path.c_str());
}

static void LoadCuratedIdLists()
{
    if (g_addon_dir.empty())
    {
        PSO_LOG("LoadCuratedIdLists: empty addon_dir, skipping");
        return;
    }
    FilterSets fs = LoadFilters(g_addon_dir);
    g_rare_ids            = std::move(fs.rare_ids);
    g_hidden_ids          = std::move(fs.hidden_item_ids);
    g_hidden_monster_ids  = std::move(fs.hidden_monster_ids);
    g_filter_error        = std::move(fs.error_msg);
}

static void LoadConfig()
{
    FILE *f = std::fopen(g_config_path.c_str(), "r");
    if (f == nullptr)
    {
        PSO_LOG("LoadConfig: no file at %s (first run)", g_config_path.c_str());
        return;
    }
    PSO_LOG("LoadConfig: opened %s", g_config_path.c_str());

    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr)
    {
        size_t n = std::strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         line[n - 1] == ' '  || line[n - 1] == '\t'))
            line[--n] = '\0';
        if (n == 0 || line[0] == '#' || line[0] == ';') continue;

        char *eq = std::strchr(line, '=');
        if (eq == nullptr) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if      (std::strcmp(key, "enabled")           == 0) g_enabled           = std::atoi(val) != 0;
        else if (std::strcmp(key, "always_visible")    == 0) g_always_visible    = std::atoi(val) != 0;
        else if (std::strcmp(key, "show_monster_hp")   == 0) g_show_monster_hp   = std::atoi(val) != 0;
        else if (std::strcmp(key, "show_floor_items")  == 0) g_show_floor_items  = std::atoi(val) != 0;
        else if (std::strcmp(key, "show_hp_numbers")   == 0) g_show_hp_numbers   = std::atoi(val) != 0;
        else if (std::strcmp(key, "show_hp_bar")       == 0) g_show_hp_bar       = std::atoi(val) != 0;
        else if (std::strcmp(key, "show_count_header") == 0) g_show_count_header = std::atoi(val) != 0;
        else if (std::strcmp(key, "collapse_boss_parts") == 0) g_collapse_boss_parts = std::atoi(val) != 0;
        else if (std::strcmp(key, "hide_unnamed_monsters") == 0) g_hide_unnamed_monsters = std::atoi(val) != 0;
        else if (std::strcmp(key, "bar_width")         == 0) g_bar_width         = static_cast<float>(std::atof(val));
        // Panel anchors. Legacy single-panel keys (anchor/window_x/...)
        // map to the floor-items panel since v1.0 had the combined panel
        // in its default position. New configs use explicit items_* and
        // monsters_* prefixes.
        else if (std::strcmp(key, "items_anchor") == 0 ||
                 std::strcmp(key, "anchor")       == 0)
        {
            int a = std::atoi(val);
            if (a >= 0 && a <= 5) g_items_anchor = static_cast<Anchor>(a);
        }
        else if (std::strcmp(key, "monsters_anchor") == 0)
        {
            int a = std::atoi(val);
            if (a >= 0 && a <= 5) g_monsters_anchor = static_cast<Anchor>(a);
        }
        // XP window (new keys; legacy stats_* keys still read for
        // backwards compat — users with old configs migrate on next save).
        else if (std::strcmp(key, "xp_window_enabled") == 0 ||
                 std::strcmp(key, "stats_enabled")     == 0) g_xp_window_enabled = std::atoi(val) != 0;
        else if (std::strcmp(key, "xp_window_anchor")  == 0 ||
                 std::strcmp(key, "stats_anchor")      == 0)
        {
            int a = std::atoi(val);
            if (a >= 0 && a <= 5) g_xp_window_anchor = static_cast<Anchor>(a);
        }
        else if (std::strcmp(key, "xp_window_x")       == 0 ||
                 std::strcmp(key, "stats_window_x")    == 0) g_xp_window_x     = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "xp_window_y")       == 0 ||
                 std::strcmp(key, "stats_window_y")    == 0) g_xp_window_y     = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "xp_window_alpha")   == 0 ||
                 std::strcmp(key, "stats_window_alpha")== 0) g_xp_window_alpha = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "xp_track_enabled") == 0) g_xp_track_enabled = std::atoi(val) != 0;
        else if (std::strcmp(key, "mag_timer_enabled") == 0) g_mag_timer_enabled = std::atoi(val) != 0;
        else if (std::strcmp(key, "chord_overlay_enabled")       == 0) g_chord_overlay_enabled       = std::atoi(val) != 0;
        else if (std::strcmp(key, "chord_overlay_alpha")         == 0) g_chord_overlay_alpha         = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "chord_overlay_show_hints")    == 0) g_chord_overlay_show_hints    = std::atoi(val) != 0;
        else if (std::strcmp(key, "chord_overlay_flash_ms")      == 0) g_chord_overlay_flash_ms      = std::atoi(val);
        else if (std::strcmp(key, "chord_overlay_calibrate_mode")== 0) g_chord_overlay_calibrate_mode= std::atoi(val) != 0;
        else if (std::strcmp(key, "chord_slot_center_x")         == 0) g_chord_slot_center_x         = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "chord_slot_bottom_y")         == 0) g_chord_slot_bottom_y         = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "chord_slot_pitch")            == 0) g_chord_slot_pitch            = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "chord_slot_width")            == 0) g_chord_slot_width            = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "chord_slot_height")           == 0) g_chord_slot_height           = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "chord_badge_gap")             == 0) g_chord_badge_gap             = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "chord_slot_shape")            == 0)
        {
            const int v = std::atoi(val);
            if (v == 0 || v == 1) g_chord_slot_shape = v;
        }
        else if (std::strncmp(key, "chord_slot_x_offset_", 20) == 0)
        {
            // Keys chord_slot_x_offset_0 .. chord_slot_x_offset_9.
            const char *idx_str = key + 20;
            const int idx = std::atoi(idx_str);
            if (idx >= 0 && idx < 10)
                g_chord_slot_x_offset[idx] = static_cast<float>(std::atof(val));
        }
        else if (std::strcmp(key, "wheel_filter_enabled")        == 0) g_wheel_filter_enabled        = std::atoi(val) != 0;
        else if (std::strcmp(key, "wheel_throttle_ms")           == 0) g_wheel_throttle_ms           = std::atoi(val);
        else if (std::strcmp(key, "wheel_filter_mode")           == 0) g_wheel_filter_mode           = std::atoi(val);
        else if (std::strcmp(key, "wheel_debug_log")             == 0) g_wheel_debug_log             = std::atoi(val) != 0;
        else if (std::strcmp(key, "stick_enabled")               == 0) g_stick_enabled               = std::atoi(val) != 0;
        else if (std::strcmp(key, "stick_zoom_enabled")          == 0) g_stick_zoom_enabled          = std::atoi(val) != 0;
        else if (std::strcmp(key, "stick_invert_y")              == 0) g_stick_invert_y              = std::atoi(val) != 0;
        else if (std::strcmp(key, "stick_deadzone")              == 0) g_stick_deadzone              = std::atoi(val);
        else if (std::strcmp(key, "stick_rate_ms")               == 0) g_stick_rate_ms               = std::atoi(val);
        // stick_left_scancode / stick_right_scancode were removed in
        // v1.1.1 when the right stick X mapping became hardcoded
        // (PgDn/PgUp). Old INIs with these keys are silently ignored.
        else if (std::strcmp(key, "vignette_enabled")            == 0) g_vignette_enabled            = std::atoi(val) != 0;
        else if (std::strcmp(key, "vignette_low_hp")             == 0) g_vignette_low_hp             = std::atoi(val) != 0;
        else if (std::strcmp(key, "vignette_poison")             == 0) g_vignette_poison             = std::atoi(val) != 0;
        else if (std::strcmp(key, "vignette_paralyzed")          == 0) g_vignette_paralyzed          = std::atoi(val) != 0;
        else if (std::strcmp(key, "vignette_frozen")             == 0) g_vignette_frozen             = std::atoi(val) != 0;
        else if (std::strcmp(key, "vignette_shocked")            == 0) g_vignette_shocked            = std::atoi(val) != 0;
        else if (std::strcmp(key, "vignette_slow")               == 0) g_vignette_slow               = std::atoi(val) != 0;
        else if (std::strcmp(key, "vignette_confused")           == 0) g_vignette_confused           = std::atoi(val) != 0;
        else if (std::strcmp(key, "vignette_jellen")             == 0) g_vignette_jellen             = std::atoi(val) != 0;
        else if (std::strcmp(key, "vignette_zalure")             == 0) g_vignette_zalure             = std::atoi(val) != 0;
        else if (std::strcmp(key, "vignette_intensity")          == 0) g_vignette_intensity          = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "vignette_pulse_hz")           == 0) g_vignette_pulse_hz           = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "vignette_thickness_frac")     == 0) g_vignette_thickness_frac     = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "buff_panel_enabled")   == 0) g_buff_panel_enabled   = std::atoi(val) != 0;
        else if (std::strcmp(key, "buff_panel_x")         == 0) g_buff_panel_x         = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "buff_panel_y")         == 0) g_buff_panel_y         = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "buff_panel_alpha")     == 0) g_buff_panel_alpha     = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "buff_shifta_reminder") == 0) g_buff_shifta_reminder = std::atoi(val) != 0;
        else if (std::strcmp(key, "buff_deband_reminder") == 0) g_buff_deband_reminder = std::atoi(val) != 0;
        // Items panel position (legacy window_* keys also accepted).
        else if (std::strcmp(key, "items_window_x")     == 0 ||
                 std::strcmp(key, "window_x")           == 0) g_items_window_x     = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "items_window_y")     == 0 ||
                 std::strcmp(key, "window_y")           == 0) g_items_window_y     = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "items_window_alpha") == 0 ||
                 std::strcmp(key, "window_alpha")       == 0) g_items_window_alpha = static_cast<float>(std::atof(val));
        // Monsters panel position (new keys only).
        else if (std::strcmp(key, "monsters_window_x")     == 0) g_monsters_window_x     = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "monsters_window_y")     == 0) g_monsters_window_y     = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "monsters_window_alpha") == 0) g_monsters_window_alpha = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "filter_mode")       == 0)
        {
            int m = std::atoi(val);
            if (m >= 0 && m <= 2) g_filter_mode = static_cast<FilterMode>(m);
        }
        else if (std::strcmp(key, "tech_disk_min_level") == 0)
        {
            int lvl = std::atoi(val);
            if (lvl >= 1 && lvl <= 30) g_tech_disk_min_level = lvl;
        }
        else if (std::strcmp(key, "filter_current_area") == 0) g_filter_current_area = std::atoi(val) != 0;
        else if (std::strcmp(key, "flash_new_drops")     == 0) g_flash_new_drops     = std::atoi(val) != 0;
        else if (std::strcmp(key, "blink_rare_items")    == 0) g_blink_rare_items    = std::atoi(val) != 0;
        else if (std::strcmp(key, "hp_alert_enabled")    == 0) g_hp_alert_enabled    = std::atoi(val) != 0;
        else if (std::strcmp(key, "hp_alert_threshold_pct") == 0)
        {
            int v = std::atoi(val);
            if (v >= 5 && v <= 90) g_hp_alert_threshold_pct = v;
        }
        else if (std::strcmp(key, "hp_alert_beep")       == 0) g_hp_alert_beep       = std::atoi(val) != 0;
        else if (std::strcmp(key, "tool_show_mask")      == 0)
        {
            // Packed bitmask of the 32 g_show_tool_sub flags, stored as
            // a decimal or 0x-prefixed hex integer so humans can edit it
            // if they really want to.
            const uint32_t mask = static_cast<uint32_t>(
                std::strtoul(val, nullptr, 0));
            for (int i = 0; i < 32; ++i)
                g_show_tool_sub[i] = (mask & (1u << i)) != 0;
        }
        else if (std::strcmp(key, "filter_hide_low_hit_weapons")   == 0) g_filter_hide_low_hit_weapons   = std::atoi(val) != 0;
        else if (std::strcmp(key, "filter_hit_min")                == 0)
        {
            int v = std::atoi(val);
            if (v >= 0 && v <= 100) g_filter_hit_min = v;
        }
        else if (std::strcmp(key, "filter_hide_low_socket_armor")  == 0) g_filter_hide_low_socket_armor  = std::atoi(val) != 0;
        else if (std::strcmp(key, "filter_hide_four_socket_armor") == 0) g_filter_hide_four_socket_armor = std::atoi(val) != 0;
        else if (std::strcmp(key, "filter_hide_units")             == 0) g_filter_hide_units             = std::atoi(val) != 0;
        else if (std::strcmp(key, "filter_hide_meseta")            == 0) g_filter_hide_meseta            = std::atoi(val) != 0;
        else if (std::strcmp(key, "show_item_distance")            == 0) g_show_item_distance            = std::atoi(val) != 0;
        else if (std::strcmp(key, "item_distance_cap_enabled")     == 0) g_item_distance_cap_enabled     = std::atoi(val) != 0;
        else if (std::strcmp(key, "item_max_distance")             == 0)
        {
            int v = std::atoi(val);
            if (v >= 0 && v <= 5000) g_item_max_distance = v;
        }
        else if (std::strcmp(key, "show_item_arrow")               == 0) g_show_item_arrow               = std::atoi(val) != 0;
        else if (std::strcmp(key, "arrow_mode")                    == 0)
        {
            int m = std::atoi(val);
            if (m == 0 || m == 1)
                g_arrow_mode = static_cast<ArrowMode>(m);
        }
        else chords::ReadConfigKey(key, val);
    }

    std::fclose(f);

    PSO_LOG("LoadConfig: done (enabled=%d always_visible=%d show_hp=%d show_items=%d)",
            g_enabled ? 1 : 0, g_always_visible ? 1 : 0,
            g_show_monster_hp ? 1 : 0, g_show_floor_items ? 1 : 0);
}

static void SaveConfig()
{
    FILE *f = std::fopen(g_config_path.c_str(), "w");
    if (f == nullptr) return;

    std::fprintf(f, "# Pixelated's PSOBB Mods — persisted automatically\n");
    std::fprintf(f, "enabled=%d\n",           g_enabled           ? 1 : 0);
    std::fprintf(f, "always_visible=%d\n",    g_always_visible    ? 1 : 0);
    std::fprintf(f, "show_monster_hp=%d\n",   g_show_monster_hp   ? 1 : 0);
    std::fprintf(f, "show_floor_items=%d\n",  g_show_floor_items  ? 1 : 0);
    std::fprintf(f, "show_hp_numbers=%d\n",   g_show_hp_numbers   ? 1 : 0);
    std::fprintf(f, "show_hp_bar=%d\n",       g_show_hp_bar       ? 1 : 0);
    std::fprintf(f, "show_count_header=%d\n", g_show_count_header ? 1 : 0);
    std::fprintf(f, "collapse_boss_parts=%d\n", g_collapse_boss_parts ? 1 : 0);
    std::fprintf(f, "hide_unnamed_monsters=%d\n", g_hide_unnamed_monsters ? 1 : 0);
    std::fprintf(f, "bar_width=%.2f\n",       g_bar_width);
    std::fprintf(f, "monsters_anchor=%d\n",       static_cast<int>(g_monsters_anchor));
    std::fprintf(f, "monsters_window_x=%.2f\n",   g_monsters_window_x);
    std::fprintf(f, "monsters_window_y=%.2f\n",   g_monsters_window_y);
    std::fprintf(f, "monsters_window_alpha=%.2f\n", g_monsters_window_alpha);
    std::fprintf(f, "items_anchor=%d\n",          static_cast<int>(g_items_anchor));
    std::fprintf(f, "items_window_x=%.2f\n",      g_items_window_x);
    std::fprintf(f, "items_window_y=%.2f\n",      g_items_window_y);
    std::fprintf(f, "items_window_alpha=%.2f\n",  g_items_window_alpha);
    std::fprintf(f, "filter_mode=%d\n",         static_cast<int>(g_filter_mode));
    std::fprintf(f, "tech_disk_min_level=%d\n", g_tech_disk_min_level);
    std::fprintf(f, "filter_current_area=%d\n", g_filter_current_area ? 1 : 0);
    std::fprintf(f, "flash_new_drops=%d\n",     g_flash_new_drops ? 1 : 0);
    std::fprintf(f, "blink_rare_items=%d\n",    g_blink_rare_items ? 1 : 0);
    std::fprintf(f, "hp_alert_enabled=%d\n",    g_hp_alert_enabled ? 1 : 0);
    std::fprintf(f, "hp_alert_threshold_pct=%d\n", g_hp_alert_threshold_pct);
    std::fprintf(f, "hp_alert_beep=%d\n",       g_hp_alert_beep ? 1 : 0);
    std::fprintf(f, "xp_window_enabled=%d\n",  g_xp_window_enabled ? 1 : 0);
    std::fprintf(f, "xp_window_anchor=%d\n",   static_cast<int>(g_xp_window_anchor));
    std::fprintf(f, "xp_window_x=%.2f\n",      g_xp_window_x);
    std::fprintf(f, "xp_window_y=%.2f\n",      g_xp_window_y);
    std::fprintf(f, "xp_window_alpha=%.2f\n",  g_xp_window_alpha);
    std::fprintf(f, "xp_track_enabled=%d\n",   g_xp_track_enabled ? 1 : 0);
    std::fprintf(f, "mag_timer_enabled=%d\n",  g_mag_timer_enabled ? 1 : 0);
    std::fprintf(f, "chord_overlay_enabled=%d\n",        g_chord_overlay_enabled ? 1 : 0);
    std::fprintf(f, "chord_overlay_alpha=%.2f\n",        g_chord_overlay_alpha);
    std::fprintf(f, "chord_overlay_show_hints=%d\n",     g_chord_overlay_show_hints ? 1 : 0);
    std::fprintf(f, "chord_overlay_flash_ms=%d\n",       g_chord_overlay_flash_ms);
    std::fprintf(f, "chord_overlay_calibrate_mode=%d\n", g_chord_overlay_calibrate_mode ? 1 : 0);
    std::fprintf(f, "chord_slot_center_x=%.2f\n",        g_chord_slot_center_x);
    std::fprintf(f, "chord_slot_bottom_y=%.2f\n",        g_chord_slot_bottom_y);
    std::fprintf(f, "chord_slot_pitch=%.2f\n",           g_chord_slot_pitch);
    std::fprintf(f, "chord_slot_width=%.2f\n",           g_chord_slot_width);
    std::fprintf(f, "chord_slot_height=%.2f\n",          g_chord_slot_height);
    std::fprintf(f, "chord_badge_gap=%.2f\n",            g_chord_badge_gap);
    std::fprintf(f, "chord_slot_shape=%d\n",             g_chord_slot_shape);
    for (int i = 0; i < 10; ++i)
        std::fprintf(f, "chord_slot_x_offset_%d=%.2f\n", i, g_chord_slot_x_offset[i]);
    std::fprintf(f, "wheel_filter_enabled=%d\n",         g_wheel_filter_enabled ? 1 : 0);
    std::fprintf(f, "wheel_throttle_ms=%d\n",            g_wheel_throttle_ms);
    std::fprintf(f, "wheel_filter_mode=%d\n",            g_wheel_filter_mode);
    std::fprintf(f, "wheel_debug_log=%d\n",              g_wheel_debug_log ? 1 : 0);
    std::fprintf(f, "stick_enabled=%d\n",                g_stick_enabled ? 1 : 0);
    std::fprintf(f, "stick_zoom_enabled=%d\n",           g_stick_zoom_enabled ? 1 : 0);
    std::fprintf(f, "stick_invert_y=%d\n",               g_stick_invert_y ? 1 : 0);
    std::fprintf(f, "stick_deadzone=%d\n",               g_stick_deadzone);
    std::fprintf(f, "stick_rate_ms=%d\n",                g_stick_rate_ms);
    std::fprintf(f, "vignette_enabled=%d\n",             g_vignette_enabled ? 1 : 0);
    std::fprintf(f, "vignette_low_hp=%d\n",              g_vignette_low_hp ? 1 : 0);
    std::fprintf(f, "vignette_poison=%d\n",              g_vignette_poison ? 1 : 0);
    std::fprintf(f, "vignette_paralyzed=%d\n",           g_vignette_paralyzed ? 1 : 0);
    std::fprintf(f, "vignette_frozen=%d\n",              g_vignette_frozen ? 1 : 0);
    std::fprintf(f, "vignette_shocked=%d\n",             g_vignette_shocked ? 1 : 0);
    std::fprintf(f, "vignette_slow=%d\n",                g_vignette_slow ? 1 : 0);
    std::fprintf(f, "vignette_confused=%d\n",            g_vignette_confused ? 1 : 0);
    std::fprintf(f, "vignette_jellen=%d\n",              g_vignette_jellen ? 1 : 0);
    std::fprintf(f, "vignette_zalure=%d\n",              g_vignette_zalure ? 1 : 0);
    std::fprintf(f, "vignette_intensity=%.2f\n",         g_vignette_intensity);
    std::fprintf(f, "vignette_pulse_hz=%.2f\n",          g_vignette_pulse_hz);
    std::fprintf(f, "vignette_thickness_frac=%.2f\n",    g_vignette_thickness_frac);
    std::fprintf(f, "buff_panel_enabled=%d\n",   g_buff_panel_enabled ? 1 : 0);
    std::fprintf(f, "buff_panel_x=%.2f\n",       g_buff_panel_x);
    std::fprintf(f, "buff_panel_y=%.2f\n",       g_buff_panel_y);
    std::fprintf(f, "buff_panel_alpha=%.2f\n",   g_buff_panel_alpha);
    std::fprintf(f, "buff_shifta_reminder=%d\n", g_buff_shifta_reminder ? 1 : 0);
    std::fprintf(f, "buff_deband_reminder=%d\n", g_buff_deband_reminder ? 1 : 0);
    {
        uint32_t mask = 0;
        for (int i = 0; i < 32; ++i)
            if (g_show_tool_sub[i]) mask |= (1u << i);
        std::fprintf(f, "tool_show_mask=0x%08X\n", mask);
    }
    std::fprintf(f, "filter_hide_low_hit_weapons=%d\n",   g_filter_hide_low_hit_weapons   ? 1 : 0);
    std::fprintf(f, "filter_hit_min=%d\n",                g_filter_hit_min);
    std::fprintf(f, "filter_hide_low_socket_armor=%d\n",  g_filter_hide_low_socket_armor  ? 1 : 0);
    std::fprintf(f, "filter_hide_four_socket_armor=%d\n", g_filter_hide_four_socket_armor ? 1 : 0);
    std::fprintf(f, "filter_hide_units=%d\n",             g_filter_hide_units             ? 1 : 0);
    std::fprintf(f, "filter_hide_meseta=%d\n",            g_filter_hide_meseta            ? 1 : 0);
    std::fprintf(f, "show_item_distance=%d\n",            g_show_item_distance            ? 1 : 0);
    std::fprintf(f, "item_distance_cap_enabled=%d\n",     g_item_distance_cap_enabled ? 1 : 0);
    std::fprintf(f, "item_max_distance=%d\n",             g_item_max_distance);
    std::fprintf(f, "show_item_arrow=%d\n",               g_show_item_arrow               ? 1 : 0);
    std::fprintf(f, "arrow_mode=%d\n",                    static_cast<int>(g_arrow_mode));
    chords::WriteConfig(f);

    std::fclose(f);
    g_config_dirty = false;
}

// ============================================================================
// ReShade add-on entry points
// ============================================================================

// Wrapped in a single extern "C" block instead of per-declaration so GCC
// doesn't warn "initialized and declared 'extern'" on each line.
extern "C" {
    __declspec(dllexport) const char *NAME = "Pixelated's PSOBB Mods";
    __declspec(dllexport) const char *DESCRIPTION =
        "Live monster HP overlay, filtered floor item list, and controller "
        "chord remapper (LT/RT + face buttons -> palette slots 1-0) for "
        "Ephinea PSOBB. Memory is read-only; input chords hook DirectInput8.";
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID /*lpReserved*/)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        g_module = hModule;
        ResolveAddonPaths();
        // Logger must be initialized before any other Install step so
        // every subsequent failure is captured.
        pso_log::Init(g_addon_dir.c_str());
        pso_log::InstallCrashHandler();
        PSO_LOG("DllMain ATTACH: hModule=0x%p", hModule);

        // Host PE fingerprint. Every memory offset we use — the item
        // arrays, the player array, the entity ref patch, the episode
        // byte, the monster struct offsets, the floor item layout —
        // is hardcoded for a specific PsoBB.exe build. If Ephinea
        // ever ships a client update the offsets can silently shift
        // and the add-on will either read garbage or read nothing.
        // Logging the PE's ImageBase, SizeOfImage, and TimeDateStamp
        // makes a post-update break take seconds to notice (just
        // grep the log for these lines and compare against the last
        // known-good session).
        {
            HMODULE psobb = GetModuleHandleA("psobb.exe");
            if (psobb != nullptr)
            {
                const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(psobb);
                if (dos != nullptr && dos->e_magic == IMAGE_DOS_SIGNATURE)
                {
                    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
                        reinterpret_cast<const uint8_t *>(psobb) + dos->e_lfanew);
                    if (nt != nullptr && nt->Signature == IMAGE_NT_SIGNATURE)
                    {
                        PSO_LOG("PsoBB PE: base=0x%p SizeOfImage=0x%08X "
                                "TimeDateStamp=0x%08X Subsystem=%u",
                                (void *)psobb,
                                (unsigned)nt->OptionalHeader.SizeOfImage,
                                (unsigned)nt->FileHeader.TimeDateStamp,
                                (unsigned)nt->OptionalHeader.Subsystem);
                    }
                }
            }
            else
            {
                PSO_LOG("PsoBB PE: psobb.exe module handle not found");
            }
        }

        // Resolve every hardcoded PSOBB global address. Two phases:
        //   1. Rebase against the loader-chosen base (no-op for the
        //      common case where PSOBB loads at its preferred
        //      0x00400000 fixed image base).
        //   2. Instruction-signature scan in PSOBB's .text for each
        //      global, replacing the rebased fallback with the operand
        //      extracted from a unique pattern match. On any failure
        //      the rebased fallback is kept.
        pso_addresses::InitializeAndLog();

        if (!reshade::register_addon(hModule))
        {
            PSO_LOG("DllMain ATTACH: reshade::register_addon FAILED");
            return FALSE;
        }
        PSO_LOG("DllMain ATTACH: reshade::register_addon ok");

        LoadConfig();
        LoadCuratedIdLists();

        PSO_LOG("DllMain ATTACH: calling chords::Install");
        if (!chords::Install())
            PSO_LOG("DllMain ATTACH: chords::Install returned false");
        else
            PSO_LOG("DllMain ATTACH: chords::Install ok");

        reshade::register_overlay("Pixelated's PSOBB Mods", draw_pixelated_mods_overlay);
        reshade::register_event<reshade::addon_event::reshade_overlay>(
            on_reshade_overlay_event);
        PSO_LOG("DllMain ATTACH: overlay + event registered, init complete");
        break;

    case DLL_PROCESS_DETACH:
        PSO_LOG("DllMain DETACH: begin");
        SaveConfig();
        WheelFilterUninstall();
        chords::Uninstall();
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(
            on_reshade_overlay_event);
        reshade::unregister_overlay("Pixelated's PSOBB Mods", draw_pixelated_mods_overlay);
        reshade::unregister_addon(hModule);
        PSO_LOG("DllMain DETACH: end");
        pso_log::Shutdown();
        break;
    }
    return TRUE;
}
