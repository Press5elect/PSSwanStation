The sandbox elevator: BlackBearReloaded's one-request elevation helper from
ps5-native-app-boilerplate (examples/sandbox-elevation, GPL-3.0-or-later), as
Crux2 carries it (its ps5/elevation/payload/main.cpp, protocol.hpp and
validate-elevation-helper.py), with this title's ID (PPSA99248) in place of
Crux2's. ps5/tools/build.sh builds payload/main.cpp into the title folder as
sandbox-elevator.elf.

The title sends it to the ELF loader on port 9021 when the usual way out of
the sandbox (a resident Lapy service, or lapy.elf) does not show the console's
own /dev, where USB CD and DVD drives are: it sets the title's root and jail
directories to the console's root, so /dev is the real one.

SPDX-FileCopyrightText: 2026 Press5elect
SPDX-License-Identifier: GPL-3.0-or-later
