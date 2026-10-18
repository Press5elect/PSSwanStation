# PSSwanStation - SwanStation as a PS5 homebrew title

This fork is SwanStation with a PS5 frontend of its own in [`ps5/`](ps5/): a native title (`PPSA99248`) for a jailbroken PS5, drawing with Vulkan through the RADV driver, with a library for the controller, every setting of the emulator, save states, cheats, RetroAchievements and more. The work is on the branch `ps5-port`.

- **Latest release: [build 24](https://github.com/Press5elect/PSSwanStation/releases/latest)** (the ZIP to copy to the console, with the source of every part)
- The manual, every screen and setting: [`ps5/MANUAL.md`](ps5/MANUAL.md)
- What it does and how to use it: [`ps5/README.txt`](ps5/README.txt)
- What changed in each build: [`ps5/CHANGELOG.txt`](ps5/CHANGELOG.txt)
- How it is built: the head of [`ps5/tools/build.sh`](ps5/tools/build.sh); a release is made by [`ps5/tools/release.py`](ps5/tools/release.py)
- What leaves the console and what is kept: [`ps5/PRIVACY.md`](ps5/PRIVACY.md)
- Licences of every part: [`ps5/licenses/README.txt`](ps5/licenses/README.txt)

### Build 24

#### Added

- **HD texture packs for 3D games too.** A pack picture replaces one block of a texture page as the PlayStation's texture cache holds it (64x64 texels of a 4-bit texture, 32x64 of an 8-bit one, 32x32 of a 15-bit one). Pictures are named `block-<4|8|15>-<hash>.png` and go in `textures/<serial>/`; they can be up to 256x256 and are drawn smoothly with every texture filter. Vulkan renderer; Settings, Emulator, Enhancement, "HD Texture Packs" (on).
- **Write Out Texture Blocks** (Settings, Emulator, Enhancement): every block a game shows is saved once to `textures/<serial>/dump/`, already named, to make packs from.
- **Accurate blending** (on): semi-transparency and the mask bit worked out the way the PlayStation does them, at 5 bits a channel, in drawing order. Not used with MSAA or the PGXP depth buffer.
- **Exact lines** (on): lines drawn pixel for pixel as the PlayStation steps them, at every resolution.
- **OPTIONS can be remapped.** Settings, Controls, Buttons offers OPTIONS for any PlayStation button, beside the touch pad's halves. "The menu opens with" gives the menu and the shortcuts' key to OPTIONS, to either half of the touch pad, or to OPTIONS held for a second (a tap is then the game's, and there are no shortcuts).

#### Changed

- Every picture preset switches on accurate blending and exact lines.
- The Shortcuts page names the button that opens the menu, as set.
- Newer parts: the RADV driver at PS5 Mesa 14c2f44 (fragment shader barycentrics, with the 120 Hz settling and timed waits kept), and libsmb2 and libnfs at their newest revisions (SMB request sizes kept within the server's limits; NFSv4 leases renewed while mounted).

#### Fixed

- A save state keeps a controller or memory card transfer in progress, the disc that was in (a game on several discs, or no disc with the lid closed) and the drive's lid, so loading it no longer swaps the disc or upsets the controller.
- A damaged or mismatched save state is refused and the console reset, instead of running on in a broken state; states made with an M3U playlist load again.

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
