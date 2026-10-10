# shaders: AMD FSR3 frame interpolation, optical flow only

The shaders of the frame generation (`src/ps5_framegen.{h,cpp}`): AMD FidelityFX SDK 2.3.0's FSR3 optical flow
and frame interpolation, MIT (see LICENSE.txt), compiled from HLSL to SPIR-V by glslang's HLSL front end into
`src/framegen/fg_shaders.inc` with `tools/build-shaders.sh` (no DXC needed). The SDK itself is DX12 only;
the host side here is plain Vulkan code of its own, not the SDK's backend.

A game seen only through its frames (a PS3 game under RPCS3, a PS2 game under PCSX2) gives no motion vectors or depth, so only FSR3's optical flow path runs (`FFX_FG_OF_ONLY`). FSR3's game vector path,
disocclusion mask, inpainting and UI composition are not built.

## What is here

- `api/internal/gpu/`: the SDK's portable shader core (unchanged).
- `framegeneration/fsr3/include/gpu/`: the optical flow, the frame interpolation and SPD headers.
- `framegeneration/fsr3/internal/shaders/ffx_opticalflow_*.hlsl`: the optical flow passes (unchanged).
- `framegeneration/fsr3/internal/shaders/fg_fi_*.hlsl`: the cut-down setup, vector field and interpolation passes.

## Changes from the SDK

- `opticalflow/ffx_opticalflow_compute_optical_flow_v5.h`: `FFX_FG_PORTABLE_WAVE` does the search's reductions in shared memory
  instead of wave operations that assume 32 or 64 lanes (software Vulkan devices have 4).
- `opticalflow/ffx_opticalflow_callbacks_hlsl.h`, `frameinterpolation/ffx_frameinterpolation_callbacks_hlsl.h`: `[[spv::format_*]]`
  on the storage images (glslang would otherwise guess r32ui, rg32i or rgba32f); the interpolation's still history
  (`rw_static_history`, R8_UINT).
- `frameinterpolation/ffx_frameinterpolation_setup.h`: no game vector field or disocclusion mask to reset under `FFX_FG_OF_ONLY`.
- `frameinterpolation/ffx_frameinterpolation_optical_flow_vector_field.h`: the confidence loads it did not use are gone, and under
  `FFX_FG_OF_ONLY` a block that does not move keeps no vector (FSR3 gave it its moving neighbours' mean).
- `frameinterpolation/ffx_frameinterpolation.h`: under `FFX_FG_OF_ONLY`, `computeFrameinterpolationQuad` replaces FSR3's blend
  (m73b): per pixel, the motions tried are no motion, the vector field blended, its 4 nearest cells and the quad's optical flow;
  the one under which the previous and the current frame agree best wins; where none agrees (an edge where something is covered
  or uncovered), the sample that another motion explains is dropped; a pixel unchanged for 8 frames in a row (HUD, text) is still.
  m73f: the optical flow of the 4 blocks around the quad as candidates too, and a pixel the quad's choice does not fit takes the
  motion that fits it alone. m73g (`FgUnlike`): a motion that is not the optical flow's own where it takes its samples is
  penalised (two dark samples agree by chance: black specks on a character's ears). m74: the motion the quad took in the last
  generated frame (`rw_quad_motion`, R16G16_SFLOAT, one texel a quad) is a candidate; the quad's best motion is refined to a
  fraction of a pixel (a parabola through the errors half a pixel to each side); a motion one of whose samples is off the
  screen uses the other one alone (the strip that comes in or goes out at the screen's edge as the camera turns).
- `opticalflow/ffx_opticalflow_callbacks_hlsl.h`, `frameinterpolation/ffx_frameinterpolation_callbacks_hlsl.h` (m74): a scene
  change stops the optical flow and the interpolation for the frame of the change only (FSR3: the 4 frames after it, and the
  first 5 after a reset), so the motion comes back a frame after a cut.

## Rebuilding

```
GLSLANG=<glslangValidator with HLSL> tools/build-shaders.sh
```

`tools/fg-test` runs the result on a software Vulkan device (SwiftShader) against scenes with a known middle frame,
and (`--seq`) on real frames of a frame dump.
