/*
	PSSwanStation - the updater: a newer build from the releases page, put in
	place of this one.

	SPDX-License-Identifier: GPL-3.0-or-later

	The releases are GitHub's, of the repository this title's source is in:
	each has the title's folder as a ZIP (PSSwanStation-PS5-PPSA99248-buildN.zip,
	the folder PPSA99248 inside), and may have that file's SHA-256 beside it
	(<the ZIP's name>.sha256).

	The way it is done is PS5_RetroArch's, which was proven on a console: the
	ZIP is downloaded into <title folder>/.update, checked, and the program's
	files unpacked beside it; installing renames each old file into
	.update/backup and the new one into its place (a rename works on a file
	that is running), keeps a journal so that a start after a power cut puts
	the old files back, and then the title closes. The user's own files (games,
	BIOS, covers, cheats, data, network.cfg, frontend.cfg) are never in the set
	that is replaced.
*/
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fe::update
{

enum class State
{
	Idle,
	Checking,
	UpToDate,		// the newest release is this build, or older
	Available,		// a newer one: build, name, notes
	NoRelease,		// the repository has no release with a build in it
	Downloading,	// done of total bytes
	Verifying,		// the checksum, the ZIP, unpacking
	Ready,			// unpacked beside the title: install() puts it in place
	Installed,		// in place: the title must close now
	Failed,			// error says why; nothing was changed
};

struct Status
{
	State state = State::Idle;
	int build = 0;				// the release's build number
	std::string name;			// "Build 10"
	std::string notes;			// the release's text, plain
	std::string error;
	uint64_t done = 0, total = 0;
	double speed = 0;			// bytes a second while downloading
	bool reinstall = false;		// UpToDate: the newest release can be put in place again
};

struct Setup
{
	std::string appDir;			// the title's folder, with a trailing '/'
	int build = 0;				// the running build
	std::string titleId;		// "PPSA99248"
	// "https://api.github.com/repos/<owner>/<repository>/releases/latest"
	std::string latestUrl;
	// One HTTP(S) GET into memory (platform::httpGet).
	std::function<int(const std::string& url, std::vector<uint8_t>& out, unsigned seconds)> httpGet;
	// One HTTP(S) GET into a file, telling how far it is; false stops it.
	// Returns the status code or -1.
	std::function<int(const std::string& url, const std::string& path, uint64_t limit,
			const std::function<bool(uint64_t done, uint64_t total)>& progress)> httpDownload;
};

// Once at start-up, before anything else looks at the title's files: an
// install that a power cut interrupted is undone, and what an earlier update
// left in .update is removed. True when something was put back.
bool recover(const std::string& appDir);
void init(const Setup& setup);
// Asks the releases page, on a thread of its own. Does nothing while a
// download or an install is going on.
void check();
// Downloads, checks and unpacks the release that check() found.
void download();
// Up to date: downloads the newest release all the same, to put it in place
// again.
void reinstall();
// Stops a download (the state goes back to Available).
void cancel();
// Puts the new files in place. True: done, close the title. False: the old
// files are back and status().error says why.
bool install();
Status status();

// The build number in a release's tag or file name ("build10", "v10",
// "PSSwanStation-PS5-PPSA99248-build10.zip"): the last run of digits after
// "build", else the last run of digits. 0 when there is none.
int buildNumberIn(const std::string& text);

}
