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
static void LoadItemNames();
static uintptr_t GetLocalPlayerPtr();

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

    constexpr uintptr_t MonsterEntityFlags = 0x30;   // u16, bit 0x0800 = dead
    constexpr uintptr_t MonsterUnitxtID    = 0x378;
    constexpr uintptr_t MonsterHP          = 0x334;
    constexpr uintptr_t MonsterHPMax       = 0x2BC;
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
    constexpr uint32_t  MaxItemsPerFloorSanityCap = 256;  // defensive cap
}

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

template <typename T>
static T SafeRead(uintptr_t addr, T fallback = T{})
{
    if (!IsValidReadPtr(reinterpret_cast<const void *>(addr), sizeof(T)))
        return fallback;
    return *reinterpret_cast<const T *>(addr);
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
    std::memcpy(out, reinterpret_cast<const void *>(addr), size);
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
// Monster list
// ============================================================================

struct Monster
{
    uintptr_t   address;
    uint32_t    unitxt_id;
    uint16_t    hp;
    uint16_t    max_hp;
    uint16_t    entity_flags;
    uint16_t    room;
    bool        is_targeted;  // local player's current target
    std::string name;
};

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
    if (local_player != 0)
    {
        player_room1 = SafeRead<uint16_t>(local_player + pso_offsets::EntityRoom);
        player_room2 = SafeRead<uint16_t>(local_player + pso_offsets::PlayerRoom2);
    }

    out.reserve(entity_count);

    for (uint32_t i = 0; i < entity_count; ++i)
    {
        const uintptr_t slot = entity_array + 4 * (i + player_count);
        const uintptr_t mon_addr = SafeRead<uintptr_t>(slot);
        if (mon_addr == 0) continue;

        Monster m{};
        m.address      = mon_addr;
        m.hp           = SafeRead<uint16_t>(mon_addr + pso_offsets::MonsterHP);
        m.max_hp       = SafeRead<uint16_t>(mon_addr + pso_offsets::MonsterHPMax);
        m.unitxt_id    = SafeRead<uint32_t>(mon_addr + pso_offsets::MonsterUnitxtID);
        m.entity_flags = SafeRead<uint16_t>(mon_addr + pso_offsets::MonsterEntityFlags);
        m.room         = SafeRead<uint16_t>(mon_addr + pso_offsets::EntityRoom);
        {
            const int16_t mon_id =
                SafeRead<int16_t>(mon_addr + pso_offsets::EntityId,
                                  static_cast<int16_t>(-1));
            m.is_targeted =
                (target_id != INT32_MIN) &&
                (static_cast<int>(mon_id) == target_id);
        }

        if (m.hp == 0 || m.max_hp == 0 || m.hp > m.max_hp) continue;
        if ((m.entity_flags & 0x0800) != 0) continue;

        // unitxt_id == 0 is PSO's "no identity assigned" placeholder
        // slot. Every real monster in every episode has a non-zero
        // unitxt index; an entity with unitxt_id=0 is by construction
        // a hitbox / anchor / internal sub-entity that the game never
        // intended to label. Confirmed via the diagnostic log in the
        // Olga Flow fight (the persistent "Unknown 8500/8500" entry
        // alongside the live boss had unitxt_id=0 / name='Unknown' /
        // room=0 / flags=0x0000). This is a structural filter, not a
        // workaround for one boss — any future unnamed sub-entity
        // with a zero unitxt slot will be caught the same way.
        if (m.unitxt_id == 0) continue;

        // Room match required. A monster in a different room than the
        // player is either pre-spawned in a room the player hasn't
        // reached yet, or — the case we care about — a stale corpse
        // left behind after a cutscene transition (Olga Flow, etc).
        if (local_player != 0 &&
            m.room != player_room1 && m.room != player_room2)
        {
            continue;
        }

        m.name = GetMonsterName(m.unitxt_id, ultimate);

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
        // hitboxes (e.g. Olga Flow's anchored body hitbox) with the
        // name "Unknown" in its internal string table, which is how
        // they show up in the HP panel. We log each distinct
        // unnamed unitxt_id once per session so the user can find
        // the exact ID to add to pixelated_mods_monster_hidden.txt.
        const bool unnamed = m.name.empty() || m.name == "Unknown";

        if (unnamed)
        {
            // Log each distinct unnamed unitxt_id the first time we
            // see it this session. Rate-limited to 16 distinct IDs
            // to avoid a log flood if the entity array has many
            // unnamed decorative hitboxes.
            static uint32_t s_seen_ids[16] = {};
            static int      s_seen_count    = 0;
            bool already_logged = false;
            for (int k = 0; k < s_seen_count; ++k)
                if (s_seen_ids[k] == m.unitxt_id) { already_logged = true; break; }
            if (!already_logged && s_seen_count < 16)
            {
                s_seen_ids[s_seen_count++] = m.unitxt_id;
                PSO_LOG("GetAliveMonsters: unnamed monster unitxt_id=%u "
                        "name='%s' hp=%u/%u flags=0x%04X room=%u addr=0x%IX",
                        m.unitxt_id, m.name.c_str(),
                        m.hp, m.max_hp, m.entity_flags,
                        m.room, m.address);
            }

            // Blanket fallback: if the user has explicitly enabled
            // "Hide unnamed monsters" they lose the diagnostic value
            // but get the immediate effect of every unnamed entity
            // disappearing. Default is off.
            if (g_hide_unnamed_monsters)
                continue;

            if (m.name.empty())
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "Monster #%u", m.unitxt_id);
                m.name = buf;
            }
        }

        out.push_back(std::move(m));
    }

    return out;
}

// ============================================================================
// Floor items
// ============================================================================

struct FloorItem
{
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
};

// Item name tables loaded from pixelated_mods_*.txt next to the DLL.
static std::unordered_map<uint32_t, std::string> g_item_names;
static std::unordered_map<uint32_t, std::string> g_tech_names;
static std::unordered_map<uint32_t, std::string> g_special_names;

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

static const char *ItemName(uint32_t id24)
{
    auto it = g_item_names.find(id24);
    return it != g_item_names.end() ? it->second.c_str() : nullptr;
}

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
static bool       g_filter_current_area    = false; // only show items on player's current floor
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
// Default 1500 world units acts as a natural auto-declutter: items
// you've walked past drop off the list as you move forward, and
// come BACK when you walk backward through the same area. Unlike
// a time-based filter, it's self-correcting. 0 means no cap.
// Rares bypass the cap regardless.
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
static bool g_hp_alert_beep        = false;  // audio beep on threshold crossing

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

    // "To Next Lv" is computed, not stored. kLevelCumExp[level] gives
    // the cumulative EXP threshold to reach level+1 from scratch, so
    // the remaining EXP is that threshold minus current total EXP.
    // Index check guards against a corrupted level read.
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

