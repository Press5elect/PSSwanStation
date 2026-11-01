# PSSwanStation - SwanStation as a PS5 homebrew title

This fork is SwanStation with a PS5 frontend of its own in [`ps5/`](ps5/): a native title (`PPSA99248`) for a jailbroken PS5, drawing with Vulkan through the RADV driver, with a library for the controller, every setting of the emulator, save states, cheats, RetroAchievements and more. The work is on the branch `ps5-port`.

- **Latest release: [build 26](https://github.com/Press5elect/PSSwanStation/releases/latest)** (the ZIP to copy to the console and its SHA-256; the release names the public revision of every part's source)
- The manual, every screen and setting: [`ps5/MANUAL.md`](ps5/MANUAL.md)
- What it does and how to use it: [`ps5/README.txt`](ps5/README.txt)
- What changed in each build: [`ps5/CHANGELOG.txt`](ps5/CHANGELOG.txt)
- How it is built: the head of [`ps5/tools/build.sh`](ps5/tools/build.sh); a release is made by [`ps5/tools/release.py`](ps5/tools/release.py)
- What leaves the console and what is kept: [`ps5/PRIVACY.md`](ps5/PRIVACY.md)
- Licences of every part: [`ps5/licenses/README.txt`](ps5/licenses/README.txt)

### Build 26

#### Added

- **The renderer changes while a game runs.** Software and Vulkan, both ways: the game goes on where it was, with no restart. The picture presets that change the renderer apply at once too.
- **Multi-disc games as one** (Settings, Interface, View; on): the discs of a game are one entry, sharing one memory card. Discs are now also found when each is in a folder of its own named after the disc (`Game (Disc 1)/...`), in folders named only `CD1`, `CD2` (`Game/CD1/...`), and by "CD2" or "Disc 2" at the end of a name. Off: each disc is an entry of its own.
- **Variable refresh, really asked for**: "120 Hz, variable refresh rate" (Display output) now asks the output to leave its fixed rate, so on a TV with VRR each frame is shown as soon as it is ready, where the console's firmware offers it (the boot log says whether it does).

#### Changed

- **The graphics driver is Mesa main of 31 October 2026** (27.0.0-devel, as Crux2 1.0.17 builds it): fixes to the shader compiler and to how graphics pipelines are found in the shader cache.
- Newer parts: libsmb2 (c6e4997) and libnfs (affe4c8).

#### Fixed

- **120 Hz fell back to 60 Hz on TVs with VRR**: before the first frame such a TV refreshes at 48 Hz, which was taken for a TV still at 60 Hz.
- **Cover downloads could stop for the rest of a run** when a cover downloaded but could not be saved (a full or read-only drive); the boot log now says why it was not saved (bobrass, pull request #5).

**Author and credit.** The PS5 title (everything in `ps5/` and the changes to SwanStation for it) is Copyright (C) 2026 [Press5elect](https://github.com/Press5elect), under the GNU General Public License, version 3 or later, with one additional term under its section 7(b): anyone who passes on this work or a work based on it, in source or binary form, must keep its author's attributions and credit "PSSwanStation by Press5elect" wherever the work shows its credits. That holds however a copy or an adaptation is made, AI tools included. The term is in [`ps5/licenses/ADDITIONAL-TERMS.txt`](ps5/licenses/ADDITIONAL-TERMS.txt). SwanStation and the libraries keep their own licences and authors.

> **Piracy is not condoned.** Neither this repository nor any release of it contains games, BIOS files, console firmware or decryption keys, and none will ever be provided or linked to. Use only games you own, from your own discs, and BIOS files dumped from hardware you own.

A work in progress: not every part has been checked on a console one by one, and each release says which has.

PSSwanStation is an unofficial port. It is not affiliated with or endorsed by Sony Interactive Entertainment, the libretro team or the DuckStation project. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment. Vulkan is a registered trademark of the Khronos Group; the RADV port in this title is not a conformant product.

What follows is SwanStation's own README.

---

# SwanStation - PlayStation 1, aka. PSX Emulator
[Features](#features) | [System Requirements](#system-requirements) | [Disclaimers](#disclaimers)

SwanStation is a hard fork and open-source Libretro core implementation of DuckStation, which is an emulator of the Sony PlayStation(TM) console, focusing on playability, speed, and long-term maintainability. The goal is to be as accurate as possible while maintaining performance suitable for low-end devices. "Hack" options are discouraged, the default configuration should support all playable games with only some of the enhancements having compatibility issues.

A "BIOS" ROM image can be used, but is not required. You can use an image from any hardware version or region, although mismatching game regions and BIOS regions may have compatibility issues. A ROM image is not provided with the emulator for legal reasons, you should dump this from your own console using Caetla or other means. If no ROM image is provided, [OpenBIOS](https://pcsx-redux.consoledev.net/openbios/) will be used instead.

## Features

SwanStation features include:

 - CPU Recompiler/JIT (x86-64, armv7/AArch32 and AArch64)
 - Hardware (D3D11, OpenGL, Vulkan) and software rendering
 - Upscaling, texture filtering, and true colour (24-bit) in hardware renderers
 - PGXP for geometry precision, texture correction, and depth buffer emulation
 - Adaptive downsampling filter
 - Post processing shader chains
 - "Fast boot" for skipping BIOS splash/intro
 - Save state support
 - Supports bin/cue images, raw bin/img files, MAME CHD, single-track ECM, MDS/MDF, and unencrypted PBP formats.
 - Direct booting of homebrew executables
 - Direct loading of Portable Sound Format (psf) files
 - Digital and analog controllers for input (rumble is forwarded to host)
 - Namco GunCon lightgun support (simulated with mouse)
 - NeGcon support
 - Emulated CPU overclocking
 - Multitap controllers (up to 8 devices)
 - RetroAchievements
 - Automatic loading/applying of PPF patches

## System Requirements
 - A CPU faster than a potato. But it needs to be x86_64, AArch32/armv7, or AArch64/ARMv8, otherwise you won't get a recompiler and it'll be slow.
 - For the hardware renderers, a GPU capable of OpenGL 3.1/OpenGL ES 3.0/Direct3D 11 Feature Level 10.0 (or Vulkan 1.0) and above. So, basically anything made in 2013 or later.

### Region detection and BIOS images
By default, SwanStation will emulate the region check present in the CD-ROM controller of the console. This means that when the region of the console does not match the disc, it will refuse to boot, giving a "Please insert PlayStation CD-ROM" message. SwanStation supports automatic detection disc regions, and if you set the console region to auto-detect as well, this should never be a problem.

The region checking can be disabled in the console options tab. This is the only way to play unlicensed games or homebrew which does not supply a correct region string on the disc, aside from using fastboot which skips the check entirely.

Mismatching the disc and console regions with the check disabled is supported, but may break games if they are patching the BIOS and expecting specific content.

### LibCrypt protection and SBI files

A number of PAL region games use LibCrypt protection, requiring additional CD subchannel information to run properly. libcrypt not functioning usually manifests as hanging or crashing, but can sometimes affect gameplay too, depending on how the game implemented it.

For these games, make sure that the CD image and its corresponding SBI (.sbi) file have the same name and are placed in the same directory. SwanStation will automatically load the SBI file when it is found next to the CD image.

For example, if your disc image was named `Spyro3.cue`, you would place the SBI file in the same directory, and name it `Spyro3.sbi`.

## Tests
 - Passes amidog's CPU and GTE tests in both interpreter and recompiler modes, partial passing of CPX tests

## Disclaimers

"PlayStation" and "PSX" are registered trademarks of Sony Interactive Entertainment Europe Limited. This project is not affiliated in any way with Sony Interactive Entertainment.
