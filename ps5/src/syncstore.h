/*
	PSSwanStation - where the other copy of a folder is kept: a network share
	(netfiles.cpp, through smb.cpp and nfs.cpp) or a cloud storage (cloud.cpp).
	netfiles.cpp keeps the two copies the same through this.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fe
{
// Paths are a folder's ("memory cards"), then '/' and the file's path in it.
struct Store
{
	virtual ~Store() = default;
	// Every file below `top`, by path relative to it, with its size. False
	// when it could not be listed (then nothing is decided from what is
	// missing); a folder that is not there yet is an empty one.
	virtual bool walk(const std::string& top, bool recurse, bool dataFolder, std::map<std::string, uint64_t>& out) = 0;
	virtual bool read(const std::string& path, std::vector<uint8_t>& out) = 0;
	virtual bool write(const std::string& path, const void *data, size_t size, std::string& error) = 0;
	virtual bool remove(const std::string& path) = 0;
	// Why the last call failed.
	virtual std::string error() = 0;
	// What it is called in what is said: "the share", "Google Drive".
	virtual std::string name() = 0;
};
}
