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
  2.08a) on its wiki.
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
   interfaces, endpoints and their kinds), reading nothing else.

## Sources

- [MrLightgun/MiSTerSindenDriver](https://github.com/MrLightgun/MiSTerSindenDriver), at 458775e
  (2026-11-07): README, `Tools/gun_serial.py`, `Tools/camtest.py`, `LightgunDriver/readme.txt`.
- [GunCon2-MiSTer](https://github.com/NolanNicholson/GunCon2-MiSTer).
- Forum threads: [RetroPie 263220](https://retropie.org.uk/forum/post/263220),
  [RetroPie 263020](https://retropie.org.uk/forum/post/263020),
  [ARpiCADE](https://arpicade.com/forums/viewtopic.php?p=10387).
- sindenwiki.org (Linux guide, original consoles, recoil, game developer) refuses automated
  reads (HTTP 403): its pages were seen only through search results.
