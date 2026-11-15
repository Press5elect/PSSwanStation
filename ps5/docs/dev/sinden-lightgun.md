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

Settings, Controls, Players: player 1's controller the GunCon, "Sinden Lightgun" on player 1's
GunCon (or `data/sinden.txt` with `on`, which also works without the setting); restart; the gun
plugged in before the title starts; 20 s in the library;
then each of the gun's buttons pressed once, in a known order, a second apart, the gun pointed
at the screen; then Settings, System, Logs, "Save the log now", and the boot log sent. If the
title or the console stops answering, the next try with `find` says how far the first stage
gets. The `sinden:` lines are all that is needed (`grep sinden: psswanstation-boot.log`).

## The title's side (build 27, after test 2)

- **The setting**: Settings, Controls, Players: "Sinden Lightgun" (Off, player 1's or player 2's
  GunCon; that player's controller must be the GunCon) and its "Border" (1-8% of the screen's
  height, 3% at first). Marked as an experiment, not yet seen to work on a console (released so
  in build 27).
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

## How the pieces join (`src/sinden_aim.cpp`)

Once a frame, on the frame's thread (`host.cpp`'s `moveAims`, before the GunCon's aim moves),
while the setting is on and that player has the GunCon: the newest camera picture
(`sinden::latestFrame`) goes through a `track::Tracker` made for the picture's shape and the
border's thickness as `ui.cpp` draws them (`ui::sindenPicture`; made again when they change),
with the gun's calibration from its serial port, and its aim into `host::lightgunAim`; a
picture without the screen, or an aim off the picture, is an aim off the screen (a shot
reloads). The gun's buttons (`sinden::buttons`, mouse or joystick bits): the first fires (R2),
the second is the GunCon's A (Y), the third its B (X), through `host::lightgunButtons`. Every ten
seconds of play: `sinden: aim: N pictures in 10 s, the screen found in N (N off the picture);
quality Q, the border's level L of 255; the tracker T ms a picture`. Not done yet: the camera's
exposure set from the border's level (the tracker reports it; the USB side has no UVC exposure
control yet), and the gun's keys.

## The tracker

The aiming core for way 3, written for the title (Sinden's software is closed; nothing of it was
used or looked into): `ps5/src/sinden_track.h` and `.cpp`, C++17 with the standard library only,
no dependency on the rest of the title, not wired into it yet. It has only seen frames drawn on a
PC (`ps5/tests/sinden`); **no real gun or camera has been tried.**

**What it takes and gives.** A frame's luma (8 bits, any size and stride; 320 x 240 is what was
measured), `Options` and the gun's `Calibration`; out comes whether the screen was found, the aim
in the picture inside the border (-1 at its left and top edges, +1 at its right and bottom:
libretro's light gun screen coordinates), whether that is off the picture (a GunCon reload), a
quality from 0 to 1, the picture's and the border's outer corners in the frame (for an overlay),
where each side came from, the screen's turn, and the border's level and the share of it clipped
(for the caller's control of the camera's exposure). `detect()` reads one frame alone; a
`Tracker` (one a gun, owned by the caller: no globals, no threads) reads frame after frame.
`Options`: whether the border is brighter or darker than its surround (a darker one is read
inverted), the picture's shape, **the border's thickness as the title draws it** (it should
always be told: see the limits), the camera's focal length (used only for a side out of view)
and the lens's barrel distortion (the Tracker measures it). The calibration's per cent is taken
as of the picture's width and height, added to the aim, positive right and down; or, with
`inCamera`, of the camera frame's, moving the point that is mapped. Which one Sinden means is not
known.

**How.**

1. *Find the outline.* Thresholds at fractions of the frame's dark (5th percentile) and bright
   (99.9th) levels, so they follow the exposure; the bright parts (8-connected, from runs); for
   the three largest at each threshold, rays from the part's middle find its outermost pixel, and
   a sequential RANSAC fits up to six lines through those points. The lines are named top, right,
   bottom and left by their outward normals less the screen's turn (near 45 degrees the picture's
   longer sides are its top and bottom; the Tracker keeps the last turn). A part scores for how
   many points lie on four lines, for being a ring (the border is a band, the picture inside it
   is not all as bright), for size, for holding the frame's middle (where the gun points) and for
   lying where the last outline was. The first clean ring ends the search; the best few go on.
2. *Measure each side.* About a hundred profiles across it, half a pixel a step, following the
   curve the lens gives the straight edge. The outer edge (the darker outside into the border) is
   where the profile crosses half way between the two levels; when the border is clipped, its
   steepest point instead. Half way is right to within 0.1 px even on a pixel-sharp edge, where
   the steepest point sticks to the pixel grid (up to 0.4 px off, measured); clipping moves half
   way outward, the steepest point stays put until the true level is twice the clipped one. The
   inner edge (border into picture) is where the profile leaves the border's level, half way to
   the picture's level read an edge's width past it. Lines are fitted robustly (RANSAC, then
   Tukey's weights) to the points with the lens's distortion taken out (Fitzgibbon's division
   model). Twice: the second pass knows the side's border level (the low quarter of the levels
   seen: a thin border of a colour darker than the picture's whites otherwise runs into the
   picture's brighter rise) and its thickness (the lowest common one: the picture beside the
   border can only make it look thicker, where it is as bright).
