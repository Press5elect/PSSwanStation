# Building PSSwanStation

The build needs four checkouts beside this repository and a `deps-src/` folder of libraries.
The PS5_Mesa, PS5_Vulkan and PS5_VKHomebrewUI revisions are ours, kept as patches in
[`../../patches`](../../patches/README.md) on revisions GitHub has; everything else is a public
revision or release. The layout, all in one parent folder:

```
PSSwanStation/       this repository, branch ps5-port
PS5_Mesa/            mihawk-99/PS5_Mesa + ps5/patches/PS5_Mesa
PS5_Vulkan/          mihawk-99/PS5_Vulkan + ps5/patches/PS5_Vulkan (its .deps/ holds the SDK and RADV)
PS5_PayloadSDK/      mihawk-99/PS5_PayloadSDK (PS5_Vulkan's setup exports its pin from here)
PS5_VKHomebrewUI/    mihawk-99/PS5_VKHomebrewUI + ps5/patches/PS5_VKHomebrewUI
deps-src/            the libraries below
```

## The pins (build 27)

| Part | Revision | From |
| --- | --- | --- |
| PS5_Mesa | `0f86253f674c` + 14 patches = `ead4d0f93427` | PS5_Mesa main of 2026-11-10, Crux2's RADV patches, OGBox's HDR10 framebuffers |
| PS5_Vulkan | `7f492b4c6f67` + 5 patches = `f274af151e40` | Its Mesa pin is the revision above; the payload SDK at `3345cde` (upstream's v0.45) |
| PS5_VKHomebrewUI | `2188642afbcc` + 1 patch = `b05fe83e74f4` | `ps5/tools/build.sh` checks this revision |
| libsmb2 | `161b89e` | sahlberg/libsmb2 master |
| libnfs | `e5a697b` | sahlberg/libnfs master |
| rcheevos | `v12.5.0` | RetroAchievements/rcheevos |
| glslang | `16.6.0` | KhronosGroup/glslang (the in-title compiler for shader presets) |
| mbedTLS | `v3.6.7`, folder `mbedtls-3.6.7`, with submodules | Mbed-TLS/mbedtls (3.6 LTS; 4.x changes the API) |
| FFmpeg | `n9.0.2`, folder `ffmpeg-n9.0.2` | github.com/FFmpeg/FFmpeg |
| slang-shaders | master | libretro/slang-shaders (crt-guest-advanced) |
| libretro-database | master, sparse: `metadat/developer metadat/redump "cht/Sony - PlayStation"` (cone mode keeps `metadat/`'s own files, where the PS1 details list is since 2026-11-07) | libretro/libretro-database |
| Lapy | `lapy.elf`, `lapy-manifest.json` and `SOURCE.txt` in `deps-src/lapy` | `lapy.elf` and its manifest from the last release's ZIP; `SOURCE.txt` (`remote:`, `revision:`, `built with:` lines) from that ZIP's `licenses/components.json` entry `lapy`, for the notices. Lapy's repository is gone ([its archive](https://github.com/Press5elect/PSSwanStation/releases/download/build24/lapy-c3bdfe3a3993.tar.gz)) |

When a pin moves, change this table, `ps5/patches/README.md` and the CHANGELOG in the same commit.

## Setting it up from nothing

```bash
git clone -b ps5-port https://github.com/Press5elect/PSSwanStation
git clone https://github.com/mihawk-99/PS5_Mesa        # a full clone: blob:none fails git am
git clone https://github.com/mihawk-99/PS5_Vulkan
git clone https://github.com/mihawk-99/PS5_PayloadSDK
git clone https://github.com/mihawk-99/PS5_VKHomebrewUI
```

Each patched part, the same way (the identity and flag give the exact revisions named above):

```bash
cd PS5_Mesa && git checkout -b ours "$(cat ../PSSwanStation/ps5/patches/PS5_Mesa/BASE)" &&
  git -c user.name=Claude -c user.email=noreply@anthropic.com am --committer-date-is-author-date \
  ../PSSwanStation/ps5/patches/PS5_Mesa/*.patch
```

(and the same for `PS5_Vulkan` and `PS5_VKHomebrewUI` with their folders.)

The tools RADV's build needs (Ubuntu 24.04):

```bash
pip install --break-system-packages meson mako
apt-get install rsync bison flex llvm-18-dev libclang-18-dev libclang-cpp18-dev libclc-18-dev libclc-18 libllvmspirvlib-18-dev spirv-tools glslang-tools libpolly-18-dev libclang-rt-18-dev pkg-config libdrm-dev libelf-dev libexpat1-dev zlib1g-dev
```

Then, in `PS5_Vulkan`:

```bash
bash tools/setup-native-dependencies.sh      # the payload SDK at its pin, zlib
bash tools/rebuild-libc.sh                   # sce_module/libc.prx
PS5_MESA_REVISION=$(git -C ../PS5_Mesa rev-parse ours) bash tools/build-radv.sh release
```

The RADV build takes a few minutes; `.deps/native/radv-release/PROVENANCE.txt` names the Mesa
revision and the SDK it was built with.

## A build

```bash
bash ps5/tools/build.sh                                   # build-ps5/dist/PPSA99248
bash ps5/tools/package.sh build-ps5/PSSwanStation-PS5-PPSA99248-build27-testN.zip
```

- A test build is named `build<N>-test<M>`; `BuildNumber` in `ps5/src/main.cpp` is the build.
- A release is built with `RELEASE=1` from committed source only ([`releasing.md`](releasing.md)).
- Shaders of the title's own (`ps5/shaders`) are compiled into `ps5/src/*_spirv.inc` by
  `ps5/tools/make-shaders.sh`, which are committed: run it after changing a shader, and check
  that the other `.inc` files didn't change.
