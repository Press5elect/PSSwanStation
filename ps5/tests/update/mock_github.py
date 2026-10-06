#!/usr/bin/env python3
"""
PSSwanStation - the updater's test: a stand-in for GitHub's releases.

SPDX-License-Identifier: GPL-3.0-or-later

    mock_github.py <work folder> [<a real release ZIP>]

Builds the test releases (small ZIPs laid out as the title's real one is, and
the ones an updater must refuse), writes them to <work folder>/fixtures/ with
a list of each ZIP's files (manifest.txt: CRC-32, size, path), and serves them
on 127.0.0.1 the way GitHub does:

    /repos/test/<release>/releases/latest    the "latest release" JSON, or 404
    /download/<release>/<file>               a redirect, as GitHub's is, to
    /objects/<release>/<file>                the file

The port is written to <work folder>/port once the server listens.
"""
import hashlib
import io
import json
import os
import sys
import time
import warnings
import zipfile
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TITLE = "PPSA99248"
BASE = "http://fixtures.invalid"    # replaced by the server's own address when served


def noise(seed, size):
    """Bytes that do not compress, the same for the same seed."""
    out = bytearray()
    block = hashlib.sha256(seed.encode()).digest()
    while len(out) < size:
        block = hashlib.sha256(block).digest()
        out += block
    return bytes(out[:size])


def param_json(title_id):
    return json.dumps({
        "ageLevel": {"default": 0},
        "applicationCategoryType": 0,
        "applicationDrmType": "free",
        "contentId": "UP9000-%s_00-SWANSTATIONPS500" % title_id,
        "contentVersion": "01.000.000",
        "localizedParameters": {"defaultLanguage": "en-US", "en-US": {"titleName": "PSSwanStation"}},
        "masterVersion": "01.00",
        "titleId": title_id,
        "versionFileUri": "",
    }, indent=2, sort_keys=True).encode()


def release_files(build, title_id=TITLE, small=False):
    """The title's folder as a release has it: path -> bytes, None for a folder."""
    tag = "build %d " % build
    files = {
        "": None,
        "CHANGELOG.txt": (tag + "changes\n").encode() * 20,
        # Larger than the block the updater writes in, and partly compressible.
        "eboot.bin": b"\x7fELF" + (tag.encode() * 4000 + noise(tag + "eboot", 70000)) * (1 if small else 9),
        "sandbox-elevator.elf": b"\x7fELF" + noise(tag + "elevator", 5000),
        "bios/": None,
        "sce_module/": None,
        "sce_module/libc.prx": noise(tag + "libc", 40000),
        "assets/": None,
        "assets/patches.zip": noise(tag + "patches", 9000),
        "BUILD.txt": ("PSSwanStation, build %d, built 2026-10-06\n" % build).encode(),
        "sce_sys/": None,
        "sce_sys/icon0.png": b"\x89PNG" + noise(tag + "icon", 3000),
        "sce_sys/param.json": param_json(title_id),
        "network.cfg": b"# the release's template: never over a user's own\n",
        "licenses/": None,
        "licenses/GPL-3.0.txt": b"GNU GENERAL PUBLIC LICENSE\n" * 300,
        "README.txt": (tag + "read me\n").encode() * 50,
        "cheats/": None,
        "covers/": None,
        "games/": None,
        # What the build before did not have: a file, and one in new folders.
        "lapy.elf": b"\x7fELF" + noise(tag + "lapy", 30000),
        "assets/new/deep/file.bin": noise(tag + "deep", 2000),
    }
    if not small:
        files.update({
            "assets/cheats.zip": noise(tag + "cheats", 120000),
            "assets/gamedb.zip": noise(tag + "gamedb", 60000),
            "assets/empty.dat": b"",
            "sce_sys/pic0.dds": b"DDS " + bytes(200000),
            "sce_sys/pic1.dds": b"DDS " + bytes(200000),
            "licenses/README.txt": b"The licences of what this title is made of.\n",
            "licenses/MIT.txt": b"Permission is hereby granted...\n" * 30,
            "lapy-manifest.json": b'{"payloads": []}\n',
        })
    return files


