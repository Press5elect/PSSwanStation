SwanStation for PS5 - what it is made of
========================================

The title as a whole is distributed under the GNU General Public License,
version 3 (GPL-3.0.txt), WITHOUT ANY WARRANTY. Its source is the SwanStation
source at the revision BUILD.txt names, plus the ps5 folder.

Part                         Licence          Text                     Origin
---------------------------  ---------------  -----------------------  ------------------------------------------
SwanStation (the emulator)   GPL-3.0          GPL-3.0.txt              github.com/libretro/swanstation, a fork of
                                                                       DuckStation by Connor McLaughlin (stenzek)
                                                                       and contributors
The PS5 frontend (ps5/)      GPL-3.0-or-later GPL-3.0.txt              this port
  runtime, libc, threads,    GPL-2.0-or-later GPL-3.0.txt (as later    PSFlyCast (shell/ps5), used under the
  network share reader                        version)                 later version
  sandbox elevation helper   GPL-3.0-or-later GPL-3.0.txt              ps5-native-app-boilerplate (BlackBearReloaded)
Mesa RADV (Vulkan driver)    MIT and others   Mesa-license.rst         the PS5 Mesa port
PS5 payload SDK, libc.prx    see the SDK      -                        the payload SDK fork PS5_Vulkan pins
OpenBIOS (built-in BIOS)     MIT              OpenBIOS-LICENSE.txt     PCSX-Redux project
Dear ImGui                   MIT              DearImGui-LICENSE.txt    github.com/ocornut/imgui
libsmb2                      LGPL-2.1         LGPL-2.1.txt             github.com/sahlberg/libsmb2
libchdr, with LZMA and zstd  BSD-3-Clause     libchdr-LICENSE.txt      github.com/rtissera/libchdr
miniz                        MIT              MIT.txt                  github.com/richgel999/miniz
xxHash                       BSD-2-Clause     BSD-2-Clause.txt         github.com/Cyan4973/xxHash
Xbyak                        BSD-3-Clause     BSD-3-Clause.txt         github.com/herumi/xbyak
stb_image, stb_image_resize  MIT / public     MIT.txt                  github.com/nothings/stb
                             domain
libretro-common              MIT              MIT.txt                  github.com/libretro/libretro-common
Roboto (font)                Apache-2.0       Apache-2.0.txt           Google
Font Awesome Free (symbols)  SIL OFL 1.1      OFL-1.1.txt              fontawesome.com (the font file only)
Cheat and patch database     see its project  -                        github.com/duckstation/chtdb (assets/
                                                                       cheats.zip and patches.zip, unmodified
                                                                       release files; the project offers them to
                                                                       other emulators)
The swan icon and pictures   GPL-3.0-or-later GPL-3.0.txt              drawn by ps5/tools/make-art.py

This title contains no games, no BIOS file of an original console, no console
firmware and no keys. Use games you own, from your own discs, and a BIOS
dumped from hardware you own.

SwanStation for PS5 is an unofficial port. It is not affiliated with or
endorsed by Sony Interactive Entertainment, the libretro team or the
DuckStation project. "PlayStation" and "PS5" are trademarks of Sony
Interactive Entertainment. Vulkan is a registered trademark of the Khronos
Group; the RADV port in this title is not a conformant product.
