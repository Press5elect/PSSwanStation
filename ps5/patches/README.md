# Patches to the PS5 stack this title is built on

PSSwanStation links three parts whose newest commits are not on their own
GitHub repositories yet. Their source is kept here instead, as each part's
commits on top of a revision its repository has (`BASE` in each folder), in
`git format-patch` form. Applied in order with `git am`, they give exactly the
tree this title was built from, and with
`git -c user.name=Claude -c user.email=noreply@anthropic.com am --committer-date-is-author-date`
the very revisions named below:

| Folder | Repository and base | Gives (the revision `licenses/components.json` names) |
| --- | --- | --- |
| `PS5_Mesa/` | [mihawk-99/PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) at `0f86253f674c` | `ead4d0f93427`: PS5_Mesa main of 2026-11-10 (Mesa 27.0.0-devel, its buffers kept until the GPU is done with them, a cached pipeline that lost a stage compiled again) with Crux2's patches 0001-0004 and 0010-0017 (Crux2's `ps5/patches/mesa-27`): the GFX10.1 workarounds, 119.88 Hz on a VRR display, variable refresh on request; and OGBox's HDR10 framebuffers on request |
| `PS5_Vulkan/` | [mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) at `7f492b4c6f67` | `f274af151e40`: its Mesa pin moved to the revision above, the payload SDK pinned at `3345cde` (upstream's v0.45), Mesa's `amd-options` |
| `PS5_VKHomebrewUI/` | [mihawk-99/PS5_VKHomebrewUI](https://github.com/mihawk-99/PS5_VKHomebrewUI) at `2188642afbcc` | `b05fe83e74f4`: an image may turn inside its rect |

```sh
git clone https://github.com/mihawk-99/PS5_Mesa && cd PS5_Mesa
git checkout "$(cat ../PSSwanStation/ps5/patches/PS5_Mesa/BASE)"
git am ../PSSwanStation/ps5/patches/PS5_Mesa/*.patch
```

Mesa's licences apply to the Mesa patches (its PS5 winsys is MIT); two of
Crux2's come from PS5CEMU-HAR under GPL-3.0, and the last, HDR10
framebuffers on request, is OGBox's (Press5elect), after VLC for PS5's
`patches/mesa/0001-videoout-hdr10.patch` (ZiZc3, GPL-3.0). The PS5_Vulkan and
PS5_VKHomebrewUI patches are GPL-3.0-or-later. Crux2's own changes: Crux2 by
Press5elect.
