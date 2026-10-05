/*
	PSSwanStation - the DualSense, read through libScePad.

	SPDX-License-Identifier: GPL-3.0-or-later

	The calls and the sample layout are PSFlyCast's (shell/ps5/ps5_pad.cpp),
	proven on the console: scePadOpen for each logged-in user, retried while the
	pad service starts, scePadReadState, the 120-byte state with the connection
	flag at 0x4c, and scePadSetVibrationMode(2) before rumble (a PS5 title's pad
	starts in haptics mode).

	Who is logged in is looked at again every two seconds, for a user who logs
	in or out while the title runs (as the earlier DuckStation build for this
	console did): players 2 to 4 are the other users, each with their pad.

	The touch pad is two buttons here: a click with the finger on its left half
	is the PlayStation's Select, on its right half Start. The state carries the
	touch points (their count at 0x34, the first one's x at 0x3c, 0 to 1919
	across the pad); a click with no finger reported takes the side touched last.
*/
#include "fe.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

extern "C"
{
int scePadInit(void);
int scePadOpen(int32_t userId, int32_t type, int32_t index, const void *param);
int scePadGetHandle(int32_t userId, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void *data);
int scePadClose(int32_t handle);
int scePadSetVibration(int32_t handle, const void *param);
int scePadSetVibrationMode(int32_t handle, int32_t mode);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *userId);
int sceUserServiceGetLoginUserIdList(int32_t *userIds);
int sceKernelUsleep(uint32_t microseconds);
}