// Short ring (Current rate — 64-second trailing)
static constexpr int     kXpSampleCount    = 128;
static constexpr DWORD   kXpSampleInterval = 500;  // ms between samples
struct XpSample { DWORD tick; uint32_t exp; };
static XpSample g_xp_samples[kXpSampleCount] = {};
static int      g_xp_head                    = 0;  // next write slot
static int      g_xp_count                   = 0;  // filled slots (<=kXpSampleCount)
static DWORD    g_xp_last_sample_tick        = 0;

// Long ring (Hour rate — 3600-second trailing, 5s cadence).
// At 5s between samples, 720 slots cover exactly 3600 seconds. We
// consider the ring "ready" once the newest-minus-oldest spread
// reaches kXpHourReadyMs (~55 minutes) so the Hour rate can be
// reported as a genuine trailing 60-minute average.
static constexpr int     kXpHourSampleCount    = 720;
static constexpr DWORD   kXpHourSampleInterval = 5000;  // 5s
static constexpr DWORD   kXpHourReadyMs        = 55 * 60 * 1000; // 55 min
static XpSample g_xp_hour_samples[kXpHourSampleCount] = {};
static int      g_xp_hour_head                         = 0;
static int      g_xp_hour_count                        = 0;
static DWORD    g_xp_hour_last_sample_tick             = 0;

static uint32_t g_xp_session_baseline        = 0;  // cumulative EXP at first
                                                   // observation this session
static bool     g_xp_session_baseline_set    = false;
static DWORD    g_xp_session_start_tick      = 0;  // when baseline was stamped

// "Peak" = highest Hour rate observed in the session. Only starts
// updating once the long ring has at least kXpHourReadyMs of data,
// so a single-fight spike never latches. Resets with the baseline
// on character switch.
static double   g_xp_peak_hour_rate          = 0.0;
static bool     g_xp_peak_valid              = false;

// Feed the rolling EXP buffer with the latest snapshot. Called once per
// frame from the stats window render path. Samples are time-gated.
//
// EXP only ever monotonically increases for a single character, so a
// decrease across calls means we're looking at a different character
// (login switch, quest reset, etc.) — drop the ring and the baseline
// and start fresh.
static void UpdateXpTracker(const PlayerXp &xp)
{
    if (!xp.valid) return;
    const DWORD now = GetTickCount();

    // Character switch: EXP can only increase for a single character,
    // so a decrease across calls means we're looking at a different
    // player. Drop both rings, the baseline, and the peak.
    if (g_xp_session_baseline_set && xp.exp < g_xp_session_baseline)
    {
        g_xp_head                 = 0;
        g_xp_count                = 0;
        g_xp_hour_head            = 0;
        g_xp_hour_count           = 0;
        g_xp_session_baseline_set = false;
        g_xp_peak_hour_rate       = 0.0;
        g_xp_peak_valid           = false;
    }

    if (!g_xp_session_baseline_set)
    {
        g_xp_session_baseline     = xp.exp;
        g_xp_session_baseline_set = true;
        g_xp_session_start_tick   = now;
    }

    // Short ring (Current rate) — 500 ms cadence.
    if (g_xp_count == 0 || now - g_xp_last_sample_tick >= kXpSampleInterval)
    {
        g_xp_last_sample_tick = now;
        g_xp_samples[g_xp_head].tick = now;
        g_xp_samples[g_xp_head].exp  = xp.exp;
        g_xp_head = (g_xp_head + 1) % kXpSampleCount;
        if (g_xp_count < kXpSampleCount) ++g_xp_count;
    }

    // Long ring (Hour rate) — 5 s cadence.
    if (g_xp_hour_count == 0 ||
        now - g_xp_hour_last_sample_tick >= kXpHourSampleInterval)
    {
        g_xp_hour_last_sample_tick = now;
        g_xp_hour_samples[g_xp_hour_head].tick = now;
        g_xp_hour_samples[g_xp_hour_head].exp  = xp.exp;
        g_xp_hour_head = (g_xp_hour_head + 1) % kXpHourSampleCount;
        if (g_xp_hour_count < kXpHourSampleCount) ++g_xp_hour_count;
    }
}

// Total EXP gained since the session baseline was first stamped.
// Returns 0 before the first valid sample or immediately after a
// character switch.
static uint32_t ComputeXpGained(const PlayerXp &xp)
{
    if (!g_xp_session_baseline_set || !xp.valid) return 0;
    if (xp.exp < g_xp_session_baseline) return 0;
    return xp.exp - g_xp_session_baseline;
}

// Helper: compute an EXP-per-hour rate across a ring buffer between
// the oldest and newest samples. Returns 0.0 if there isn't at least
// one second of history, or if the samples aren't monotonically
// increasing (which would indicate a character switch mid-window).
// Out parameters report the span length for the caller.
static double RingRatePerHour(const XpSample *ring,
                              int count,
                              int capacity,
                              int head,
                              DWORD *out_span_ms = nullptr)
{
    if (count < 2)
    {
        if (out_span_ms) *out_span_ms = 0;
        return 0.0;
    }
    const int oldest_idx = (count < capacity) ? 0 : head;
    const int newest_idx = (head - 1 + capacity) % capacity;
    const XpSample &oldest = ring[oldest_idx];
    const XpSample &newest = ring[newest_idx];
    const DWORD dt_ms = newest.tick - oldest.tick;
    if (out_span_ms) *out_span_ms = dt_ms;
    if (dt_ms < 1000)               return 0.0;
    if (newest.exp < oldest.exp)    return 0.0;

    const double dexp = static_cast<double>(newest.exp - oldest.exp);
    const double dhr  = static_cast<double>(dt_ms) / 3600000.0;
    return dexp / dhr;
}

// Current rate: trailing ~64 seconds, responsive to the very last
// minute of gameplay. Zero before the ring has any useful history
// and zero while the player is idle.
static double ComputeXpPerHour()
{
    return RingRatePerHour(
        g_xp_samples, g_xp_count, kXpSampleCount, g_xp_head);
}

