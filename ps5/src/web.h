/*
	PSSwanStation - the web panel: the title from a phone or a computer on the
	same network (web.cpp).

	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once
#include <string>

namespace fe::web
{

constexpr int Port = 3311;

// Starts or stops the server as the setting says (options::frontend().web).
void apply();
// Once a frame, on the main thread: what the server asked for is done here,
// and what it tells is brought up to date.
void tick();
// The title closes.
void shutdown();

bool listening();
// What it is doing ("Listening on 192.168.1.20:3311"), or why it is not.
std::string status();
// The address to open, with the key: http://192.168.1.20:3311/?key=...
// Empty when this console's address is not known.
std::string url();
const std::string& key();
// A new key: what had the old one cannot do anything any more.
void newKey();

}