3. *Check that each side is the border's.* It stands clearly above its outside; its band does not
   run on past its corners (the border is a closed band); and no band at the border's level lies
   further out along most of it (the border is the outermost band; if one does, the side moves
   out to it and is measured again). With the thickness known, a candidate on which no inner
   edge shows anywhere (a window, a lamp's shade) gives way to one that does, and counts at all
   only if the gun points into it (the screen showing a picture all as bright as the border).
4. *Place the picture.* Each side of the picture is its outer edge moved in by the border's
   thickness, through the homography of the outer outline (so the perspective is right). The
   outer edge is the cleanest edge there is, against the bezel's or the display's black. The
   told thickness and the inner edge seen are two measures of the same line: blur and clipping
   push both of the border's edges out from its middle by the same, and the inner edge also
   scatters with the picture beside it. Weighted a third to the seen one: of none, a third and
   half (their mean, which cancels the blur exactly), a third had the least error in most
   conditions. Without the whole outline, a side is its outer edge moved in by the thickness seen
   along it. One side out of view is placed from the other three: the two sides that run towards
   it meet at their vanishing point, which with the focal length gives their direction in space;
   the opposite side's direction is square to it in the plane its line sees; the picture's shape
   gives the distance. Placed only if it really is out of view.
5. *Aim.* The frame's centre (or the point the calibration in the camera moves it to) through the
   inverse homography to the picture; the calibration in the picture added.
6. *Tracker.* The last threshold is tried first (one labelling a frame, mostly); the last turn
   and outline are preferred. The border's thickness on each side is learnt (the lowest common
   value of the last 31 frames that saw it), for when it is not told and for sides a frame does
   not see. The lens's distortion is measured on frames whose outline is a third of the frame
   wide or more: the value that makes the edge points straightest (golden-section search), the
   median of the last 63 (one frame's measure scatters by 0.016 at 120 px wide, by 0.05 at 85).
   Optional smoothing, off by default: a One Euro filter (steady at rest, opening with speed).

**Measured** (`ps5/tests/sinden/run.sh`, 2026-11-15; 150 frames a condition unless said;
the error in per cent of the picture's width). The frames: a 55-inch television, the 4:3
picture with the border round it and black beside (unless said), a border 2-5% of the display's
height, a dark room, the camera 1.8-3 m away and up to 0.6 m to the side, turned up to 15
degrees, a 64-degree lens (focal 0.8 of the frame's width) with barrel distortion (k1 -0.1 in
focal lengths, told), blur 0.7-1.2 px, sensor noise, vignetting, exposure 0.75-1.25 (the border
clips above 1), the camera's JPEG, the aim anywhere in the picture. The picture is then about 90 px
wide in the frame: 0.1% of its width is 0.09 px.

| Condition | Found | Median | 95% | Worst |
| --- | --- | --- | --- | --- |
| Ideal: sharp (0.5 px), little noise, no JPEG, seen square on | 100% | 0.074 | 0.27 | 0.57 |
| Typical (as above) | 100% | 0.115 | 0.31 | 0.76 |
| The border at the display's edges round a 16:9 picture | 100% | 0.092 | 0.35 | 0.51 |
| Far: 3-4.5 m (the picture 57 px wide) | 99.3% | 0.27 | 0.98 | 1.8 |
| Seen at an angle, to about 40 degrees | 100% | 0.15 | 0.52 | 6.0 |
| Turned up to 45 degrees | 100% | 0.12 | 0.41 | 1.1 |
| A bright game, white areas against the border | 100% | 0.12 | 0.48 | 1.0 |
| A dark game | 100% | 0.094 | 0.76 | 1.4 |
| White bars along two whole sides of the picture | 100% | 0.14 | 0.54 | 1.0 |
| Underexposed (0.25-0.4) | 100% | 0.098 | 0.35 | 0.90 |
| Overexposed (1.8-3: the border clipped) | 100% | 0.27 | 1.0 | 1.4 |
| Blur 1.4-2 px, motion smear to 6 px, noise 7 | 100% | 0.36 | 1.5 | 2.1 |
| A backlight's banding (15%) | 100% | 0.14 | 0.41 | 0.73 |
| A lit room, a thin bezel, a window as bright as the screen | 100% | 0.12 | 0.34 | 0.73 |
| A coloured border (luma 105) and the game's whites brighter | 100% | 0.41 | 1.6 | 3.2 |
| Strong barrel (k1 -0.25) | 100% | 0.11 | 0.51 | 0.71 |
| The lens's distortion not told | 100% | 0.093 | 0.33 | 0.45 |
| A narrower lens (focal 1.2 of the width), 2.6-4 m | 100% | 0.069 | 0.33 | 0.66 |
| The border's thickness not told, a bright game | 87% | 0.50 | 2.5 | 9.4 |
| Aimed at the corners (80) | 100% | 0.15 | 0.59 | 1.1 |
| A side out of view (100 frames 1-1.5 m away that do not see the whole border) | 82% | 0.094 | 1.7 | 11 |
| An all-white game (60) | 100% | 0.045 | 0.16 | 0.21 |
| A black game (60) | 100% | 0.12 | 0.61 | 1.2 |

- Aimed off the picture (1.5-12.5% of its width beyond an edge or a corner) with the screen in
  view: all 100 found and called off the picture. A frame of noise only, an all-white frame, no
  frame: not found.
- A side out of view: placed in 29 of the 82 found. Of 159 such frames found in another run,
  the 51 with a side placed: median 0.16%, 95% 1.75%, worst 3.5%, with the focal length told
  right; told 20% wrong (0.65 or 1.0 for 0.8): median about 0.65%, 95% up to 2.2%.
- The Tracker: at rest the raw aim moves by 0.017% of the width (standard deviation over 120
  frames: 0.03 px), with the smoothing at 1 Hz 0.005%. Sweeping a picture's width a second,
  the raw error stays 0.095%; the smoothing adds lag (0.25%). A game flickering black every
  other frame: all 60 found, 0.15%. The lens learnt from frames 1.6-2.2 m away with k1 -0.25 not
  told: -0.122 (the first-order equal of the test's lens is -0.098; the two models differ beyond
  the first order), the error 0.07% before and after (the screen near the frame's middle bends
  little).
  The border not told, a bright game, the same television throughout: learnt 0.037 x 0.049 for
  a true 0.033 x 0.044 (its blurred edges read it thicker), the error 0.26% median and 0.6% at
  95% over the second half, every frame found (`detect()` alone, the row above with a new
  television each frame: 87% found, 2.5% at 95%).
- The calibration's arithmetic in both ways, and a dark border on a bright surround read inverted
  (the same answer as the bright one): checked.
- Time on one thread of a 2.3 GHz Xeon (a cloud machine, two runs): `detect()` 1.6 ms a frame
  (median), 4.4-4.7 ms at the 95th percentile, 10-14 ms at worst, over every condition; the
  Tracker, which tries the last threshold first, 1.05 ms (median) and 1.6-2.2 ms at worst over
  120 frames at rest. Not timed on the console.

**Limits.**

- The border must be clearly brighter than what is round it outside (the bezel, the display's
  black, the room): a bright wall right against the screen, or a bezel as bright, loses it.
- An exposure that clips the border: its edges read up to a pixel out (overexposed row). The
  caller should set the camera's exposure (UVC) to keep `borderLevel` under about 230;
  `clipped` says how much of it is clipped.
- A thin border under blur: under about 2 px in the frame its edges merge; the error grows (far
  row, blur row). A thicker border is easier: 3-4% of the display's height.
- A coloured border darker than the game's whites: 0.4% median, 1.6% at 95%. White is better.
- The thickness should always be told: without it a frame alone cannot tell the border from a
  white area of the game beside it (87% found, 2.5% at 95%); the Tracker learns it.
- A television that crops the picture (overscan) crops the border: a third of that is
  corrected. Overscan should be off ("Just Scan", "Screen Fit"), as Sinden's own setup asks.
- A side out of view is placed with the focal length told; two sides out of view: not found.
- Turned beyond 45 degrees: read as the nearest quarter turn unless tracking.
- Game pictures that mimic the border can still mislead it: about one frame in 150 in the harder
  conditions is off by several per cent (worst 6% seen at an angle, 11% with a side out of
  view, 9% without the thickness told). The Tracker's preference for the last outline helps.
- A curved screen (a CRT) is not modelled: its sides are fitted as straight lines.

## Sources

- [MrLightgun/MiSTerSindenDriver](https://github.com/MrLightgun/MiSTerSindenDriver), at 458775e
  (2026-11-07): README, `Tools/gun_serial.py`, `Tools/camtest.py`, `LightgunDriver/readme.txt`.
- [GunCon2-MiSTer](https://github.com/NolanNicholson/GunCon2-MiSTer).
- Forum threads: [RetroPie 263220](https://retropie.org.uk/forum/post/263220),
  [RetroPie 263020](https://retropie.org.uk/forum/post/263020),
  [ARpiCADE](https://arpicade.com/forums/viewtopic.php?p=10387).
- sindenwiki.org (Linux guide, original consoles, recoil, game developer) refuses automated
  reads (HTTP 403): its pages were seen only through search results.
