// Controller chord remapping for Ephinea PSOBB.
//
// Hold LT or RT on a gamepad, press a face button, get a keyboard palette
// slot (1-0) — giving controller users access to all 10 action-palette
// slots, which the game's native Pad Button Config does not expose.

#pragma once

#include <cstdio>

namespace chords {

bool Install();
void Uninstall();

void ReadConfigKey(const char *key, const char *val);
void WriteConfig(std::FILE *f);

// Renders the chord config section inside the add-on's overlay panel.
// Returns true if the user changed any setting this frame (so the caller
// can flag the config as dirty and save it).
bool RenderConfigPanelSection();

// Snapshot of the most recent chord-modifier state, safe to read from
// any thread. Populated by the joystick poll hook on every frame.
//
//   enabled : chord remap is turned on in the config
//   lt      : LT held above trigger threshold
//   rt      : RT held above trigger threshold
//   rb      : RB held (Ctrl modifier → palette set 2)
//
// The overlay uses this to render a visual guide above the palette bar
// showing which face-button mapping is currently active.
struct HeldState {
    bool enabled;
    bool lt;
    bool rt;
    bool rb;
};
HeldState GetHeldState();

// Per-slot chord-fire event record. Indexed 0..9 where index 0 = hotkey
// digit '1' (slot 1) and index 9 = digit '0' (slot 10). Each slot holds
// a monotonically-increasing counter (so the render thread can detect a
// new fire even when two taps land in the same GetTickCount() tick) and
// the GetTickCount() value at the moment of fire, for flash-timeout math.
struct FireEvents {
    uint32_t last_counter[10];  // 0 = never fired
    uint32_t last_tick[10];     // GetTickCount() at last fire
};
FireEvents GetFireEvents();

} // namespace chords
