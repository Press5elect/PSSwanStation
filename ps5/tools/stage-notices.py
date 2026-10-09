#!/usr/bin/env python3
"""PSSwanStation - licenses/components.json of a staged title folder.

    stage-notices.py APP_DIR SRC VULKAN_DIR SDK_DIR IMGUI_DIR LIBSMB2_DIR RCHEEVOS_DIR LAPY_DIR DATABASE_DIR

Called by ps5/tools/build.sh. It writes every part of the title as data: what
it is, its licence, which of the folder's files it is in, and the source it
was built from, by address and revision. licenses/README.txt says the same
for a reader; ps5/tools/release.py archives each part's source from this file.
A part whose repository has uncommitted changes is recorded as "dirty": a
release refuses it. No path of the machine that built it is written.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import json
import re
import subprocess
import sys
from pathlib import Path


def git(repo, *args):
    done = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
    return done.stdout.strip() if done.returncode == 0 else ""


def at(repo, remote=None):
    """Where a checkout is: its address, its revision, and whether it differs from it."""
    address = remote or git(repo, "remote", "get-url", "origin")
    return dict(kind="git", remote=address.removesuffix(".git"), revision=git(repo, "rev-parse", "HEAD") or "unknown",
                dirty=bool(git(repo, "status", "--porcelain", "--untracked-files=no")))


def main():
    app, src, vulkan, sdk, imgui, libsmb2, rcheevos, lapy, database = (Path(a) for a in sys.argv[1:10])
    libnfs = Path(sys.argv[10]) if len(sys.argv) > 10 else src.parent / "deps-src/libnfs"
    slang = Path(sys.argv[11]) if len(sys.argv) > 11 else src.parent / "deps-src/slang-shaders"
    parts = []
    title = at(src, "https://github.com/Press5elect/PSSwanStation")
    parts.append(dict(
        id="psswanstation",
        name="PSSwanStation: SwanStation (the emulator, with the libraries in its tree: libchdr with LZMA and zstd, "
             "xxHash, Xbyak, stb, libretro-common, OpenBIOS) and the PS5 frontend in ps5/ (with miniz, stb_vorbis, "
             "dr_mp3, AMD's FSR 1 and CAS headers, NVIDIA Image Scaling, Project Nayuki's QR Code generator, the sandbox elevation "
             "client, Roboto and Font Awesome Free)",
        licence="GPL-3.0 as a whole; each library's own in licenses/README.txt",
        artifacts=["eboot.bin", "sce_sys/", "README.txt", "CHANGELOG.txt", "LEGAL.txt", "licenses/"],
        source=title))
    parts.append(dict(
        id="ps5-vulkan", name="PS5_Vulkan: the RADV link recipe, the packaging tool and sce_module/libc.prx",
        licence="GPL-3.0-or-later", artifacts=["eboot.bin", "sce_module/libc.prx"], source=at(vulkan)))
    revision = (sdk / ".ps5-sdk-revision").read_text().strip() if (sdk / ".ps5-sdk-revision").exists() else "unknown"
    parts.append(dict(
        id="platform", name="the PS5 platform layer (libps5platform.a) and the SDK headers, from the payload SDK fork, "
                            "with musl's regex and dlmalloc",
        licence="GPL-3.0-or-later; musl's regex MIT; dlmalloc public domain", artifacts=["eboot.bin"],
        source=dict(kind="git", remote="https://github.com/mihawk-99/PS5_PayloadSDK", revision=revision, dirty=False)))
    provenance = vulkan / ".deps/native/radv-release/PROVENANCE.txt"
    text = provenance.read_text() if provenance.exists() else ""
    mesa = re.search(r"^revision: ([0-9a-f]{40})", text, re.M)
    radv_sdk = re.search(r"^sdk: ([0-9a-f]{40})", text, re.M)
    parts.append(dict(
        id="radv", name="RADV, Mesa's Vulkan driver (with ACO, NIR and Mesa's Vulkan runtime), from the PS5_Mesa fork",
        licence="MIT, with other licences stated per file (licenses/Mesa-license.rst)", artifacts=["eboot.bin"],
        source=dict(kind="git", remote="https://github.com/mihawk-99/PS5_Mesa", revision=mesa.group(1) if mesa else "unknown",
                    dirty=False, built_with_sdk=radv_sdk.group(1) if radv_sdk else None)))
    parts.append(dict(
        id="llvm-runtime", name="LLVM libc++, libc++abi, libunwind and compiler-rt builtins (linked into eboot.bin)",
        licence="Apache-2.0 WITH LLVM-exception", artifacts=["eboot.bin"],
        source=dict(kind="fixed", revision="ps5-payload-dev SDK release archives",
                    url="https://github.com/ps5-payload-dev/sdk/releases")))
    parts.append(dict(id="imgui", name="Dear ImGui", licence="MIT", artifacts=["eboot.bin"], source=at(imgui)))
    parts.append(dict(id="libsmb2", name="libsmb2 (network shares)", licence="LGPL-2.1-or-later", artifacts=["eboot.bin"],
                      source=at(libsmb2)))
    parts.append(dict(id="libnfs", name="libnfs (NFS shares)", licence="LGPL-2.1-or-later; its protocol files BSD-2-Clause",
                      artifacts=["eboot.bin"], source=at(libnfs)))
    parts.append(dict(id="rcheevos", name="rcheevos (RetroAchievements)", licence="MIT", artifacts=["eboot.bin"],
                      source=at(rcheevos)))
    note = (lapy / "SOURCE.txt").read_text() if (lapy / "SOURCE.txt").exists() else ""
    remote = re.search(r"^remote: (\S+)", note, re.M)
    rev = re.search(r"^revision: ([0-9a-f]{40})", note, re.M)
    built = re.search(r"^built with: (.+)", note, re.M)
    manifest = json.loads((lapy / "lapy-manifest.json").read_text())
    parts.append(dict(
        id="lapy", name="the Lapy helper (lapy.elf): PS5-Lapy-JB-Daemon's one-shot helper for this title, built unchanged",
        licence="MIT", artifacts=["lapy.elf", "lapy-manifest.json"],
        source=dict(kind="git", remote=remote.group(1) if remote else "unknown", revision=rev.group(1) if rev else "unknown",
                    dirty=False, built_with=built.group(1) if built else "unknown", elf_sha256=manifest.get("elf_sha256"))))
    if (app / "assets/gamedb.zip").exists():
        source = at(database)
        source["paths"] = ["metadat/developer/Sony - PlayStation.dat", "metadat/redump/Sony - PlayStation.dat", "LICENSE"]
        parts.append(dict(
            id="gamedb", name="the game database (assets/gamedb.zip): the libretro database's PlayStation lists, "
                              "rearranged by ps5/tools/make-gamedb.py",
            licence="CC BY-SA 4.0", artifacts=["assets/gamedb.zip"], source=source))
    if (app / "assets/libretro-cheats.zip").exists():
        source = at(database)
        source["paths"] = ["cht/Sony - PlayStation", "metadat/redump/Sony - PlayStation.dat", "LICENSE"]
        parts.append(dict(
            id="libretro-cheats", name="the libretro cheats (assets/libretro-cheats.zip): the libretro database's "
                                      "PlayStation cheat files, matched to serials by ps5/tools/make-libretro-cheats.py",
            licence="CC BY-SA 4.0", artifacts=["assets/libretro-cheats.zip"], source=source))
    if (app / "assets/shaders/crt-guest-advanced/chain.txt").exists():
        source = at(slang)
        source["paths"] = ["crt/crt-guest-advanced.slangp", "crt/shaders/guest/advanced"]
        parts.append(dict(
            id="slang-shaders", name="crt-guest-advanced by guest(r), from the libretro slang shaders "
                                    "(assets/shaders/crt-guest-advanced): its passes compiled to SPIR-V by "
                                    "ps5/tools/make-chains.py, its lookup pictures unchanged",
            licence="GPL-2.0-or-later", artifacts=["assets/shaders/"], source=source))
    for name in ("cheats.zip", "patches.zip"):
        if (app / "assets" / name).exists():
            parts.append(dict(
                id="chtdb-" + name.split(".")[0], name="the chtdb project's " + name + ", an unmodified release file",
                licence="none given: the entries belong to their authors (a release refuses this part)",
                artifacts=["assets/" + name],
                source=dict(kind="fixed", revision="the release named latest", url="https://github.com/duckstation/chtdb/releases")))
    (app / "licenses").mkdir(exist_ok=True)
    (app / "licenses/components.json").write_text(json.dumps(parts, indent=1, ensure_ascii=False) + "\n")
    dirty = [p["id"] for p in parts if p["source"].get("dirty")]
    print("notices: licenses/components.json (%d parts%s)" % (len(parts), "; dirty: " + ", ".join(dirty) if dirty else ""))


if __name__ == "__main__":
    main()
