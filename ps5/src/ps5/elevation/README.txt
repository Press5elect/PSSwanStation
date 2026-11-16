The Lapy cooperative elevation client, from BlackBearReloaded's
ps5-native-app-boilerplate (examples/sandbox-elevation, revision 6cec460,
GPL-3.0-or-later): elevation.cpp, elevation.hpp and protocol.hpp, with only
their include lines changed. From build 28 PSSwanStation uses only its
requestHelper(): the title's own sandbox elevator (sandbox-elevator.elf,
ps5/elevator, as Crux2 and OGBox send it) through the ELF loader on port
9021. It no longer asks a resident Lapy service, nor ships Lapy's helper
(lapy.elf): an older Lapy service leaves the sandbox without counting the
console root's references, which in time takes the console down, and the
title could not tell which service answered. Builds 13 to 27 carried
PS5-Lapy-JB-Daemon's exact-title helper (MIT, BlackBearReloaded's fork at
c3bdfe3); its source is attached to the build 24 release.
