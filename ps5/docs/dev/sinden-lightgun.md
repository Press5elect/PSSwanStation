# The Sinden Lightgun: what is known, and the ways to support it

Research of 2026-11-15, for supporting the Sinden Lightgun in PSSwanStation's GunCon (and
Justifier) games. Nothing here has been tried with a gun yet.

## How the gun works

- The gun is a camera on a USB composite device: a **UVC camera**, a **USB serial port** and a
  **HID device** (mouse and keyboard, or a joystick in "joystick mode", which firmware 1.09+ and
  the Windows software switch on). USB IDs seen in MrLightgun's MiSTer maps: vendor `16c0`,
  products `0f01`, `0f02`, `0f38`, `0f39`
  ([MiSTerSindenDriver](https://github.com/MrLightgun/MiSTerSindenDriver) `Config/inputs/*.map`).
- **The host does the aiming.** Software on the computer reads the camera, finds the screen's
  border (the game is shown with a plain border, white by default) and works out where the gun
  points; the gun then reports that position with its buttons. Without that software the gun is
  only a camera, a keyboard and a mouse
  ([RetroPie forum](https://retropie.org.uk/forum/post/263220)).
- **That software is Sinden's and closed**: the Windows program, and the Linux/MiSTer
  `LightgunDriver` (an ARM Linux binary built from their Mono program, 19 MB, in the MiSTer
  repository, with no licence file; Sinden's licence reserves all rights and does not allow
  redistribution, per the [RetroPie](https://retropie.org.uk/forum/post/263020) and
  [ARpiCADE](https://arpicade.com/forums/viewtopic.php?p=10387) forums). No open replacement for
  the border tracking was found.
- **The camera** is asked for at 320 x 240, MJPEG first, by the repository's `Tools/camtest.py`
  (V4L2); a camera's video endpoint is usually isochronous.
- **The serial port** (an ATmega32u4, "Leonardo bootloader"): 115200 baud, fixed 7-byte frames
  `AA cmd p1 p2 p3 p4 BB`; commands seen in `Tools/gun_serial.py`: 101 firmware version (2 bytes
  back), 102 the camera's name (15 bytes), 103 write one character of it, 104 and 105 the X and Y
  calibration (2 bytes, big-endian, value − 10000, in hundredths). Recoil has its own commands
  that were not found; Sinden documents recoil through its Windows software (a TCP server since
  2.08a) on its wiki. The answers come back as plain bytes, not in frames; the tool clears what
  came before each question, waits 2 s after opening the port, and shows a calibration over 99
  either way as 0.
- **The camera's name is kept in the gun** (102 reads it; 103 writes one character of it,
  `AA 67 00 <index> 00 <character> BB`), and Sinden's software will not start when no camera of
  that name is found: `gun_serial.py` was written for a camera module swap. So the camera is a
  module of its own, and may be a USB device of its own behind a hub inside the gun rather than
  an interface of the 0x16c0 device; the first test's descriptors say which.
- **The MiSTer driver** has three modes: "low resource" crunches the video only while the trigger
  is held (about 4% of an 800 MHz ARM core), "medium" continuously but not every frame, "normal"
  every frame. The PS5's processors have far more room than that.

## The ways to support it

Every way needs the title to **draw the border** around the game's picture while a Sinden is in
use (a "Lightgun border" picture setting: colour and thickness), and to give the emulated GunCon
an **absolute position** from outside (today it is aimed with the sticks and the motion sensor:
`host.cpp`, `aims`).

1. **A computer beside the console runs Sinden's own driver** (a Raspberry Pi 4 on Linux, or a
   PC), and a small bridge sends the gun's position and buttons to PSSwanStation over the local
   network (UDP). Uses Sinden's tracking as it is; no new hardware beyond that computer; the
   PS5 side is small and fully testable on a PC. The latency is a wired network's (about a
   millisecond).
2. **Sinden's "original console" kit**: a Raspberry Pi 4 runs the driver and an Arduino Micro
   makes the gun look like a Namco GunCon 2 on USB (Sinden's wiki setup guide, as
   [GunCon2-MiSTer](https://github.com/NolanNicholson/GunCon2-MiSTer) uses it). PSSwanStation
   would read a USB GunCon 2 (its reports are documented by the open Linux guncon2 driver) through
   its USB controller layer (`usbpad_ps5.cpp`), and map its coordinates to the game's picture.
3. **On the console alone**: PSSwanStation reads the gun's camera over USB and finds the border
   itself, its buttons from the HID device and recoil over the serial port. No other hardware,
   but the most work and the most unknown: whether a title gets isochronous transfers on the
   PS5 (FreeBSD's `ugen` has them in `USB_FS`; libSceUsbd is libusb-like; neither is proven on a
   console for this, and `usbpad_ps5.cpp`'s own `ugen` path has not run on a console yet), and
   how good a tracker of our own is next to Sinden's (accuracy, the camera's exposure). A first,
   safe step: a test build that only lists the gun's USB descriptors in the boot log (its
   interfaces, endpoints and their kinds), reading nothing else: "The console side" below, with
   `find`.

## The console side

The third way's console part, built in `sinden-usb` and **not run on a console yet**: a thread
of its own that finds the gun, reads its serial port, its buttons and its camera, and hands the
newest picture (brightness), the buttons, the calibration and the gun's state to the rest of the
title through `src/sinden.h`. Nothing tracks the border yet (`src/sinden_track.*` is another
branch's), and nothing aims the GunCon with it. Every step is a `sinden:` line in the boot log,
so that one test run says what the console and the gun did.

- `src/ps5/sinden_ps5.cpp`: the stages, the two ways into the USB, the thread, the hand-over.
- `src/sinden_usb.cpp`: what needs no device (descriptors, the UVC stream, the serial frames,
  the HID reports, libSceUsbd's transfer layout), checked on the PC (`ps5/tests/sinden-usb`).
- `src/sinden_mjpeg.cpp`: MJPEG pictures to brightness with FFmpeg's MJPEG decoder (added to
  `tools/build-ffmpeg.sh` for this).
- `src/host/sinden_host.cpp`: on a PC there is no gun (the state stays `off`).

### Switching it on

Until the gun has its setting: a file `data/sinden.txt` in the title's root (or
`SWANSTATION_SINDEN` in the environment). Its first word: `on` for every stage, `find` for the
first stage only (descriptors, nothing sent to the gun), `usbd` or `ugen` for that way only.
Without the file nothing of this runs. The setting will call `sinden::start()` from
`platform::lateInit` in its place, before `usbPadStart()`: while the gun's thread runs,
`usbpad_ps5.cpp` leaves the gun alone (in its joystick mode it would look like a gamepad).

### The two ways, and the order

1. **libSceUsbd**, the console's libusb-like library, loaded at run time by name
   (`sceKernelLoadStartModule`, each call looked up with `sceKernelDlsym` and said: `found:`,
   `not found:`). Its asynchronous transfers are libusb 1.0's, whose structure is not declared
   for the console and differs between libusb's own and FreeBSD's: a transfer is filled by the
   library's own fill functions with values that cannot be mistaken, and the boot log says where
   each one was found (`the transfer's layout: handle +0, endpoint +9, ...`). If the values are
   not all found, the library's transfers are not used and the filled bytes are logged, for the
   next build. A start that never came back from `sceUsbdInit` is not tried again (the marker
   `data/sinden-usbd-marker.txt`, as usbpad has its own).
2. **The kernel's ugen nodes** (`/dev/ugenB.A`): the device-info ioctl first, the kernel's own copy
   of the descriptors, `USB_DO_REQUEST` on endpoint 0, and FreeBSD's `USB_FS` for the transfers
   (isochronous ones included), each endpoint on a descriptor of its own.

Stage 1 lists the devices both ways. Stages 2 to 4 run one way; when the camera sends no picture
that way (5 s), they run again the other way.

### The stages, and what the boot log says

1. **Find.** `sinden: ugen: /dev's USB nodes: ...` (which kernel drivers made nodes: `uhid`,
   `ums`, `ukbd`, `cuaU`, `video` ...), a line for each device each way (`ugen: /dev/ugen0.3:
   16c0:0f38 class 0, bus 0 address 3, hub at address 2 port 1, speed 3, "..."`, `libSceUsbd:
   device 1: 16c0:0f38 class 239, bus 1 address 5, ...`), then for the gun: `find: ...
   manufacturer, product, serial`, `find: device descriptor: ...`, its configuration descriptor's
   bytes (64 a line) and every descriptor decoded (`find: descriptor: interface 1 alternate 6:
   class 14/2/0 (video streaming)`, `endpoint 0x81: isochronous in (asynchronous), 1024 bytes x 3
   = 3072 a (micro)frame`, `format 2: uncompressed YUY2`, `frame 2: 320x240, intervals 166666
   (60.0) ...`, the CDC union, the HID descriptor). **Look for:** whether the camera is in the
   gun's own configuration (a video streaming interface) or is a device of its own (another
   vendor and product behind the same hub; the camera's name from stage 2 is then matched with
   its product name); which formats and sizes it offers and how fast; which HID interfaces there
   are (boot mouse, keyboard, joystick); whether a 0x16c0 device of another product shows up
   (`not one of the gun's products ... described, nothing more`: say its numbers).
2. **Serial.** Each interface's driver (`serial: interface 2: umodem0 has it; let go: yes`, or
   `no driver has it`), the line coding before and after (`115200 8N1: set`), DTR, then 2 s for
   the gun to settle (as `gun_serial.py` waits) and the four questions:
   `serial: 101 (firmware): sent aa6500000000bb; answer (2 bytes) 0109: firmware 1.9`, `102
   (camera name)`, `104 (calibration X): ... raw 10124, 1.24`, `105`. **Look for:** answers, or
   `no answer in 1.5 s` / `only N bytes back`, or the endpoints `not opened` (the kernel's driver
   kept them).
3. **Buttons.** Each HID interface's report descriptor in bytes and read (`buttons: interface 4
   (subclass 1, protocol 2): mouse buttons 1 to 3 (bits 0 to 2)`), its driver let go, then the
   first four reports and each change after them (the first 100), raw and decoded: `buttons:
   interface 4: report 3 (3 bytes): 020000: mouse 0x2, ...`. **Look for:** a line for each press;
   which button sends what (the mapping to the GunCon's trigger, A and B is made from it).
4. **Camera.** Which device (`the gun's own device has the video interfaces`, `its name is the
   camera's name the gun gave`, `behind the same hub as the gun`), the choice (`chosen: format 2
   frame 2: 320x240 YUY2 at 60.0 a second`; uncompressed is taken before MJPEG at the same size),
   the probe as it was and as the camera answered it (`largest picture 153600 bytes, payload 3000
   bytes`), `committed`, the alternate setting (`alternate 6: ... the smallest of 6 that carries
   3000`), `reading endpoint 0x81: 8 transfers of ...`, the first three packets' headers, then
   `camera: the first picture, 0.01 s after the stream began: 153600 bytes, ...`, and at 5 s and
   30 s after it (then every 10 minutes): `camera: 60.0 pictures a second over 5.0 s (301), ...;
   dropped N (...), packets N (N empty, ...), transfers N (N failed)`. Without a picture in 5 s:
   `camera: no picture in 5 s through libSceUsbd: transfers N (...), packets N, payload bytes N,
   bad headers N, error bits N, frame bit changes N, end-of-frame bits N, dropped N (last: ...)`,
   the bandwidth given back (`back to alternate setting 0`), and the other way. **Look for:** the
   rate, and in a failure which count is 0 (no packets at all; packets but no payload; payload but
   no picture whole).
5. **Hand-over.** No lines of its own: the state (`off`, `no gun`, `found`, `camera streaming`,
   `failed` with why) is what a settings page can show. Pulled out: `the gun's USB device is
   gone` or `the gun was pulled out`, then it is looked for again every 2 s.

MJPEG pictures are decoded on the gun's thread; 30 in a row that do not decode stop the stream
(the first is logged with FFmpeg's words). Twenty transfers in a row that fail stop it too, and
50 empty reads in a row stop the serial port's or a HID interface's reading: nothing is sent
again as fast as it fails.

### What it never does

- Opens a device the kernel or the library has not said what it is first, or reads any device's
  endpoints but the gun's (PS5SX2's lesson, as `usbpad_ps5.cpp` keeps it). Sony's devices, hubs
  and storage are never opened beyond the kernel's device-info ioctl (and that once in 30 s for
  Sony's and hubs). Other devices' configuration descriptors are read only to find the gun's
  camera, from the kernel's copy, or (libSceUsbd) for video and composite devices while a gun is
  there, once.
- Takes an interface from its driver that is not the gun's (or its camera's).
- Writes anything into the gun: command 103 (a character of the camera's name) is never sent; the
  line coding is never 1200 baud (that restarts an ATmega32u4 into its boot loader).
- Keeps bandwidth: the camera's alternate setting goes back to 0 when the stream stops, the gun is
  pulled out or the thread stops.
- Spins: every wait has a timeout, and a wait that comes back early with nothing done sleeps the
  rest of it.

### The first test

`data/sinden.txt` with `on`; the gun plugged in before the title starts; 20 s in the library;
then each of the gun's buttons pressed once, in a known order, a second apart, the gun pointed
at the screen; then Settings, System, Logs, "Save the log now", and the boot log sent. If the
title or the console stops answering, the next try with `find` says how far the first stage
gets. The `sinden:` lines are all that is needed (`grep sinden: psswanstation-boot.log`).

## The title's side (build 27, after test 2)

- **The setting**: Settings, Controls, Players: "Sinden Lightgun" (Off, player 1's or player 2's
  GunCon; that player's controller must be the GunCon) and its "Border" (1-8% of the screen's
  height, 3% at first). Marked as an experiment that does not work yet until the camera side is
  in.
- **The border** (`ui.cpp`, `drawGame`): while it is on and that player has the GunCon, the game's
  picture is made smaller by the border on each side and a plain white frame is drawn just
  outside it: none of the picture is covered, and the frame's inner edge is the picture's edge,
  which is what the tracker maps the aim to. Checked on the PC (a screenshot at 1920 x 1080:
  a 32-pixel frame round the 4:3 picture).
- **The aim's way in** (`host::lightgunAim(port, x, y, offscreen)`): x, y in -1..1 across and
  down the picture, as libretro's light gun coordinates; used while newer than a tenth of a
  second, the sticks aiming again after that. A shot while the gun points off the picture is
  given to the game as the GunCon's shot away from the screen (its reload): the fire buttons
  become L2, so the netplay record is unchanged.

## Sources

- [MrLightgun/MiSTerSindenDriver](https://github.com/MrLightgun/MiSTerSindenDriver), at 458775e
  (2026-11-07): README, `Tools/gun_serial.py`, `Tools/camtest.py`, `LightgunDriver/readme.txt`.
- [GunCon2-MiSTer](https://github.com/NolanNicholson/GunCon2-MiSTer).
- Forum threads: [RetroPie 263220](https://retropie.org.uk/forum/post/263220),
  [RetroPie 263020](https://retropie.org.uk/forum/post/263020),
  [ARpiCADE](https://arpicade.com/forums/viewtopic.php?p=10387).
- sindenwiki.org (Linux guide, original consoles, recoil, game developer) refuses automated
  reads (HTTP 403): its pages were seen only through search results.
