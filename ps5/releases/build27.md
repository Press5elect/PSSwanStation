# PSSwanStation build 27

## New in build 27

### Added

- **HDR** (Settings, Picture; off): the picture goes to an HDR TV as HDR10, white at the HDR white level (203 nits at first) and the game's brightest parts raised toward the TV's peak (HDR highlights). **Adjust HDR brightness** finds both with two test pictures. It begins at the next start, and the TV is asked for HDR only while it is on. Needs the console's own HDR on and a TV that takes it.
- **Network shares from the console** (Settings, Games and saving, Network shares): add, change and remove the folders with games, the account and the folder for your files, without editing `network.cfg` over FTP. The SMB servers on the network are found for you, and a change is used at once.
- **A game on a share that does not answer yet waits for it** ("Waiting for the network share", Circle goes back) instead of failing, and the shares are woken as PSSwanStation starts, so a NAS's disks are turning by the time a game starts.
- **Logs** (Settings, System): written as they come, only when something goes wrong, or off; in the title's folder or on a USB drive; the boot log alone, with the emulator's log, or everything. Kept in memory, they keep the session's start too, and a game that stands still for 15 s writes them by itself.
- **Save states on a USB drive** (Settings, Games and saving).
- **Vibration strength** (Settings, Controls).
- **The Sinden Lightgun, an experiment** (Settings, Controls, Players): it aims a player's GunCon. PSSwanStation reads the gun's camera over USB and finds the white border it draws round the game's picture itself, without Sinden's software. **Not tested with a gun yet**: if you have one, please try it and open an issue with the boot log's `sinden:` lines.

### Changed

- **Fewer writes to the console's storage**: settings written when PSSwanStation closes and when the console's menu opens, files written only when they changed, one shader cache, an update replacing only the files that changed, and the sleep-safe resume state saved only when the console's menu opens.
- **Network games read faster**: large TCP buffers for every SMB, NFS and FTP connection.
- **Closing while your files are being brought up to date** finishes the file it is copying, with a bar, and closes; the rest follows at the next start.
- **The stick dead zone** is taken round the stick's middle: small diagonals stay diagonals.
- **The graphics driver** is PS5_Mesa main of 10 November 2026 with Crux2's patches: a cached pipeline that lost a stage is compiled again instead of crashing at every start. Newer payload SDK (upstream's v0.45), libsmb2 and libnfs.
- Every cover of the library is fetched in the background with "Download covers" on.

### Fixed

- **The console could go down after PSSwanStation was closed a number of times with USB or disc drives on**: the sandbox elevator now keeps the console root's reference counts right (as Crux2 and OGBox do).
- **Quick taps on the controller** that never reached the game.
- The sound running away when the console's sound output refused a block.
- The game database missing after the libretro database moved its lists.
- Safe start's "Stay in the sandbox" now also switches USB controllers off.

Your settings, saves, states and games are kept when this build is installed over an earlier one.

## Install or update

1. Copy the `PPSA99248` folder from the ZIP to `/data/homebrew/` on the console, over the old one, or update from the menu's Update.
2. Your own games in `games/` (or on a USB drive or a network share), your own BIOS in `bios/`. None included, none ever will be.
3. Read `LEGAL.txt` and `README.txt` in the folder.

## Reporting problems

Open an issue with `psswanstation-boot.log` from the title folder (and `.1.log`, `.2.log`), copied before PSSwanStation is started again. With Logs set to "only when something goes wrong", choose "Save the log now" right after the problem. Never attach games, BIOS files, firmware or keys.

## Source

This ZIP was built from PSSwanStation commit `{{COMMIT}}`, this release's tag. The title is distributed under the GNU General Public License, version 3, as a whole, with the additional terms in `licenses/ADDITIONAL-TERMS.txt`: keep the credit to Press5elect. The source of every part is public at the revision it was built from; our commits on PS5_Mesa, PS5_Vulkan and PS5_VKHomebrewUI are in [`ps5/patches`](https://github.com/Press5elect/PSSwanStation/tree/{{COMMIT}}/ps5/patches):

{{SOURCES}}

`licenses/` in the title's folder holds each part's licence, and `licenses/components.json` the same revisions.

SHA-256 of `{{ZIP}}`: `{{ZIP_SHA256}}`

No games, BIOS files, console firmware or keys are included, now or ever: use games you own, from your own discs.

## Build

Component | Version
-- | --
PSSwanStation | build 27
RADV | PS5_Mesa `0f86253f674c` with `ps5/patches/PS5_Mesa` (Crux2's patches, OGBox's HDR10): `ead4d0f93427`
PS5_Vulkan | `7f492b4c6f67` with `ps5/patches/PS5_Vulkan`: `f274af151e40`
PS5_PayloadSDK | `3345cde` (upstream's v0.45)
PS5_VKHomebrewUI | `2188642afbcc` with `ps5/patches/PS5_VKHomebrewUI`: `b05fe83e74f4`
libsmb2 | `177a9de`
libnfs | `331d391`
FFmpeg | n9.0.2
mbedTLS | v3.6.7
