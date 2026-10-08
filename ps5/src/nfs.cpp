/*
	PSSwanStation - games (and files of the title's own) on an NFS share.

	SPDX-License-Identifier: GPL-3.0-or-later

	The third kind of network source, beside the SMB share (smb.cpp, through
	whose functions the rest of the title reaches this one) and the FTP server
	(ftp.cpp). network.cfg names the folder:

		path = nfs://192.168.1.10/volume1/games/psx
		path = nfs://192.168.1.10/volume1/games/psx?uid=1026&gid=100&version=4

	NFS through libnfs (github.com/sahlberg/libnfs, LGPL-2.1, built into the
	title), versions 3 and 4, with AUTH_SYS: the server is told a user and a
	group number and trusts them. They are 0 unless the line says otherwise;
	a NAS that "squashes" root then treats the console as its nobody, who can
	read what everyone may read. To write (memory card copies, files kept on
	the share), give the numbers of a user who may write there.

	Where the export ends and the folder begins is the server's to say: NFS 3
	lists its exports (the mount protocol), and the longest one the folder
	begins with is mounted; the rest is the path inside it. A server that does
	not list them (an NFS 4 server without the mount protocol), or a line
	with version=4, is mounted at its root ("/"), from which NFS 4 reaches
	every export by its path.

	One connection (an nfs_context) for each folder of network.cfg, made the
	first time it is needed and shared by every thread, one request at a time:
	a libnfs context is not thread-safe. The port is asked first (smb.cpp's
	probe), so that a server that is off costs five seconds and not libnfs's
	own time limits; a request is then given a minute, for a NAS whose disks
	are asleep. A connection that failed is tried again after 30 seconds, or
	at once when the user asks. A request that fails on a connection that is
	gone is made once more on a new one.

	The console's sockets as libsmb2 has them (ps5/nfs/compat.c): left
	blocking, read only after poll says there is something. libnfs binds its
	source port to one of the reserved ports below 1024 where it may; where
	the console refuses that, a server that accepts only those (Linux's
	default "secure" exports) needs the export's "insecure" option.
*/
#include "net.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <sys/stat.h>

extern "C"
{
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <libnfs-raw-mount.h>
#include <libnfs-raw-nfs.h>
}

namespace fe::nfs
{
namespace
{
using namespace smb::net;

constexpr int NfsPort = 2049;
constexpr int NfsVersion4 = 4;	// libnfs's NFS_V4 (its NFS 4 header is not needed here)
constexpr size_t PieceBytes = 1 << 20;	// the most a read or write asks for at once

// A folder of network.cfg and its connection.
struct Mount
{
	std::string folder;			// nfs://server/full/path, as the title names it
	std::string server;			// the IP address
	std::string fullPath;		// /full/path on the server
	int uid = 0, gid = 0, version = 0;	// version 0: 3, else 4 where the server lists no exports
	std::recursive_mutex mutex;
	nfs_context *context = nullptr;
	std::string exportPath;		// what was mounted
	unsigned generation = 0;	// a new connection: files opened on the last must be opened again
	long long failedAt = 0;
	std::string error;

	~Mount()
	{
		drop();
	}

	void drop()
	{
		if (context != nullptr)
			nfs_destroy_context(context);
		context = nullptr;
	}

	// The path inside the export of a path below `folder`.
	std::string inside(const std::string& path) const
	{
		std::string rest = fullPath + path.substr(folder.size());
		if (exportPath != "/" && rest.compare(0, exportPath.size(), exportPath) == 0)
			rest = rest.substr(exportPath.size());
		if (rest.empty() || rest[0] != '/')
			rest = "/" + rest;
		return rest;
	}