// Hour rate + Peak. Peak is the highest Hour rate observed since the
// long ring accumulated at least kXpHourReadyMs of data, so it never
// latches onto a single-fight spike — you have to actually sustain
// the rate for nearly a full hour before it can claim a new peak.
//
//   span_ms  — age spread of the long ring (newest.tick - oldest.tick)
//   hour_rate — rate computed over the long ring's full span
//   ready    — true once span_ms >= kXpHourReadyMs (~55 min). Before
//              that, hour_rate is still returned (it's an honest
//              short-window extrapolation) but peak is not updated.
struct XpHourState
{
    double hour_rate;
    bool   ready;
    DWORD  span_ms;
    double peak_rate;
    bool   peak_valid;
};

static XpHourState ComputeXpHourState()
{
    XpHourState s{};
    s.hour_rate = RingRatePerHour(
        g_xp_hour_samples, g_xp_hour_count, kXpHourSampleCount,
        g_xp_hour_head, &s.span_ms);
    s.ready = s.span_ms >= kXpHourReadyMs;

    if (s.ready && s.hour_rate > g_xp_peak_hour_rate)
    {
        g_xp_peak_hour_rate = s.hour_rate;
        g_xp_peak_valid     = true;
    }
    s.peak_rate  = g_xp_peak_hour_rate;
    s.peak_valid = g_xp_peak_valid;
    return s;
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
// Grinder 15 / 30 are still below what fully-grinded rares reach
// (most rare weapons cap in the +30..+60 range, boss-gated rares
// higher), so a freshly-dropped rare at +0..+5 still has to earn
// its "show" via attributes or special, not via grinder inflation.
static bool WeaponHasNotableStats(const uint8_t b[12])
{
    const uint8_t grinder = b[3];
    const uint8_t special = b[4] & 0x3F;

    // Find the highest non-hit attribute (types 1..4 = Native, ABeast,
    // Machine, Dark) and the hit% (type 5) — both across all 3 slots.
    uint8_t max_attr = 0;
    uint8_t hit_pct  = 0;
    for (int i = 0; i < 3; ++i)
    {
        const uint8_t t = b[6 + i * 2];
        const uint8_t v = b[7 + i * 2];
        if (t == 0) continue;
        if (t == 5) { if (v > hit_pct) hit_pct = v; }
        else        { if (v > max_attr) max_attr = v; }
    }

    // Exceptional single feature — always show.
    if (grinder  >= 30) return true;
    if (max_attr >= 50) return true;
    if (hit_pct  >= 40) return true;

    // Otherwise require two "notable" features.
    int features = 0;
    if (grinder  >= 15) ++features;
    if (special  >  0)  ++features;
    if (max_attr >= 40) ++features;
    if (hit_pct  >= 30) ++features;
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
static bool ShouldShowItem(const uint8_t b[12], const uint8_t b2[4],
                           uint32_t id24)
{
    const uint8_t type = b[0];
    const uint8_t sub  = b[1];

    if (g_filter_mode == FilterMode::All)
        return true;

    // Rares always pass. Has to be checked BEFORE the hidden-list check
    // so that a rare item the user accidentally added to pixelated_mods_hidden
    // still surfaces.
    if (IsRareItem(b, id24))
        return true;

    // User-curated hidden list with stat-based overrides. An item in
    // pixelated_mods_hidden.txt normally hides, but if its stats exceed the
    // thresholds in WeaponHasNotableStats / ArmorHasNotableBonus /
    // ShieldHasNotableBonus, we surface it anyway. The result: trash
    // base-tier drops are filtered out, lucky high-stat rolls of the
    // same ID still appear.
    if (g_hidden_ids.find(id24) != g_hidden_ids.end())
    {
        if (type == 0x00 && WeaponHasNotableStats(b)) return true;
        if (type == 0x01 && sub == 0x01 && ArmorHasNotableBonus(b))  return true;
        if (type == 0x01 && sub == 0x02 && ShieldHasNotableBonus(b)) return true;
        // Units (type 0x01 sub 0x03), mags (type 0x02), tools, meseta,
        // etc. have no stat-based override — they're binary hide/show.
        return false;
    }

    if (g_filter_mode == FilterMode::Gear)
        return type == 0x00 || type == 0x01 || type == 0x02;

    // ===== Notable mode (the default) =====

    // Weapons (type 0x00).
    if (type == 0x00)
    {
        // HideLowHitWeapons: scan the 3 attribute slots for hit% (type
        // byte 5) and hide if it's below HitMin. Item Reader's "stats[6]"
        // is preprocessed; our 12-byte item record stores attributes as
        // (type, value) pairs at bytes [6,7] [8,9] [10,11].
        if (g_filter_hide_low_hit_weapons)
        {
            int hit = 0;
            for (int i = 0; i < 3; ++i)
            {
                if (b[6 + i * 2] == 5)  // attr type 5 == hit%
                {
                    hit = b[7 + i * 2];
                    break;
                }
            }
            if (hit < g_filter_hit_min) return false;
        }
        return true;
    }

    // Frames / armor (type 0x01 sub 0x01). Item Reader's nested socket
    // logic: HideLowSocketArmor on -> only 4-slot armor passes, unless
    // HideFourSocketArmor is also on, in which case nothing passes.
    if (type == 0x01 && sub == 0x01)
    {
        if (g_filter_hide_low_socket_armor)
        {
            const uint8_t slots = b[5];
            if (slots != 4) return false;
            if (g_filter_hide_four_socket_armor) return false;
        }
        return true;
    }

    // Shields / barriers (type 0x01 sub 0x02). Item Reader has no
    // shield-specific filter — shields always pass in Notable mode.
    if (type == 0x01 && sub == 0x02)
        return true;

    // Units (type 0x01 sub 0x03). HideUnits hides every unit
    // unconditionally. Item Reader calls this HideUselessUnits but the
    // implementation has no per-unit logic, so the name is misleading.
    if (type == 0x01 && sub == 0x03)
    {
        if (g_filter_hide_units) return false;
        return true;
    }

    // Mags (type 0x02). Always pass — Item Reader has no mag filter.
    if (type == 0x02)
        return true;

    // Tools (type 0x03). Per-sub-type toggles, same as before.
    if (type == 0x03)
    {
        // Tech disks: sub-type toggle plus level slider.
        if (sub == 0x02)
        {
            if (!g_show_tool_sub[0x02]) return false;
            const int min_idx = g_tech_disk_min_level - 1;
            return static_cast<int>(b[2]) >= min_idx;
        }
        // Event badges 0x12..0x15 share one toggle.
        if (sub >= 0x12 && sub <= 0x15)
            return g_show_tool_sub[0x12];
        if (sub < 32)
            return g_show_tool_sub[sub];
        return true;
    }

    // Meseta (type 0x04). Single all-or-nothing toggle, same as Item
    // Reader's ignoreMeseta. (We do not implement an amount threshold;
    // it's not part of the BBMod model and the user opted to match.)
    if (type == 0x04)
    {
        (void)b2;  // amount lives in b2 if we ever revisit this
        return !g_filter_hide_meseta;
    }

    // Unknown type: show by default. Matches Item Reader's permissive
    // behavior — anything new PSO adds will surface so the user can
    // see it and curate.
    return true;
}

static std::string FormatItem(const uint8_t b[12], const uint8_t b2[4],
                              uint32_t id24)
{
    const uint8_t type = b[0];
    const uint8_t sub  = b[1];
    const char *name   = ItemName(id24);

    char id_buf[16];
    std::snprintf(id_buf, sizeof(id_buf), "[%06X]", id24);
    const char *display_name = name ? name : id_buf;

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
        // Weapon: Name +grinder native/abeast/machine/dark|hit
        int native = 0, abeast = 0, machine = 0, dark = 0, hit = 0;
        for (int i = 0; i < 3; ++i)
        {
            const uint8_t attr_type = b[6 + i * 2];
            const uint8_t attr_val  = b[7 + i * 2];
            switch (attr_type)
            {
            case 1: native  = attr_val; break;
            case 2: abeast  = attr_val; break;
            case 3: machine = attr_val; break;
            case 4: dark    = attr_val; break;
            case 5: hit     = attr_val; break;
            default: break;
            }
        }
        const uint8_t grinder = b[3];
        const uint8_t special = b[4] & 0x3F;

        const char *special_name = "";
        auto sp_it = g_special_names.find(special);
        if (special != 0 && sp_it != g_special_names.end())
            special_name = sp_it->second.c_str();

        if (grinder > 0 || native || abeast || machine || dark || hit)
        {
            std::snprintf(out, sizeof(out), "%s%s%s +%u  %d/%d/%d/%d|%d",
                          special_name, (*special_name ? " " : ""),
                          display_name, grinder,
                          native, abeast, machine, dark, hit);
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
        // Tech disk
        const uint8_t level   = b[2];
        const uint8_t tech_id = b[4];
        auto t_it = g_tech_names.find(tech_id);
        const char *tech_name = (t_it != g_tech_names.end())
                                    ? t_it->second.c_str() : "Technique";
        std::snprintf(out, sizeof(out), "%s Lv%u", tech_name, level + 1);
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

static std::vector<FloorItem> GetFloorItems()
{
    std::vector<FloorItem> out;

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
    const uint32_t player_floor =
        g_filter_current_area ? player_floor_actual : 0xFFFFFFFF;

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

    for (uint32_t area = 0; area < pso_offsets::AreaCount; ++area)
    {
        if (g_filter_current_area && player_floor != 0xFFFFFFFF
            && area != player_floor)
            continue;

        const uintptr_t floor_items_base =
            SafeRead<uintptr_t>(ptrs_array + area * 4);
        const uint32_t  floor_item_count =
            SafeRead<uint32_t>(counts_array + area * 4);
        if (floor_items_base == 0 || floor_item_count == 0) continue;

        // Defensive cap: a corrupted count read could send us iterating
        // off the end of the array. Real floors top out around a few
        // dozen items, but PSO has been observed with much more during
        // chaotic boss fights.
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

            // Distance filter. Two purposes: declutter the list by
            // hiding items you've walked past, AND auto-surface them
            // again when you walk back into range (self-correcting
            // vs a time-based age filter that loses drops forever).
            // Rares always bypass. 0 means unlimited.
            if (g_item_max_distance > 0 && have_player_pos &&
                fi.dist_xz > static_cast<float>(g_item_max_distance) &&
                !is_rare_item)
            {
                continue;
            }

            if (!ShouldShowItem(fi.bytes, fi.bytes2, fi.id24)) continue;

            fi.is_rare         = is_rare_item;
            fi.display         = FormatItem(fi.bytes, fi.bytes2, fi.id24);
            fi.first_seen_tick = item_first_seen;

            out.push_back(std::move(fi));
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

// Tunables — persisted to pixelated_mods.ini next to the DLL. Defaults place the
// HUD below PSO's in-game minimap (top-right corner, dropped ~280px).
bool   g_enabled          = true;
bool   g_always_visible   = true;
bool   g_show_monster_hp  = true;
bool   g_show_floor_items = true;
bool   g_show_hp_numbers  = true;
bool   g_show_hp_bar      = true;
float  g_bar_width        = 140.0f;
Anchor g_anchor           = Anchor::TopRight;
float  g_window_x         = 10.0f;
float  g_window_y         = 280.0f;
float  g_window_alpha     = 0.60f;
bool   g_show_count_header = true;

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
static const std::unordered_set<std::string> &CollapseBossNames()
{
    static const std::unordered_set<std::string> s = {
        "Vol Opt",
        "Vol Opt ver.2",
        "Barba Ray",
        "Gol Dragon",
        "Saint-Milion",
        "Shambertin",
        "Kondrieu",
    };
    return s;
}
// g_hidden_monster_ids and g_hide_unnamed_monsters live at file
// scope (above, near g_hidden_ids) because GetAliveMonsters needs
// them and is defined earlier in the file than this namespace.
bool   g_config_dirty      = false;

// ---- Stats & alerts window (separate from monster HP / floor items HUD) ----
// Holds the low-HP alert, mag feeding timer, and XP/hour tracker. Default
// anchor is top-center so it doesn't overlap the minimap (top-right) or
// the action palette (bottom-center). Has its own enable/anchor/offset/
// opacity controls independent of the main HUD window above.
bool   g_stats_enabled      = true;
Anchor g_stats_anchor       = Anchor::TopCenter;
float  g_stats_window_x     = 0.0f;
float  g_stats_window_y     = 40.0f;
float  g_stats_window_alpha = 0.70f;

// One row in the monster HP table. Either a single monster (targeted
// or when collapse-by-name is off) or an aggregate of N monsters
// sharing a display name (untargeted, when collapse-by-name is on).
struct HpRow
{
    std::string name;       // display name, no count suffix
    uint32_t    hp;         // sum across aggregated members
    uint32_t    max_hp;     // sum across aggregated members
    uint32_t    count;      // 1 for single rows, N for aggregates
    bool        is_targeted;
};

// Build the rendered row list from the raw monsters vector.
//
// Rules:
//   - Targeted entity always gets its own row, rendered cyan, never
//     folded. Lets the player see the HP of the specific boss part
//     they're currently hitting.
//   - Untargeted entities whose display name is in the hardcoded
//     CollapseBossNames() set fold into one aggregate row per name
//     with summed HP/MaxHP and a "(×N)" suffix.
//   - Everything else (Boomas, Canadines, every normal mob) renders
//     as individual rows — we never touch non-boss spawns.
//   - When g_collapse_boss_parts is false, even boss parts render
//     individually (useful if you want raw part visibility).
//   - Row order is first-seen-stable to avoid flicker across frames.
static std::vector<HpRow> BuildHpRows(const std::vector<Monster> &monsters)
{
    std::vector<HpRow> rows;
    if (monsters.empty()) return rows;

    if (!g_collapse_boss_parts)
    {
        rows.reserve(monsters.size());
        for (const auto &m : monsters)
            rows.push_back({m.name, m.hp, m.max_hp, 1, m.is_targeted});
        return rows;
    }

    const auto &boss_names = CollapseBossNames();
    rows.reserve(monsters.size());
    // Indices into `rows` for boss-name aggregate entries, keyed by
    // display name. Non-boss rows never appear here.
    std::unordered_map<std::string, size_t> agg_by_name;

    for (const auto &m : monsters)
    {
        const bool is_boss_name =
            boss_names.find(m.name) != boss_names.end();

        // Targeted entities or non-boss names: always an individual row.
        if (m.is_targeted || !is_boss_name)
        {
            rows.push_back({m.name, m.hp, m.max_hp, 1, m.is_targeted});
            continue;
        }

        // Untargeted boss part: fold into the aggregate for that name.
        auto it = agg_by_name.find(m.name);
        if (it == agg_by_name.end())
        {
            agg_by_name[m.name] = rows.size();
            rows.push_back({m.name, m.hp, m.max_hp, 1, false});
        }
        else
        {
            HpRow &r = rows[it->second];
            r.hp     += m.hp;
            r.max_hp += m.max_hp;
            r.count  += 1;
        }
    }
    return rows;
}

void render_hp_table(const std::vector<Monster> &monsters)
{
    if (monsters.empty())
    {
        ImGui::TextDisabled("No enemies.");
        return;
    }

    if (g_show_count_header)
        ImGui::Text("Enemies: %zu", monsters.size());

    const std::vector<HpRow> rows = BuildHpRows(monsters);

    if (ImGui::BeginTable("monsters", 3,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("HP",   ImGuiTableColumnFlags_WidthFixed,   96.0f);
        ImGui::TableSetupColumn("Bar",  ImGuiTableColumnFlags_WidthFixed,   g_bar_width + 4.0f);
        ImGui::TableHeadersRow();

        for (const auto &r : rows)
        {
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

            // Compose the display label — suffix "(×N)" when this is
            // an aggregate row of more than one monster.
            char label[192];
            if (r.count > 1)
                std::snprintf(label, sizeof(label), "%s  (\xc3\x97%u)",
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
            if (g_show_hp_numbers)
                ImGui::Text("%u / %u", r.hp, r.max_hp);
            else
                ImGui::TextDisabled("-");

            ImGui::TableSetColumnIndex(2);
            if (g_show_hp_bar && r.max_hp > 0)
            {
                const float frac = static_cast<float>(r.hp) /
                                   static_cast<float>(r.max_hp);
                char overlay[32];
                std::snprintf(overlay, sizeof(overlay), "%.0f%%", frac * 100.0f);

                // Color the bar by HP percentage for at-a-glance
                // threat assessment. ImGui::ProgressBar pulls its
                // fill color from ImGuiCol_PlotHistogram; push a
                // temporary override scoped to this bar only so
                // other ProgressBar users (XP bar etc.) keep the
                // default style.
                ImVec4 bar_color;
                if      (frac > 0.66f) bar_color = ImVec4(0.35f, 0.85f, 0.35f, 1.0f); // green
                else if (frac > 0.33f) bar_color = ImVec4(0.95f, 0.85f, 0.25f, 1.0f); // yellow
                else                   bar_color = ImVec4(0.95f, 0.30f, 0.30f, 1.0f); // red

                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_color);
                ImGui::ProgressBar(frac, ImVec2(g_bar_width, 0.0f), overlay);
                ImGui::PopStyleColor();
            }
        }

        ImGui::EndTable();
    }
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

void render_items_table(const std::vector<FloorItem> &items)
{
    if (items.empty())
    {
        // Different wording depending on the current-area filter so
        // the user knows whether they've seen "nothing here" or
        // "nothing anywhere in the ship."
        ImGui::TextDisabled(g_filter_current_area
                                ? "No items on current floor."
                                : "No items on any floor.");
        return;
    }

    if (g_show_count_header)
        ImGui::Text("Floor items: %zu", items.size());

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
                // Pulse between muted red and saturated red at ~1 Hz.
                const float phase = static_cast<float>((now % 1000)) / 1000.0f;
                const float s = 0.5f + 0.5f * std::sin(phase * 6.2831853f);
                const ImVec4 red(1.0f, 0.25f + 0.15f * s, 0.25f + 0.15f * s, 1.0f);
                ImGui::TextColored(red, "%s", it.display.c_str());
            }
            else if (is_fresh)
            {
                // Fade yellow -> white linearly over the full fresh
                // window. At t=0 the blue channel is 0.3 (saturated
                // yellow); at t=1 it reaches 1.0 (pure white).
                const float t = static_cast<float>(age_ms) /
                                static_cast<float>(kFreshFadeMs);
                const ImVec4 yellow(1.0f, 1.0f, 0.3f + 0.7f * t, 1.0f);
                ImGui::TextColored(yellow, "%s", it.display.c_str());
            }
            else
            {
                ImGui::TextUnformatted(it.display.c_str());
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
}

void render_combined_body()
{
    const bool want_hp    = g_show_monster_hp;
    const bool want_items = g_show_floor_items;

    if (want_hp)
        render_hp_table(GetAliveMonsters());

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

    ImGui::Separator();
    ImGui::TextUnformatted("Sections");
    DIRTY_IF(ImGui::Checkbox("Monster HP", &g_show_monster_hp));
    ImGui::SameLine();
    DIRTY_IF(ImGui::Checkbox("Floor items", &g_show_floor_items));
    ImGui::SameLine();
    DIRTY_IF(ImGui::Checkbox("Count headers", &g_show_count_header));

    ImGui::Separator();
    ImGui::TextUnformatted("Monster HP options");
    DIRTY_IF(ImGui::Checkbox("HP numbers", &g_show_hp_numbers));
    ImGui::SameLine();
    DIRTY_IF(ImGui::Checkbox("HP bar", &g_show_hp_bar));
    DIRTY_IF(ImGui::Checkbox("Collapse multi-part bosses",
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
        "Blanket: hide all unnamed / 'Unknown' monsters",
        &g_hide_unnamed_monsters));
    ImGui::TextDisabled(
        "Fallback if you don't want to curate IDs. Default off.");

    DIRTY_IF(ImGui::SliderFloat("Bar width", &g_bar_width, 40.0f, 300.0f, "%.0f"));

    ImGui::Separator();
    ImGui::TextUnformatted("Stats & alerts window");
    DIRTY_IF(ImGui::Checkbox("Show stats & alerts window", &g_stats_enabled));
    if (g_stats_enabled)
    {
        int s_anchor_idx = static_cast<int>(g_stats_anchor);
        const char *anchor_labels[] = {
            "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right",
            "Top-Center", "Bottom-Center",
        };
        if (ImGui::Combo("Stats anchor", &s_anchor_idx, anchor_labels, 6))
        {
            g_stats_anchor = static_cast<Anchor>(s_anchor_idx);
            g_config_dirty = true;
        }
        DIRTY_IF(ImGui::SliderFloat(
            "Stats X offset", &g_stats_window_x, -2000.0f, 2000.0f, "%.0f px"));
        DIRTY_IF(ImGui::SliderFloat(
            "Stats Y offset", &g_stats_window_y, 0.0f, 2000.0f, "%.0f px"));
        DIRTY_IF(ImGui::SliderFloat(
            "Stats BG opacity", &g_stats_window_alpha, 0.0f, 1.0f, "%.2f"));
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Low HP alert");
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
            "Alert is drawn in the Stats & alerts window above.");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("EXP/hour tracker");
    DIRTY_IF(ImGui::Checkbox(
        "Show level + EXP/hour in stats window", &g_xp_track_enabled));
    ImGui::TextDisabled(
        "Current = trailing 60s. Hour = trailing 60min. Peak = highest\n"
        "Hour rate observed after the long ring has 55min of data, so\n"
        "a single big kill can't spike it.");

    ImGui::Separator();
    ImGui::TextUnformatted("Mag feed timer");
    DIRTY_IF(ImGui::Checkbox(
        "Show mag feed countdown in stats window", &g_mag_timer_enabled));
    ImGui::TextDisabled(
        "Shows seconds until next feed; pulses green when ready.");

    ImGui::Separator();
    ImGui::TextUnformatted("Floor items filter");
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
    }

    DIRTY_IF(ImGui::Checkbox(
        "Only show items on my current floor", &g_filter_current_area));
    DIRTY_IF(ImGui::Checkbox(
        "Flash new drops (yellow for 8s)", &g_flash_new_drops));
    DIRTY_IF(ImGui::Checkbox(
        "Pulse rare items (red blink)", &g_blink_rare_items));

    DIRTY_IF(ImGui::Checkbox(
        "Show distance to each item", &g_show_item_distance));
    DIRTY_IF(ImGui::SliderInt(
        "Auto-hide past (world units, 0 = off)",
        &g_item_max_distance, 0, 3000));
    ImGui::TextDisabled(
        "Self-declutters as you move forward; items reappear when you");
    ImGui::TextDisabled(
        "walk back into range. Rares always bypass the cap.");

    DIRTY_IF(ImGui::Checkbox(
        "Show direction arrow to each item", &g_show_item_arrow));
    if (g_show_item_arrow)
    {
        int mode_idx = static_cast<int>(g_arrow_mode);
        const char *mode_labels[] = {
            "World (fixed minimap-style compass)",
            "Player-relative (rotates with character)",
        };
        if (ImGui::Combo("Arrow mode", &mode_idx, mode_labels, 2))
        {
            g_arrow_mode = static_cast<ArrowMode>(mode_idx);
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

    ImGui::Separator();
    ImGui::TextUnformatted("Position");

    int anchor_idx = static_cast<int>(g_anchor);
    const char *anchor_labels[] = {
        "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right",
        "Top-Center", "Bottom-Center",
    };
    if (ImGui::Combo("Anchor corner", &anchor_idx, anchor_labels, 6))
    {
        g_anchor = static_cast<Anchor>(anchor_idx);
        g_config_dirty = true;
    }

    // Quick-set presets. "Below Minimap" anchors top-right and drops the
    // HUD below PSO's in-game minimap box.
    if (ImGui::Button("Preset: Below Minimap (default)"))
    {
        g_anchor = Anchor::TopRight;
        g_window_x = 10.0f;
        g_window_y = 280.0f;
        g_config_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Preset: Top-Right"))
    {
        g_anchor = Anchor::TopRight;
        g_window_x = 20.0f;
        g_window_y = 20.0f;
        g_config_dirty = true;
    }
    if (ImGui::Button("Preset: Bottom-Right"))
    {
        g_anchor = Anchor::BottomRight;
        g_window_x = 20.0f;
        g_window_y = 180.0f;  // clear the action palette
        g_config_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Preset: Top-Left"))
    {
        g_anchor = Anchor::TopLeft;
        g_window_x = 20.0f;
        g_window_y = 20.0f;
        g_config_dirty = true;
    }

    DIRTY_IF(ImGui::SliderFloat("X offset", &g_window_x, 0.0f, 2000.0f, "%.0f px"));
    DIRTY_IF(ImGui::SliderFloat("Y offset", &g_window_y, 0.0f, 2000.0f, "%.0f px"));
    DIRTY_IF(ImGui::SliderFloat("BG opacity", &g_window_alpha, 0.0f, 1.0f, "%.2f"));

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

// Render the body of the stats & alerts window. Called from inside the
// window's Begin/End pair in on_reshade_overlay_event. Draws the HP
// alert (if firing), followed by the mag feeding timer and XP/hour
// (both TODO — stubs for now). Returns true if any section had
// content to show so the caller can decide whether to draw the window
// frame at all.
static bool render_stats_body(const HpAlertState &hp_alert)
{
    bool had_any = false;

    if (hp_alert.firing)
    {
        const DWORD t = GetTickCount();
        const float phase = static_cast<float>(t % 700) / 700.0f;
        const float pulse = 0.75f + 0.25f * std::sin(phase * 6.2831853f);
        const ImVec4 red(1.0f, 0.15f * pulse, 0.15f * pulse, 1.0f);
        ImGui::TextColored(red, "!!! LOW HP  %u / %u  (%d%%) !!!",
                           hp_alert.hp, hp_alert.max_hp, hp_alert.pct);
        had_any = true;
    }

    if (g_xp_track_enabled)
    {
        const PlayerXp xp = GetLocalPlayerXp();
        if (xp.valid && xp.level > 0)
        {
            UpdateXpTracker(xp);
            const double      rate   = ComputeXpPerHour();
            const XpHourState hour   = ComputeXpHourState();
            const uint32_t    gained = ComputeXpGained(xp);

            // Three-row compact XP display:
            //   1. Level + current EXP + to-next-level.
            //   2. Session cumulative gain.
            //   3. All three rates inline with " | " separators —
            //      Current (60s trailing, what you're doing right now),
            //      Predicted (60min trailing, where the hour is
            //      heading; short-form "warming Nm" while the long
            //      ring is filling), Best Hour (highest Predicted
            //      ever observed once the long ring had 55min of
            //      data; short-form "need 1h" before it's valid).
            ImGui::Text("Lv %u   EXP %u / NXT %u",
                        xp.level, xp.exp, xp.to_next_level);
            ImGui::Text("Total EXP Gained This Session: %u", gained);

            char current_str[32], predicted_str[40], best_str[40];
            if (g_xp_count < 2)
                std::snprintf(current_str, sizeof(current_str), "gathering");
            else
                std::snprintf(current_str, sizeof(current_str), "%.0f", rate);

            if (hour.ready)
            {
                std::snprintf(predicted_str, sizeof(predicted_str),
                              "%.0f", hour.hour_rate);
            }
            else
            {
                const int remaining_sec =
                    static_cast<int>((kXpHourReadyMs - hour.span_ms) / 1000);
                const int mm = (remaining_sec + 59) / 60;
                std::snprintf(predicted_str, sizeof(predicted_str),
                              "warming %dm", mm);
            }

            if (hour.peak_valid)
                std::snprintf(best_str, sizeof(best_str),
                              "%.0f", hour.peak_rate);
            else
                std::snprintf(best_str, sizeof(best_str), "need 1h");

            ImGui::Text("EXP/hr Current: %s | Predicted: %s | Best Hour: %s",
                        current_str, predicted_str, best_str);

            had_any = true;
        }
    }

    if (g_mag_timer_enabled)
    {
        const MagTimer mag = GetMagFeedTimer();
        if (mag.valid)
        {
            if (mag.ready)
            {
                // Pulse green so the "you can feed now" moment is obvious.
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
            had_any = true;
        }
    }

    return had_any;
}

void on_reshade_overlay_event(reshade::api::effect_runtime *runtime)
{
    if (!g_enabled || !g_always_visible) return;

    // Invalidate the per-frame read cache at the top of every
    // frame. The render body below will resolve player ptr / episode
    // at most once via the cache and all the sub-calls will see
    // the same value.
    FrameCacheReset();

    uint32_t w = 0, h = 0;
    if (runtime != nullptr)
        runtime->get_screenshot_width_and_height(&w, &h);
    if (w == 0 || h == 0) { w = 1920; h = 1080; }

    // HP alert state (also used by the stats window below).
    const HpAlertState hp_alert = CheckLowHpAlert();

    ImVec2 pos, pivot;
    compute_anchor_pos(g_anchor, g_window_x, g_window_y,
                       static_cast<float>(w), static_cast<float>(h),
                       &pos, &pivot);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(g_window_alpha);
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f, 0.0f),
                                        ImVec2(800.0f, FLT_MAX),
                                        nullptr, nullptr);

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

    if (ImGui::Begin("##PSO_HUD_Overlay", nullptr, kFlags))
        render_combined_body();
    ImGui::End();

    // ---- Stats & alerts window (second independently-anchored window) ----
    //
    // Separate from the monster HP / floor items HUD so the low-HP alert,
    // mag feeding timer, and XP/hour tracker can live in their own spot
    // (defaults to top-center) without crowding the main HUD.
    if (g_stats_enabled)
    {
        ImVec2 s_pos, s_pivot;
        compute_anchor_pos(g_stats_anchor, g_stats_window_x, g_stats_window_y,
                           static_cast<float>(w), static_cast<float>(h),
                           &s_pos, &s_pivot);
        ImGui::SetNextWindowPos(s_pos, ImGuiCond_Always, s_pivot);

        // Pulse the background red while the HP alert is firing so the
        // window itself catches the eye. Otherwise use the configured
        // normal opacity.
        if (hp_alert.firing)
        {
            const DWORD t = GetTickCount();
            const float phase = static_cast<float>(t % 700) / 700.0f;
            const float pulse = 0.55f + 0.35f * std::sin(phase * 6.2831853f);
            ImGui::PushStyleColor(
                ImGuiCol_WindowBg,
                ImVec4(0.55f * pulse, 0.0f, 0.0f, 0.88f));
        }
        else
        {
            ImGui::SetNextWindowBgAlpha(g_stats_window_alpha);
        }

        if (ImGui::Begin("##PSO_HUD_Stats", nullptr, kFlags))
        {
            if (!render_stats_body(hp_alert))
            {
                // Nothing active right now; keep a faint placeholder so the
                // window doesn't collapse to zero size (AlwaysAutoResize
                // hates that) and users know where the stats window lives.
                ImGui::TextDisabled("(no alerts)");
            }
        }
        ImGui::End();

        if (hp_alert.firing)
            ImGui::PopStyleColor();
    }
}

} // namespace

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

// Load a Drop Checker-style text file: lines of "HEXID  Name With Spaces".
static void load_name_file(const std::string &path,
                           std::unordered_map<uint32_t, std::string> &out)
{
    FILE *f = std::fopen(path.c_str(), "r");
    if (f == nullptr) return;

    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr)
    {
        size_t n = std::strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         line[n - 1] == ' '  || line[n - 1] == '\t'))
            line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;

        char *sp = std::strchr(line, ' ');
        if (sp == nullptr) continue;
        *sp = '\0';
        const char *id_str = line;
        const char *name_str = sp + 1;
        while (*name_str == ' ') ++name_str;
        if (*name_str == '\0') continue;

        const uint32_t id = static_cast<uint32_t>(
            std::strtoul(id_str, nullptr, 16));
        out[id] = name_str;
    }

    std::fclose(f);
}

// Load a rare ID list file. Format: one entry per line, either a bare
// 6-hex-digit ID ("009D00") or an ID followed by a space and a name
// ("009D00 Dark Flow"). Lines starting with '#' are comments. Missing
// file is fine — the rare set just stays empty.
static void load_rares_file(const std::string &path,
                            std::unordered_set<uint32_t> &out)
{
    FILE *f = std::fopen(path.c_str(), "r");
    if (f == nullptr) return;

    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr)
    {
        size_t n = std::strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         line[n - 1] == ' '  || line[n - 1] == '\t'))
            line[--n] = '\0';
        if (n == 0 || line[0] == '#' || line[0] == ';') continue;

        // First whitespace-delimited token is the hex ID.
        char *sp = std::strchr(line, ' ');
        if (sp != nullptr) *sp = '\0';
        const uint32_t id = static_cast<uint32_t>(
            std::strtoul(line, nullptr, 16));
        if (id != 0)
            out.insert(id);
    }
    std::fclose(f);
}

static void LoadItemNames()
{
    if (g_addon_dir.empty())
    {
        PSO_LOG("LoadItemNames: empty addon_dir, skipping");
        return;
    }
    load_name_file(g_addon_dir + "pixelated_mods_items.txt",    g_item_names);
    load_name_file(g_addon_dir + "pixelated_mods_techs.txt",    g_tech_names);
    load_name_file(g_addon_dir + "pixelated_mods_specials.txt", g_special_names);
    load_rares_file(g_addon_dir + "pixelated_mods_rares.txt",   g_rare_ids);
    // pixelated_mods_hidden.txt reuses the rare-list file format (one hex ID
    // per line, optional name comment after the ID, # for comments).
    // Missing file is fine — the set just stays empty.
    load_rares_file(g_addon_dir + "pixelated_mods_hidden.txt",  g_hidden_ids);
    // Monster hide list: reuses the same file format (hex ID per
    // line, optional name comment after the ID, # for comments).
    // These IDs are unitxt indices (decimal or 0x-prefix both work
    // because strtoul(16) accepts either).
    load_rares_file(g_addon_dir + "pixelated_mods_monster_hidden.txt",
                    g_hidden_monster_ids);
    PSO_LOG("LoadItemNames: items=%zu techs=%zu specials=%zu rares=%zu "
            "hidden=%zu monster_hidden=%zu",
            g_item_names.size(), g_tech_names.size(),
            g_special_names.size(), g_rare_ids.size(),
            g_hidden_ids.size(), g_hidden_monster_ids.size());
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
        else if (std::strcmp(key, "anchor")            == 0)
        {
            int a = std::atoi(val);
            if (a >= 0 && a <= 5) g_anchor = static_cast<Anchor>(a);
        }
        else if (std::strcmp(key, "stats_enabled")     == 0) g_stats_enabled = std::atoi(val) != 0;
        else if (std::strcmp(key, "stats_anchor")      == 0)
        {
            int a = std::atoi(val);
            if (a >= 0 && a <= 5) g_stats_anchor = static_cast<Anchor>(a);
        }
        else if (std::strcmp(key, "stats_window_x")    == 0) g_stats_window_x    = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "stats_window_y")    == 0) g_stats_window_y    = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "stats_window_alpha")== 0) g_stats_window_alpha = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "xp_track_enabled") == 0) g_xp_track_enabled = std::atoi(val) != 0;
        else if (std::strcmp(key, "mag_timer_enabled") == 0) g_mag_timer_enabled = std::atoi(val) != 0;
        else if (std::strcmp(key, "window_x")          == 0) g_window_x          = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "window_y")          == 0) g_window_y          = static_cast<float>(std::atof(val));
        else if (std::strcmp(key, "window_alpha")      == 0) g_window_alpha      = static_cast<float>(std::atof(val));
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
        // `episode_override=N` lines from older builds are silently
        // ignored — episode is now auto-detected from 0x00A9B1C8.
        else if (std::strcmp(key, "episode_override") == 0) {}
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
        // Stale calibration keys from earlier arrow iterations —
        // silently consume them so existing ini files load clean.
        else if (std::strcmp(key, "arrow_mirror_x")                == 0) { /* obsolete */ }
        else if (std::strcmp(key, "arrow_rotation_90")             == 0) { /* obsolete */ }
        else if (std::strcmp(key, "arrow_debug")                   == 0) { /* obsolete */ }
        else if (std::strcmp(key, "arrow_world_rotation")          == 0) { /* obsolete */ }
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
    std::fprintf(f, "anchor=%d\n",            static_cast<int>(g_anchor));
    std::fprintf(f, "window_x=%.2f\n",        g_window_x);
    std::fprintf(f, "window_y=%.2f\n",        g_window_y);
    std::fprintf(f, "window_alpha=%.2f\n",    g_window_alpha);
    std::fprintf(f, "filter_mode=%d\n",         static_cast<int>(g_filter_mode));
    std::fprintf(f, "tech_disk_min_level=%d\n", g_tech_disk_min_level);
    std::fprintf(f, "filter_current_area=%d\n", g_filter_current_area ? 1 : 0);
    std::fprintf(f, "flash_new_drops=%d\n",     g_flash_new_drops ? 1 : 0);
    std::fprintf(f, "blink_rare_items=%d\n",    g_blink_rare_items ? 1 : 0);
    std::fprintf(f, "hp_alert_enabled=%d\n",    g_hp_alert_enabled ? 1 : 0);
    std::fprintf(f, "hp_alert_threshold_pct=%d\n", g_hp_alert_threshold_pct);
    std::fprintf(f, "hp_alert_beep=%d\n",       g_hp_alert_beep ? 1 : 0);
    std::fprintf(f, "stats_enabled=%d\n",       g_stats_enabled ? 1 : 0);
    std::fprintf(f, "stats_anchor=%d\n",        static_cast<int>(g_stats_anchor));
    std::fprintf(f, "stats_window_x=%.2f\n",    g_stats_window_x);
    std::fprintf(f, "stats_window_y=%.2f\n",    g_stats_window_y);
    std::fprintf(f, "stats_window_alpha=%.2f\n", g_stats_window_alpha);
    std::fprintf(f, "xp_track_enabled=%d\n",   g_xp_track_enabled ? 1 : 0);
    std::fprintf(f, "mag_timer_enabled=%d\n",  g_mag_timer_enabled ? 1 : 0);
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
        LoadItemNames();

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
