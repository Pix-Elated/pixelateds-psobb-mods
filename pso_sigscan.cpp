// PE-aware signature / pattern scanner for PSOBB.
// See pso_sigscan.hpp for what and why.

#include "pso_sigscan.hpp"

#include <Windows.h>

#include <cstring>

namespace pso_sigscan {

// ----------------------------------------------------------------------------
// Page-safe byte probe.
//
// `VirtualQuery` + state/protect checks, same approach as SafeRead in
// pixelated_mods.cpp but specialized to a single byte read. Used by FindPattern
// so a malformed PE (or a bogus caller) can't crash the scanner with
// an access violation.
// ----------------------------------------------------------------------------

static bool IsReadableByte(uintptr_t addr)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void *>(addr), &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

    constexpr DWORD kReadable =
        PAGE_READONLY | PAGE_READWRITE |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
        PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & kReadable) != 0;
}

// ----------------------------------------------------------------------------
// GetModuleRange
// ----------------------------------------------------------------------------

bool GetModuleRange(const char *module_name, ModuleRange &out)
{
    out = ModuleRange{};

    const HMODULE hmod = GetModuleHandleA(module_name);
    if (hmod == nullptr) return false;

    const auto base = reinterpret_cast<uintptr_t>(hmod);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
        base + static_cast<uintptr_t>(dos->e_lfanew));
    if (nt == nullptr || nt->Signature != IMAGE_NT_SIGNATURE) return false;

    out.base = base;
    out.size_of_image = nt->OptionalHeader.SizeOfImage;
    out.preferred_image_base =
        static_cast<uintptr_t>(nt->OptionalHeader.ImageBase);

    // Walk the section table for ".text". IMAGE_FIRST_SECTION is a
    // Windows SDK macro that yields a pointer to the first section
    // header immediately after the OptionalHeader.
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    const WORD section_count = nt->FileHeader.NumberOfSections;

    for (WORD i = 0; i < section_count; ++i, ++section)
    {
        // Section names are 8-byte null-padded; memcmp with length 5
        // is enough for ".text" and won't read past the header.
        if (std::memcmp(section->Name, ".text", 5) == 0)
        {
            out.text_begin = base + section->VirtualAddress;
            // VirtualSize may be 0 on ancient binaries that only filled
            // out SizeOfRawData. Prefer VirtualSize when present.
            out.text_size = (section->Misc.VirtualSize != 0)
                                ? section->Misc.VirtualSize
                                : section->SizeOfRawData;
            out.valid = (out.text_begin != 0 && out.text_size != 0);
            return out.valid;
        }
    }

    return false;
}

// ----------------------------------------------------------------------------
// GetRebaseDelta
// ----------------------------------------------------------------------------

intptr_t GetRebaseDelta(const ModuleRange &mod)
{
    if (!mod.valid) return 0;
    return static_cast<intptr_t>(mod.base) -
           static_cast<intptr_t>(mod.preferred_image_base);
}

// ----------------------------------------------------------------------------
// Pattern parser: "8B 05 ?? ?? 85 C0" -> (bytes[], mask[]).
//
// Stored on the stack as fixed-size arrays; no allocation. Patterns
// longer than kMaxPatternBytes are truncated (which is fine — no
// realistic x86 instruction signature needs more than a few dozen
// bytes of context).
// ----------------------------------------------------------------------------

namespace {

constexpr size_t kMaxPatternBytes = 128;

struct Pattern
{
    uint8_t bytes[kMaxPatternBytes] = {};
    bool    mask[kMaxPatternBytes]  = {}; // true = literal, false = wildcard
    size_t  length = 0;
    bool    ok = false;
};

int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

Pattern ParsePattern(const char *pattern)
{
    Pattern out;
    if (pattern == nullptr) return out;

    const char *p = pattern;
    while (*p != '\0' && out.length < kMaxPatternBytes)
    {
        // Skip whitespace.
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0') break;

        // Wildcard token: `??` or `?`.
        if (*p == '?')
        {
            out.bytes[out.length] = 0;
            out.mask[out.length]  = false;
            ++out.length;
            ++p;
            if (*p == '?') ++p;
            continue;
        }

        // Expect exactly two hex digits.
        const int hi = HexDigit(p[0]);
        if (hi < 0) return out; // parse error — out.ok stays false
        const int lo = (p[1] != '\0') ? HexDigit(p[1]) : -1;
        if (lo < 0) return out;

        out.bytes[out.length] = static_cast<uint8_t>((hi << 4) | lo);
        out.mask[out.length]  = true;
        ++out.length;
        p += 2;
    }

    out.ok = (out.length > 0);
    return out;
}

} // namespace

size_t FindPatternAll(uintptr_t begin,
                      size_t size,
                      const char *pattern,
                      uintptr_t *out_matches,
                      size_t out_capacity)
{
    const Pattern pat = ParsePattern(pattern);
    if (!pat.ok) return 0;
    if (size < pat.length) return 0;
    if (!IsReadableByte(begin)) return 0;

    const uint8_t *base = reinterpret_cast<const uint8_t *>(begin);
    const size_t last = size - pat.length;

    size_t total = 0;
    for (size_t i = 0; i <= last; ++i)
    {
        bool match = true;
        for (size_t j = 0; j < pat.length; ++j)
        {
            if (pat.mask[j] && base[i + j] != pat.bytes[j])
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            if (out_matches != nullptr && total < out_capacity)
                out_matches[total] = begin + i;
            ++total;
            // Overlapping matches are theoretically possible but for
            // the sizes of our signatures (8+ bytes) and the purpose
            // (instruction anchors), advancing by 1 is correct —
            // we want every starting offset where the pattern holds.
        }
    }
    return total;
}

} // namespace pso_sigscan
