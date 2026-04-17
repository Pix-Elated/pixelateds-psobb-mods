# Pixelated's PSOBB Mods

A ReShade add-on for Phantasy Star Online Blue Burst.

Tested on Ephinea and [PSOBB.io](https://psobb.io).

Memory-read only. No binary patches, no network packets. The only
write path is `SendInput` keyboard events for the optional
controller chord remapper.

![Main HUD with monster HP and floor items panels](images/player_aware_mob_object_tracking.webp)

---

## Features

The HUD is split across several independently-anchored windows so you
can lay each one out where it doesn't collide with the game's native
UI.

### Monster HP panel

Live name + HP + colour-coded bar for every enemy in the current
room. Targeted enemy highlights cyan and pins to the top. Distance
and a compass arrow pointing to the enemy are shown per row.

Special handling for bosses that don't play nice with the generic
enemy panel:

- De Rol Le / Barba Ray: real body + shell HP with pinned max.
- Vol Opt: phase 1 sub-parts disambiguated into individual rows
  (Body, Chandelier, Monitor, Panel, Spire); phase 2 collapses
  into a single aggregate `(×N)` row.
- Dark Falz: tracks the real active body across phases 1/2/3.
- Pan Arms: shows only the active form — combined, or Hidoom +
  Migium after split, never both.

![Pan Arms split and combine](images/monster_tracking_pan_arms.webp)
![Vol Opt sub-parts + aggregation](images/vol_opt_monster_parts_tracking.webp)
![Multi-part boss compaction](images/noisy_mob_compaction.webp)

### Floor items panel

Closest-first sort with live distance. Three filter modes (Notable
/ All / Gear only), per-tool-subtype toggles, a tech-disk minimum
level slider, a yellow "new drop" flash, and a red pulse on rare
item IDs.

Three scopes chosen independently:

- **Global** — everything on the ship.
- **Nearby** — distance-capped; self-declutters as you walk.
- **Room** — only the current floor.

All item, tech, and weapon-special names are read from the game's
own tables at runtime, so they follow the game's locale and
automatically pick up new items without a data-file update.

Curated hide list for base-tier drops, with stat-based overrides so
a notable roll (a Hard Shield `[+20 DFP +15 EVP]`) still shows.

### Status vignette

Pulsing screen-edge tint for nine negative-state conditions, each
with its own colour. When multiple fire at once the vignette
alternates between them so every active effect is visible.

- Low HP, Poison, Paralysis, Frozen, Shocked, Slow, Confused,
  Jellen, Zalure.

Intensity, pulse rate, edge thickness, and per-condition toggles
are all in the config panel.

![Low-HP vignette pulsing the screen edge](images/low_hp_warning.webp)

### EXP tracker window

Current level, EXP to next level, session gained with a timer, and
a rolling EXP / hour rate. Width-locked top-right by default.

### Mag feed timer

Countdown to the next mag sync tick, pulses green when it's time
to feed. Separate top-left window above the game HP bar. Optional
audible chime on the countdown → ready edge: drop in your own
`pixelated_mods_mag_chime.wav` to customize the sound.

### Buff panel

Live Shifta / Deband / Jellen / Zalure timers with level display
and matching colours. Optional Shifta and Deband **reminders**:
with a reminder on and the buff down, the window background pulses
the buff's colour until you cast it. Both down → alternates.

![Buff timers, reminder pulses, mag feed countdown](images/buff_debuf_mag_feed_tracking.webp)

### Low HP audible alert

On the safe → danger edge transition, plays
`pixelated_mods_alert.wav` from the add-on directory (or the
Windows warning chime if absent). Threshold configurable 5–90%.
Drop in your own `.wav` to customize the sound.

### Controller chord remapper

PSO's native Pad Button Config only exposes 5 of the 10 action
palette slots. The chord remapper gives controller players access
to all of them by holding a trigger modifier and tapping a face
button:

| Modifier | A | X | Y | B |
|---|---|---|---|---|
| LT | 1 | 2 | 3 | 4 |
| RT | 5 | 6 | 7 | 8 |
| LT + RT | 9 | 0 | – | – |

Adding **RB** on any of the above swaps to palette set 2 (Ctrl +
digit in game terms).

A **chord overlay** draws live badges above the palette bar
showing which face button triggers which slot under the currently
held modifier. Per-slot X-offset nudge lets you align the badges
against a custom HUD.

![Chord modifier keys showing live LT/RT/RB state](images/modifier_keys.webp)

### Right-stick remapping

Since the game doesn't use the right stick, it's repurposed for
menu / UI navigation:

- **Up / down** → mouse wheel (camera zoom in / out)
- **Push left** → PgDn
- **Push right** → PgUp
- **L3 (left stick click)** → Enter (confirm — edge-triggered)
- **R3 (right stick click)** → Esc (cancel — edge-triggered)

PgUp / PgDn are sent with the extended-key flag so Windows routes
them to the nav cluster instead of NumPad 9 / NumPad 3 (which share
the same hardware scan codes).

Axis-dominance filter so an angled push doesn't fire both at once.
Configurable deadzone, rate limit, and Y-axis invert.

### Mouse wheel rate limiter

Fixes the "runaway zoom" trackpad users see in PSO, where a
two-finger scroll keeps zooming for seconds after you stop. Works
for touchpads and some gaming mice.

---

## Configuration

Every setting is in the add-on's ReShade overlay panel. Changes
persist to `pixelated_mods.ini` automatically; the runtime log is
`pixelated_mods.log`.

![ReShade add-on configuration panel](images/configuration_panel.gif)

---

## Installation

Grab `pixelateds-psobb-mods-<ver>.zip` from the
[Releases](../../releases) page. Drop-in for an existing ReShade
install — no bundled ReShade, no installer.

1. **ReShade 6.7.3+ with full add-on support** from
   [reshade.me](https://reshade.me). Use the "with full add-on
   support" variant (second button); the standard build silently
   ignores `.addon32` files. Point its installer at `PsoBB.exe` and
   pick Direct3D 9 for most PSOBB setups (including dgVoodoo,
   d3d8to9, and DXVK). Skip the effects pack.

2. Extract the zip next to `PsoBB.exe`:

   - `pixelated_mods.addon32` — the add-on DLL
   - `pixelated_mods_rares.txt` — rare ID list (drives the red
     pulse highlight)
   - `pixelated_mods_hidden.txt` — floor-item hide list
   - `pixelated_mods_monster_hidden.txt` — monster hide list
   - `pixelated_mods_alert.wav` — *optional*. Low-HP chime.
   - `pixelated_mods_mag_chime.wav` — *optional*. Mag-ready chime.

3. Launch PSOBB, press **Home**, open the **Add-ons** tab, confirm
   "Pixelated's PSOBB Mods" is listed.

**Uninstall:** delete those files. ReShade itself is separate.

---

## Optional audio fix — PSOBB choppy / stuttering music on Windows 10 / 11

Bundled under `audio_fix/` in the release zip. If you've ever heard
Phantasy Star Online Blue Burst music **skip, chop, cut out, or
stutter** — especially when entering a lobby, loading a block,
transitioning from the character select screen, or on the main menu
— this is the fix.

### What's broken

PSO was built against DirectSound, a Windows 98 / XP-era audio API.
Starting with Windows Vista, Microsoft deprecated real DirectSound
and replaced it with a legacy compatibility layer that translates
DirectSound calls into WASAPI under the hood. That translation layer
has long-standing bugs that produce steady-state stuttering on
certain old games, especially during scene transitions. It is not
PSO's fault, not Ephinea's fault, and not a hardware issue — it's
Windows' emulation code path. Happens regardless of DXVK shader
cache state, regardless of audio device format (44.1 kHz vs 48 kHz),
and regardless of which D3D wrapper you use.

### The fix

**[DSOAL](https://github.com/kcat/dsoal)** — a drop-in `dsound.dll`
replacement that reimplements DirectSound on top of
**[OpenAL Soft](https://openal-soft.org/)**, a modern, actively-
maintained audio library. Instead of Microsoft's buggy emulation,
PSO's audio goes through `DirectSound → OpenAL Soft → WASAPI`. Same
output path, working mixer. The chop is gone.

### Install

Copy these three files from the release zip's `audio_fix/` folder
into your PSOBB install directory (next to `PsoBB.exe`):

- `dsound.dll`
- `dsoal-aldrv.dll`
- `alsoft.ini`

**Uninstall** = delete those three files. System DirectSound is
restored, chop returns.

It does not patch the game, does not touch network packets, is
trusted by a huge number of old-game communities (Elder Scrolls,
Half-Life, Deus Ex, etc.). Zero anti-cheat risk.

---

## Optional cosmetic bundle

A separate release zip ships alongside the main addon:

**`pixelateds-psobb-cosmetic-bundle-<ver>.zip`** — my tuned
ReShade presets (`PSOBB_HD.ini`, `PSOBB_NoEffects.ini`, `ReShade.ini`)
plus an install recipe listing every shader pack and texture mod I
use, with links to each source. None of the third-party work is
redistributed inside the zip — their licenses forbid it — but the
recipe walks you through reproducing the exact look from the
screenshots above.

Grab it from the [Releases](../../releases) page if you want the
full visual setup. Skip it if you're only here for the HUD.

---

## Why a ReShade add-on instead of BBMod

ReShade was already in my stack for HD textures and preset grading,
and it supports both native Direct3D and translation-layer (DXVK)
backends as first-class, so an add-on drawn through ReShade renders
cleanly regardless of which D3D path the client uses.

**The two can coexist** — BBMod owns the `dinput8.dll` file slot,
this add-on hooks from inside ReShade. Run both if you want.

---

## Building from source

Requires Visual Studio 2025 with the "Desktop development with
C++" workload. PSOBB is 32-bit, so the add-on must be built x86.

```bat
build.bat
```

Output: `build/pixelated_mods.addon32`. Self-contained `/MT` static
CRT — no VC++ redistributable needed on the user's machine. All
dependencies (MinHook, ReShade SDK, Dear ImGui, nlohmann/json) are
vendored under `deps/`.

---

## Credits

- **[Solybum](https://github.com/Solybum)** —
  [PSOBBMod-Addons](https://github.com/Solybum/PSOBBMod-Addons) /
  solylib, source of almost every memory offset this add-on reads.
- **[jakeprobst](https://github.com/jakeprobst)** —
  [Drop Checker](https://github.com/jakeprobst/psodropcheckaddon),
  source of the drop-table walk logic.
- **[HybridEidolon](https://github.com/HybridEidolon)** — original
  author of [BBMod](https://github.com/HybridEidolon/psobbaddonplugin).

Bundled audio:

- `pixelated_mods_mag_chime.wav` — "Festive Chime" by **Dragon
  Studio**, sourced from
  [Pixabay](https://pixabay.com/sound-effects/) under Pixabay's
  Content License (free commercial use, no attribution required;
  credited here anyway).

Third-party libraries under `deps/`:

- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2)
- [ReShade](https://github.com/crosire/reshade) add-on SDK (BSD-3)
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT)
- [nlohmann/json](https://github.com/nlohmann/json) (MIT)
- [DSOAL](https://github.com/kcat/dsoal) (LGPL-2.1) — bundled as
  the optional audio fix
- [OpenAL Soft](https://openal-soft.org/) (LGPL-2.1) — DSOAL's
  backend, bundled alongside it

---

## License

MIT — see [LICENSE](LICENSE). Third-party code under `deps/`
retains its original license.
