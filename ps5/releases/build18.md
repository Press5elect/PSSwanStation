# PSSwanStation build {{BUILD}}

A PlayStation (PS1) emulator as a PS5 homebrew title: SwanStation, the libretro fork of DuckStation, with a controller interface of its own, drawing with Vulkan (the RADV driver, linked into the title). It needs a jailbroken console with kstuff running.

> [!IMPORTANT]
> **Piracy is not condoned.** This release contains no games, no BIOS files, no console firmware and no decryption keys, and none will ever be provided or linked to. Use only **legally obtained backups of games you own**, made yourself from your own discs, and BIOS files dumped from **hardware you own**. Please don't ask for, or post links to, games, BIOS files, firmware or keys in issues or discussions: they will be removed.

> [!WARNING]
> **A work in progress.** This build works on my console, but not every feature new since build 15 has been tried there one by one. "Tested for this release" below says what has been checked where.

## What it does

- **A library for the controller**: games from the title's folder, USB drives and network shares (SMB, NFS or FTP), with covers, descriptions, search, sorting and filters, favourites, and seven views (a cover grid, a list, and five that stand the covers in space; Aurora layout files work too).
- **Every setting of the emulator**, for every game or for one game alone, and the title's own: picture presets, scaling filters (sharp bilinear, FSR 1, NIS, CAS), a CRT shader, video signals, colours, what is beside a 4:3 picture, frame pacing, 120 Hz output, frame generation.
- **While playing**: save states with pictures, fast forward and rewind (kept in memory), cheats and patches, disc change, memory card manager with backups, button remapping and turbo, up to four players, RetroAchievements, netplay for two (an experiment).
- **No BIOS needed**: the built-in OpenBIOS starts most games; an original BIOS dumped from your own console is more compatible.
- **Looks after itself**: an updater that fetches a newer release and puts the earlier files back if it is interrupted, a safe start (hold L1 and R1), and your files kept outside the title's folder, on a USB drive or on the network share if you want.

> [!NOTE]
> **Coming from build 14, 16 or 17? Copy the folder.** The updater in those builds downloads and checks a new build, then stops with "The unpacked update is no longer there" and changes nothing. Copy this release's `PPSA99248` folder over the old one by FTP: your games, saves, states, covers and settings stay. From build 18 on, the updater works.

## New in build 18

- **The updater works on the console.** It checked the files it had unpacked in a way the console does not answer, so every update stopped before installing. It now checks them the way the rest of the title checks its files, and still never follows a link.
- **Install again** (Menu, Update, Square, when you have the newest build): downloads the newest release again and puts it in place, to mend a damaged title folder. Your games, saves and settings stay.

