# PSSwanStation build {{BUILD}}

A PlayStation (PS1) emulator as a PS5 homebrew title: SwanStation, the libretro fork of DuckStation, with a controller interface of its own, drawing with Vulkan (the RADV driver, linked into the title). It needs a jailbroken console with kstuff running.

> [!IMPORTANT]
> **Piracy is not condoned.** This release contains no games, no BIOS files, no console firmware and no decryption keys, and none will ever be provided or linked to. Use only **legally obtained backups of games you own**, made yourself from your own discs, and BIOS files dumped from **hardware you own**. Please don't ask for, or post links to, games, BIOS files, firmware or keys in issues or discussions: they will be removed.

> [!WARNING]
> **A work in progress.** This build has not run on my console yet; earlier builds have. "Tested for this release" below says what has been checked where.

## What it does

- **A library for the controller**: games from the title's folder, USB drives and network shares (SMB, NFS or FTP), with covers, descriptions, search, sorting and filters, favourites, and seven views (a cover grid, a list, and five that stand the covers in space; Aurora layout files work too).
- **Every setting of the emulator**, for every game or for one game alone, and the title's own: picture presets, scaling filters (sharp bilinear, FSR 1, NIS, CAS), a CRT shader, video signals, colours, what is beside a 4:3 picture, frame pacing, 120 Hz output, frame generation.
- **While playing**: save states with pictures, fast forward and rewind (kept in memory), cheats and patches, disc change, memory card manager with backups, button remapping and turbo, up to four players, RetroAchievements, netplay for two (an experiment).
- **No BIOS needed**: the built-in OpenBIOS starts most games; an original BIOS dumped from your own console is more compatible.
- **Looks after itself**: an updater that fetches a newer release and puts the earlier files back if it is interrupted, a safe start (hold L1 and R1), and your files kept outside the title's folder, on a USB drive or on the network share if you want.

## New in build {{BUILD}}

- **Sleep-safe saving.** With it on, a game's resume state is saved every five minutes of play and whenever the console's own menu opens over it, quietly and without a pause. If the console closes PSSwanStation (rest mode, or closing it from that menu), the next start offers to continue.
- **Speedrun timer.** Game time or real time over the game, splits by hand (OPTIONS with Cross) or by themselves when a value in the game's memory changes, a start by itself the same way, and the best run and best splits kept for each game. A run with fast forward, rewind or a state load counts as practice.
- **Find in memory.** The classic cheat search: a value, or one that went up, down, changed or stayed, narrowed down while you play. What is left can be watched over the game, set, frozen, or made a cheat in the game's own file.
- **Phone and web control.** Scan the code in Settings with a phone (port 3311): start and close games, save and load states, run the speedrun timer, change some settings, and put discs and covers in or take saves out from a computer. The address carries a key; nothing is given out without it.
- **Shader presets from a USB drive.** A libretro slang preset (.slangp) in `PSSwanStation/shaders/` is compiled on the console and becomes one of the picture tube's choices.
- **Debug settings.** Recordings of what each player presses, played back frame for frame from where they began; verbose logging with a line about speed, sound and memory every ten seconds.
- **Picture preset Speedrun**: the picture as the PlayStation drew it, with nothing added (no PGXP, frame generation, picture tube or hacks).
- **Easier to read**: the interface up to 140%, high contrast, and colour-blind safe colours.
- **Texture packs on USB** are now read from `PSSwanStation/textures/<serial>/` on the drive (the old place still works).
- Under the hood: SwanStation's newest upstream (a video playback fix, fuzzing fixes, a check that a save state belongs to the disc), glslang 16.6.0 added.

## Tested for this release

Build {{BUILD}} has **not run on my console yet**; build 15 works there, all but NFS. What I have a result for one by one, and the build it was seen with:

| What | On the console | Seen with |
| --- | --- | --- |
| Starting games from the title's folder and from an SMB share, sound, picture, controller | works | build 9 |
| Library: covers downloaded, search, jumping by letter, menu and start-up sounds | works | build 9 |
| Cheats and patches, games on several discs, save states and continuing, cancelling a load | works | build 9 |
| RetroAchievements | works | build 10 |
| A game's own picture settings, Circle to close, the cheat database fetched | works | build 14 |
| Scaling filters, picture tube, video signals, colours, frame generation, files on the share and USB drive, libretro cheats | works | build 15 |
| NFS shares | not tried yet | |

On a PC (the same frontend on the same Vulkan code paths, with the validation layers silent): sleep-safe saving with the title killed during a game and the offer at the next start; a recording played back to the same memory, byte for byte; a value found, watched, frozen and made a cheat; the speedrun timer with splits by hand and by memory, and a best kept; the web panel from a phone-sized browser, every request, a wrong key and paths outside the folder refused, the code read back by a camera library; slang presets compiled (crt-guest-advanced gives the same passes as the built-in one; crt-lottes and crt-yah draw); the Speedrun preset, 140% and high contrast; texture packs from the new place; the interface test and the module tests.

## Known issues

- Everything new in this build is untried on a console. Sleep-safe saving asks the console whether its menu is open over the title, a call no build has made before: if PSSwanStation stops when a game starts, switch it off. The web panel is the first time the title listens on the network for anything but netplay. Compiling a shader preset on the console may take a while and some are too slow at 4K.
- Sparkles along polygon edges (gaps between polygons) in some 3D games, as in SwanStation elsewhere: I am working on it for a later build.
- Frame generation: a thin fringe where something moves across a background that moves another way; at 20 pictures a second the made ones outnumber the game's and errors show most.
- A recording plays back the same only with the same settings, cheats and build.
- A card changed on two consoles before they met keeps the newer one without asking (the other is kept beside it as `.conflict-<date>`). An FTP server can hold games but not your files.
- 120 Hz output has not run on a console from this title: if the screen stays dark after choosing it, start PSSwanStation holding L1 and R1, wait ten seconds, close it and start it again.
- Netplay is an experiment that has only run between two PCs.

## Install or update

1. Unpack the ZIP and copy the complete `PPSA99248` folder to `/data/homebrew/` on the console (FTP), so that `/data/homebrew/PPSA99248/eboot.bin` exists, and let your homebrew loader register it.
2. Copying over an earlier build keeps your games, saves, states, covers and settings: the ZIP holds only the program's files. From this build on, Menu, "Update" can fetch the next one.
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
