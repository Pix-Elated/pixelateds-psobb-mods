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

} // namespace chords