	bool connect()
	{
		if (context != nullptr)
			return true;
		if (failedAt != 0 && nowMs() - failedAt < RetrySeconds * 1000ll)
			return false;
		const Probe answer = probe(server, NfsPort, "nfs");
		if (answer == Probe::Cancelled)
			return false;
		if (answer == Probe::NoName)
			return failed("\"" + server + "\" is a name: give the NFS server's IP address in network.cfg (192.168.x.x)");
		if (answer == Probe::Closed)
			return failed(server + " does not answer (NFS, port 2049): is it switched on, is NFS switched on in it, "
					"and is the address in network.cfg right?");
		// The export: the longest one the folder begins with, as the server lists them.
		exportPath.clear();
		if (version != 4)
		{
			if (exportnode *exports = mount_getexports_timeout(server.c_str(), RequestSeconds * 1000))
			{
				for (exportnode *node = exports; node != nullptr; node = node->ex_next)
				{
					const std::string candidate = node->ex_dir != nullptr ? node->ex_dir : "";
					const bool contains = !candidate.empty() && fullPath.compare(0, candidate.size(), candidate) == 0
							&& (fullPath.size() == candidate.size() || fullPath[candidate.size()] == '/'
							|| candidate == "/");
					if (contains && candidate.size() > exportPath.size())
						exportPath = candidate;
				}
				mount_free_export_list(exports);
			}
		}
		const bool asFour = version == 4 || exportPath.empty();
		if (asFour)
			exportPath = "/";
		context = nfs_init_context();
		if (context == nullptr)
			return failed("the NFS library could not start");
		nfs_set_timeout(context, RequestSeconds * 1000);
		nfs_set_uid(context, uid);
		nfs_set_gid(context, gid);
		if (asFour)
			nfs_set_version(context, NfsVersion4);
		if (nfs_mount(context, server.c_str(), exportPath.c_str()) != 0)
		{
			const std::string why = stripped(nfs_get_error(context));
			drop();
			return failed("nfs://" + server + exportPath + " could not be mounted (" + why + "): is the console's "
					"address allowed in the export, and, where the export is \"secure\", is \"insecure\" (non-reserved "
					"ports) switched on?");
		}
		generation++;
		failedAt = 0;
		error.clear();
		diag::mark("nfs: %s mounted (NFS %d) for %s", (server + ":" + exportPath).c_str(), asFour ? 4 : 3,
				folder.c_str());
		return true;
	}

	bool failed(const std::string& why)
	{
		failedAt = nowMs();
		error = why;
		fail(why);
		return false;
	}

	// After a request failed: whether the connection itself is gone (then it
	// is dropped, and the request may be made once more on a new one).
	bool lost(int code)
	{
		if (code == -ENOENT || code == -EACCES || code == -EPERM || code == -ENOTDIR || code == -EISDIR
				|| code == -EEXIST || code == -ENOTEMPTY || code == -EROFS || code == -ENOSPC)
			return false;
		drop();
		return true;
	}
};

std::mutex mountsMutex;
std::map<std::string, std::unique_ptr<Mount>> mounts;

// "nfs://server/path?uid=1&gid=2&version=4" into its parts. False when it is
// not one.
bool parse(const std::string& value, std::string& server, std::string& path, int& uid, int& gid, int& version)
{
	if (lowercase(value.substr(0, 6)) != "nfs://")
		return false;
	std::string rest = value.substr(6);
	std::string query;
	const size_t question = rest.find('?');
	if (question != std::string::npos)
	{
		query = rest.substr(question + 1);
		rest = rest.substr(0, question);
	}
	const size_t slash = rest.find('/');
	server = rest.substr(0, slash);
	path = slash == std::string::npos ? "/" : rest.substr(slash);
	while (path.size() > 1 && path.back() == '/')
		path.pop_back();
	uid = gid = version = 0;
	size_t at = 0;
	while (at < query.size())
	{
		const size_t end = std::min(query.find('&', at), query.size());
		const std::string pair = query.substr(at, end - at);
		const size_t equals = pair.find('=');
		const std::string key = lowercase(pair.substr(0, equals));
		const int number = equals == std::string::npos ? 0 : atoi(pair.c_str() + equals + 1);
		if (key == "uid")
			uid = number;
		else if (key == "gid")
			gid = number;
		else if (key == "version")
			version = number == 4 ? 4 : 0;
		at = end + 1;
	}
	return !server.empty();
}

// The mount a path is below, and the path inside its export (once connected).
Mount *locate(const std::string& path)
{
	std::lock_guard<std::mutex> lock(mountsMutex);
	Mount *best = nullptr;
	for (auto& [folder, mount] : mounts)
		if (path.compare(0, folder.size(), folder) == 0 && (path.size() == folder.size() || path[folder.size()] == '/')
				&& (best == nullptr || folder.size() > best->folder.size()))
			best = mount.get();
	return best;
}

// A file read as the game asks, with a handle opened again when the
// connection was made anew.
class Stream : public StreamFile
{
public:
	Stream(Mount *mount, const std::string& path, uint64_t bytes) : StreamFile(bytes), mount(mount), path(path) {}

