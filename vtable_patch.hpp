// Shared helper for in-place COM vtable interception.
//
// The vtable lives in read-only memory at image load time, so we use
// VirtualProtect to temporarily make it writable, swap the slot, and
// restore protection.

#pragma once

#include <Windows.h>

namespace pixelated_mods {

// Swap one entry in a COM vtable. On success `*out_old` (if non-null)
// receives the previous slot value so the caller can chain through to the
// original implementation. Returns false if VirtualProtect failed, in
// which case the vtable is unchanged.
inline bool VtablePatch(void **vtable, int index, void *new_fn, void **out_old)
{
    DWORD old_protect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void *), PAGE_READWRITE, &old_protect))
        return false;
    if (out_old) *out_old = vtable[index];
    vtable[index] = new_fn;
    DWORD tmp = 0;
    VirtualProtect(&vtable[index], sizeof(void *), old_protect, &tmp);
    return true;
}

} // namespace pixelated_mods
