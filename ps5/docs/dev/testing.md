# Testing on the PC

What can be checked without the console, and how. Every change is checked here first; what
needs the console is listed in [`open-work.md`](open-work.md) until a test build confirms it.

## The host build

The front end and the emulator for this PC, without a window or a pad (Vulkan's headless
surface; lavapipe is enough):

```bash
apt-get install mesa-vulkan-drivers
cmake -S ps5 -B build-host -G Ninja && cmake --build build-host --target swanstation
```

It must build without new warnings in the files a change touches. The platform code in
`ps5/src/ps5/` (pad, sound, the sandbox elevator, VideoOut hooks) is only built by the console
build: compile it with `ps5/tools/build.sh`.

## The screenshot run

```bash
JOBS=2 bash ps5/tools/host-test.sh        # needs python3's pycdlib
```

It makes test discs (`ps5/tools/make-test-disc.py`, no game needed) and runs the title with a
script of button presses, saving screenshots into `build-host/test-root`: the start-up
animation, the library, a game's details, its options and cheats, a game running, the menu, a
saved state, back to the library. A crash fails it. Look at the screenshots after a change to the
menus; extend the script when a change adds a page.

## The tests in `ps5/tests`

Each folder has a `run.sh`; most build a small test program against the front end's code.

| Test | Checks |
| --- | --- |
| `achievements` | The achievements module (`run.sh [plain|asan|tsan|slow]`, plain, asan and tsan by default) |
| `clock` | The title's clock from the processor's counter (`src/ps5/clock_ps5.c`) |
| `discdrive` | The USB disc drive on a PC: the three ways to play a disc |
| `esde` | The ES-DE theme reader (`esde.cpp`), with AddressSanitizer and UndefinedBehaviorSanitizer |
| `gpu` | `gpu_scenes.c` drawn by the software and the hardware renderer, video memory compared cell by cell |
| `hdr` | HDR output: `hdr.frag` and the 10-bit copy on lavapipe, every pixel against the maths in double precision (needs libvulkan-dev, vulkan-validationlayers) |
| `library` | The library's disc grouping (`src/library_names.h`) |
| `memcard` | Memory cards |
| `netplay` | Sessions between two copies of a test program on 127.0.0.1, their outputs compared |
| `pacing` | The pacing wait (`src/pacing_wait.h`) |
| `tls` | The title's own HTTPS client (`src/tls_client.cpp`) |
| `tuning` | The console tuning (`src/tuning.h`) |
| `update` | The updater alone against a local stand-in for GitHub's releases (`run.sh plain`) |

Switches for a run by hand:

- `SWANSTATION_SHARES_TEST=1`: the share form's file (`network.cfg`) written, read back and changed, and the servers on the network looked for; it prints `shares-test:` lines and exits.
- `SWANSTATION_SIZE=3840x2160`, `SWANSTATION_GPU=<name>`, `SWANSTATION_REFRESH=<Hz>`.
- A test ROM as BIOS: build `tests/guest/pad_poll.c` (`crt0.S`, `rom.ld`) and make it a ROM with
  `tests/guest/mkrom.py` into `bios/`, so scripted games run without a console's BIOS.
- `SWANSTATION_FAKE_DRIVE`, `SWANSTATION_FAKE_SANDBOX`: the disc drive's code without a drive.

## What the PC cannot show

The console's driver (RADV on the PS5's GPU), VideoOut (120 Hz, VRR, HDR), the pad and sound
services, the sandbox and its elevator, SMB/NFS against the owner's NAS, and the console's
storage. A change there is "Not run on the console" until a test build says otherwise; give the
owner the exact boot log lines to look for.
