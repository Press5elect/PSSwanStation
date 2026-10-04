/*
	SwanStation for PS5 - a disc image's serial number, read without starting it.

	SPDX-License-Identifier: GPL-3.0-or-later

	The emulator's own disc reader opens the image (any format it knows, on a
	share as well) and its own code finds the boot file's name, which is the
	serial: SLUS_005.94 is SLUS-00594. A game's details use it for a game that
	has not run yet.

	A file of its own because the emulator's disc header is C++17 and the rest
	of the frontend is built as C++20.
*/
#include "fe.h"

#include "common/cd_image.h"
#include "common/error.h"
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

}
