# PSSwanStation build 28

## New in build 28

### Added

- **Settings redesigned as one board**: Console, Graphics, Display, Audio, Controllers, Library, Saving, Interface, Network and System, each a tile with a light for every setting, lit when it is not at its default. Nothing is hidden behind another page. Inside an area the sections follow the path of the picture, the sound or a press (L2/R2 between sections, L1/R1 between areas), and beside the list each setting shows what it does, its current and default value, whether it applies at once or after a restart, and whether it is SwanStation's or PSSwanStation's. Over a running game, Graphics and Display leave a window on the game that shows each change live.
- **SwanStation's options in SwanStation's own words**, word for word; PSSwanStation's own settings named and described the same way.
- **Search** (Triangle in Settings), **Undo** (L3) and **Quick Settings**: R3 pins a setting, and the menu over a game shows the pinned ones beside its actions (R1).
- **Folders of your choice** (Settings, Library, Folders): game folders anywhere on the console's storage or a USB drive, and a folder of your own for the BIOS, memory cards, save states, cheats, covers and texture packs, picked with a folder browser.
- **Covers by serial**: a game the libretro thumbnails collection has no box art for is looked up in xlenore's psx-covers collection by its disc's serial.

### Changed

- **The menu over a game** keeps the game in view, with its newest save states' pictures and the quick settings.
- **A game's details** show its save states and how many settings are its own; its Game Settings follow the board.
- **GPU On Separate Core** (Settings, Graphics, Renderer; on): the graphics driver's thread and the software renderer's each run on a processor core of their own.
- **The touch pad**: Select or Start is the half of the finger that touched last, so a thumb resting on the other half no longer decides it.
- Newer libsmb2 and libnfs.

### Fixed

- **The console could still go down after PSSwanStation was closed a number of times**: a Lapy service already running on the console was asked first, and an older one leaves the sandbox without counting the console root's references. PSSwanStation now leaves its sandbox only with its own helper, as Crux2 and OGBox do.
- **An update made from inside PSSwanStation did not replace its sandbox helper**, so build 27's fix only reached consoles where the folder was copied over by hand. The updater now replaces it.

Your settings, saves, states and games are kept when this build is installed over an earlier one. USB drives, the disc drive and files kept outside the title's folder now need the ELF loader running on port 9021 (a Lapy service is no longer used).

## Install or update

1. Copy the `PPSA99248` folder from the ZIP to `/data/homebrew/` on the console, over the old one, or update from the menu's Update.
2. Your own games in `games/` (or on a USB drive, a network share, or a folder you add), your own BIOS in `bios/`. None included, none ever will be.
3. Read `LEGAL.txt` and `README.txt` in the folder.

## Reporting problems

Open an issue with `psswanstation-boot.log` from the title folder (and `.1.log`, `.2.log`), copied before PSSwanStation is started again. With Log Mode set to "Only when something goes wrong", choose "Write Log Now" (Settings, System, Logging) right after the problem. Never attach games, BIOS files, firmware or keys.

## Source

This ZIP was built from PSSwanStation commit `{{COMMIT}}`, this release's tag. The title is distributed under the GNU General Public License, version 3, as a whole, with the additional terms in `licenses/ADDITIONAL-TERMS.txt`: keep the credit to Press5elect. The source of every part is public at the revision it was built from; our commits on PS5_Mesa, PS5_Vulkan and PS5_VKHomebrewUI are in [`ps5/patches`](https://github.com/Press5elect/PSSwanStation/tree/{{COMMIT}}/ps5/patches):

{{SOURCES}}

`licenses/` in the title's folder holds each part's licence, and `licenses/components.json` the same revisions.

SHA-256 of `{{ZIP}}`: `{{ZIP_SHA256}}`

No games, BIOS files, console firmware or keys are included, now or ever: use games you own, from your own discs.

## Build

Component | Version
-- | --
PSSwanStation | build 28
RADV | PS5_Mesa `0f86253f674c` with `ps5/patches/PS5_Mesa` (Crux2's patches, OGBox's HDR10): `ead4d0f93427`
PS5_Vulkan | `7f492b4c6f67` with `ps5/patches/PS5_Vulkan`: `f274af151e40`
PS5_PayloadSDK | `3345cde` (upstream's v0.45)
PS5_VKHomebrewUI | `2188642afbcc` with `ps5/patches/PS5_VKHomebrewUI`: `b05fe83e74f4`
libsmb2 | `161b89e`
libnfs | `e5a697b`
FFmpeg | n9.0.2
mbedTLS | v3.6.7
