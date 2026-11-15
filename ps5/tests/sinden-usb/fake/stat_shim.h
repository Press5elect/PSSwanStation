// PSSwanStation - forced into src/ps5/sinden_ps5.cpp's PC build (console_test.cpp):
// its stat() calls go to the test, which says the console's libSceUsbd.sprx is
// there. `struct stat` itself is not touched (the macro takes arguments).
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <sys/stat.h>

extern "C" int sinden_test_stat(const char *path, struct stat *st);
#define stat(path, st) sinden_test_stat(path, st)
