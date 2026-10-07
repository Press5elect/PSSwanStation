#!/usr/bin/env python3
"""PSSwanStation - a release: the title's ZIP and the source of every part in it.

    RELEASE=1 ps5/tools/build.sh           the title folder a release is made of
    ps5/tools/release.py [--out DIR] [--offline] [--notes FILE]

It takes the built title folder (build-ps5/dist/<TITLE_ID>) and writes
DIR (default: build-ps5/release-build<N>):

  PSSwanStation-PS5-<TITLE_ID>-build<N>.zip   the title folder, as
                        ps5/tools/package.sh packs it (every entry 0777, as the
                        console wants an app's files); unpacked again and
                        compared with the folder
  ...zip.sha256         what the title's updater checks a download against
  source/<part>-<revision>.tar.gz
                        each part's source at the revision the title was built
                        from (git archive); a part fixed upstream is named in
                        SOURCES.txt with its address
  source/SOURCES.txt, source/SHA256SUMS
  SHA256SUMS            the ZIP's and the archives' digests
  NOTES.md              the release's text (--notes), with the commit and the
                        ZIP's digest filled in
  attach/               every file the release page carries, side by side: the
                        ZIP, its .sha256, the archives, SOURCES.txt and
                        SHA256SUMS (--attach-limit cuts a large archive into
                        parts)

It refuses, and says why:
  - a folder not built with RELEASE=1 from the commit that is checked out, or
    from uncommitted source (licenses/components.json records "dirty");
  - anything a release must never carry: a game, a BIOS, a save, a memory card,
    a state, a network.cfg, a settings file, a log, the chtdb files;
  - a path of the machine it was built on, or a word .release-private lists
    (an address, a share: what is private on the machine that builds);
  - a licence text licenses/README.txt names that is not in the folder;
  - a pinned revision GitHub does not have (not with --offline). The title's
    own commit not being there yet is said, and is not a refusal: it is pushed
    before the release is published.

The release itself is a step of its own: the tag on the commit the ZIP was
built from (the release page's "target" is the branch ps5-port, not the
fork's default branch), the files of attach/ and NOTES.md as its text. Build
14 was published that way by hand, as a full release, so that the title's
updater finds it; then it is read back (the ZIP's digest, the updater against
the page).

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

PS5 = Path(__file__).resolve().parent.parent
SRC = PS5.parent

# Where each part's source is on this machine (a sibling of the repository
# unless the environment says otherwise).
LOCAL = {
    "psswanstation": ("", SRC),
    "ps5-vulkan": ("PS5_VULKAN_DIR", SRC.parent / "PS5_Vulkan"),
    "platform": ("PS5_PAYLOAD_SDK_FORK", SRC.parent / "PS5_PayloadSDK"),
    "radv": ("PS5_MESA_DIR", SRC.parent / "PS5_Mesa"),
    "imgui": ("IMGUI_DIR", SRC.parent / "imgui"),
    "libsmb2": ("LIBSMB2_DIR", SRC.parent / "deps-src/libsmb2"),
    "rcheevos": ("RCHEEVOS_DIR", SRC.parent / "deps-src/rcheevos"),
    "lapy": ("LAPY_SOURCE_DIR", SRC.parent / "deps-src/lapy-source"),
    "gamedb": ("LIBRETRO_DATABASE_DIR", SRC.parent / "deps-src/libretro-database"),
}
# Folders of the title that are the user's: a release has them empty.
USER_FOLDERS = ("games", "bios", "covers", "cheats", "textures", "layouts", "borders", "music", "memcards", "data",
                "logs", "screenshots", ".update")
NEVER_NAMES = ("network.cfg", "frontend.cfg", ".env", "klog.txt", "psswanstation-boot.log", "cheats.zip", "patches.zip")
NEVER_ENDINGS = (".cue", ".chd", ".iso", ".img", ".pbp", ".ecm", ".mds", ".m3u", ".exe", ".psexe", ".mcd", ".mcr",
                 ".srm", ".sav", ".state", ".rap", ".pup", ".cht", ".log")


def private_words():
    """What tells where the title was built, or for whom: the builder's home
    and the folder the repositories are in, and whatever else is private here
    (an address, a share's name): the lines of .release-private beside the
    repository's ps5 folder, which is never committed, and the words of
    PS5_RELEASE_PRIVATE, separated by colons."""
    words = [str(Path.home()), str(SRC.parent)]
    listed = SRC / ".release-private"
    if listed.exists():
        words += [line.strip() for line in listed.read_text().splitlines() if line.strip() and not line.startswith("#")]
    words += [w for w in os.environ.get("PS5_RELEASE_PRIVATE", "").split(":") if w]
    return tuple(w.encode() for w in words if len(w) >= 4)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=True).stdout.strip()


def refuse(why):
    raise SystemExit("release: " + why)


def check_folder(folder, parts):
    problems = []
    never_bytes = private_words()
    dirty = [p["id"] for p in parts if p["source"].get("dirty")]
    if dirty:
        problems.append("built from uncommitted source: " + ", ".join(dirty))
    unknown = [p["id"] for p in parts if p["source"].get("revision") in (None, "", "unknown")]
    if unknown:
        problems.append("no revision recorded for: " + ", ".join(unknown))
    if any(p["id"].startswith("chtdb") for p in parts):
        problems.append("the chtdb files are in the folder (build with RELEASE=1)")
    head = git(SRC, "rev-parse", "HEAD")
    if git(SRC, "status", "--porcelain", "--untracked-files=no"):
        problems.append("the repository has uncommitted changes")
    built = re.search(r"^SwanStation: ([0-9a-f]{40})", (folder / "BUILD.txt").read_text(), re.M)
    if not built or built.group(1) != head:
        problems.append("BUILD.txt names %s, the repository is at %s: build again" % (built.group(1)[:12] if built else "nothing", head[:12]))
    title = next((p for p in parts if p["id"] == "psswanstation"), None)
    if not title or title["source"]["revision"] != head:
        problems.append("licenses/components.json is not of this commit: build again")
    for path in sorted(folder.rglob("*")):
        relative = path.relative_to(folder).as_posix()
        top = relative.split("/")[0]
        if path.is_file():
            if top in USER_FOLDERS:
                problems.append("a file in a folder that is the user's: " + relative)
            elif path.name in NEVER_NAMES or path.name.lower().endswith(NEVER_ENDINGS) \
                    or (path.name.lower().endswith(".bin") and relative != "eboot.bin"):
                problems.append("a release never carries " + relative)
            else:
                data = path.read_bytes()
                for needle in never_bytes:
                    if needle in data:
                        problems.append("%s holds %r" % (relative, needle.decode()))
    table = (folder / "licenses/README.txt").read_text()
    for name in sorted(set(re.findall(r"\b([A-Za-z0-9.-]+-?[A-Za-z0-9.]*\.(?:txt|rst))\b", table))):
        if name not in ("README.txt", "LEGAL.txt", "BUILD.txt") and not (folder / "licenses" / name).exists():
            problems.append("licenses/README.txt names %s, which is not in licenses/" % name)
    for name in ("LEGAL.txt", "README.txt", "CHANGELOG.txt", "BUILD.txt", "eboot.bin", "sce_sys/param.json",
                 "sce_module/libc.prx", "lapy.elf", "lapy-manifest.json", "licenses/GPL-3.0.txt"):
        if not (folder / name).is_file():
            problems.append(name + " is missing")
    if problems:
        refuse("the title folder cannot be released:\n  - " + "\n  - ".join(problems))
    return head


def has_commit(remote, revision):
    """Whether a repository on GitHub has a commit: the commit alone is fetched
    (no trees, no files) into a repository that is thrown away."""
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["git", "init", "-q", "--bare", tmp], check=True)
        done = subprocess.run(["git", "-C", tmp, "fetch", "-q", "--depth", "1", "--filter=tree:0", remote, revision],
                              capture_output=True, text=True)
        return done.returncode == 0


def check_pins(parts):
    """Every revision the title was built from is one GitHub has."""
    missing, not_yet = [], []
    for p in parts:
        src = p["source"]
        if src["kind"] != "git":
            continue
        if not re.match(r"https://github.com/[^/]+/[^/]+$", src["remote"]):
            missing.append("%s: %s is not a GitHub address" % (p["id"], src["remote"]))
            continue
        for revision in filter(None, (src["revision"], src.get("built_with_sdk"))):
            remote = "https://github.com/mihawk-99/PS5_PayloadSDK" if revision == src.get("built_with_sdk") else src["remote"]
            if not has_commit(remote, revision):
                (not_yet if p["id"] == "psswanstation" else missing).append("%s: %s at %s" % (p["id"], remote, revision[:12]))
    if missing:
        refuse("GitHub does not have:\n  - " + "\n  - ".join(missing))
    return not_yet


def archive(repo, revision, prefix, dest, paths=()):
    with open(dest, "wb") as out:
        subprocess.run(["git", "-C", str(repo), "archive", "--format=tar.gz", "--prefix=%s/" % prefix, revision, *paths],
                       stdout=out, check=True)


def local(part_id):
    variable, default = LOCAL[part_id]
    return Path(os.environ.get(variable) or default) if variable else default


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out")
    parser.add_argument("--offline", action="store_true", help="do not ask GitHub whether it has every revision")
    parser.add_argument("--notes", help="the release's text; {{COMMIT}}, {{ZIP}}, {{ZIP_SHA256}} and {{BUILD}} are filled in")
    parser.add_argument("--attach-limit", type=int, default=0, metavar="MIB",
                        help="also write attach/: every file the release page carries, in one folder, an archive "
                             "larger than this many MiB cut into parts (0: no parts)")
    args = parser.parse_args()

    title_id = json.loads((PS5 / "sce_sys/param.json").read_text())["titleId"]
    build = int(re.search(r"^const int BuildNumber = (\d+);", (PS5 / "src/main.cpp").read_text(), re.M).group(1))
    folder = SRC / "build-ps5/dist" / title_id
    if not (folder / "licenses/components.json").exists():
        refuse("%s is not a built title folder (RELEASE=1 ps5/tools/build.sh)" % folder)
    parts = json.loads((folder / "licenses/components.json").read_text())
    head = check_folder(folder, parts)
    not_yet = [] if args.offline else check_pins(parts)

    out = Path(args.out) if args.out else SRC / "build-ps5" / ("release-build%d" % build)
    shutil.rmtree(out, ignore_errors=True)
    (out / "source").mkdir(parents=True)
    zip_path = out / ("PSSwanStation-PS5-%s-build%d.zip" % (title_id, build))
    subprocess.run(["bash", str(PS5 / "tools/package.sh"), str(zip_path), str(folder)], check=True, stdout=subprocess.DEVNULL)
    with zipfile.ZipFile(zip_path) as z, tempfile.TemporaryDirectory() as tmp:
        if z.testzip():
            refuse("the ZIP is damaged")
        z.extractall(tmp)
        unpacked = Path(tmp) / folder.name
        files = [p for p in folder.rglob("*") if p.is_file()]
        for path in files:
            if sha256(path) != sha256(unpacked / path.relative_to(folder)):
                refuse("%s differs once unpacked" % path.relative_to(folder))
        if len(files) != sum(1 for p in unpacked.rglob("*") if p.is_file()):
            refuse("the ZIP holds other files than the folder")
    print("==> %s: %d files, %.1f MB, tested and compared with the folder" % (zip_path.name, len(files), zip_path.stat().st_size / 1e6))

    sources = []
    for p in parts:
        src = p["source"]
        if src["kind"] != "git":
            sources.append(("(not attached)", p["name"], "%s: %s" % (src["revision"], src["url"])))
            continue
        repo = local(p["id"])
        if not (repo / ".git").exists():
            refuse("the source of %s is not at %s" % (p["id"], repo))
        name = "%s-%s" % (p["id"], src["revision"][:12])
        archive(repo, src["revision"], name, out / "source" / (name + ".tar.gz"), src.get("paths", ()))
        where = "%s at %s" % (src["remote"], src["revision"])
        if src.get("paths"):
            where += " (only what the title is made from: %s)" % ", ".join(src["paths"])
        if src.get("built_with"):
            where += "; built with " + src["built_with"]
        sources.append((name + ".tar.gz", p["name"], where))
        if src.get("built_with_sdk"):
            sname = "%s-sdk-%s" % (p["id"], src["built_with_sdk"][:12])
            archive(local("platform"), src["built_with_sdk"], sname, out / "source" / (sname + ".tar.gz"))
            sources.append((sname + ".tar.gz", "the payload SDK fork the RADV archive was built against",
                            "https://github.com/mihawk-99/PS5_PayloadSDK at " + src["built_with_sdk"]))
    with open(out / "source/SOURCES.txt", "w") as f:
        f.write("The source of PSSwanStation build %d (%s), one archive per part, at the revision the\n"
                "title was built from. The first is the title itself: SwanStation and the ps5 folder.\n\n" % (build, title_id))
        for file, what, where in sources:
            f.write("%s\n  %s\n  %s\n\n" % (file, what, where))
    sums = [(sha256(a), a.name) for a in sorted((out / "source").glob("*.tar.gz"))]
    (out / "source/SHA256SUMS").write_text("".join("%s  %s\n" % s for s in sums))
    zip_sum = sha256(zip_path)
    (out / "SHA256SUMS").write_text("%s  %s\n" % (zip_sum, zip_path.name) + "".join("%s  source/%s\n" % s for s in sums))
    total = sum(a.stat().st_size for a in (out / "source").glob("*.tar.gz"))
    print("==> source: %d archives, %.1f MB; SHA256SUMS, SOURCES.txt" % (len(sums), total / 1e6))
    if args.notes:
        text = Path(args.notes).read_text()
        for key, value in (("COMMIT", head), ("ZIP", zip_path.name), ("ZIP_SHA256", zip_sum), ("BUILD", str(build))):
            text = text.replace("{{%s}}" % key, value)
        left = re.findall(r"\{\{[A-Z_0-9]+\}\}", text)
        if left:
            refuse("the notes still hold %s" % ", ".join(sorted(set(left))))
        (out / "NOTES.md").write_text(text)
        print("==> NOTES.md")
    # The release page holds its files side by side: the ZIP, its checksum,
    # the archives, what they are and their digests. An archive too large for
    # the way it travels there is cut into parts that `cat` joins again.
    attach = out / "attach"
    attach.mkdir()
    shutil.copy2(zip_path, attach)
    shutil.copy2(str(zip_path) + ".sha256", attach)
    limit = args.attach_limit << 20
    joining = []
    for path in sorted((out / "source").glob("*.tar.gz")):
        if not limit or path.stat().st_size <= limit:
            shutil.copy2(path, attach)
            continue
        data = path.read_bytes()
        count = -(-len(data) // limit)
        names = ["%s.part%dof%d" % (path.name, i + 1, count) for i in range(count)]
        for i, name in enumerate(names):
            (attach / name).write_bytes(data[i * limit:(i + 1) * limit])
        joining.append((path.name, names, sha256(path)))
    text = (out / "source/SOURCES.txt").read_text()
    if joining:
        note = "\nThese archives are attached in parts. Join them before unpacking:\n\n"
        for name, names, digest in joining:
            note += "  cat %s > %s\n" % (" ".join(names), name)
        note += "\n(on Windows: copy /b part1 + part2 ... whole). The joined files' SHA-256:\n\n"
        note += "".join("  %s  %s\n" % (digest, name) for name, names, digest in joining)
        head_end = text.index("\n\n") + 1
        text = text[:head_end] + note + text[head_end:]
    (attach / "SOURCES.txt").write_text(text)
    listed = sorted(p for p in attach.iterdir() if p.name.endswith((".zip", ".tar.gz")) or ".tar.gz.part" in p.name)
    (attach / "SHA256SUMS").write_text("".join("%s  %s\n" % (sha256(p), p.name) for p in listed))
    print("==> attach/: %d files for the release page" % sum(1 for _ in attach.iterdir()))
    print("==> sha256 %s  %s" % (zip_sum, zip_path.name))
    print("==> built from %s" % head)
    for line in not_yet:
        print("==> not on GitHub yet (push before publishing): " + line)
    print("==> %s" % out)


if __name__ == "__main__":
    main()
