# PSSwanStation build {{BUILD}}

A PlayStation (PS1) emulator as a PS5 homebrew title: SwanStation, the libretro fork of DuckStation, with a controller interface of its own, drawing with Vulkan (the RADV driver, linked into the title). It needs a jailbroken console with kstuff running.

> [!IMPORTANT]
> **Piracy is not condoned.** This release contains no games, no BIOS files, no console firmware and no decryption keys, and none will ever be provided or linked to. Use only **legally obtained backups of games you own**, made yourself from your own discs, and BIOS files dumped from **hardware you own**. Please don't ask for, or post links to, games, BIOS files, firmware or keys in issues or discussions: they will be removed.

> [!WARNING]
> **A work in progress.** USB controllers and netplay have not been tested on a console. The three things made last for this build (the HTTPS fix, the drive speed setting and the drive's animation) were checked on a PC. "Tested for this release" below says what has been checked where.

## What it does

- **A library for the controller**: games from the title's folder, USB drives, network shares (SMB, NFS or FTP) and a disc in a USB CD or DVD drive, with covers, descriptions, search, sorting and filters, favourites, and eight views (shelves, a cover grid, a list, and five that stand the covers in space; Aurora layout files work too).
- **Thirty-one themes**: the interface is drawn by the PS5 homebrew UI kit, each theme with its own colours, panels, type, motion and sounds, and a swan that lives in the corner.
- **Every setting of the emulator**, for every game or for one game alone, and the title's own: picture presets, scaling filters (sharp bilinear, FSR 1, NIS, CAS), a CRT shader, video signals, colours, film grain, what is beside a 4:3 picture, frame pacing, 120 Hz output with variable refresh rate, frame generation (the title's own, AMD FSR3's optical flow, or MihawkGen's trained network).
- **While playing**: save states with pictures, fast forward and rewind (kept in memory), cheats and patches, disc change, memory card manager with backups, button remapping and turbo, up to four players, USB gamepads the console does not take as its own (an experiment), run-ahead, RetroAchievements, netplay for two (an experiment).
- **No BIOS needed**: the built-in OpenBIOS starts most games; an original BIOS dumped from your own console is more compatible.
- **Looks after itself**: an updater that fetches a newer release and puts the earlier files back if it is interrupted, a safe start (hold L1 and R1), and your files kept outside the title's folder, on a USB drive or on the network share if you want.

> [!NOTE]
> **Coming from build 18, 19, 20 or 21?** Menu, Update fetches this one. **From build 14, 16 or 17**, copy this release's `PPSA99248` folder over the old one by FTP: their updater stops before installing. Your games, saves, states, covers and settings stay either way.

## New in build {{BUILD}}

- **MihawkGen frame generation.** A third Generation method (Settings, Picture, with Frame generation on): Mihawk's trained network of 432,308 parameters ([PS5_MihawkGen](https://github.com/mihawk-99/PS5_MihawkGen)). It looks at the game's two pictures alone, estimates the movement and a mask at four scales, warps both pictures and mixes them, and leaves what did not move exactly as it was. It makes one, two or three pictures between two of the game's, at even steps, and none ahead. Game by game like the other picture settings, and in the web panel.
- **RetroAchievements, covers and the update check work with the USB drives on.** With "USB drives" or "USB disc drive" on, PSSwanStation leaves its sandbox through the sandbox elevator, and from then on the console's own HTTPS client refused every certificate (`0x8095f00c` in the boot log), so nothing could reach its server. PSSwanStation now has an HTTPS client of its own, on mbedTLS 3.6.7, checking certificates against the console's list; it takes over from the moment the elevator answers yes, or from the first request the console's client refuses. BlackBearReloaded's ps5-native-app-boilerplate documents the same behaviour of the console's client.
- **USB disc drives the console does not attach** (it lists them only as a USB device) are read by PSSwanStation itself, over USB.
- **Drive speed: Quiet, Normal or Max** (Settings, Games and network, under "USB disc drive", and in the web panel). Max, the usual, reads as fast as the drive goes, so a game played from the drive and a copy into memory no longer wait on a drive held at about 2 MB/s; Normal is 24 times a CD's speed, Quiet 8 times. A drive that fails a read twice is slowed a step (24x, 16x, 8x, 4x) for the rest of the session, and a failed read is tried again after a pause. The boot log says the speed and how many MB/s a copy and a disc read into memory really took.
- **The drive comes alive in the library.** The Disc Drive tab draws the drive and the disc in it instead of a plain picture: the disc drops in when it arrives and floats while it rests, turns while a game plays from it, and spins fast with a laser glint, a blinking light and a progress ring while it is read; an empty drive slides its tray out for a disc, and a drive not found sends out search pulses. The line under the name says what the drive is doing. It follows the interface's motion setting.
- With "USB disc drive" on and no drive found, the library's **Disc Drive tab says why**, as far as the title can tell. The boot log names each drive's maker, model and bus, the USB ports' state and the console's own USB messages.

