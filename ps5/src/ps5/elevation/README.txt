The Lapy cooperative elevation client, from BlackBearReloaded's
ps5-native-app-boilerplate (examples/sandbox-elevation, revision 6cec460,
GPL-3.0-or-later): elevation.cpp, elevation.hpp and protocol.hpp, with only
their include lines changed. It asks a resident Lapy service first, and
otherwise sends the packaged helper (lapy.elf in the title folder) to the ELF loader on
port 9021. The helper is upstream PS5-Lapy-JB-Daemon's exact-title one-shot
helper for PPSA99248 (MIT): BlackBearReloaded's fork at c3bdfe3, which is
mpereiraesaa's cooperative helper at 54a095c with the credential attributes
kept whole, as firmware 13.60 needs. It is built unchanged, with the payload
SDK v0.42, by the boilerplate's tools/build-lapy-helper.py with those two pins
set as ProsperoEden's tools/deps.json has them (ps5/tools/build.sh takes the
result from LAPY_HELPER_DIR); lapy-manifest.json beside it is the record that
build wrote. Up to build 12 the helper was 54a095c built with SDK v0.40. The
raw-pointer helper earlier builds carried is gone, as the boilerplate dropped
it.
