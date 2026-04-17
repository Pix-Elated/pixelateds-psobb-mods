// Runtime-resolved PSOBB global addresses. See pso_addresses.hpp.

#include "pso_addresses.hpp"

#include "pso_log.hpp"
#include "pso_sigscan.hpp"

#include <Windows.h>

#include <cstdio>
#include <cstring>

namespace pso_offsets {

// Initial values are the hardcoded addresses captured against the
// Ephinea PsoBB.exe build in use at the time of writing. They serve
// two purposes:
//
//   (a) Fallback if the instruction signature scan fails to locate a
//       unique match (e.g., Ephinea rebuilt the binary and the
//       surrounding bytes also moved). We still get correct behavior
//       on the build we know about.
//
//   (b) Expected-value sanity check on the signature output. If the
//       signature scan resolves a value that DIFFERS from the
//       hardcoded fallback, we log both and trust the scan (assuming
//       the sig is fresh information from a newer binary). If they
//       match, we log "ok" and know the sig is still aligned with
//       the canonical constant.
//
// InitializeAndLog() overwrites these at startup, first by applying
// the loader rebase delta and then (where a signature is available)
// by extracting the operand from the pattern-matched instruction.
uintptr_t PlayerArray         = 0x00A94254;
uintptr_t PlayerIndex         = 0x00A9C4F4;
uintptr_t PlayerCount         = 0x00AAE168;
uintptr_t EntityCount         = 0x00AAE164;
uintptr_t Difficulty          = 0x00A9CD68;
uintptr_t UnitxtPointer       = 0x00A9CD50;
uintptr_t EntityArrayRef      = 0x007B4BA2;
uintptr_t FloorItemsArrayPtr  = 0x00A8D8A0;
uintptr_t FloorItemsCountPtr  = 0x00A8D89C;
uintptr_t EpisodeByte         = 0x00A9B1C8;
uintptr_t ItemPmtPtr          = 0x00A8DC94;
uintptr_t BossDataPtr         = 0x00A43CC8;
uintptr_t EphineaMonsterArray = 0x00B5F800;
uintptr_t EphineaHPScale      = 0x00B5F804;
} // namespace pso_offsets