def make_zip(files, top=TITLE, extra=None, stored=("sce_sys/icon0.png",)):
    """A ZIP with the attributes the real one has: made on Unix, folders
    0777, files 0666. `extra` is a list of (ZipInfo or full name, bytes) put
    in after the title's own entries."""
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for path, data in files.items():
            info = zipfile.ZipInfo(top + "/" + path, date_time=(2026, 10, 6, 12, 0, 0))
            info.create_system = 3
            if data is None:
                info.external_attr = (0o40777 << 16) | 0x10
                archive.writestr(info, b"")
            else:
                info.external_attr = 0o100666 << 16
                info.compress_type = zipfile.ZIP_STORED if path in stored else zipfile.ZIP_DEFLATED
                archive.writestr(info, data)
        for name, data in extra or []:
            info = name
            if not isinstance(info, zipfile.ZipInfo):
                info = zipfile.ZipInfo(name, date_time=(2026, 10, 6, 12, 0, 0))
                info.create_system = 3
                info.external_attr = 0o100666 << 16
                info.compress_type = zipfile.ZIP_DEFLATED
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                archive.writestr(info, data)
    return buffer.getvalue()


def symlink_entry(name):
    info = zipfile.ZipInfo(name, date_time=(2026, 10, 6, 12, 0, 0))
    info.create_system = 3
    info.external_attr = 0o120777 << 16
    return info


def bomb_zip(path):
    """Unpacks to a little over 1 GiB, all of it in a file the updater would
    not even unpack. Written to a file: made once, kept between runs."""
    if os.path.exists(path):
        return
    with zipfile.ZipFile(path + ".part", "w", zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
        for name, data in release_files(10, small=True).items():
            info = zipfile.ZipInfo(TITLE + "/" + name, date_time=(2026, 10, 6, 12, 0, 0))
            info.create_system = 3
            info.external_attr = ((0o40777 << 16) | 0x10) if data is None else (0o100666 << 16)
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data or b"")
        info = zipfile.ZipInfo(TITLE + "/games/big.bin", date_time=(2026, 10, 6, 12, 0, 0))
        info.create_system = 3
        info.external_attr = 0o100666 << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        zeros = bytes(1 << 20)
        with archive.open(info, "w", force_zip64=True) as out:
            for _ in range(1025):
                out.write(zeros)
    os.replace(path + ".part", path)


RELEASE_NOTES = (
    "## What's new in build 10\r\n"
    "\r\n"
    "* **Faster** start: the `eboot.bin` is “smaller” \U0001F680\r\n"
    "- Netplay — two consoles, one game\r\n"
    "  + nested: memory cards are shared\r\n"
    "\r\n"
    "\r\n"
    "\r\n"
    "\r\n"
    "\r\n"
    "### Fixes\r\n"
    "> Thanks to everyone who sent a log.\r\n"
    "1. A path with a back\\slash and a \"quote\" and a\ttab\r\n"
    "---\r\n"
    "Full list: https://github.com/Press5elect/PSSwanStation/compare/build9...build10\r\n"
)


def user(login="Press5elect"):
    api = "https://api.github.com/users/" + login
    return {
        "login": login, "id": 4242424, "node_id": "MDQ6VXNlcjQyNDI0MjQ=",
        "avatar_url": "https://avatars.githubusercontent.com/u/4242424?v=4", "gravatar_id": "",
        "url": api, "html_url": "https://github.com/" + login,
        "followers_url": api + "/followers", "following_url": api + "/following{/other_user}",
        "gists_url": api + "/gists{/gist_id}", "starred_url": api + "/starred{/owner}{/repo}",
        "subscriptions_url": api + "/subscriptions", "organizations_url": api + "/orgs",
        "repos_url": api + "/repos", "events_url": api + "/events{/privacy}",
        "received_events_url": api + "/received_events", "type": "User", "user_view_type": "public",
        "site_admin": False,
    }