	~Stream() override
	{
		std::lock_guard<std::recursive_mutex> lock(mount->mutex);
		if (handle != nullptr && mount->context != nullptr && generation == mount->generation)
			nfs_close(mount->context, handle);
	}

protected:
	int fetch(uint8_t *to, uint64_t offset, size_t want) override
	{
		std::lock_guard<std::recursive_mutex> lock(mount->mutex);
		Busy busy;
		for (int attempt = 0; attempt < 2; attempt++)
		{
			if (!mount->connect())
				break;
			if (handle == nullptr || generation != mount->generation)
			{
				handle = nullptr;
				const int opened = nfs_open(mount->context, mount->inside(path).c_str(), O_RDONLY, &handle);
				if (opened != 0)
				{
					handle = nullptr;
					if (!mount->lost(opened) || attempt == 1)
						break;
					continue;
				}
				generation = mount->generation;
			}
			const int got = nfs_pread(mount->context, handle, to, std::min(want, PieceBytes), offset);
			if (got >= 0)
				return got;
			handle = nullptr;
			if (!mount->lost(got))
				break;
			if (attempt == 1)
				fail("nfs://" + mount->server + " stopped answering while " + lastComponent(path) + " was read");
		}
		hasFailed = true;
		return -1;
	}

private:
	Mount *mount;
	std::string path;
	nfsfh *handle = nullptr;
	unsigned generation = 0;
};

// Runs `request` on the mount's connection, once more on a new one when the
// connection was gone: its result, 0 or more for success, -errno else.
template <typename Request>
int request(Mount *mount, Request&& requestOn)
{
	std::lock_guard<std::recursive_mutex> lock(mount->mutex);
	Busy busy;
	int code = -EIO;
	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (!mount->connect())
			return -EIO;
		code = requestOn(mount->context);
		if (code >= 0 || !mount->lost(code))
			return code;
	}
	fail("nfs://" + mount->server + " stopped answering");
	return code;
}

} // namespace

bool isPath(const std::string& path)
{
	return path.rfind("nfs://", 0) == 0;
}

std::string addFolder(const std::string& value)
{
	std::string server, path;
	int uid, gid, version;
	if (!parse(value, server, path, uid, gid, version))
		return "";
	const std::string folder = "nfs://" + server + (path == "/" ? "" : path);
	std::lock_guard<std::mutex> lock(mountsMutex);
	std::unique_ptr<Mount>& mount = mounts[folder];
	if (mount == nullptr)
		mount = std::make_unique<Mount>();
	mount->folder = folder;
	mount->server = server;
	mount->fullPath = path;
	mount->uid = uid;
	mount->gid = gid;
	mount->version = version;
	return folder;
}

std::vector<smb::Entry> list(const std::string& path)
{
	std::vector<smb::Entry> entries;
	Mount *mount = locate(path);
	if (mount == nullptr)
		return entries;
	std::string base = path;
	while (!base.empty() && base.back() == '/')
		base.pop_back();
	request(mount, [&](nfs_context *context) {
		entries.clear();
		nfsdir *dir = nullptr;
		const int opened = nfs_opendir(context, mount->inside(path).c_str(), &dir);
		if (opened != 0)
		{
			if (!(opened == -ENOENT || opened == -EACCES || opened == -ENOTDIR))
				return opened;
			note("cannot list " + path.substr(6) + ": " + stripped(nfs_get_error(context)));
			return 0;
		}
		while (nfsdirent *entry = nfs_readdir(context, dir))
		{
			const std::string name = entry->name != nullptr ? entry->name : "";
			if (name.empty() || name == "." || name == "..")
				continue;
			smb::Entry item;
			item.name = name;
			item.path = base + "/" + name;
			item.directory = entry->type == NF3DIR || S_ISDIR(entry->mode);
			item.size = entry->size;
			entries.push_back(std::move(item));
		}
		nfs_closedir(context, dir);
		return 0;
	});
	return entries;
}

