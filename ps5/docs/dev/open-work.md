# Open work

What is unfinished, untested on the console, or decided. Update it in the same commit as the
change; when a test build confirms something, move it from the first list to the record below.

## Untested on the console (as of build 27 test 2)

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
- Safe start's "Stay in the sandbox" switching USB controllers off too (409f5d2, after test 2:
  not in its ZIP).
- Waiting for a network share at a game's start (boot log: `does not answer: the game waits for
  it, asking each second` … `answered after N s`), and the shares woken at start (`share: … woken:
  N KiB of …`). Checked on the PC over FTP only: SMB and NFS go through the same paths (`stat`,
  `probe`, `status`) but no server of either kind was there. A 5 s probe of a server that is off,
  not seen.

## Known limits, by how the emulator is built

- **A share that stalls during play**: the panel ("Waiting for the network share (N s)",
  `drawGameOverlay`) shows only while the read that waits is the read-ahead's. The core runs on
  the interface's thread, and a sector the read-ahead doesn't have yet blocks it in
  `CDROM::WaitForReadToComplete` (`src/core/cdrom.cpp`): no frame, no panel, until the share
  answers. With "Load network games into memory" (the default) a game reads nothing from the
  share while it plays, except streamed games and a multi-disc game's later discs. A way out:
  let the emulated drive wait for a slow network read as `CDROM::WaitForSlowMedia` does for USB
  drives; it changes the emulation's timing, so it needs a test build of its own.

## Ideas not taken up yet

Offered to the owner and not asked for, or found in OGBox and not ported:

- GPU timing of the presenter's passes in the boot log (OGBox 2ae7417, 20600bb).
- Dual Analog joystick mode; core pinning; Mesa's single-file/database shader cache (needs timing
  on the console).
- From OGBox: a radial stick dead zone per stick (88d9998; PSSwanStation's is per axis), the
  in-memory log written after 15 s without a frame and keeping the session's first 512 KiB
  (f521b35), quitting that waits for a save sync with a bar and stops it after the current file
  (5fa1abd), rumble strength 0-100% (05f29b3), Debanding (8723618), Lanczos-2 down-sampling for
  pictures larger than the screen (96e7ec4), a USB drive recommended for data with the console's
  write slowdown explained (ce39c0d).

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

## Waiting for the owner

- The pushed history of `ps5-port` (69 commits up to build 26) still carries `Claude-Session:`
  trailers. Removing them means rewriting published commits and a force-push: only on his word.
- mbedTLS is on 3.6.7 (the 3.6 LTS line); 4.x exists and has not been tried.
