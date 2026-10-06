The Lapy cooperative elevation client, from BlackBearReloaded's
ps5-native-app-boilerplate (examples/sandbox-elevation, revision 6cec460,
GPL-3.0-or-later): elevation.cpp, elevation.hpp and protocol.hpp, with only
their include lines changed. It asks a resident Lapy service first, and
otherwise sends the packaged helper (lapy.elf in the title folder) to the ELF loader on
port 9021. The helper is upstream PS5-Lapy-JB-Daemon's exact-title one-shot
helper for PPSA99248 (mpereiraesaa's fork at 54a095c, MIT), built unchanged by
the boilerplate's tools/build-lapy-helper.py (ps5/tools/build.sh takes the result
from LAPY_HELPER_DIR); lapy-manifest.json beside it is
the record that build wrote. The raw-pointer helper earlier builds carried is
gone, as the boilerplate dropped it.