Everything new in builds 15 to 17 is in the [build 17 release](https://github.com/Press5elect/PSSwanStation/releases/tag/build17).

## Tested for this release

Build {{BUILD}} works on my console; NFS I have not tried there. What I have a result for one by one, and the build it was seen with:

| What | On the console | Seen with |
| --- | --- | --- |
| Starting games from the title's folder and from an SMB share, sound, picture, controller | works | build 9 |
| Library: covers downloaded, search, jumping by letter, menu and start-up sounds | works | build 9 |
| Cheats and patches, games on several discs, save states and continuing, cancelling a load | works | build 9 |
| RetroAchievements | works | build 10 |
| A game's own picture settings, Circle to close, the cheat database fetched | works | build 14 |
| Scaling filters, picture tube, video signals, colours, frame generation, files on the share and USB drive, libretro cheats | works | build 15 |
| Starting, the library, What's new after an update | works | build 17 |
| The updater: Install again downloaded build 17, checked it and put it in place | works | build 18 |
| NFS shares | not tried yet | |

On a PC (the same frontend on the same Vulkan code paths, with the validation layers silent): for build 18, the updater's 2,450 checks, also with the console's way of answering file checks imitated, and Install again through a whole install of the real build 17 ZIP; for build 17, a test disc of textured meshes counted for gaps and stray texels at 1x, 4x and 8x and with PGXP and smoothing; every scaling filter at three sharpness levels, games drawn at 1x, 4x and 8x, 4K and 1080p screens, measured for sharpness, shift and overshoot against the game's own picture; frame generation against pictures of known movement at 60 and 120 Hz, games of 20, 30 and 60 pictures a second, every quality, ahead, the 60 cap and run-ahead, and on a disc with cuts. For build 16: sleep-safe saving with the title killed during a game; a recording played back to the same memory, byte for byte; a value found, watched, frozen and made a cheat; the speedrun timer; the web panel from a phone-sized browser, a wrong key and paths outside the folder refused; slang presets compiled; the interface test and the module tests.

## Known issues

- Not every feature new since build 15 has been tried on a console one by one. Sleep-safe saving asks the console whether its menu is open over the title, a call no build has made before: if PSSwanStation stops when a game starts, switch it off. The web panel is the first time the title listens on the network for anything but netplay. Compiling a shader preset on the console may take a while and some are too slow at 4K.
- Closing the gaps draws 3D objects' outlines a quarter of a pixel fuller; a polygon edge can still show a single texel of its texture's neighbour where the PlayStation itself shows one.
- A cut detected in a fade shows the fade without made pictures between its steps (nothing moves in a fade, so nothing is lost).
- Frame generation: a thin fringe where something moves across a background that moves another way; at 20 pictures a second the made ones outnumber the game's and errors show most.
- A recording plays back the same only with the same settings, cheats and build.
- A card changed on two consoles before they met keeps the newer one without asking (the other is kept beside it as `.conflict-<date>`). An FTP server can hold games but not your files.
- 120 Hz output has not run on a console from this title: if the screen stays dark after choosing it, start PSSwanStation holding L1 and R1, wait ten seconds, close it and start it again.
- Netplay is an experiment that has only run between two PCs.

## Install or update

1. Unpack the ZIP and copy the complete `PPSA99248` folder to `/data/homebrew/` on the console (FTP), so that `/data/homebrew/PPSA99248/eboot.bin` exists, and let your homebrew loader register it.
2. Copying over an earlier build keeps your games, saves, states, covers and settings: the ZIP holds only the program's files. From this build on, Menu, "Update" fetches the next one; builds before 18 cannot, so copy this one over by hand.
3. Your own legally obtained backups go in `games/` (or on a USB drive, or a network share named in `network.cfg`, which the title writes with an explanation at its first start). A BIOS dumped from your own console may go in `bios/`. None is included, and none ever will be.
4. Once, for cheats and patches: Settings, Games and network, "Fetch the newest cheat database".
5. Read `LEGAL.txt` and `README.txt` in the folder.

## Reporting problems

Say what you did, what you saw, which build (top right of the library) and which firmware. `logs/` in the title's folder keeps the last two runs: copy them before starting the title again. Never attach game files, BIOS files, firmware or keys, and please leave game titles out: "a 30 fps racing game" is enough.

## Source

This ZIP was built from PSSwanStation commit `{{COMMIT}}`, which is this release's tag. The title is distributed under the GNU General Public License, version 3, as a whole. The source of every part at the revision it was built from is attached (`SOURCES.txt` lists the archives, `SHA256SUMS` their digests); `licenses/` in the title's folder holds each part's licence and `licenses/components.json` the revisions.

## Build

| Component | Version |
| --- | --- |
| PSSwanStation | `{{COMMIT}}` (SwanStation at libretro `dd48056`, plus the `ps5` folder) |
| PS5_Vulkan (link recipe, libc.prx) | `5b5e4fc2d80f` |
| PS5_PayloadSDK fork (platform layer) | `611893fc25ef` |
| RADV, from PS5_Mesa | `7b59ef27c1b0` (built against the SDK fork at `b83202be73e9`) |
| Dear ImGui | v1.92.9b |
| libsmb2 | `fc710a3ebd58` |
| libnfs | `761323646431` (libnfs-8.0.0 and four commits) |
| rcheevos | v12.5.0 |
| Lapy helper | PS5-Lapy-JB-Daemon `c3bdfe3a3993`, built with the payload SDK v0.42 |
| Game database and libretro cheats | libretro-database `bf825e3ec48d` |
| crt-guest-advanced | libretro slang-shaders `e1d75632a205` |
| glslang | 16.6.0 |
| QR Code generator | Project Nayuki `3c6d0b3` |
| NIS, CAS | NVIDIAImageScaling `35e13ba`, FidelityFX-CAS `9fabcc9` |

SHA-256 of `{{ZIP}}`: `{{ZIP_SHA256}}`

PSSwanStation is an unofficial port. It is not affiliated with or endorsed by Sony Interactive Entertainment, the libretro team or the DuckStation project. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment. Vulkan is a registered trademark of the Khronos Group; the RADV port in this title is not a conformant product.
