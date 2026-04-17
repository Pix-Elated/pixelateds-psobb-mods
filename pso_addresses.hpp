// Runtime-resolved PSOBB global addresses.
//
// Historical background: every absolute address in this add-on
// (PlayerArray, FloorItemsArrayPtr, the entity-array MOV operand,
// etc.) was a `constexpr uintptr_t` baked into pixelated_mods.cpp at build
// time. This worked because PSOBB is a legacy Win32 binary without
// DYNAMICBASE: the loader always honors the PE header's preferred
// ImageBase (0x00400000), so addresses captured against a specific
// build of psobb.exe remain literally valid in the loaded process.
//
// The failure mode is Ephinea shipping a client update. When the
// binary is rebuilt, its .data / .bss layout reshuffles and the
// hardcoded constants silently point at the wrong bytes. The add-on
// keeps running but reads garbage, and the only visible symptom is
// the HP panel going blank.
//
// This module is the first step toward making those addresses
// survive a rebuild:
//
//   1. Variables — not constexpr — are defined here and declared
//      `extern` so pixelated_mods.cpp reads them through ordinary load
//      instructions. The `pso_offsets::<name>` call sites in
//      pixelated_mods.cpp are unchanged.
//
//   2. At startup, `InitializeAndLog()` walks the loaded psobb.exe
//      PE header, computes the rebase delta against the PE's
//      preferred ImageBase, and shifts every global address by that
//      delta. For the common case (PSOBB loaded at 0x00400000) the
//      delta is zero and the shift is a no-op; for a future ASLR-
//      enabled or manually-rebased binary, the constants are
//      auto-corrected at runtime.
//
//   3. The init function also runs a diagnostic pass: for each data
//      global, scan .text for 4-byte little-endian references and
//      dump the instruction-byte context around each call site into
//      pixelated_mods.log. This is the raw material for writing real
//      instruction-pattern signatures (the next phase — pattern
//      scans in .text that locate the instructions that reference
//      each global and read the operand directly, making the
//      resolver independent of the hardcoded constant entirely).
//
// TL;DR: today this file rebases. Tomorrow it sig-scans. The call
// sites in pixelated_mods.cpp never change.

#pragma once

#include <cstdint>

namespace pso_offsets {

// Static globals in PsoBB.exe — every value here is the address at
// the binary's preferred ImageBase (0x00400000). At runtime, after
// InitializeAndLog(), these hold the loader-adjusted addresses.
extern uintptr_t PlayerArray;
extern uintptr_t PlayerIndex;
extern uintptr_t PlayerCount;
extern uintptr_t EntityCount;
extern uintptr_t Difficulty;         // 0 Normal .. 3 Ultimate
extern uintptr_t UnitxtPointer;

// The entity array base is embedded as the 4-byte operand of a MOV
// instruction inside a function at this .text address. We read the
// literal at this location to discover the array base. Solylib does
// the same thing: `_EntityArray = read_u32(0x7B4BA0 + 2)`.
extern uintptr_t EntityArrayRef;

extern uintptr_t FloorItemsArrayPtr;
extern uintptr_t FloorItemsCountPtr;

// Pointer-to-pointer into the decoded ItemPMT tables. *ItemPmtPtr is
// the PMT root; the first five DWORDs of the root lead to per-type
// record arrays (weapons / armor-shield / units / tools / mags) each
// of which stores name unitxt indices as u32 at record+0x00. See
// FindItemPmtRecord in pixelated_mods.cpp.
extern uintptr_t ItemPmtPtr;

// Episode auto-detect. The u32 at this address holds 0 for Ep1,
// 1 for Ep2, 2 for Ep4.
extern uintptr_t EpisodeByte;

// Boss data struct pointer — some multi-segment bosses (De Rol Le,
// Barba Ray) store real HP at offsets from this pointer.
extern uintptr_t BossDataPtr;

// Ephinea-specific: monster HP i32 array and f64 boss HP scale.
// These live in Ephinea's injected .data range (0x00B5xxxx) and
// are NOT in the original PSO binary. No sig-scan possible — they
// resolve via rebase delta only, which is correct because Ephinea
// injects them at a fixed offset from the binary's base.
extern uintptr_t EphineaMonsterArray;
extern uintptr_t EphineaHPScale;

} // namespace pso_offsets

namespace pso_addresses {

// Walk the loaded psobb.exe, compute rebase delta, shift every
// `pso_offsets::*` global, and log the diagnostic context bytes for
// each. Call once, from DllMain DLL_PROCESS_ATTACH, after pso_log
// is initialized. Safe to call even if psobb.exe isn't loaded (no-op
// with a log line).
void InitializeAndLog();

} // namespace pso_addresses
