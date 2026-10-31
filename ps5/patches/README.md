# Patches to the PS5 stack this title is built on

PSSwanStation links three parts whose newest commits are not on their own
GitHub repositories yet. Their source is kept here instead, as each part's
commits on top of a revision its repository has (`BASE` in each folder), in
`git format-patch` form. Applied in order with `git am`, they give exactly the
tree this title was built from:

| Folder | Repository and base | Gives (the revision `licenses/components.json` names) |
| --- | --- | --- |
| `PS5_Mesa/` | [mihawk-99/PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) at `1ba9874f4786` | `c3eab8ac8b24`: Mesa main of 2026-10-31 (27.0.0-devel) with Crux2's patches 0001-0017 (Crux2's `ps5/patches/mesa-27`): the GFX10.1 workarounds, 119.88 Hz on a VRR display, variable refresh on request |
| `PS5_Vulkan/` | [mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) at `f410e67a0241` | `e5e62cb`: its Mesa pin moved to the revision above |
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