namespace pso_addresses {

namespace {

// ----------------------------------------------------------------------------
// Per-global signature definitions.
//
// Every signature here was crafted by reading the diagnostic byte
// dump of a known-working build of PSOBB from pixelated_mods.log. Each one
// encodes enough surrounding instruction context to make it unique
// inside .text (verified by the FindPatternAll-returns-1 check in
// ResolveViaSignature).
//
// Format:
//   pattern:        AOB string with `??` wildcards for the operand
//                   (and occasionally for displacements in follow-on
//                   instructions that aren't worth pinning).
//   operand_offset: byte offset into the matched pattern where the
//                   4-byte little-endian absolute address literal
//                   begins. Typically 1 (for `A1 <imm32>`), 2 (for
//                   `8B 0D <imm32>` / `FF 35 <imm32>` / `8B 2D <imm32>`
//                   / `89 0D <imm32>`), 3 (for `8B 0C 9D <imm32>` —
//                   SIB-based scaled indexed loads), 4 (for
//                   `33 D2 03 05 <imm32>`), or 8 (for the second
//                   operand in the combined FloorItems pattern).
// ----------------------------------------------------------------------------

struct Signature
{
    const char *pattern;
    uint32_t    operand_offset;
};

// --- Simple "one instruction, one global" signatures ---

// Difficulty: `mov eax, [Difficulty]; cmp eax, 3; je +0x2F`
// The `cmp eax, 3` immediately after an absolute-address mov is the
// "is this Ultimate?" check — extremely distinctive.
constexpr Signature kSigDifficulty = {
    "A1 ?? ?? ?? ?? 83 F8 03 74 2F", 1
};

// UnitxtPointer: one of the four tiny unitxt accessor functions.
// `mov edx, [UnitxtPointer]; mov ecx, [edx+8]; mov eax, [ecx+eax*4]; ret`.
// There are three near-identical versions of this function (for
// offsets +0x08, +0x10, +0x14) and we anchor on the +0x08 one.
constexpr Signature kSigUnitxtPointer = {
    "8B 15 ?? ?? ?? ?? 8B 4A 08 8B 04 81 C3", 2
};

// PlayerArray: SIB-based scaled indexed load.
// `mov ecx, [ebx*4 + PlayerArray]; test ecx, ecx; jz +0x0C`.
// The `9D` SIB byte encodes (scale=*4, index=ebx, base=none+disp32)
// which is how compilers emit an access into a static pointer array
// indexed by a variable — a pattern unique enough to identify this
// array even when there are 21 indexed references in the binary.
constexpr Signature kSigPlayerArray = {
    "8B 0C 9D ?? ?? ?? ?? 85 C9 74 0C", 3
};

// PlayerIndex: `movzx ebp, word [PlayerIndex]; push ebp; call rel32`.
// PSOBB reads PlayerIndex as a 16-bit value even though it's stored
// as u32, and the pattern of "movzx into ebp then immediate push and
// call" is rare in this binary.
constexpr Signature kSigPlayerIndex = {
    "0F B7 2D ?? ?? ?? ?? 55 E8", 3
};

// PlayerCount: `mov edi, [PlayerCount]; xor esi, esi; mov eax, 4`.
// The `mov eax, 4` right after a static-address mov is the
// "initialize iteration bound to the 4-player cap" pattern.
constexpr Signature kSigPlayerCount = {
    "8B 3D ?? ?? ?? ?? 33 F6 B8 04 00 00 00", 2
};

// EntityCount: `xor edx, edx; add eax, [EntityCount]; mov [esp+0x34], edx;
//              fild [esp+0x34]`.
// The `fild` (integer→FP convert) right after the count load is the
// distinctive bit — x87 floating point integer load, used for
// ratio/average computation.
constexpr Signature kSigEntityCount = {
    "33 D2 03 05 ?? ?? ?? ?? 89 54 24 34 DB 44 24 34", 4
};

// EpisodeByte: `mov ebp, [EpisodeByte]; mov ebx, [<other addr>]`.
// Two back-to-back absolute-address MOV loads into different
// general-purpose registers. Wildcarded the second address so a
// rebuild that reshuffles .data doesn't break the anchor.
constexpr Signature kSigEpisodeByte = {
    "8B 2D ?? ?? ?? ?? 8B 1D", 2
};

// ItemPmtPtr: anchored on one of the tiny PMT stat-descriptor
// accessors (fcn.005e4ac8 in Ephinea's build). The tail sequence
// `lea ecx, [eax+eax]; add ecx, ecx; lea eax, [ecx+ecx*8]; add eax,
// [edx+0x18]` — 11 bytes — is unique in the whole binary, so the
// short `8B 15 <imm32>` load right before it is unambiguously the
// ItemPMT global. Operand offset = 2 (past the `8B 15` opcode).
constexpr Signature kSigItemPmtPtr = {
    "8B 15 ?? ?? ?? ?? 8D 0C 00 03 C9 8D 04 C9 03 42 18", 2
};

// --- Combined signature: FloorItemsArrayPtr + FloorItemsCountPtr ---
//
// These two globals are adjacent in .bss and, more importantly, are
// initialized back-to-back by the same instruction sequence:
//
//   push [FloorItemsArrayPtr]       ; FF 35 <FIA>
//   mov  [FloorItemsCountPtr], 0    ; C7 05 <FIC> 00 00 00 00
//
// One 16-byte signature produces both operands from a single scan.
constexpr Signature kSigFloorItemsArrayPtr = {
    "FF 35 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 00 00 00 00", 2
};
constexpr Signature kSigFloorItemsCountPtr = {
    "FF 35 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 00 00 00 00", 8
};

// ----------------------------------------------------------------------------
// Resolver table entry.
// ----------------------------------------------------------------------------

struct Entry
{
    const char     *name;
    uintptr_t      *variable;
    bool            is_text_ref;   // true = EntityArrayRef-style (.text addr)
    const Signature *signature;    // nullptr = no sig, rely on rebase fallback
};

// ----------------------------------------------------------------------------
// ResolveViaSignature
//
// Runs a FindPatternAll over .text, requires EXACTLY one match to
// consider the signature safe, reads the 4-byte operand from the
// match+offset location, and returns it. On failure (not found,
// ambiguous, or operand page unreadable) returns 0.
//
// Logs every step: match count, match addresses (up to 3), resolved
// operand, comparison against the rebased fallback. The intent is
// that anyone reading pixelated_mods.log can see at a glance which
// resolvers are working and which are falling back to hardcoded.
// ----------------------------------------------------------------------------

uintptr_t ResolveViaSignature(const pso_sigscan::ModuleRange &mod,
                              const char *label,
                              const Signature &sig,
                              uintptr_t fallback)
{
    constexpr size_t kMaxMatches = 8;
    uintptr_t matches[kMaxMatches] = {};
    const size_t total = pso_sigscan::FindPatternAll(
        mod.text_begin, mod.text_size, sig.pattern, matches, kMaxMatches);

    if (total == 0)
    {
        PSO_LOG("pso_addresses: %-20s SIG MISS: pattern \"%s\" not found "
                "in .text — falling back to hardcoded 0x%08X",
                label, sig.pattern, static_cast<unsigned>(fallback));
        return 0;
    }

    if (total != 1)
    {
        // Ambiguous. Log up to 3 match addresses so we can tighten
        // the signature in a follow-up commit.
        char addr_list[96] = {};
        size_t pos = 0;
        const size_t shown = total < 3 ? total : 3;
        for (size_t i = 0; i < shown && pos + 12 < sizeof(addr_list); ++i)
        {
            const int n = std::snprintf(addr_list + pos, sizeof(addr_list) - pos,
                                        " 0x%08X",
                                        static_cast<unsigned>(matches[i]));
            if (n > 0) pos += static_cast<size_t>(n);
        }
        PSO_LOG("pso_addresses: %-20s SIG AMBIGUOUS: pattern \"%s\" matched "
                "%zu times in .text —%s%s — falling back to hardcoded 0x%08X",
                label, sig.pattern, total,
                addr_list,
                (total > shown) ? " ..." : "",
                static_cast<unsigned>(fallback));
        return 0;
    }

    // Unique match. Read the 4-byte operand.
    const uintptr_t match_addr = matches[0];
    const uintptr_t operand_addr = match_addr + sig.operand_offset;

    // Sanity-check that the 4 bytes of operand are inside .text. If
    // they aren't, something's wrong with the offset math.
    if (operand_addr < mod.text_begin ||
        operand_addr + 4 > mod.text_begin + mod.text_size)
    {
        PSO_LOG("pso_addresses: %-20s SIG OOB: match=0x%08X operand=0x%08X "
                "outside .text [0x%08X, 0x%08X] — using hardcoded 0x%08X",
                label,
                static_cast<unsigned>(match_addr),
                static_cast<unsigned>(operand_addr),
                static_cast<unsigned>(mod.text_begin),
                static_cast<unsigned>(mod.text_begin + mod.text_size),
                static_cast<unsigned>(fallback));
        return 0;
    }

    uint32_t operand_value = 0;
    std::memcpy(&operand_value,
                reinterpret_cast<const void *>(operand_addr),
                sizeof(operand_value));

    const uintptr_t resolved = operand_value;
    const bool matches_fallback = (resolved == fallback);

    PSO_LOG("pso_addresses: %-20s SIG OK match=0x%08X operand=0x%08X%s",
            label,
            static_cast<unsigned>(match_addr),
            static_cast<unsigned>(resolved),
            matches_fallback ? " (== hardcoded, all good)"
                             : " (!= hardcoded, trusting sig)");
    return resolved;
}

} // namespace

// ----------------------------------------------------------------------------
// InitializeAndLog
// ----------------------------------------------------------------------------

void InitializeAndLog()
{
    pso_sigscan::ModuleRange mod;
    if (!pso_sigscan::GetModuleRange("psobb.exe", mod))
    {
        PSO_LOG("pso_addresses: psobb.exe module not found; keeping "
                "hardcoded addresses unchanged (rebase disabled, "
                "sig scan skipped, diagnostics skipped)");
        return;
    }

    PSO_LOG("pso_addresses: psobb.exe loaded base=0x%08X size=0x%08X "
            "preferred ImageBase=0x%08X",
            static_cast<unsigned>(mod.base),
            static_cast<unsigned>(mod.size_of_image),
            static_cast<unsigned>(mod.preferred_image_base));
    PSO_LOG("pso_addresses: psobb.exe .text=[0x%08X, 0x%08X] size=0x%08X",
            static_cast<unsigned>(mod.text_begin),
            static_cast<unsigned>(mod.text_begin + mod.text_size),
            static_cast<unsigned>(mod.text_size));

    const intptr_t delta = pso_sigscan::GetRebaseDelta(mod);
    PSO_LOG("pso_addresses: rebase delta = 0x%08X (%+d bytes)",
            static_cast<unsigned>(delta),
            static_cast<int>(delta));

    // Resolver table. Every entry has a hardcoded fallback (already
    // living in pso_offsets::*) that gets rebased first, then — if a
    // signature is attached — superseded by the sig-resolved value
    // unless the sig scan is ambiguous or missing.
    //
    // EntityArrayRef is the one entry without a signature: it is a
    // .text address rather than a .data global, and resolving it via
    // sig would require matching the specific MOV instruction that
    // loads the entity array — material for a future commit if the
    // rebase fallback ever stops being enough.
    //
    // PlayerArray is listed AFTER the rest because its signature was
    // the last one crafted and the ordering in the log mirrors the
    // order here.
    const Entry entries[] = {
        { "Difficulty",         &pso_offsets::Difficulty,          false, &kSigDifficulty         },
        { "UnitxtPointer",      &pso_offsets::UnitxtPointer,       false, &kSigUnitxtPointer      },
        { "PlayerArray",        &pso_offsets::PlayerArray,         false, &kSigPlayerArray        },
        { "PlayerIndex",        &pso_offsets::PlayerIndex,         false, &kSigPlayerIndex        },
        { "PlayerCount",        &pso_offsets::PlayerCount,         false, &kSigPlayerCount        },
        { "EntityCount",        &pso_offsets::EntityCount,         false, &kSigEntityCount        },
        { "EpisodeByte",        &pso_offsets::EpisodeByte,         false, &kSigEpisodeByte        },
        { "FloorItemsArrayPtr", &pso_offsets::FloorItemsArrayPtr,  false, &kSigFloorItemsArrayPtr },
        { "FloorItemsCountPtr", &pso_offsets::FloorItemsCountPtr,  false, &kSigFloorItemsCountPtr },
        { "ItemPmtPtr",         &pso_offsets::ItemPmtPtr,          false, &kSigItemPmtPtr         },
        // Rebase-only: no sig available (or, for Ephinea addresses,
        // not in the original binary's .text at all).
        { "EntityArrayRef",     &pso_offsets::EntityArrayRef,      true,  nullptr                 },
        { "BossDataPtr",        &pso_offsets::BossDataPtr,         false, nullptr                 },
        { "EphineaMonsterArray",&pso_offsets::EphineaMonsterArray,  false, nullptr                 },
        { "EphineaHPScale",     &pso_offsets::EphineaHPScale,      false, nullptr                 },
    };

    // --- Phase 1: rebase fallback --------------------------------------------
    //
    // Apply the PE delta to the hardcoded constant so the fallback is
    // correct even on a future ASLR-enabled / manually-rebased binary.
    // This runs unconditionally for every entry — signatures in phase
    // 2 only OVERRIDE the value, they don't eliminate the fallback.

    for (const Entry &e : entries)
    {
        const uintptr_t before = *e.variable;
        const uintptr_t after  = static_cast<uintptr_t>(
            static_cast<intptr_t>(before) + delta);
        *e.variable = after;
        PSO_LOG("pso_addresses: %-20s rebase 0x%08X -> 0x%08X%s",
                e.name,
                static_cast<unsigned>(before),
                static_cast<unsigned>(after),
                e.is_text_ref ? "  (text ref)" : "");
    }

    // --- Phase 2: signature resolution ---------------------------------------
    //
    // For each entry with a signature attached, run a unique-match
    // scan in .text and (on success) overwrite the rebased hardcoded
    // value with the sig-extracted operand. On ambiguous or missing
    // matches, keep the rebased hardcoded value — ResolveViaSignature
    // returns 0 and the overwrite is skipped.

    PSO_LOG("pso_addresses: --- phase 2: instruction signature scan ---");

    for (const Entry &e : entries)
    {
        if (e.signature == nullptr) continue;

        const uintptr_t resolved = ResolveViaSignature(
            mod, e.name, *e.signature, *e.variable);
        if (resolved != 0)
            *e.variable = resolved;
    }

    PSO_LOG("pso_addresses: --- init complete ---");
}

} // namespace pso_addresses
