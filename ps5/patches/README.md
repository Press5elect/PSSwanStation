# Patches to the PS5 stack this title is built on

PSSwanStation links three parts whose newest commits are not on their own
GitHub repositories yet. Their source is kept here instead, as each part's
commits on top of a revision its repository has (`BASE` in each folder), in
`git format-patch` form. Applied in order with `git am`, they give exactly the
tree this title was built from:

| Folder | Repository and base | Gives (the revision `licenses/components.json` names) |
| --- | --- | --- |
| `PS5_Mesa/` | [mihawk-99/PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) at `f5610fe2709b` | `8f2964be1e6b`: PS5_Mesa main of 2026-11-03 (Mesa 27.0.0-devel, its buffers kept until the GPU is done with them) with Crux2's patches 0001-0004 and 0010-0017 (Crux2's `ps5/patches/mesa-27`): the GFX10.1 workarounds, 119.88 Hz on a VRR display, variable refresh on request |
| `PS5_Vulkan/` | [mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) at `ace98a99460c` | `224fc460cc5d`: its Mesa pin moved to the revision above, the payload SDK pinned at `e105cf6`, Mesa's `amd-options` |
| `PS5_VKHomebrewUI/` | [mihawk-99/PS5_VKHomebrewUI](https://github.com/mihawk-99/PS5_VKHomebrewUI) at `2188642afbcc` | `ea00f4febd38`: an image may turn inside its rect |

```sh
git clone https://github.com/mihawk-99/PS5_Mesa && cd PS5_Mesa
git checkout "$(cat ../PSSwanStation/ps5/patches/PS5_Mesa/BASE)"
git am ../PSSwanStation/ps5/patches/PS5_Mesa/*.patch
```

Mesa's licences apply to the Mesa patches (its PS5 winsys is MIT); two of
Crux2's come from PS5CEMU-HAR under GPL-3.0. The PS5_Vulkan and
PS5_VKHomebrewUI patches are GPL-3.0-or-later. Crux2's own changes: Crux2 by
Press5elect.
