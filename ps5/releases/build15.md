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

- **The picture's look.** A scaling filter of its own: smooth, square pixels, **sharp bilinear** (crisp, even pixels at any size), **FSR 1**, **NIS** (NVIDIA Image Scaling), **CAS** (AMD Contrast Adaptive Sharpening), with one sharpening setting. The picture tube adds **crt-guest-advanced** (guest(r)'s shader from the libretro collection) in five kinds: home television, studio monitor, arcade monitor, soft, and the author's own. **Video signal**: dither smoothed (the PlayStation's checkered dither undone, edges kept), S-Video and composite. **Brightness, contrast, colour and gamma.** Every one can be a game's own.
- **Frame generation, more of it.** Quality (Performance, Balanced, and Quality, which checks each movement from both frames' side), pictures **ahead** of the game's instead of between them (no waiting for the next), up to 60 a second at 120 Hz, videos left alone if you want, **one frame of run-ahead** to take back its delay, a view of the movement it found, and **any rate** with frame pacing by the clock (each refresh shows where the game is at that moment). Games that never swap their picture are now counted by whether they drew, and uneven cadences are evened out. The frame counter shows the game's pictures and the screen's ("30 -> 60"). It works on the game's own picture, before the look, so every filter works with it.
- **NFS shares** (versions 3 and 4) beside SMB and FTP: `path = nfs://server/export/games`.
- **Your files on the network share.** Memory cards kept in the share's "memory cards" folder too and brought up to date both ways, so several consoles at home play on the same cards (a card changed on two keeps the newer one and saves the other beside it); covers read from the share; or the whole PSSwanStation folder kept on the share, or on a USB drive.
- **Cheats for more games**: the libretro database's PlayStation cheats (CC BY-SA 4.0, carried in the ZIP) are used for a game the chtdb collection has no codes for. Each says where it came from and that it may not fit your version.
- Under the hood: SwanStation's newest upstream (save-state checks, out-of-bounds fixes, its own threads and atomics), libsmb2 `fc710a3`, libnfs added.

## Tested for this release

Build {{BUILD}} has **not run on my console yet**. What I have a result for one by one, and the build it was seen with:

| What | On the console | Seen with |
| --- | --- | --- |
| Starting games from the title's folder and from an SMB share, sound, picture, controller | works | build 9 |
| Library: covers downloaded, search, jumping by letter, menu and start-up sounds | works | build 9 |
| Cheats and patches, games on several discs, save states and continuing, cancelling a load | works | build 9 |
| RetroAchievements | works | build 10 |
| A game's own picture settings, Circle to close, the cheat database fetched | works | build 14 |
| Frame generation | ran, and smeared; reworked in build 13 | build 12 |

On a PC (the same frontend on the same Vulkan code paths, with the validation layers silent): every scaling filter, the five CRT kinds and the three signals on a test picture; frame generation against pictures of known movement (Balanced 25.8 dB from the true pictures, Quality 25.5, ahead 21.4; the game's picture shown twice is 13.5); NFS versions 3 and 4 against nfs-ganesha (library, starting a game, streamed and read into memory); memory cards shared between two title folders over NFS and SMB, with a conflict and a removal; covers from the share; the whole folder on the share and on a USB drive; the libretro cheats for a disc chtdb lacks; the interface test and the module tests.

## Known issues

- Most of what is new in this build has not run on a console yet. NFS has met no console socket yet; the files on the share, the USB drive's files, the CRT shaders' speed at 4K and frame generation's Quality kind are untried there.
- Frame generation: a thin fringe where something moves across a background that moves another way; stripes can break; at 20 pictures a second the made ones outnumber the game's and errors show most. Ahead of the game's pictures makes more mistakes where movement turns or stops. Movement told by the emulator itself (from the polygons) and depth for overlaps are not done: a PlayStation game gives nothing that says which polygon is which from one frame to the next.
- Composite shows strong colour fringes on one-pixel checkerboards, as composite does.
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
| PSSwanStation | `{{COMMIT}}` (SwanStation at libretro `1db8c9b`, plus the `ps5` folder) |
| PS5_Vulkan (link recipe, libc.prx) | `5b5e4fc2d80f` |
| PS5_PayloadSDK fork (platform layer) | `611893fc25ef` |
| RADV, from PS5_Mesa | `7b59ef27c1b0` (built against the SDK fork at `b83202be73e9`) |
| Dear ImGui | v1.92.9b |
| libsmb2 | `fc710a3ebd58` |
| libnfs | `761323646431` (libnfs-8.0.0 and four commits) |
| rcheevos | v12.5.0 |
| Lapy helper | PS5-Lapy-JB-Daemon `c3bdfe3a3993`, built with the payload SDK v0.42 |
| Game database and libretro cheats | libretro-database `fbeefcb46c2e` |
| crt-guest-advanced | libretro slang-shaders `1e0238f9fdd4` |
| NIS, CAS | NVIDIAImageScaling `35e13ba`, FidelityFX-CAS `9fabcc9` |

SHA-256 of `{{ZIP}}`: `{{ZIP_SHA256}}`

PSSwanStation is an unofficial port. It is not affiliated with or endorsed by Sony Interactive Entertainment, the libretro team or the DuckStation project. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment. Vulkan is a registered trademark of the Khronos Group; the RADV port in this title is not a conformant product.
