# ps5-framegen (vendored)

The frame generation of [swordpdf/ps5-framegen](https://github.com/Swordpdf/ps5-framegen) at
`64362101757cdd6f7df9eeb984cb53f03f1fbfca`, copied unchanged:

- `src/ps5_framegen.h`, `src/ps5_framegen.cpp`: the interpolator (GPL-3.0-or-later, copyright swordpdf; `COPYING`);
- `src/framegen/fg_shaders.inc`: its shaders compiled to SPIR-V, and `shaders/`, their HLSL source: AMD FidelityFX SDK 2.3.0's
  FSR3 optical flow and the interpolation passes made from it (MIT, `shaders/LICENSE.txt`);
- `tools/`: what compiles `shaders/` into `fg_shaders.inc` (GPL-3.0-or-later);
- `algorithm.md`: how it works, from its `docs/`.

The optical flow is AMD's (FidelityFX SDK 2.3.0, FSR3); ps5-framegen is what runs it on plain Vulkan and its interpolation.
PSSwanStation uses it as the second way of making frames between the game's (Settings, Picture, "Generation method", "AMD
FSR3 optical flow"), beside its own; `ps5/src/display.cpp` (`generatedFlow`) drives it. To bring in a newer revision, copy these files again
and change the revision above and in `ps5/licenses/README.txt`.
