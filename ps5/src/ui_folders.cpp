/*
	PSSwanStation - Settings, Library, Folders: game folders anywhere, and a
	folder of your choice for the BIOS, memory cards, save states, cheats,
	covers and texture packs (folders.h; after Crux2's Settings > Folders).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	The folder browser is made of lists: the places first (the title's folder,
	the folder for your files, the console's storage, each USB drive), then a
	folder's folders, with "Choose This Folder" and "Up One Folder" on top.
*/
#include "ui_internal.h"
#include "folders.h"

#include <functional>
#include <string>
#include <vector>

namespace fe::ui
{
using namespace platform;

namespace
{
std::function<void(const std::string&)> browseDone;

std::string parentOf(const std::string& path)
{
	std::string p = path;
	while (p.size() > 1 && p.back() == '/')
		p.pop_back();
	const size_t slash = p.rfind('/');
	return slash == std::string::npos || slash == 0 ? std::string("/") : p.substr(0, slash + 1);
}

bool isPlace(const std::string& path)
{
	for (const auto& place : folders::places())
		if (place.second == path)
			return true;
	return false;
}

const char *kindKey(folders::Kind kind)
{
	static const char *const keys[] = { "folder:bios", "folder:memory_cards", "folder:save_states", "folder:cheats",
		"folder:covers", "folder:textures" };
	return keys[kind];
}
}

// Opens the folder browser; `done` gets the folder chosen (nothing on Circle).
void browseFolder(const std::string& title, std::function<void(const std::string&)> done)
{
	browseDone = std::move(done);
	push(Page::FolderBrowse, 0, 0, title, "");
}

void folderBrowsePage(Frame& f)
{
	std::vector<Item> items;
	const std::string path = f.s2;
	if (path.empty())
	{
		for (const auto& place : folders::places())
		{
			const std::string to = place.second;
			Item item = action(place.second.compare(0, 8, "/mnt/usb") == 0 ? icon::Drive : icon::Folder, place.first,
					"", [to] {
						if (!stack.empty())
						{
							stack.back().s2 = to;
							stack.back().cursor = 0;
							stack.back().fresh = true;
						}
					});
			item.value = place.second;
			items.push_back(std::move(item));
		}
		if (!platform::usbAvailable())
			items.push_back(fact("USB drives", "Not available",
					"USB drives are listed when PSSwanStation has left its sandbox: enable Scan USB Drives (Settings, "
					"Library) and restart."));
	}
	else
	{
		items.push_back(action(icon::Check, "Choose This Folder", path, [path] {
			const std::function<void(const std::string&)> done = browseDone;
			pop();
			if (done)
				done(path);
		}));
		items.push_back(action(icon::Upload, "Up One Folder", "", [path] {
			if (!stack.empty())
			{
				stack.back().s2 = isPlace(path) ? std::string() : parentOf(path);
				stack.back().cursor = 0;
				stack.back().fresh = true;
			}
		}));
		const std::vector<std::string> subs = folders::subFolders(path);
		if (!subs.empty())
			items.push_back(header("Folders"));
		for (const std::string& name : subs)
		{
			const std::string to = path + name + "/";
			items.push_back(action(icon::Folder, name, "", [to] {
				if (!stack.empty())
				{
					stack.back().s2 = to;
					stack.back().cursor = 0;
					stack.back().fresh = true;
				}
			}));
		}
	}
	const Item *focused = menuPage(f, f.s, path.empty() ? std::string("Where to look") : path, items, 900);
	standardHints(focused, "Open");
}

void gameFoldersPage(Frame& f)
{
	std::vector<Item> items;
	items.push_back(action(icon::Download, "Add Game Folder",
			"Adds a folder of games from the console's storage or a USB drive. The folder and the folders inside it "
			"are searched for games, which stay where they are.",
			[] {
				browseFolder("Add Game Folder", [](const std::string& path) {
					std::string why;
					if (!folders::addGameFolder(path, why))
						message("Add Game Folder", why);
					else
						scanEverything();
				});
			}));
	const std::vector<std::string> list = folders::gameFolders();
	items.push_back(header(list.empty() ? "No folder added yet" : "Added"));
	for (const std::string& folder : list)
	{
		Item item = fact(folder, dirExists(folder) ? "" : "Not there now",
				"Searched for games with the folders inside it. Square takes it off the list; its games stay where "
				"they are.");
		item.icon = icon::Folder;
		item.alt = [folder] {
			folders::removeGameFolder(folder);
			scanEverything();
		};
		item.altHint = "Remove";
		items.push_back(std::move(item));
	}
	items.push_back(header("Found by PSSwanStation"));
	items.push_back(fact(shownRoot() + "games", "", "The title's own games folder, always searched."));
	items.push_back(fact("psx, ps1 or playstation on a USB drive", "", "A folder by one of these names at the top of a "
			"USB drive, searched with Scan USB Drives on."));
	standardHints(menuPage(f, "Game Folders", "", items, 1000));
}

// The rows of Settings, Library, Folders (the settings board takes them from
// frontendItems, kind 4).
void folderItems(std::vector<Item>& items)
{
	items.push_back(header("Folders"));
	{
		const size_t added = folders::gameFolders().size();
		Item item = action(icon::Folder, "Game Folders", "", [] { push(Page::GameFolders); });
		item.value = added == 0 ? std::string("None added") : format("%zu added", added);
		item.lit = added > 0;
		items.push_back(std::move(item));
	}
	for (int k = 0; k < folders::KindCount; k++)
	{
		const folders::Kind kind = (folders::Kind)k;
		const std::string own = folders::chosen(kind);
		Item item = action(icon::Folder, std::string(folders::name(kind)) + " Folder", "", [kind] {
			browseFolder(std::string(folders::name(kind)) + " Folder", [kind](const std::string& path) {
				std::string why;
				if (!folders::choose(kind, path, why))
					message(std::string(folders::name(kind)) + " Folder", why);
			});
		});
		item.key = kindKey(kind);
		item.value = own.empty() ? std::string("Default") : own;
		item.lit = !own.empty();
		item.info = "\n\nNow: " + folders::folder(kind);
		if (!own.empty())
		{
			item.alt = [kind] {
				std::string why;
				folders::choose(kind, "", why);
			};
			item.altHint = "Default";
		}
		items.push_back(std::move(item));
	}
}
}