Everything new in builds 15 to 21 is in the [build 17](https://github.com/Press5elect/PSSwanStation/releases/tag/build17), [build 18](https://github.com/Press5elect/PSSwanStation/releases/tag/build18), [build 19](https://github.com/Press5elect/PSSwanStation/releases/tag/build19), [build 20](https://github.com/Press5elect/PSSwanStation/releases/tag/build20) and [build 21](https://github.com/Press5elect/PSSwanStation/releases/tag/build21) releases.

## Tested for this release

| What | On the console | Seen with |
| --- | --- | --- |
| Starting games from the title's folder and from an SMB share, sound, picture, controller | works | build 9 |
| Library: covers downloaded, search, jumping by letter, menu and start-up sounds | works | build 9 |
| Cheats and patches, games on several discs, save states and continuing, cancelling a load | works | build 9 |
| RetroAchievements | works | build 10 |
| The updater: Install again downloaded build 17, checked it and put it in place | works | build 18 |
| The new interface: themes, Shelves, the swan, the regrouped menus, film grain, the picture's light | looks right | build 19 |
| AMD FSR3 optical flow frame generation; 120 Hz at every start; run-ahead at 120 Hz | works | build 20 |
| USB disc drive found through the sandbox elevator, tray unlocked, Disc Drive tab, cover | works | build 21 |
| Playing a disc from the drive: eight minutes, read with its sub-channel, CD music, no hitching once the disc was in memory (228 s for a full CD) | works | build 21 |
| MihawkGen frame generation: two games, one at 320 x 240 and one at up to 3312 x 4320, 30 pictures a second shown at about 120 at 120 Hz, no errors in the boot log | works | build 22's test build |
| Copying a disc to a folder or into memory, a LibCrypt-protected disc from the drive, CD music while the disc is still being read, NFS shares, reading a drive over USB | works | build 22's test builds |
| RetroAchievements and the update check with the USB drives on (the boot logs showed `0x8095f00c` before the fix) | fixed, checked on a PC | |
| Drive speed setting (Quiet, Normal, Max); the drive's animation in the library | checked on a PC | |
| USB controllers, netplay | not tried yet | |

On a PC (the same frontend on the same Vulkan code paths): the new HTTPS client against a local server with a certificate authority of its own, over TLS 1.3 and TLS 1.2, 20 cases, all passing: plain, chunked, redirected and closed-ended answers, an error with its body, a 3 MB download in both forms identical byte for byte, a certificate list in DER, and a wrong authority, a wrong host name and a refused port each refused with its reason; the title's whole scripted run (menus, cheats, a game, the menu, a save state) with the update check through it. MihawkGen on moving test pictures: its made pictures 40.6 dB from the true ones, a plain blend 33.2 dB. Earlier builds' PC checks are in their notes.

## Known issues

- The HTTPS client of this build was checked on a PC only. Should RetroAchievements still not connect with the USB drives on, the boot log's `tls:` and `http:` lines say why; turning "USB drives" and "USB disc drive" off and starting PSSwanStation again uses the console's own client as before.
- MihawkGen has no movement view, and its time on the graphics processor at a large resolution scale has not been measured; should it fail to start, the title's own frame generation is used and the boot log says why.

- A console turned on with a disc in a USB drive cannot turn itself off afterwards; hold its power button. Start the console with the drive empty.
- Playing from the drive reads the whole disc behind the game, as fast as the drive goes; until it is done (a few minutes) the drive is audible and a jump to a part of the disc not read yet takes the drive a fraction of a second, which the game waits for as on a PlayStation.
- A drive that does not let PSSwanStation send it SCSI commands gives 2048 bytes a sector: games start, but without videos, XA sound and CD music.
- The pages other than the library keep their layouts, in the kit's themed panels; they are not yet built from the kit's own components.
- Compiling a shader preset on the console may take a while and some are too slow at 4K.
- Frame generation: a thin fringe where something moves across a background that moves another way; at 20 pictures a second the made ones outnumber the game's and errors show most.
- A card changed on two consoles before they met keeps the newer one without asking (the other is kept beside it as `.conflict-<date>`). An FTP server can hold games but not your files.
- Should the screen stay dark after choosing 120 Hz, start PSSwanStation holding L1 and R1, wait ten seconds, close it and start it again.
- Run-ahead at 60 Hz has no refresh between two frames to do its rollback in, so a press there still costs two frames' work in one refresh.
- USB controllers have not been seen to work on a console yet. Netplay is an experiment that has only run between two PCs.

## Install or update

1. Unpack the ZIP and copy the complete `PPSA99248` folder to `/data/homebrew/` on the console (FTP), so that `/data/homebrew/PPSA99248/eboot.bin` exists, and let your homebrew loader register it.
2. Copying over an earlier build keeps your games, saves, states, covers and settings: the ZIP holds only the program's files. Menu, "Update" fetches the next one; builds before 18 cannot, so copy this one over by hand.
3. Your own legally obtained backups go in `games/` (or on a USB drive, or a network share named in `network.cfg`, which the title writes with an explanation at its first start). A BIOS dumped from your own console may go in `bios/`. None is included, and none ever will be.
4. Once, for cheats and patches: Settings, Games and network, "Fetch the newest cheat database".
5. Read `LEGAL.txt` and `README.txt` in the folder.

## Reporting problems

Say what you did, what you saw, which build (top right of the library) and which firmware. `logs/` in the title's folder keeps the last two runs: copy them before starting the title again. Never attach game files, BIOS files, firmware or keys, and please leave game titles out: "a 30 fps racing game" is enough.

## Source

This ZIP was built from PSSwanStation commit `{{COMMIT}}`, which is this release's tag. The title is distributed under the GNU General Public License, version 3, as a whole. The source of every part at the revision it was built from is published (`SOURCES.txt` lists the archives, `SHA256SUMS` their digests): this release attaches the two that are new in it, PSSwanStation's own and mbedTLS's; the other thirteen are byte for byte the archives attached to the [build 21 release](https://github.com/Press5elect/PSSwanStation/releases/tag/build21) (the same revisions, the same digests); `licenses/` in the title's folder holds each part's licence and `licenses/components.json` the revisions. The RADV driver and PS5_Vulkan revisions of this build are not on GitHub: their attached archives are their published source, and `SOURCES.txt` says which GitHub commit each is built on.

## Build

| Component | Version |
| --- | --- |
| PSSwanStation | `{{COMMIT}}` (SwanStation at libretro `dd48056`, plus the `ps5` folder) |
| PS5_Vulkan (link recipe, libc.prx) | `b7dad6fefd89` (GitHub's `252bf91` and the RADV pin below; in the attached archive) |
| PS5_PayloadSDK fork (platform layer) | `b5efad528e0a` (its newest) |
| RADV, from PS5_Mesa | `5e99244a5567`: GitHub's `47e9eb61ee7`, my 120 Hz fix and timed waits on the right clock, in the attached archive (built against the SDK fork at `b83202be73e9`) |
| Interface kit | PS5_VKHomebrewUI `2188642afbcc` (BlackBearReloaded's ps5-homebrew-ui with a Vulkan backend) |
| libsmb2 | `fc710a3ebd58` |
| libnfs | `761323646431` (libnfs-8.0.0 and four commits) |
| rcheevos | v12.5.0 |
| Lapy helper | PS5-Lapy-JB-Daemon `c3bdfe3a3993`, built with the payload SDK v0.42 |
| Game database and libretro cheats | libretro-database `bf825e3ec48d` |
| crt-guest-advanced | libretro slang-shaders `e1d75632a205` |
| glslang | 16.6.0 |
| QR Code generator | Project Nayuki `3c6d0b3` |
| NIS, CAS | NVIDIAImageScaling `35e13ba`, FidelityFX-CAS `9fabcc9` |
| Optical flow frame generation | AMD FidelityFX SDK 2.3.0 FSR3 optical flow shaders (vendored, see `licenses/`) |
| USB pad decoder | vendored at `6ccf48047a12` (see `licenses/`) |
| Clock and pacing wait | from Crux2 1.0.1 (see `licenses/`) |
| MihawkGen | PS5_MihawkGen `bb45cf1c5369` (vendored with its network, see `licenses/`) |
| mbedTLS | 3.6.7 (`068ff080b369`) |
| Sandbox elevator | BlackBearReloaded's one-request elevator, as Crux2 sends it (`sandbox-elevator.elf`, source in `ps5/elevator`) |

SHA-256 of `{{ZIP}}`: `{{ZIP_SHA256}}`

The interface kit's fonts (Inter, Montserrat, Press Start 2P, Patrick Hand, DejaVu Sans Mono), sounds and songs are its own, under the licences in `licenses/`.

PSSwanStation is an unofficial port. It is not affiliated with or endorsed by Sony Interactive Entertainment, the libretro team or the DuckStation project. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment. Vulkan is a registered trademark of the Khronos Group; the RADV port in this title is not a conformant product.
