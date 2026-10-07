# PSSwanStation build {{BUILD}}

A PlayStation (PS1) emulator as a PS5 homebrew title: SwanStation, the libretro fork of DuckStation, with a controller interface of its own, drawing with Vulkan (the RADV driver, linked into the title). It needs a jailbroken console with kstuff running.

> [!IMPORTANT]
> **Piracy is not condoned.** This release contains no games, no BIOS files, no console firmware and no decryption keys, and none will ever be provided or linked to. Use only **legally obtained backups of games you own**, made yourself from your own discs, and BIOS files dumped from **hardware you own**. Please don't ask for, or post links to, games, BIOS files, firmware or keys in issues or discussions: they will be removed.

> [!WARNING]
> **A work in progress.** This build has run on my console, but not every part of it has been checked there one by one. "Tested for this release" below says which.

## What it does

- **A library for the controller**: games from the title's folder, USB drives and a network share (SMB or FTP), with covers, descriptions, search, sorting and filters, favourites, and seven views (a cover grid, a list, and five that stand the covers in space; Aurora layout files work too).
- **Every setting of the emulator**, for every game or for one game alone, and the title's own: picture presets, FSR 1 scaling, picture-tube lines, what is beside a 4:3 picture, frame pacing, 120 Hz output, frame generation.
- **While playing**: save states with pictures, fast forward and rewind (kept in memory), cheats and patches, disc change, memory card manager with backups, button remapping and turbo, up to four players, RetroAchievements, netplay for two (an experiment).
- **No BIOS needed**: the built-in OpenBIOS starts most games; an original BIOS dumped from your own console is more compatible.
- **Looks after itself**: an updater that fetches a newer release and puts the earlier files back if it is interrupted, a safe start (hold L1 and R1), and your files kept outside the title's folder if you want.

## New in build {{BUILD}}

- **Picture settings can be a game's own.** Scaling (FSR 1 too) and its sharpening, smooth scaling, the picture preset, the picture tube, what is beside the picture, frame pacing, black frame insertion and frame generation: set them in a game's details (Options) or, while it runs, in the menu under Game settings, Picture. They are remembered for that game; other games keep the general ones. Display output (60 or 120 Hz) stays one setting for every game, because the console reads it when the title starts.
- **Circle in the library asks whether to close PSSwanStation.** The menu's "Close PSSwanStation" is still there.
- **Frame generation smears far less than the first attempt did** (build 13's change, first released here). The title now counts the pictures a game really draws (30, 20 or 60 a second) and fills only the gaps between them; a game that draws 60 at 60 Hz is left alone. A made picture is taken from the nearer of the two game pictures, not mixed from both. On my test scene, where the true picture between two is known, the made pictures are 26.4 dB from the true ones; the first attempt's were 21.5, and showing the nearer game picture twice is 15.9. Off, On, or "On, lighter".
- **The cheat and patch database is fetched, not carried.** Its entries belong to their authors and the chtdb project gives no licence to pass them on, so the release does not hold it: Settings, Games and network, "Fetch the newest cheat database" gets it from that project's own releases, once.
- **The Lapy helper is the newer one** (the fix for firmware 13.60 that ProsperoEden carries), for leaving the sandbox when USB drives or files outside the title's folder are wanted.

## Tested for this release

Build {{BUILD}}, the build in this ZIP, has run on my console (a jailbroken PS5 with kstuff). What I have a result for one by one, and the build it was seen with:

| What | On the console | Seen with |
| --- | --- | --- |
| Starting games from the title's folder and from an SMB share, sound, picture, controller | works | build 9 |
| Library: covers downloaded, search, jumping by letter, menu and start-up sounds | works | build 9 |
| Cheats and patches (from the database carried then), games on several discs, save states and continuing, cancelling a load | works | build 9 |
| RetroAchievements | works | build 10 |
| Frame generation | ran, and smeared; reworked in build 13 | build 12 |

No separate result on a console yet for: 120 Hz output, black frame insertion and the variable refresh rate, USB drives, FTP, a second controller and the multitap, netplay, keeping files outside the title's folder, the updater (this is the first release it can find), and speed at 8x internal resolution. Frame rates were not measured.

On a PC (the same frontend on the same Vulkan code paths, with the validation layers silent): the interface test, the module tests (updater, memory cards, RetroAchievements against a stand-in server, netplay between two processes), a game's own picture settings kept and brought back, fetching the cheat database from its project, and frame generation against pictures of known movement.

## Known issues

- Frame generation: a thin fringe where something moves across a background that moves another way; stripes and other even patterns can break; a game that draws straight into the shown picture cannot be counted and gains nothing unless it draws 60. At 20 pictures a second the made ones outnumber the game's two to one, and errors show most.
- 120 Hz output has not run on a console from this title: if the screen stays dark after choosing it, start PSSwanStation holding L1 and R1, wait ten seconds, close it and start it again.
- Netplay is an experiment that has only run between two PCs.
- A controller's charge is not shown: I know of no call a title can make that gives it.

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
| PSSwanStation | `{{COMMIT}}` (SwanStation at libretro `b6c30a7`, plus the `ps5` folder) |
| PS5_Vulkan (link recipe, libc.prx) | `5b5e4fc2d80f` |
| PS5_PayloadSDK fork (platform layer) | `611893fc25ef` |
| RADV, from PS5_Mesa | `7b59ef27c1b0` (built against the SDK fork at `b83202be73e9`) |
| Dear ImGui | v1.92.9b |
| libsmb2 | `7e4ff97cf00e` |
| rcheevos | v12.5.0 |
| Lapy helper | PS5-Lapy-JB-Daemon `c3bdfe3a3993`, built with the payload SDK v0.42 |
| Game database | libretro-database `fbeefcb46c2e` |

SHA-256 of `{{ZIP}}`: `{{ZIP_SHA256}}`

PSSwanStation is an unofficial port. It is not affiliated with or endorsed by Sony Interactive Entertainment, the libretro team or the DuckStation project. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment. Vulkan is a registered trademark of the Khronos Group; the RADV port in this title is not a conformant product.
