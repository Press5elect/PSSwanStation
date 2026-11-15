/*
	PSSwanStation - the Sinden Lightgun on a PC: none (sinden.h). The gun is
	read over the console's USB only (src/ps5/sinden_ps5.cpp); here its state
	stays Off, so the host build links and runs without it.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "sinden.h"

namespace fe::sinden
{

void start(const Options&) {}

void stop() {}

bool startIfAsked()
{
	return false;
}

bool claims(uint16_t, uint16_t)
{
	return false;
}

Status status()
{
	return Status();
}

State state()
{
	return State::Off;
}

bool latestFrame(Frame&)
{
	return false;
}

Buttons buttons()
{
	return Buttons();
}

Calibration calibration()
{
	return Calibration();
}

}
