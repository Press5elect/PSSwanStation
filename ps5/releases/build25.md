# PSSwanStation build 25

## Added

- **GameShark codes typed in.** "Add a GameShark code" in a game's Cheats asks for a name, then the code on a keypad; it is switched on at once and kept in the game's own `cheats/<serial>.cht`.
- **Cloud saves** (Settings, Games and saving): memory cards, and save states if wanted, kept in Google Drive or Dropbox too, both ways. Signing in takes a phone and a code.
- **Sound delay** (Settings, Sound): the game's sound kept 60, 32 or 16 ms ahead of the speakers.
- **MMPX**, a scaling filter for pixel art.
- **The PlayStation Mouse**: the touch pad (or the right stick) moves it; the touch pad pressed, Cross or R1 is its left button, Circle or L1 its right.
- **"Differs from the original"** (Settings, Picture; "Look" in the menu over a game): every setting in force that is not as the PlayStation had it, put back one by one or all at once.
- **Console tuning** (Settings, System): sound and the USB controllers ahead of the emulator and background jobs after it, the graphics driver on its own thread, and how long the driver checks and sleeps while it waits for the GPU. All off, or the driver's own, unless chosen.
- **The boot log measures the game**: with "Measure the game in the boot log", a line every five seconds of play (speed, worst frame, what each part of a frame took, draws, render passes and barriers per frame); always, a line when a game has run under 95 % for two seconds, and its average speed when it closes.

## Changed

- **A new install looks and runs as the PlayStation did**: the software renderer at the console's resolution, its interlacing, the cracks between polygons, the edges a television hid cropped, no texture pack, the CPU's instruction cache. An install from before keeps its settings.
- **Picture presets**: Original, Original on a TV (composite signal, curved picture tube), Fast (2x), Normal (4x), Enhanced (6x), Beautiful (8x) and Speedrun.
- **The CPU's instruction cache as the PlayStation has it**: a line takes 4 to 7 cycles to fill from RAM (it took 1 cycle a word), and the same code reached through either of its addresses is cached once.
- **The graphics driver is Mesa 27's RADV** with Crux2's fixes for this console: the GPU's GFX10.1 hardware workarounds (against wrong shader results and GPU hangs), memory kept until the GPU is done with it (against a whole-console hang), a shader cache that stays on, cheaper draw calls.
- **While a game falls behind the screen** a third frame may wait to be shown, so a late frame no longer holds the next one back.
- Newer parts: SwanStation's newest (libretro/swanstation 631f0fb), libsmb2 (727feaa) and libnfs (371333f).

## Fixed

- Closing a game no longer brings every file up to date with the network share: only its memory card and save states. The rest is brought up to date when PSSwanStation starts.
- A save state with an impossible timing event, MDEC command or video timing is refused instead of running on broken.

## Source

This ZIP was built from PSSwanStation commit `{{COMMIT}}`, this release's tag. The title is distributed under the GNU General Public License, version 3, as a whole; the source of every part is public at the revision it was built from:

{{SOURCES}}

`licenses/` in the title's folder holds each part's licence, and `licenses/components.json` the same revisions.

SHA-256 of `{{ZIP}}`: `{{ZIP_SHA256}}`

No games, BIOS files, console firmware or keys are included, now or ever: use games you own, from your own discs.
