# PS5SX2's USB pad decoder (vendored)

`OrbisUsbPadDecode.h` and its PC test `test_usbpad.cpp`, from
[swordpdf/PS5SX2](https://github.com/Swordpdf/PS5SX2) at `6ccf48047a12` (`ps5/coreorbis/orbis-shims/` and
`ps5/coreorbis/tests/usbpad/`), copied unchanged. GPL-3.0-or-later, copyright swordpdf.

It parses a USB HID report descriptor and decodes the reports of PS3 USB pads and guitars, a DualShock 3 on a cable,
Xbox 360 (XInput) pads and guitars and any HID gamepad into one state. It uses none of the console's APIs.

PSSwanStation's reader of those devices, `ps5/src/ps5/usbpad_ps5.cpp`, follows PS5SX2's `ProsperoUsbPad.cpp` (the same
licence and author), with only its routes that identify a device before reading it.

The test: `c++ -std=c++17 -Wall -Wextra -I. -o test_usbpad test_usbpad.cpp && ./test_usbpad`.
