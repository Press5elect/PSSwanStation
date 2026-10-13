# PSSwanStation build {{BUILD}}

A PlayStation (PS1) emulator as a PS5 homebrew title: SwanStation, the libretro fork of DuckStation, with a controller interface of its own, drawing with Vulkan (the RADV driver, linked into the title). It needs a jailbroken console with kstuff running.

> [!IMPORTANT]
> **Piracy is not condoned.** This release contains no games, no BIOS files, no console firmware and no decryption keys, and none will ever be provided or linked to. Use only **legally obtained backups of games you own**, made yourself from your own discs, and BIOS files dumped from **hardware you own**. Please don't ask for, or post links to, games, BIOS files, firmware or keys in issues or discussions: they will be removed.

> [!WARNING]
> **A work in progress.** The USB disc drive is new in this build and has been played on one drive and one console. "Tested for this release" below says what has been checked where.

## What it does

- **A library for the controller**: games from the title's folder, USB drives, network shares (SMB, NFS or FTP) and a disc in a USB CD or DVD drive, with covers, descriptions, search, sorting and filters, favourites, and eight views (shelves, a cover grid, a list, and five that stand the covers in space; Aurora layout files work too).
- **Thirty-one themes**: the interface is drawn by the PS5 homebrew UI kit, each theme with its own colours, panels, type, motion and sounds, and a swan that lives in the corner.
- **Every setting of the emulator**, for every game or for one game alone, and the title's own: picture presets, scaling filters (sharp bilinear, FSR 1, NIS, CAS), a CRT shader, video signals, colours, film grain, what is beside a 4:3 picture, frame pacing, 120 Hz output with variable refresh rate, frame generation (the title's own, or AMD FSR3's optical flow).
- **While playing**: save states with pictures, fast forward and rewind (kept in memory), cheats and patches, disc change, memory card manager with backups, button remapping and turbo, up to four players, USB gamepads the console does not take as its own (an experiment), run-ahead, RetroAchievements, netplay for two (an experiment).
- **No BIOS needed**: the built-in OpenBIOS starts most games; an original BIOS dumped from your own console is more compatible.
- **Looks after itself**: an updater that fetches a newer release and puts the earlier files back if it is interrupted, a safe start (hold L1 and R1), and your files kept outside the title's folder, on a USB drive or on the network share if you want.

> [!NOTE]
> **Coming from build 18, 19 or 20?** Menu, Update fetches this one. **From build 14, 16 or 17**, copy this release's `PPSA99248` folder over the old one by FTP: their updater stops before installing. Your games, saves, states, covers and settings stay either way.

## New in build {{BUILD}}

- **Discs from a USB CD or DVD drive.** Plug a drive into the console and put a PlayStation disc you own in it: it appears under the library's new **Disc Drive** tab, named from its serial, with its cover. Before it starts, PSSwanStation asks how to play it:
  - **Copy it, then play the copy**: a `.bin` and `.cue` (with an `.sbi` for a LibCrypt-protected disc) in the games folder, on a USB drive, the SMB share or the NFS share. The copy then plays without the disc.
  - **Copy it into memory and play it**: the whole disc is read first, and the game never waits for the drive.
  - **Play it from the drive**: starts at once. Behind the game the whole disc is read into memory (what the game is about to read first, then the start of every track, then the rest), and the drive then rests. A seek of the real drive makes the emulated drive take longer instead of stopping the emulator.
  
  The drive is read as whole sectors with their sub-channel, which is everything a PlayStation disc holds (videos, XA sound, CD music, LibCrypt's protection), when the console lets PSSwanStation send it SCSI commands; otherwise 2048 bytes a sector, which loses videos, XA sound and CD music. The choice page and the boot log (`disc drive:` lines) say which. It needs PSSwanStation out of its sandbox for USB (Settings, Games and network: "USB disc drive"), and the ELF loader on port 9021 for the sandbox elevator that shows the console's drives.
- **Start the console with the drive empty.** A console turned on with a disc in a USB drive locks the tray and later cannot turn itself off (only by holding its power button). PSSwanStation unlocks the tray when it sees the drive, and the drive's page has "Eject the disc".
- **Less work for the processor**, as in Crux2 1.0.1: the clock comes from the processor's own counter (about 12 ns a read instead of about 20 µs), two of the graphics driver's waits sleep instead of spinning, and frame pacing sleeps by a margin measured at each start.
- The payload SDK's platform layer at its newest.
- The boot log is always in the title's folder, where the manual says it is, wherever your other files are kept.
- PSSwanStation's licence names its author and asks for credit when its source is used, including by AI tools (`licenses/ADDITIONAL-TERMS.txt`, a term under section 7(b) of the GPL).

Everything new in builds 15 to 20 is in the [build 17](https://github.com/Press5elect/PSSwanStation/releases/tag/build17), [build 18](https://github.com/Press5elect/PSSwanStation/releases/tag/build18), [build 19](https://github.com/Press5elect/PSSwanStation/releases/tag/build19) and [build 20](https://github.com/Press5elect/PSSwanStation/releases/tag/build20) releases.

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
| No freeze at the start of a CD music track while the disc is still being read | fixed, not tried yet | |
| Copying a disc to a folder or into memory | not tried yet | |
| A LibCrypt-protected disc from the drive | not tried yet | |
| USB controllers, NFS shares | not tried yet | |

On a PC (the same frontend on the same Vulkan code paths): a disc image standing in for the drive was copied byte for byte (the `.cue` with its pregaps, the `.sbi` with its altered sectors), played from memory and played from the "drive", for a drive that gives everything, one with no sub-channel, one that gives 2048 bytes a sector and one that refuses Form 2 sectors to such reads; a slow drive, an empty one and one unplugged while watched. Earlier builds' PC checks are in their notes.

## Known issues

- A console turned on with a disc in a USB drive cannot turn itself off afterwards; hold its power button. Start the console with the drive empty.
- Playing from the drive reads the whole disc behind the game, as fast as the drive goes; until it is done (a few minutes) the drive is audible and a jump to a part of the disc not read yet takes the drive a fraction of a second, which the game waits for as on a PlayStation.
- A drive that does not let PSSwanStation send it SCSI commands gives 2048 bytes a sector: games start, but without videos, XA sound and CD music.
- The pages other than the library keep their layouts, in the kit's themed panels; they are not yet built from the kit's own components.
- Not every feature new since build 15 has been tried on a console one by one. Compiling a shader preset on the console may take a while and some are too slow at 4K.
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

This ZIP was built from PSSwanStation commit `{{COMMIT}}`, which is this release's tag. The title is distributed under the GNU General Public License, version 3, as a whole. The source of every part at the revision it was built from is attached (`SOURCES.txt` lists the archives, `SHA256SUMS` their digests); `licenses/` in the title's folder holds each part's licence and `licenses/components.json` the revisions. The RADV driver and PS5_Vulkan revisions of this build are not on GitHub: their attached archives are their published source, and `SOURCES.txt` says which GitHub commit each is built on.

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
| Sandbox elevator | BlackBearReloaded's one-request elevator, as Crux2 sends it (`sandbox-elevator.elf`, source in `ps5/elevator`) |

SHA-256 of `{{ZIP}}`: `{{ZIP_SHA256}}`

The interface kit's fonts (Inter, Montserrat, Press Start 2P, Patrick Hand, DejaVu Sans Mono), sounds and songs are its own, under the licences in `licenses/`.

PSSwanStation is an unofficial port. It is not affiliated with or endorsed by Sony Interactive Entertainment, the libretro team or the DuckStation project. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment. Vulkan is a registered trademark of the Khronos Group; the RADV port in this title is not a conformant product.
