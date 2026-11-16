# Open work

What is unfinished, untested on the console, or decided. Update it in the same commit as the
change; when a test build confirms something, move it from the first list to the record below.

## Untested on the console (as of build 27)

From build 27 test 1:

- The boot log's `smb: the connection's TCP buffers: … KiB`, and the network load speed with the
  large buffers.
- Network shares finding the owner's NAS (the scan needs non-blocking sockets; the page says so
  when they don't work).
- Logs kept in memory only, and logs on a USB drive.
- Save states on a USB drive.
- Settings surviving a close from the PS menu (they are written when that menu opens).

From build 27 test 2:

- The sandbox elevator's traded root references (boot log: `sandbox: the elevator gave the
  console's root, its references traded`); only runs with USB or disc drives on, when Lapy's way
  shows no drive in `/dev`.
- The payload SDK's v0.45 binaries (3345cde): the title starting at all is the first check.
- PS5_Mesa main 0f86253 (the pipeline-cache fix) under the title.
- HDR output: `display: HDR output on (0; ...)` or the reason it is not; the TV going into HDR
  only when HDR is on; Adjust HDR brightness; screenshots while it is on.
- The pad read through `scePadRead` with the tap latch (boot log: no `scePadRead refused`).
- Sound output's failed calls timed by the clock (boot log: `sceAudioOutOutput failed` only if
  it happens).
- The stick dead zone round the stick's middle, and Vibration strength (9d43cd0): how analog
  games feel at the usual 10%; a game's vibration at 50%.
- Safe start's "Stay in the sandbox" switching USB controllers off too (6ebe6f5, after test 2:
  not in its ZIP).
- Waiting for a network share at a game's start (boot log: `does not answer: the game waits for
  it, asking each second` … `answered after N s`), and the shares woken at start (`share: … woken:
  N KiB of …`). Checked on the PC over FTP only: SMB and NFS go through the same paths (`stat`,
  `probe`, `status`) but no server of either kind was there. A 5 s probe of a server that is off,
  not seen.
- Closing while the files are being brought up to date (from test 3): the bar, then
  the boot log's `close: waiting for the files' sync first` … `files: Stopped as PSSwanStation
  closed: …` … `close: the files' sync stopped N s after the close was asked; closing`, and the
  next start's sync bringing the rest. Checked on the PC against an SMB share (impacket's server)
  only; not with NFS or a cloud storage.
- Logs kept in memory keeping the session's first 512 KiB and its last 8 MiB, and the watchdog
  writing them after 15 s without a frame in a game (from test 3; boot log:
  `watchdog: no frame for 15 s while a game runs`, then `[log: the lines kept in memory,
  written: …]`). Not known yet: whether the console's menu over a game, or the title in the
  background, stops the title's frames (the watchdog does not count that time), and whether the
  clock runs on while the console suspends the title (a second of the watchdog's that took more
  than five starts its count again).

From build 27 test 3, released in build 27 as an experiment; the owner has not tried it with
the gun, and waits for users' reports (an issue with the boot log's `sinden:` lines):

