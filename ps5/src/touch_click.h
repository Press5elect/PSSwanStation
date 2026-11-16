/*
	PSSwanStation - which half of the touch pad a click is on (after Crux2's
	PS5InputLogic::TouchClick, 35c80d6; Crux2 by Press5elect).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	The left half is the PlayStation's Select, the right half Start. The pad
	reports up to two fingers with an id each, and the click as one button for
	the whole pad. The finger that clicks is the one that touched last: a
	thumb resting on the pad is the first in the report, and taking the first
	pressed whichever half the resting thumb was on. A reading may carry the
	click before it carries the finger: then the finger seen last, within
	200 ms, is the one. With no finger at all, the click is Start, as before.
	Pure logic, so ps5/tests/touch tests it on the PC.
*/
#pragma once

#include <cstdint>

namespace fe::touch
{
class Click
{
public:
	static constexpr uint16_t Middle = 960;			// the pad is 1920 units wide
	static constexpr uint64_t RecentUs = 200000;	// a finger seen this recently still counts

	// Each reading's fingers (count 0 to 2; x and id of each), whether the pad
	// is clicked or not.
	void touches(unsigned count, const uint16_t *x, const uint8_t *id, uint64_t nowUs)
	{
		bool seen[2] = { false, false };
		for (unsigned i = 0; i < count && i < 2; i++)
		{
			int slot = -1;
			for (int s = 0; s < 2; s++)
				if (finger[s].present && finger[s].id == id[i])
					slot = s;
			if (slot < 0)
			{
				for (int s = 0; s < 2 && slot < 0; s++)
					if (!finger[s].present && !seen[s])
						slot = s;
				if (slot < 0)
					slot = 0;
				finger[slot].present = true;
				finger[slot].id = id[i];
				finger[slot].order = ++order;
			}
			finger[slot].x = x[i];
			seen[slot] = true;
		}
		for (int s = 0; s < 2; s++)
			if (!seen[s])
				finger[s].present = false;
		const int n = newest();
		if (n >= 0)
		{
			lastX = finger[n].x;
			lastUs = nowUs;
			haveLast = true;
		}
	}

	// At the start of a click (after touches() for the same reading): true for
	// the left half.
	bool leftHalf(uint64_t nowUs) const
	{
		const int n = newest();
		if (n >= 0)
			return finger[n].x < Middle;
		if (haveLast && nowUs - lastUs <= RecentUs)
			return lastX < Middle;
		return false;
	}

	// The finger that touched last, or -1; and its x.
	bool newestX(uint16_t& x) const
	{
		const int n = newest();
		if (n < 0)
			return false;
		x = finger[n].x;
		return true;
	}

private:
	struct Finger
	{
		bool present = false;
		uint8_t id = 0;
		uint16_t x = 0;
		uint64_t order = 0;	// when it touched, as a count: the larger touched later
	};
	int newest() const
	{
		int n = -1;
		for (int s = 0; s < 2; s++)
			if (finger[s].present && (n < 0 || finger[s].order > finger[n].order))
				n = s;
		return n;
	}
	Finger finger[2];
	uint64_t order = 0;
	bool haveLast = false;
	uint16_t lastX = 0;
	uint64_t lastUs = 0;
};
}
