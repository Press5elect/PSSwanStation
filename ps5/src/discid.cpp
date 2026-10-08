/*
	PSSwanStation - a disc image's serial number, read without starting it.

	SPDX-License-Identifier: GPL-3.0-or-later

	The emulator's own disc reader opens the image (any format it knows, on a
	share as well) and its own code finds the boot file's name, which is the
	serial: SLUS_005.94 is SLUS-00594. A game's details use it for a game that
	has not run yet.

	The same reader gives RetroAchievements the disc's sectors, from which the
	game is told (achievements.cpp makes the hash; host.cpp passes these on).

	A file of its own because the emulator's disc header is C++17 and the rest
	of the frontend is built as C++20.
*/
#include "fe.h"

#include "common/cd_image.h"
#include "common/error.h"
#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/gpu.h"
#include "core/mdec.h"
#include "core/system.h"

#include <memory>

namespace fe::host
{

std::string readSerial(const std::string& imagePath)
{
	const std::string ext = extension(imagePath);
	if (imagePath.empty() || ext == ".exe" || ext == ".psexe" || ext == ".psf" || ext == ".minipsf")
		return "";
	Common::Error error;
	std::unique_ptr<CDImage> image = CDImage::Open(imagePath.c_str(), CDImage::OpenFlags::None, &error);
	if (!image)
		return "";
	return System::GetGameCodeForImage(image.get(), false);
}

void *discOpen(const std::string& imagePath)
{
	Common::Error error;
	std::unique_ptr<CDImage> image = CDImage::Open(imagePath.c_str(), CDImage::OpenFlags::None, &error);
	// A disc whose first track is music has no game on it to tell.
	if (!image || image->GetTrackMode(1) == CDImage::TrackMode::Audio)
		return nullptr;
	return image.release();
}

// The 2048 bytes of user data of a sector of the first track, counted from
// the track's own beginning (sector 16 is the volume descriptor).
bool discRead(void *disc, uint32_t sector, uint8_t *out2048)
{
	CDImage *image = static_cast<CDImage *>(disc);
	return image != nullptr && image->Seek(1, sector) && image->Read(CDImage::ReadMode::DataOnly, 1, out2048) == 1;
}

void discClose(void *disc)
{
	delete static_cast<CDImage *>(disc);
}

// The emulated machine's memory, for RetroAchievements and for netplay's
// checksums: the main RAM, the 1 KiB scratchpad, the BIOS image in use.
uint8_t *coreRam(uint32_t& size)
{
	size = Bus::g_ram_size;
	return Bus::g_ram;
}

uint8_t *coreScratchpad()
{
	return CPU::g_state.dcache.data();
}

uint32_t coreDisplayChanges()
{
	return g_gpu ? g_gpu->GetDisplayStartChangeCount() : 0;
}

uint32_t coreDrawCommands()
{
	return g_gpu ? g_gpu->GetDrawCommandCount() : 0;
}

uint32_t coreDisplayLines()
{
	return g_gpu ? g_gpu->GetDisplayVRAMHeight() : 0;
}

uint32_t coreDrawResolutionScale()
{
	return g_gpu ? g_gpu->GetDrawResolutionScale() : 1;
}

uint32_t coreVideoBlocks()
{
	return g_mdec.GetDecodedBlockCount();
}

const uint8_t *coreBios(uint32_t& size)
{
	size = Bus::BIOS_SIZE;
	return Bus::g_bios;
}

}
