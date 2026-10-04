/*
	PSFlyCast - the one call the title makes to leave its sandbox.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later

	A C++17-callable front for elevation.hpp (ps5-native-app-boilerplate's
	sandbox-elevation example, which is C++20): 0 when the helper granted the
	filesystem capability, its status code otherwise.
*/
#include "elevation.hpp"

namespace ps5
{
unsigned elevateFilesystem(const char *helperPath)
{
	return static_cast<unsigned>(elevation::request(elevation::Capability::filesystem, helperPath));
}
}
