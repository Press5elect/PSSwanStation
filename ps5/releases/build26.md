# PSSwanStation build 26

## Added

- **The renderer changes while a game runs.** Software and Vulkan, both ways: the game goes on where it was, with no restart. The picture presets that change the renderer apply at once too.
- **Multi-disc games as one** (Settings, Interface, View; on): the discs of a game are one entry, sharing one memory card. Discs are now also found when each is in a folder of its own named after the disc (`Game (Disc 1)/...`), in folders named only `CD1`, `CD2` (`Game/CD1/...`), and by "CD2" or "Disc 2" at the end of a name. Off: each disc is an entry of its own.
- **Variable refresh, really asked for**: "120 Hz, variable refresh rate" (Display output) now asks the output to leave its fixed rate, so on a TV with VRR each frame is shown as soon as it is ready, where the console's firmware offers it (the boot log says whether it does).

## Changed

- **The graphics driver is Mesa main of 31 October 2026** (27.0.0-devel, as Crux2 1.0.17 builds it): fixes to the shader compiler and to how graphics pipelines are found in the shader cache.
- Newer parts: libsmb2 (c6e4997) and libnfs (affe4c8).

## Fixed

- **120 Hz fell back to 60 Hz on TVs with VRR**: before the first frame such a TV refreshes at 48 Hz, which was taken for a TV still at 60 Hz.
- **Cover downloads could stop for the rest of a run** when a cover downloaded but could not be saved (a full or read-only drive); the boot log now says why it was not saved (bobrass, pull request #5).

## Source

This ZIP was built from PSSwanStation commit `{{COMMIT}}`, this release's tag. The title is distributed under the GNU General Public License, version 3, as a whole; the source of every part is public at the revision it was built from:

{{SOURCES}}

`licenses/` in the title's folder holds each part's licence, and `licenses/components.json` the same revisions.

SHA-256 of `{{ZIP}}`: `{{ZIP_SHA256}}`

No games, BIOS files, console firmware or keys are included, now or ever: use games you own, from your own discs.
