# PSSwanStation build {{BUILD}}

A PlayStation (PS1) emulator as a PS5 homebrew title: SwanStation, the libretro fork of DuckStation, with a controller interface of its own, drawing with Vulkan (the RADV driver, linked into the title). It needs a jailbroken console with kstuff running.

> [!IMPORTANT]
> **Piracy is not condoned.** This release contains no games, no BIOS files, no console firmware and no decryption keys, and none will ever be provided or linked to. Use only **legally obtained backups of games you own**, made yourself from your own discs, and BIOS files dumped from **hardware you own**. Please don't ask for, or post links to, games, BIOS files, firmware or keys in issues or discussions: they will be removed.

> [!WARNING]
> **A work in progress.** This build looks right on my console, but not every feature new since build 15 has been tried there one by one, and the USB controllers of this build not at all. "Tested for this release" below says what has been checked where.

## What it does

- **A library for the controller**: games from the title's folder, USB drives and network shares (SMB, NFS or FTP), with covers, descriptions, search, sorting and filters, favourites, and eight views (shelves, a cover grid, a list, and five that stand the covers in space; Aurora layout files work too).
- **Thirty-one themes**: the interface is drawn by the PS5 homebrew UI kit, each theme with its own colours, panels, type, motion and sounds, and a swan that lives in the corner.
- **Every setting of the emulator**, for every game or for one game alone, and the title's own: picture presets, scaling filters (sharp bilinear, FSR 1, NIS, CAS), a CRT shader, video signals, colours, film grain, what is beside a 4:3 picture, frame pacing, 120 Hz output with variable refresh rate, frame generation (the title's own, or AMD FSR3's optical flow).
- **While playing**: save states with pictures, fast forward and rewind (kept in memory), cheats and patches, disc change, memory card manager with backups, button remapping and turbo, up to four players, USB gamepads the console does not take as its own (an experiment), run-ahead, RetroAchievements, netplay for two (an experiment).
- **No BIOS needed**: the built-in OpenBIOS starts most games; an original BIOS dumped from your own console is more compatible.
- **Looks after itself**: an updater that fetches a newer release and puts the earlier files back if it is interrupted, a safe start (hold L1 and R1), and your files kept outside the title's folder, on a USB drive or on the network share if you want.

> [!NOTE]
> **Coming from build 18 or 19?** Menu, Update fetches this one. **From build 14, 16 or 17**, copy this release's `PPSA99248` folder over the old one by FTP: their updater stops before installing. Your games, saves, states, covers and settings stay either way.

## New in build 20

- **A second frame generation: AMD FSR3's optical flow**, from AMD's FidelityFX SDK, beside PSSwanStation's own (Settings, Picture, Generation method). It finds how the picture moves without the game's help and keeps what stands still (a score, a map) still. On my test pictures its made frames are about half as far from the true ones as the title's own. It makes one, two or three frames between two of the game's, never ahead. The title's own stays the default.
- **USB controllers** the console does not take as its own (an experiment): a PS3 pad, a DualShock 3 on a cable, an Xbox 360 pad or another USB gamepad, as the next player or together with player 1. Settings, Controllers, USB controllers; off at first. Only a device that says what it is gets read, and Sony's controllers are left to the console. The boot log's `usbpad` lines say what the console offers and what the controller sends.
- **120 Hz at every start.** It used to come up at 60 Hz at some starts, and with variable refresh rate the About page could say 60 Hz while the screen ran at 120. The graphics driver decided from one look at the screen while it was still changing mode; it now waits for the mode to settle (up to 1.5 s, once, at start) and keeps 120 Hz with variable refresh rate.
- **Run-ahead without the hitch.** A button pressed with run-ahead slowed the game for a moment, and with variable refresh rate the screen dimmed a little: the change of input makes run-ahead run the frame before again, two frames' work in one refresh. At 120 Hz that work now happens in the refresh between two of the game's frames, with the pads read then, so a press is also taken half a frame sooner.
- **The picture's look over the optical flow's frames**: film grain, colours, scaling filters, sharpening and the picture tube were left out with it; they are drawn now, as with the title's own frames.
- The boot log says every ten seconds of play how many frames took longer than a refresh and a half, and what each part of the worst one took.