def release_json(repo, tag, name, body, assets, digests=True):
    """GitHub's answer to releases/latest, every field of it. `assets` is a
    list of (file name, bytes or a path, the size to state or None)."""
    api = "https://api.github.com/repos/Press5elect/PSSwanStation/releases"
    listed = []
    for number, (asset_name, data, stated) in enumerate(assets):
        if isinstance(data, str):
            size = os.path.getsize(data)
            digest = hashlib.sha256()
            with open(data, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    digest.update(block)
            digest = digest.hexdigest()
        else:
            size = len(data)
            digest = hashlib.sha256(data).hexdigest()
        listed.append({
            "url": "%s/assets/%d" % (api, 300000000 + number),
            "id": 300000000 + number,
            "node_id": "RA_kwDOPSSwan%d" % number,
            "name": asset_name,
            "label": None if number else "",
            "uploader": user(),
            "content_type": "application/zip" if asset_name.endswith(".zip") else "application/octet-stream",
            "state": "uploaded",
            "size": size if stated is None else stated,
            "digest": ("sha256:" + digest) if digests else None,
            "download_count": 1234 + number,
            "created_at": "2026-10-06T10:11:12Z",
            "updated_at": "2026-10-06T10:11:14Z",
            "browser_download_url": "%s/download/%s/%s" % (BASE, repo, asset_name),
        })
    return {
        "url": api + "/250000001",
        "assets_url": api + "/250000001/assets",
        "upload_url": "https://uploads.github.com/repos/Press5elect/PSSwanStation/releases/250000001/assets{?name,label}",
        "html_url": "https://github.com/Press5elect/PSSwanStation/releases/tag/" + tag,
        "id": 250000001,
        "author": user(),
        "node_id": "RE_kwDOPSSwanM4O5uCB",
        "tag_name": tag,
        "target_commitish": "main",
        "name": name,
        "draft": False,
        "immutable": False,
        "prerelease": False,
        "created_at": "2026-10-06T09:00:00Z",
        "updated_at": "2026-10-06T10:12:00Z",
        "published_at": "2026-10-06T10:12:00Z",
        "assets": listed,
        "tarball_url": "https://api.github.com/repos/Press5elect/PSSwanStation/tarball/" + tag,
        "zipball_url": "https://api.github.com/repos/Press5elect/PSSwanStation/zipball/" + tag,
        "body": body,
        "reactions": {"url": api + "/250000001/reactions", "total_count": 3, "+1": 2, "-1": 0, "laugh": 0,
                      "hooray": 1, "confused": 0, "heart": 0, "rocket": 0, "eyes": 0},
        "mentions_count": 1.0e0,
    }


class Releases:
    def __init__(self, work, real_zip):
        self.dir = os.path.join(work, "fixtures")
        os.makedirs(self.dir, exist_ok=True)
        self.answers = {}   # repo -> (status, JSON text)
        self.files = {}     # (repo, name) -> bytes or a path
        self.slow = set()
        self.build(real_zip)

    def put(self, repo, tag, name, body, assets, status=200, digests=True, ascii_only=False, raw=None,
            escape_slashes=False):
        folder = os.path.join(self.dir, repo)
        os.makedirs(folder, exist_ok=True)
        if raw is None:
            raw = json.dumps(release_json(repo, tag, name, body, assets, digests), ensure_ascii=ascii_only,
                             indent=2)
            if escape_slashes:
                raw = raw.replace("/", "\\/")
        self.answers[repo] = (status, raw)
        with open(os.path.join(folder, "latest.json"), "w", encoding="utf-8") as f:
            f.write(raw)
        with open(os.path.join(folder, "status"), "w") as f:
            f.write("%d\n" % status)
        for asset_name, data, _ in assets:
            self.files[(repo, asset_name)] = data
            if isinstance(data, str):
                with open(os.path.join(folder, asset_name + ".path"), "w") as f:
                    f.write(data)
            else:
                with open(os.path.join(folder, asset_name), "wb") as f:
                    f.write(data)
            source = data if isinstance(data, str) else io.BytesIO(data)
            if asset_name.endswith(".zip") and TITLE in asset_name:
                try:
                    with zipfile.ZipFile(source) as archive, \
                            open(os.path.join(folder, "manifest.txt"), "w", encoding="utf-8") as f:
                        for info in archive.infolist():
                            if not info.is_dir() and info.filename.startswith(TITLE + "/"):
                                f.write("%08x %d %s\n" % (info.CRC, info.file_size, info.filename[len(TITLE) + 1:]))
                except zipfile.BadZipFile:
                    pass

    def with_zip(self, repo, data, build=10, checksum="right", stated=None, digests=False, tag=None, name=None,
                 zip_name=None, more=None, body="Test release.", **options):
        """A release of one ZIP and, unless told otherwise, its .sha256."""
        zip_name = zip_name or "PSSwanStation-PS5-%s-build%d.zip" % (TITLE, build)
        if isinstance(data, str):
            digest = hashlib.sha256()
            with open(data, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    digest.update(block)
            digest = digest.hexdigest()
        else:
            digest = hashlib.sha256(data).hexdigest()
        assets = [(zip_name, data, stated)]
        if checksum == "right":
            assets.append((zip_name + ".sha256", ("%s  %s\n" % (digest, zip_name)).encode(), None))
        elif checksum == "bare":
            assets.append((zip_name + ".sha256", digest.upper().encode(), None))
        elif checksum == "wrong":
            assets.append((zip_name + ".sha256", ("%s  %s\n" % ("0" * 63 + "1", zip_name)).encode(), None))
        elif checksum == "garbage":
            assets.append((zip_name + ".sha256", b"<html>Not a checksum</html>\n", None))
        assets += more or []
        self.put(repo, tag or "build%d" % build, "Build %d" % build if name is None else name,
                 body, assets, digests=digests, **options)

    def build(self, real_zip):
        good = make_zip(release_files(10))
        good_name = "PSSwanStation-PS5-%s-build10.zip" % TITLE
        older = make_zip(release_files(9, small=True))

        # The usual release: an escaped name, Markdown notes, the ZIP with its
        # checksum, an older ZIP, the source, and a ZIP that is not the title's.
        self.put("good", "build10", "PSSwanStation build 10 — “faster” \U0001F680", RELEASE_NOTES, [
            ("psswanstation-ps5-source-build10.tar.gz", b"\x1f\x8b" + noise("source", 500), None),
            ("PSSwanStation-PS5-%s-build9.zip" % TITLE, older, None),
            (good_name, good, None),
            (good_name + ".sha256", ("%s  %s\n" % (hashlib.sha256(good).hexdigest(), good_name)).encode(), None),
            ("some-other-tool.zip", make_zip({"": None, "tool.txt": b"x"}, top="tool"), None),
        ], ascii_only=True)
        # The same, as GitHub really sends it (UTF-8, no \u escapes), and with
        # every '/' escaped, which JSON allows.
        self.with_zip("good-utf8", good, name="Build 10 — “faster”", digests=True)
        self.with_zip("good-slashes", good, escape_slashes=True, checksum="bare")
        self.with_zip("small", make_zip(release_files(10, small=True)))
        self.with_zip("long-notes", good, body="# Long\n" + "\u00e9t\u00e9 " * 3000)
        self.with_zip("uptodate", make_zip(release_files(9, small=True)), build=9)
        self.put("norelease", "", "", "", [], status=404, raw=json.dumps({
            "message": "Not Found",
            "documentation_url": "https://docs.github.com/rest/releases/releases#get-the-latest-release",
            "status": "404"}))
        self.put("ratelimit", "", "", "", [], status=403, raw=json.dumps({
            "message": "API rate limit exceeded for 203.0.113.7.",
            "documentation_url": "https://docs.github.com/rest/overview/rate-limits-for-the-rest-api"}))
        self.put("garbage", "", "", "", [], raw="<!DOCTYPE html><html><body>Sign in</body></html>")
        whole = json.dumps(release_json("good", "build10", "Build 10", "x", [(good_name, good, None)]))
        self.put("truncated", "", "", "", [], raw=whole[:len(whole) // 3])
        self.put("deep", "", "", "", [], raw="[" * 100000)

        # Where the build number comes from.
        self.with_zip("tag-v", good, tag="v10")
        self.with_zip("tag-latest", good, tag="latest", name="")
        self.with_zip("no-build", good, tag="latest", zip_name="PSSwanStation-PS5-%s.zip" % TITLE)
        self.put("no-zip", "build11", "Build 11", "The files are still being uploaded.", [
            ("psswanstation-ps5-source-build11.tar.gz", b"\x1f\x8b" + noise("source", 500), None)])

        # Checksums and sizes.
        self.with_zip("no-checksum", good, checksum=None)
        self.with_zip("digest-only", good, checksum=None, digests=True)
        self.with_zip("bad-checksum", good, checksum="wrong")
        self.with_zip("bad-checksum-file", good, checksum="garbage")
        self.with_zip("bad-size", good, stated=len(good) + 1)
        flipped = bytearray(good)
        flipped[len(flipped) // 2] ^= 0x40
        self.put("bad-digest", "build10", "Build 10", "x", [(good_name, bytes(flipped), None)])
        self.answers["bad-digest"] = (200, self.answers["bad-digest"][1].replace(
            hashlib.sha256(bytes(flipped)).hexdigest(), hashlib.sha256(good).hexdigest()))
        with open(os.path.join(self.dir, "bad-digest", "latest.json"), "w", encoding="utf-8") as f:
            f.write(self.answers["bad-digest"][1])
        self.with_zip("slow", good)
        self.slow.add("slow")

        # ZIPs that must be refused whole.
        files = release_files(10, small=True)
        self.with_zip("zip-dotdot", make_zip(files, extra=[(TITLE + "/../evil.txt", b"evil")]))
        self.with_zip("zip-dotdot-deep", make_zip(files, extra=[(TITLE + "/assets/../../evil.txt", b"evil")]))
        self.with_zip("zip-absolute", make_zip(files, extra=[("/tmp/psswan-evil.txt", b"evil")]))
        self.with_zip("zip-symlink", make_zip(files, extra=[(symlink_entry(TITLE + "/assets/link"), b"/etc")]))
        self.with_zip("zip-backslash", make_zip(files, extra=[(TITLE + "\\..\\evil.txt", b"evil")]))
        self.with_zip("zip-control", make_zip(files, extra=[(TITLE + "/assets/a\x01b.bin", b"evil")]))
        self.with_zip("zip-longname", make_zip(files, extra=[(TITLE + "/assets/" + "n" * 300, b"evil")]))
        self.with_zip("zip-twotop", make_zip(files, extra=[("OTHER/eboot.bin", b"evil")]))
        self.with_zip("zip-rootfile", make_zip(files, extra=[("readme-at-the-root.txt", b"evil")]))
        self.with_zip("zip-prefix", make_zip(files, extra=[(TITLE + "x/eboot.bin", b"evil")]))
        self.with_zip("zip-duplicate", make_zip(files, extra=[(TITLE + "/eboot.bin", b"evil")]))
        self.with_zip("zip-file-under-file", make_zip(files, extra=[
            (TITLE + "/assets/a", b"1"), (TITLE + "/assets/a/b", b"2")]))
        self.with_zip("zip-wrong-title", make_zip(release_files(10, title_id="PPSA00000", small=True)))
        self.with_zip("zip-other-top", make_zip(release_files(10, title_id="PPSA00000", small=True),
                                                top="PPSA00000"))
        self.with_zip("zip-no-eboot", make_zip({k: v for k, v in files.items() if k != "eboot.bin"}))
        self.with_zip("zip-no-param", make_zip({k: v for k, v in files.items() if k != "sce_sys/param.json"}))
        self.with_zip("zip-eboot-folder", make_zip({(k + "/" if k == "eboot.bin" else k): (
            None if k == "eboot.bin" else v) for k, v in files.items()}))
        self.with_zip("zip-not-a-zip", noise("not a zip", 50000))
        self.with_zip("zip-cut-short", good[:len(good) // 2])
        self.with_zip("zip-empty", make_zip({}, extra=[]))
        # A stored file with one bit changed: the list is sound, the data is not.
        damaged = bytearray(make_zip(files))
        at = damaged.find(files["sce_sys/icon0.png"])
        assert at > 0
        damaged[at + 100] ^= 1
        self.with_zip("zip-damaged-data", bytes(damaged))
        bomb = os.path.join(self.dir, "bomb.zip")
        bomb_zip(bomb)
        self.with_zip("zip-too-large", bomb)

        if real_zip and os.path.exists(real_zip):
            self.with_zip("real", real_zip, build=9, zip_name=os.path.basename(real_zip), digests=True)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    releases = None

    def log_message(self, *args):
        pass

    def send(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parts = self.path.split("?")[0].strip("/").split("/")
        base = "http://" + self.headers.get("Host", "127.0.0.1")
        if len(parts) == 5 and parts[0] == "repos" and parts[3] == "releases" and parts[4] == "latest":
            status, text = self.releases.answers.get(parts[2], (404, '{"message": "Not Found"}'))
            self.send(status, text.replace(BASE, base).replace(BASE.replace("/", "\\/"), base.replace("/", "\\/"))
                      .encode("utf-8"), "application/json; charset=utf-8")
        elif len(parts) == 3 and parts[0] == "download" and tuple(parts[1:]) in self.releases.files:
            self.send_response(302)
            self.send_header("Location", "%s/objects/%s/%s" % (base, parts[1], parts[2]))
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif len(parts) == 3 and parts[0] == "objects" and tuple(parts[1:]) in self.releases.files:
            data = self.releases.files[tuple(parts[1:])]
            if isinstance(data, str):
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(os.path.getsize(data)))
                self.end_headers()
                with open(data, "rb") as f:
                    for block in iter(lambda: f.read(1 << 20), b""):
                        self.wfile.write(block)
            elif parts[1] in self.releases.slow and parts[2].endswith(".zip"):
                # A slow line: a few seconds for the whole file.
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                step = max(len(data) // 150, 1)
                try:
                    for at in range(0, len(data), step):
                        self.wfile.write(data[at:at + step])
                        self.wfile.flush()
                        time.sleep(0.02)
                except (BrokenPipeError, ConnectionResetError):
                    pass
            else:
                self.send(200, data, "application/octet-stream")
        else:
            self.send(404, b"Not Found", "text/plain")


def main():
    work = sys.argv[1]
    real_zip = sys.argv[2] if len(sys.argv) > 2 else None
    Handler.releases = Releases(work, real_zip)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    with open(os.path.join(work, "port.part"), "w") as f:
        f.write("%d\n" % server.server_address[1])
    os.replace(os.path.join(work, "port.part"), os.path.join(work, "port"))
    server.serve_forever()


if __name__ == "__main__":
    main()