int stat(const std::string& path, smb::Entry& entry)
{
	Mount *mount = locate(path);
	if (mount == nullptr)
		return 0;
	nfs_stat_64 st{};
	const int code = request(mount, [&](nfs_context *context) {
		return nfs_stat64(context, mount->inside(path).c_str(), &st);
	});
	if (code < 0)
		return code == -ENOENT || code == -ENOTDIR || code == -EACCES ? 0 : -1;
	entry.name = lastComponent(path);
	entry.path = path;
	entry.directory = S_ISDIR(st.nfs_mode);
	entry.size = st.nfs_size;
	return 1;
}

smb::File *openStream(const std::string& path)
{
	Mount *mount = locate(path);
	if (mount == nullptr)
		return nullptr;
	smb::Entry entry;
	if (nfs::stat(path, entry) != 1 || entry.directory)
		return nullptr;
	return new Stream(mount, path, entry.size);
}

bool makeFolders(const std::string& path)
{
	Mount *mount = locate(path);
	if (mount == nullptr)
		return false;
	const int code = request(mount, [&](nfs_context *context) {
		const std::string inside = mount->inside(path);
		for (size_t at = 1; at <= inside.size(); at++)
			if (at == inside.size() || inside[at] == '/')
			{
				const int made = nfs_mkdir(context, inside.substr(0, at).c_str());
				if (made != 0 && made != -EEXIST)
				{
					nfs_stat_64 st{};
					if (nfs_stat64(context, inside.substr(0, at).c_str(), &st) != 0 || !S_ISDIR(st.nfs_mode))
						return made;
				}
			}
		return 0;
	});
	return code >= 0;
}

bool writeFile(const std::string& path, const void *data, size_t bytes, std::string& error)
{
	Mount *mount = locate(path);
	if (mount == nullptr)
	{
		error = "not on an NFS folder of network.cfg";
		return false;
	}
	const size_t slash = path.find_last_of('/');
	if (slash != std::string::npos && slash > mount->folder.size() && !makeFolders(path.substr(0, slash)))
	{
		error = "its folder could not be made on the share";
		return false;
	}
	// Written beside it and then put in its place: a copy cut short is never
	// taken for the file.
	const int code = request(mount, [&](nfs_context *context) {
		const std::string inside = mount->inside(path), part = inside + ".part";
		nfsfh *handle = nullptr;
		int result = nfs_open2(context, part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666, &handle);
		if (result != 0)
			return result;
		const uint8_t *from = static_cast<const uint8_t *>(data);
		size_t done = 0;
		while (done < bytes)
		{
			const int wrote = nfs_pwrite(context, handle, from + done, std::min(bytes - done, PieceBytes), done);
			if (wrote <= 0)
			{
				nfs_close(context, handle);
				nfs_unlink(context, part.c_str());
				return wrote < 0 ? wrote : -EIO;
			}
			done += (size_t)wrote;
		}
		result = nfs_close(context, handle);
		if (result == 0)
			result = nfs_rename(context, part.c_str(), inside.c_str());
		if (result != 0)
			nfs_unlink(context, part.c_str());
		return result;
	});
	if (code < 0)
	{
		error = code == -EACCES || code == -EPERM || code == -EROFS
				? "the share does not let this console write there (the uid and gid in network.cfg's nfs:// line)"
				: "the share could not be written (" + std::string(strerror(-code)) + ")";
		return false;
	}
	return true;
}

bool remove(const std::string& path)
{
	Mount *mount = locate(path);
	if (mount == nullptr)
		return false;
	const int code = request(mount, [&](nfs_context *context) {
		return nfs_unlink(context, mount->inside(path).c_str());
	});
	return code >= 0 || code == -ENOENT;
}

void retryNow()
{
	std::lock_guard<std::mutex> lock(mountsMutex);
	for (auto& [folder, mount] : mounts)
	{
		std::unique_lock<std::recursive_mutex> own(mount->mutex, std::try_to_lock);
		if (own.owns_lock())
			mount->failedAt = 0;
	}
}

}
