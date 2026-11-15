/*
	PSSwanStation - variable refresh and HDR output, as the graphics driver asks
	for them (from Crux2's ps5_host.cpp and ps5_keyboard.cpp, and OGBox's
	ps5_modules.cpp).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	With "119.88 Hz with a variable refresh rate" chosen (Display output), the
	title puts PS5_VIDEOOUT_VRR=1 in the driver's environment before it starts
	Vulkan (main.cpp), and once 119.88 Hz is configured the driver asks the
	output to leave its fixed rate (PS5_Mesa, Crux2's patches 0016 and 0017).
	The call for that, sceVideoOutVrrUnpegFromFixedRate, is not exported by
	every firmware's libSceVideoOut, and a title that imports what the console
	lacks does not start: the driver calls this weak hook instead, which looks
	the function up while the title runs, by its name or its NID, in the
	modules the process has. Without it the driver refuses variable refresh and
	119.88 Hz stays as configured; the boot log says which.
*/
#include "fe.h"

#include <array>
#include <cstdint>

extern "C"
{
int sceKernelGetModuleList(int32_t *handles, size_t capacity, size_t *count);
int sceKernelDlsym(int handle, const char *symbol, void **addrp);
}

namespace
{
void *find(const char *name, const char *nid)
{
	std::array<int32_t, 256> handles{};
	size_t count = 0;
	if (sceKernelGetModuleList(handles.data(), handles.size(), &count) != 0)
		count = 0;
	const auto in = [&](int32_t handle) -> void * {
		void *address = nullptr;
		if (sceKernelDlsym(handle, name, &address) == 0 && address != nullptr)
			return address;
		address = nullptr;
		if (sceKernelDlsym(handle, nid, &address) == 0 && address != nullptr)
			return address;
		return nullptr;
	};
	for (size_t i = 0; i < count && i < handles.size(); i++)
		if (void *address = in(handles[i]))
			return address;
	// The system's modules are not all in the list a title sees, but they have
	// handles: the small numbers the loader gives in load order, and the 0x2000
	// range (an unused handle answers at once).
	for (int32_t handle = 0; handle < 0x2040; handle = handle == 511 ? 0x2000 : handle + 1)
		if (void *address = in(handle))
			return address;
	return nullptr;
}
}

// The driver's hook (PS5_Mesa, Crux2's patch 0017): -1 when the console has no
// such call, and the driver then refuses variable refresh.
extern "C" int ps5_title_videoout_vrr_unpeg(int32_t handle)
{
	using Unpeg = int (*)(int32_t);
	static const Unpeg unpeg = [] {
		const Unpeg found = reinterpret_cast<Unpeg>(find("sceVideoOutVrrUnpegFromFixedRate", "T4ucGB8CsnM"));
		fe::diag::mark("display: sceVideoOutVrrUnpegFromFixedRate %s",
				found != nullptr ? "found" : "not on this console: no variable refresh");
		return found;
	}();
	return unpeg != nullptr ? unpeg(handle) : -1;
}

// HDR output (from OGBox 0.0.9's ps5_modules.cpp): the driver retypes the
// framebuffers through sceVideoOutSubmitChangeBufferAttribute2 and reads the
// output's dynamic range through sceVideoOutGetOutputStatus (PS5_Mesa: the
// wsi/videoout HDR10 framebuffers patch), both looked up here as the variable
// refresh one is. -1 when the console has no such call: HDR is then refused,
// and the output reads as unknown.
extern "C" int ps5_title_videoout_change_attribute(int32_t handle, int32_t set, const void *attribute)
{
	using Change = int (*)(int32_t, int32_t, const void *, const void *);
	static const Change change = [] {
		const Change found = reinterpret_cast<Change>(find("sceVideoOutSubmitChangeBufferAttribute2", "HuViW4HnrOw"));
		fe::diag::mark("display: sceVideoOutSubmitChangeBufferAttribute2 %s",
				found != nullptr ? "found" : "not on this console: no HDR output");
		return found;
	}();
	return change != nullptr ? change(handle, set, attribute, nullptr) : -1;
}

extern "C" int ps5_title_videoout_output_status(int32_t handle, void *status)
{
	using Status = int (*)(int32_t, void *);
	static const Status get = [] {
		const Status found = reinterpret_cast<Status>(find("sceVideoOutGetOutputStatus", "utPrVdxio-8"));
		fe::diag::mark("display: sceVideoOutGetOutputStatus %s",
				found != nullptr ? "found" : "not on this console: the output's range unknown");
		return found;
	}();
	return get != nullptr ? get(handle, status) : -1;
}