Everything new in builds 15 to 19 is in the [build 17](https://github.com/Press5elect/PSSwanStation/releases/tag/build17), [build 18](https://github.com/Press5elect/PSSwanStation/releases/tag/build18) and [build 19](https://github.com/Press5elect/PSSwanStation/releases/tag/build19) releases.

## Tested for this release

Build {{BUILD}} looks right on my console; NFS and USB controllers I have not tried there. What I have a result for one by one, and the build it was seen with:

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
| The new interface: themes, Shelves, the swan, the regrouped menus, film grain, the picture's light | looks right | build 19 |
| AMD FSR3 optical flow frame generation | works | build 20 |
| 120 Hz at every start, and with variable refresh rate | works | build 20 |
| Run-ahead (1 frame) at 120 Hz: no slowdown or dimming on a press | works | build 20 |
| USB controllers | not tried yet | |
| NFS shares | not tried yet | |

On a PC (the same frontend on the same Vulkan code paths): for build 20, both frame generations on test pictures of known movement at four speeds against the true middle frame (the optical flow 33.7 to 50.2 dB, the title's own 29.7 to 48.7 dB), film grain over the optical flow's frames, run-ahead's early rollback giving the same pictures as before frame for frame, and the USB pad decoder's 82 checks; for build 19, every page in five themes and the settings page in all thirty-one. Earlier builds' PC checks are in their notes.

## Known issues

- The new interface has been looked at on my console; how smooth it runs there with every theme (the frosted-glass ones blur the page each frame), how the new sounds feel and the swan's moves have not been measured one by one.
- The pages other than the library keep their layouts, in the kit's themed panels; they are not yet built from the kit's own components.
- Not every feature new since build 15 has been tried on a console one by one. Sleep-safe saving asks the console whether its menu is open over the title, a call no build has made before: if PSSwanStation stops when a game starts, switch it off. The web panel is the first time the title listens on the network for anything but netplay. Compiling a shader preset on the console may take a while and some are too slow at 4K.
- Closing the gaps draws 3D objects' outlines a quarter of a pixel fuller; a polygon edge can still show a single texel of its texture's neighbour where the PlayStation itself shows one.
- A cut detected in a fade shows the fade without made pictures between its steps (nothing moves in a fade, so nothing is lost).
- Frame generation: a thin fringe where something moves across a background that moves another way; at 20 pictures a second the made ones outnumber the game's and errors show most.
- A recording plays back the same only with the same settings, cheats and build.
- A card changed on two consoles before they met keeps the newer one without asking (the other is kept beside it as `.conflict-<date>`). An FTP server can hold games but not your files.
- Should the screen stay dark after choosing 120 Hz, start PSSwanStation holding L1 and R1, wait ten seconds, close it and start it again.
- Run-ahead at 60 Hz has no refresh between two frames to do its rollback in, so a press there still costs two frames' work in one refresh.
- USB controllers have not been seen to work on a console yet.
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

This ZIP was built from PSSwanStation commit `{{COMMIT}}`, which is this release's tag. The title is distributed under the GNU General Public License, version 3, as a whole. The source of every part at the revision it was built from is attached (`SOURCES.txt` lists the archives, `SHA256SUMS` their digests); `licenses/` in the title's folder holds each part's licence and `licenses/components.json` the revisions. The RADV driver and PS5_Vulkan revisions of this build are not on GitHub: their attached archives are their published source, and `SOURCES.txt` says which GitHub commit each is built on.

## Build

| Component | Version |
| --- | --- |
| PSSwanStation | `{{COMMIT}}` (SwanStation at libretro `dd48056`, plus the `ps5` folder) |
| PS5_Vulkan (link recipe, libc.prx) | `7c49954b` (GitHub's `252bf91` and the RADV pin below; in the attached archive) |
| PS5_PayloadSDK fork (platform layer) | `611893fc25ef` |
| RADV, from PS5_Mesa | `08ba1939678b`: GitHub's `47e9eb61ee7` and my 120 Hz fix, in the attached archive (built against the SDK fork at `b83202be73e9`) |
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

SHA-256 of `{{ZIP}}`: `{{ZIP_SHA256}}`

The interface kit's fonts (Inter, Montserrat, Press Start 2P, Patrick Hand, DejaVu Sans Mono), sounds and songs are its own, under the licences in `licenses/`.

PSSwanStation is an unofficial port. It is not affiliated with or endorsed by Sony Interactive Entertainment, the libretro team or the DuckStation project. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment. Vulkan is a registered trademark of the Khronos Group; the RADV port in this title is not a conformant product.