- The Sinden Lightgun read by the console itself (its setting, or `data/sinden.txt`; the boot
  log's `sinden:` lines, [`sinden-lightgun.md`](sinden-lightgun.md), "The console side"): whether
  libSceUsbd loads and lists the gun, whether ugen's nodes are there, the gun's descriptors, the
  serial port's answers, the buttons' reports, and whether the camera streams either way (its
  rate, or the counts that say where it stops). Checked on the PC against a simulated gun only.
  Then the aim: `sinden: aim:` lines (the screen found, the quality, the border's level, the
  tracker's time), the GunCon following the gun in a game, and its buttons. The tracker was
  measured on drawn frames only (sinden-lightgun.md, "The tracker").

After build 27 (not in a test build yet):

- "GPU on a separate core", on by default (`tuning.cpp`): boot log `tuning: the title's threads, but
  the GPU's, on processors 0x1fc3 (0)`, `tuning: the graphics driver's recording thread on
  processors 0xc (0)`, and with the software renderer `the software renderer's thread on processors
  0x30 (0)`; a result other than 0 is a refusal. To compare with "Measure the game in the boot log":
  the same scene with it on and off (`test: window` lines). Threaded recording itself was never
  reported on from the console either: this turns it on for everyone, so it is the first thing to
  check, with both renderers.

## Known limits, by how the emulator is built

- **A share that stalls during play**: the panel ("Waiting for the network share (N s)",
  `drawGameOverlay`) shows only while the read that waits is the read-ahead's. The core runs on
  the interface's thread, and a sector the read-ahead doesn't have yet blocks it in
  `CDROM::WaitForReadToComplete` (`src/core/cdrom.cpp`): no frame, no panel, until the share
  answers. With "Load network games into memory" (the default) a game reads nothing from the
  share while it plays, except streamed games and a multi-disc game's later discs. A way out:
  let the emulated drive wait for a slow network read as `CDROM::WaitForSlowMedia` does for USB
  drives; it changes the emulation's timing, so it needs a test build of its own. With the logs
  kept in memory, the watchdog (`diag.cpp`) writes the log after 15 s of it.

## Ideas not taken up yet

Offered to the owner and not asked for, or found in OGBox and not ported:

- GPU timing of the presenter's passes in the boot log (OGBox 2ae7417, 20600bb).
- Dual Analog joystick mode; core pinning; Mesa's single-file/database shader cache (needs timing
  on the console).
- From OGBox, not ported yet:
  - Debanding (8723618): its thresholds are for the Xbox's 16- and 32-bit colour (2 and 5 levels
    of 255); the PlayStation's 5-bit channels step by about 8 levels, so the values need finding
    again on test gradients, and the pass belongs in `look_signal.frag` (the picture at its own
    size, before scaling).
  - Lanczos-2 down-sampling for pictures larger than the screen (96e7ec4; `look_down.frag` is an
    even average now), as a choice.
  - A dead zone set for each stick and the triggers (88d9998): the one setting is round now (build
    27), for both sticks.
  - The rumble strength's test buzz (05f29b3): the strength is there (build 27), without it.

## Decisions on record

- **HDR's param.json attribute (0x20000000) is declared only while HDR is on**, from the next
  start, so the TV is not put into HDR for anyone who leaves it off (OGBox declares it always).
  `attribute3` is the display mode's (0x80040 for 120 Hz), unrelated to HDR.
- **The sandbox elevator's protocol stays version 1**: `lapy.elf` speaks it. Its "applied the old
  way" status is 12, because the title's own timeout is 11 (never sent).
- **The old "destroyed buffer waits" Mesa patch is not carried**: PS5_Mesa main's fix (f5610fe)
  covers it.
- **No pipeline cache file of the emulator's** (`SWANSTATION_NO_PIPELINE_FILE`): RADV's shader
  cache is the only one, sparing the console's storage.
- **No session links in commits**: from build 27 on, the commits carry only the Co-Authored-By
  trailer (the unpushed build 27 commits were rewritten on the owner's word).
- **A close waits for the file the sync is copying; Circle does not cut it short** (OGBox lets
  Circle leave only its cache drives' last bytes, which the game copies again). On an SMB share a
  file is replaced by removing the old one and renaming the new one into place, and a folder's
  lists (the share's `.psswanstation-sync`, `data/sync/<name>.state`) are written when it stops:
  leaving in between could leave the share without the file, which another console then takes
  for a removal. A share that stops answering in the middle of a file is waited for until the
  request times out (`RequestSeconds`, 60 s); not tried.

## Waiting for the owner

- The pushed history of `ps5-port` (69 commits up to build 26) still carries `Claude-Session:`
  trailers. Removing them means rewriting published commits and a force-push: only on his word.
- mbedTLS is on 3.6.7 (the 3.6 LTS line); 4.x exists and has not been tried.