namespace fe::platform
{
namespace
{

// libScePad's button bits (PS5 RetroArch src/input_ps5.cpp, PSFlyCast).
enum : uint32_t
{
	RawL3 = 0x000002,
	RawR3 = 0x000004,
	RawOptions = 0x000008,
	RawUp = 0x000010,
	RawRight = 0x000020,
	RawDown = 0x000040,
	RawLeft = 0x000080,
	RawL2 = 0x000100,
	RawR2 = 0x000200,
	RawL1 = 0x000400,
	RawR1 = 0x000800,
	RawTriangle = 0x001000,
	RawCircle = 0x002000,
	RawCross = 0x004000,
	RawSquare = 0x008000,
	RawTouchPad = 0x100000,
	// Set while the system intercepts the pad (its own menu is up).
	RawIntercepted = 0x80000000,
};

struct alignas(8) PadData
{
	uint32_t buttons;			// 0x00
	uint8_t lx, ly, rx, ry;		// 0x04
	uint8_t l2, r2;				// 0x08
	uint8_t padding[2];
	float orientation[4];		// 0x0c
	float acceleration[3];		// 0x1c
	float angularVelocity[3];	// 0x28
	uint8_t touchCount;			// 0x34
	uint8_t touchReserved[3];
	uint32_t touchReserved1;
	struct
	{
		uint16_t x, y;
		uint8_t id;
		uint8_t reserved[3];
	} touch[2];					// 0x3c
	int32_t connected;			// 0x4c
	uint64_t timestamp;			// 0x50
	uint8_t rest[256];			// the state is 120 bytes; room for a larger one
};
static_assert(offsetof(PadData, touchCount) == 0x34, "touch count at 0x34");
static_assert(offsetof(PadData, touch) == 0x3c, "touch points at 0x3c");
static_assert(offsetof(PadData, connected) == 0x4c, "connection flag at 0x4c");
static_assert(offsetof(PadData, timestamp) == 0x50, "timestamp at 0x50");

struct Device
{
	int32_t user = -1;
	int32_t handle = -1;
	bool touchRight = true;		// the half of the touch pad touched last
	uint8_t sentLarge = 0, sentSmall = 0;
};
std::array<Device, MaxPads> devices;
std::array<Pad, MaxPads> pads;
bool opened;
int32_t firstUser = -1;		// who started the title: player 1

int32_t openPad(int32_t user)
{
	int32_t handle = -1;
	// The pad service can publish the device after the title starts.
	for (int attempt = 0; attempt < 10 && handle < 0; attempt++)
	{
		handle = scePadOpen(user, 0, 0, nullptr);
		if (handle < 0)
			handle = scePadGetHandle(user, 0, 0);
		if (handle < 0)
			sceKernelUsleep(100000);
	}
	return handle;
}

void openAll()
{
	if (opened)
		return;
	opened = true;
	int rc = sceUserServiceInitialize(nullptr);
	diag::mark("pad: sceUserServiceInitialize %#x", (unsigned)rc);
	rc = scePadInit();
	diag::mark("pad: scePadInit %#x", (unsigned)rc);

	int32_t users[4] = { -1, -1, -1, -1 };
	int32_t initialUser = -1;
	sceUserServiceGetInitialUser(&initialUser);
	if (sceUserServiceGetLoginUserIdList(users) < 0)
		users[0] = initialUser;
	// Player 1 is the user who started the title.
	for (int i = 1; i < 4; i++)
		if (users[i] == initialUser)
			std::swap(users[0], users[i]);
	if (users[0] == -1)
		users[0] = initialUser;
	firstUser = users[0];
	// The others follow, one place each with no gaps: a user whose pad does
	// not open now is looked for again later (lookForUsers).
	int place = 0;
	for (int i = 0; i < 4 && place < MaxPads; i++)
	{
		if (users[i] == -1)
			continue;
		const int32_t handle = openPad(users[i]);
		diag::mark("pad: %d: user %d handle %d", place + 1, users[i], handle);
		if (handle < 0)
		{
			// Player 1's place is kept for whoever started the title.
			if (i == 0)
				place++;
			continue;
		}
		scePadSetVibrationMode(handle, 2);	// rumble, not haptics
		devices[place].user = users[i];
		devices[place].handle = handle;
		place++;
	}
}

// Who is logged in now: a user who left gives the place up, a user who came
// takes the first free one. Nobody moves: a player keeps the number they have.
void lookForUsers()
{
	int32_t users[4] = { -1, -1, -1, -1 };
	if (sceUserServiceGetLoginUserIdList(users) < 0)
		return;
	for (int i = 0; i < MaxPads; i++)
	{
		Device& device = devices[i];
		// Whoever started the title stays player 1 whatever the list says.
		if (device.user == -1 || device.user == firstUser)
			continue;
		bool here = false;
		for (const int32_t user : users)
			here = here || user == device.user;
		if (here)
			continue;
		diag::mark("pad: %d: user %d logged out", i + 1, device.user);
		if (device.handle >= 0)
			scePadClose(device.handle);
		device = Device();
	}
	for (const int32_t user : users)
	{
		if (user == -1)
			continue;
		int place = -1;
		bool has = false;
		for (int i = MaxPads - 1; i >= 0; i--)
		{
			has = has || devices[i].user == user;
			// Place 1 is only ever the first user's.
			if (devices[i].user == -1 && (i > 0 || user == firstUser))
				place = i;
		}
		if (has || place < 0)
			continue;
		// Once: the pad service is running by now, and this is the frame's thread.
		int32_t handle = scePadOpen(user, 0, 0, nullptr);
		if (handle < 0)
			handle = scePadGetHandle(user, 0, 0);
		if (handle < 0)
			continue;
		scePadSetVibrationMode(handle, 2);
		devices[place].user = user;
		devices[place].handle = handle;
		diag::mark("pad: %d: user %d logged in, handle %d", place + 1, user, handle);
	}
}

uint32_t translate(uint32_t raw, bool touchRight)
{
	static const struct
	{
		uint32_t raw, button;
	} map[] = {
		{ RawCross, Cross }, { RawCircle, Circle }, { RawSquare, Square }, { RawTriangle, Triangle },
		{ RawL1, L1 }, { RawR1, R1 }, { RawL2, L2 }, { RawR2, R2 }, { RawL3, L3 }, { RawR3, R3 },
		{ RawUp, Up }, { RawDown, Down }, { RawLeft, Left }, { RawRight, Right }, { RawOptions, Options },
	};
	uint32_t buttons = 0;
	for (const auto& entry : map)
		if (raw & entry.raw)
			buttons |= entry.button;
	if (raw & RawTouchPad)
		buttons |= touchRight ? TouchRight : TouchLeft;
	return buttons;
}

} // namespace

void padOpen()
{
	openAll();
}

void padPoll()
{
	openAll();
	static unsigned polls;
	if (++polls % 120 == 0)
		lookForUsers();
	for (int i = 0; i < MaxPads; i++)
	{
		Device& device = devices[i];
		Pad& pad = pads[i];
		const uint32_t before = pad.buttons;
		if (device.handle < 0)
		{
			pad = Pad();
			continue;
		}
		PadData raw{};
		const int rc = scePadReadState(device.handle, &raw);
		if (rc < 0 || raw.connected == 0 || (raw.buttons & RawIntercepted) != 0)
		{
			// Disconnected, or the system menu has the pad: nothing is held.
			pad = Pad();
			pad.connected = rc >= 0 && raw.connected != 0;
			pad.released = before;
			continue;
		}
		if (raw.touchCount > 0 && raw.touchCount <= 2)
			device.touchRight = raw.touch[0].x >= 960;
		pad.connected = true;
		// While the touch pad is held, the side it was pressed on stays.
		if ((before & (TouchLeft | TouchRight)) != 0 && (raw.buttons & RawTouchPad) != 0)
			pad.buttons = translate(raw.buttons & ~RawTouchPad, false) | (before & (TouchLeft | TouchRight));
		else
			pad.buttons = translate(raw.buttons, device.touchRight);
		pad.pressed = pad.buttons & ~before;
		pad.released = before & ~pad.buttons;
		pad.lx = (raw.lx - 128) / 127.5f;
		pad.ly = (raw.ly - 128) / 127.5f;
		pad.rx = (raw.rx - 128) / 127.5f;
		pad.ry = (raw.ry - 128) / 127.5f;
		pad.l2 = raw.l2 / 255.f;
		pad.r2 = raw.r2 / 255.f;
	}
}

const Pad& pad(int index)
{
	static const Pad none;
	return index >= 0 && index < MaxPads ? pads[index] : none;
}

int padCount()
{
	int n = 0;
	for (const Pad& pad : pads)
		n += pad.connected;
	return n;
}

void padRumble(int index, float strong, float weak)
{
	if (index < 0 || index >= MaxPads || devices[index].handle < 0)
		return;
	Device& device = devices[index];
	const uint8_t large = (uint8_t)std::lround(std::fmin(std::fmax(strong, 0.f), 1.f) * 255.f);
	const uint8_t small = (uint8_t)std::lround(std::fmin(std::fmax(weak, 0.f), 1.f) * 255.f);
	if (large == device.sentLarge && small == device.sentSmall)
		return;
	// { large motor, small motor }
	const uint8_t param[2] = { large, small };
	scePadSetVibration(device.handle, param);
	device.sentLarge = large;
	device.sentSmall = small;
}

}
