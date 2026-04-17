# Pixelated's PSOBB Cosmetic Bundle

An opt-in recipe to reproduce the ReShade + texture-mod look that the
main HUD addon screenshots show, layered on top of the recommended
**[psobb.io](https://psobb.io)** client.

Only files I authored or configured myself ship inside this zip —
the third-party shader packs and texture mods have licenses that
forbid redistribution, so instead this README walks you through
acquiring the HD assets and other personal favorites I've found.
Expect the setup to take ~20 minutes once you have the downloads.

---

## What's in this zip

| File | Purpose |
|---|---|
| `ReShade.ini` | ReShade global config — relative shader search paths, overlay toggles, screenshot key |
| `PSOBB_HD.ini` | Active ReShade preset: MXAO + Deband + Clarity + LUT + Tonemap + MagicBloom, tuned for PSOBB's palette |
| `PSOBB_NoEffects.ini` | Bare preset with all effects disabled — swap to this to A/B the HD look |

---

## Install order

### 1. Install psobb.io

Grab the installer from **[psobb.io](https://psobb.io)**. Install to
userland (e.g. `C:\Users\<you>\PSOBB.IO\`), not `Program Files`, so
ReShade and the HUD addon can write their logs without UAC prompts.

### 2. Install ReShade with full add-on support

From **[reshade.me](https://reshade.me)**, download the **ReShade
with full add-on support** build (the standard build silently drops
`.addon32` files). Point its installer at psobb.io's `PsoBB.exe`,
pick **Direct3D 9**, and skip the effects pack on the shader
selection screen — we install shaders manually below.

### 3. Install shaders

Download each into `<PSOBB>\reshade-shaders\Shaders\` so all `.fx` /
`.fxh` files sit in the same flat directory (not nested under
`iMMERSE/`, `PD80/`, etc.):

- **Standard ReShade shaders** — from
  [crosire/reshade-shaders](https://github.com/crosire/reshade-shaders).
  Provides Tonemap, LUT, Deband, Vignette, Colourfulness, MagicBloom,
  and the base `.fxh` includes.
  *MIT / various permissive licenses.*

- **iMMERSE by Pascal Gilcher / Marty McFly** — from
  [martymcmodding/iMMERSE](https://github.com/martymcmodding/iMMERSE).
  Provides `MartysMods_MXAO.fx` (ambient occlusion),
  `MartysMods_SHARPEN.fx`, `MartysMods_LAUNCHPAD.fx`, and the
  `mmx_*.fxh` include set.
  *License forbids redistribution — grab it direct from the source.*

- **prod80's PD80 pack** — from
  [prod80/prod80-ReShade-Repository](https://github.com/prod80/prod80-ReShade-Repository).
  Provides `PD80_03_Shadows_Midtones_Highlights.fx`, `Clarity.fx`,
  the `PD80_00_*.fxh` utility headers.
  *BSD-3-Clause — free to use.*

ReShade itself has an in-app shader installer (ReShade Home → Add-ons
→ Effect Shader Repository) that can fetch the crosire and PD80 packs
for you. iMMERSE must be installed manually.

### 4. Install the HUD addon

Drop the contents of the main release zip (`pixelateds-psobb-mods-<ver>.zip`)
next to `PsoBB.exe`. That's `pixelated_mods.addon32`, the rare /
hide-list text files, and the alert wav. See the main
[README](https://github.com/Pix-Elated/pixelateds-psobb-mods) for
per-file details.

### 5. Drop in this bundle's INI files

Copy the three INI files from this zip next to `PsoBB.exe`:

- `ReShade.ini`
- `PSOBB_HD.ini`
- `PSOBB_NoEffects.ini`

Launch the game. Press **Insert** to open the ReShade overlay; the
preset should auto-load as `PSOBB_HD`. Switch to `PSOBB_NoEffects`
from the preset dropdown to toggle back to a clean look.

### 6. Install the audio fix

The main release zip already includes DSOAL under `audio_fix/` —
drop `dsound.dll`, `dsoal-aldrv.dll`, and `alsoft.ini` next to
`PsoBB.exe`. Music chop is gone. (Skip this if you aren't running
into DirectSound stutter, but most Windows 10 / 11 users are.)

### 7. (Optional) Texture / model mods

The screenshots include a stack of community texture and model mods
that aren't bundled here. Pick what you want:

- **AshenbubsHD** — HD upscales of every boss, lobby, and common
  enemy. Search "PSOBB AshenbubsHD" on the
  [PSO-World](https://www.pso-world.com/) forums.
- **9 Echelon HD Packs** — 9 area-specific HD remake packs (Forest,
  Caves, Mines, Ruins, Temple, etc.). Distributed via pso.io /
  Echelon Discord.
- **Smokey's Modular Pack** — HUD overhaul, font upscales, UI
  reworks. Available on the
  [Ephinea / psobb modding community](https://www.pso-world.com/forums/forumdisplay.php?f=59).
- **Modern Glass HUD (Dim Gray)** — the glass-style HUD in some of
  the screenshots. Drop the author a thank-you if you use it.

Drop each mod's contents into `<PSOBB>\data\` (or the modpack's
specified subdirectory). Most PSOBB texture overrides use whole-file
XVM replacements which work on any PSOBB client (Ephinea, psobb.io,
etc.). Per-item overrides under `data/ephinea/custom/` are Ephinea-
specific and won't apply on psobb.io.

---

## Credits

Preset tuning is mine; everything the presets *reference* belongs to
the shader authors above. In particular:

- **Pascal Gilcher (Marty McFly)** — iMMERSE shader suite. Do not
  redistribute his work; if you use the HD preset, you're using his
  MXAO, and he deserves the download click.
- **prod80** — the PD80 shader pack that drives the
  shadows / midtones / highlights grading.
- **crosire** and the broader ReShade contributor community — the
  baseline Tonemap, LUT, Deband, MagicBloom, Vignette, etc.

Texture mod authors (AshenbubsHD, the 9 Echelon team, Smokey, the
Modern Glass HUD author, and countless PSO-World contributors) —
thank you for keeping PSOBB looking alive 25 years in.

---

## License

The INI files in this zip are MIT-licensed. Shader packs and
texture mods referenced above retain their original licenses — check
each source repo before copying, modifying, or redistributing.
