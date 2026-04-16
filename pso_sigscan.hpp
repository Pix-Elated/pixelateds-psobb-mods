// PE-aware signature / pattern scanner for PSOBB.
//
// Purpose: replace hardcoded absolute addresses in pixelated_mods with
// runtime-resolved lookups that survive (a) the PE loader picking a
// different load address than the binary's preferred ImageBase, and
// (b) Ephinea rebuilding the client and reshuffling .data / .bss
// layout, as long as the surrounding instruction bytes stay stable.
//
// This module provides three things:
//
//   1. A walker for the loaded `psobb.exe` PE headers that identifies
//      the `.text` section range and reports the rebase delta between
//      the PE's preferred ImageBase and its actually-loaded base.
//
//   2. A classic AOB (array-of-bytes) pattern scanner over a byte
//      range, with `??` / `?` wildcards. Intended for locating
//      specific x86 instructions by their surrounding byte context
//      so the caller can then read the 4-byte operand out of the
//      match.
//
//   3. A diagnostic "find all 4-byte little-endian references to this
//      u32 value" scan, used at startup to dump the instruction-byte
//      context around every known call site of each PSOBB global.
//      The intent is that the pixelated_mods.log output from this diagnostic
//      gives a human operator (me) the raw material needed to craft
//      stable instruction-pattern signatures for FindPattern in a
//      future commit, without having to disassemble psobb.exe by
//      hand in a separate tool.
//
// Memory reads inside the scanner are guarded by the same rules the
// rest of the add-on follows: we only touch pages we just identified
// as committed readable via the PE header walk, and we never write.

#pragma once

#include <cstddef>
#include <cstdint>

namespace pso_sigscan {

// Description of a loaded PE module's interesting geometry.
struct ModuleRange
{
    uintptr_t base = 0;                 // Loader-chosen base (== HMODULE cast)
    uintptr_t size_of_image = 0;        // OptionalHeader.SizeOfImage
    uintptr_t preferred_image_base = 0; // OptionalHeader.ImageBase
    uintptr_t text_begin = 0;           // Runtime address of .text
    uintptr_t text_size = 0;            // VirtualSize of .text
    bool      valid = false;
};

// Locate a loaded module by name and fill `out`. Returns true on success.
// `module_name` is case-insensitive and may omit the extension.
// On failure `out.valid` is false.
bool GetModuleRange(const char *module_name, ModuleRange &out);

// Difference between the loader-chosen base and the PE's preferred
// ImageBase. Zero if the binary loaded where its header asked to go
// (the common case for legacy Win32 EXEs with no DYNAMICBASE flag
// set — PSOBB is one of these). Nonzero means every hardcoded
// absolute address in that module's preferred image space needs to
// be shifted by this delta to point at real runtime memory.
intptr_t GetRebaseDelta(const ModuleRange &mod);

// Classic AOB pattern scan. The pattern is a whitespace-separated
// string of hex byte tokens; `??` or `?` is a wildcard that matches
// any byte. Scans the full range, writes up to `out_capacity` match
// addresses into `out_matches`, and returns the total number of
// matches found (which may exceed the capacity — the caller learns
// how ambiguous the signature is even when it only keeps the first
// few matches).
//
// Typical use:
//
//   uintptr_t m[4];
//   size_t n = FindPatternAll(text_begin, text_size, sig, m, 4);
//   if (n == 0) { /* not found, maybe binary rebuilt */ }
//   else if (n == 1) { /* unique, safe to use */ }
//   else { /* ambiguous, sig needs more context */ }
size_t FindPatternAll(uintptr_t begin,
                      size_t size,
                      const char *pattern,
                      uintptr_t *out_matches,
                      size_t out_capacity);

} // namespace pso_sigscan
