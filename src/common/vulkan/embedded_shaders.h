// Copyright 2026 LibretroAdmin
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once

#include "../types.h"
#include "vulkan_loader.h"
#include <cstddef>
#include <cstdint>

// Pre-compiled SPIR-V blobs for the Vulkan backend.
//
// Each blob is generated offline by tools/regen_vulkan_spirv.py from the
// matching GLSL source under data/shaders/vulkan/, and checked into
// src/common/vulkan/embedded_spirv/. Nothing in the build system invokes
// glslangValidator - the .inc files are consumed as plain C++ arrays. See
// data/shaders/vulkan/README.md for the editing workflow.
//
// This path replaces the runtime glslang -> SPIR-V step done in
// Vulkan::ShaderCompiler::CompileShader for the shaders that have been
// pre-baked. Until every shader has been pre-baked, the runtime path and
// glslang still cover the remainder.
namespace Vulkan::EmbeddedShaders {

// Screen-quad vertex shader (fullscreen-triangle helper used by VRAM ops,
// display, downsample, etc.). Equivalent to
// ShaderGen::GenerateScreenQuadVertexShader() in Vulkan mode.
//
// The array is sized in the .cpp; consumers see only the bare extern.
extern const uint32_t k_screen_quad_vs[];
extern const size_t k_screen_quad_vs_size_bytes;

// UV-quad vertex shader. Variant of the screen-quad that maps the swept
// triangle into a configurable [u_uv_min..u_uv_max] UV rect via push
// constants. Equivalent to ShaderGen::GenerateUVQuadVertexShader() in
// Vulkan mode.
extern const uint32_t k_uv_quad_vs[];
extern const size_t k_uv_quad_vs_size_bytes;

// Presentation-stage fullscreen-quad VS. Distinct from the GPU_HW screen
// quad above - has a u_src_rect push constant. Used only by
// LibretroVulkanHostDisplay to blit the rendered frame (and the software
// cursor) into the libretro framebuffer.
extern const uint32_t k_present_fullscreen_vs[];
extern const size_t k_present_fullscreen_vs_size_bytes;

// Presentation-stage display FS. Writes the source texture opaque.
extern const uint32_t k_present_display_fs[];
extern const size_t k_present_display_fs_size_bytes;

// Presentation-stage cursor FS. Preserves source alpha for blending.
extern const uint32_t k_present_cursor_fs[];
extern const size_t k_present_cursor_fs_size_bytes;

// Adaptive-downsample blur FS. Single-channel 3x3-ish smoothing of the
// energy texture produced by the mip FS. No specialization constants.
extern const uint32_t k_adaptive_downsample_blur_fs[];
extern const size_t k_adaptive_downsample_blur_fs_size_bytes;

// Adaptive-downsample composite FS. Resolves the mip pyramid into the
// final downsampled output using the per-pixel bias from the blur pass.
// RESOLUTION_SCALE spec constant at id=0; reads two textures at bindings
// 1 (samp0, color pyramid) and 2 (samp1, bias).
extern const uint32_t k_adaptive_downsample_composite_fs[];
extern const size_t k_adaptive_downsample_composite_fs_size_bytes;

// Adaptive-downsample mip FS. One blob serves both the first-pass
// (FIRST_PASS=true: samples a vec3 color texture) and the subsequent mid
// passes (FIRST_PASS=false: feeds back vec4 from a prior mip). FIRST_PASS
// is a bool specialization constant at id=100 supplied at pipeline-
// creation time via Vulkan::SpecConstants.
extern const uint32_t k_adaptive_downsample_mip_fs[];
extern const size_t k_adaptive_downsample_mip_fs_size_bytes;

// Box-sample downsample FS. Averages a RESOLUTION_SCALE x RESOLUTION_SCALE
// block of upscaled texels back to PSX-native resolution. Single sampler
// at binding 1. RESOLUTION_SCALE spec constant at id=0.
extern const uint32_t k_box_sample_downsample_fs[];
extern const size_t k_box_sample_downsample_fs_size_bytes;

// VRAM fill FS. One blob, four-to-eight specialisations:
//   id =   3 PGXP_DEPTH (bool)  - common-knob convention.
//   id = 100 INTERLACED (bool)  - shader-specific.
//   id = 101 WRAPPED    (bool)  - shader-specific.
// No texture binding; reads only its push constant block.
extern const uint32_t k_vram_fill_fs[];
extern const size_t k_vram_fill_fs_size_bytes;

// VRAM copy FS. Single SPIR-V blob; the only non-shader knob (depth_test)
// is a pipeline-state toggle, not a spec constant. Two spec constants:
//   id = 0 RESOLUTION_SCALE (uint) - drives VRAM_SIZE in wrap-around math.
//   id = 3 PGXP_DEPTH       (bool) - depth source selection.
// Single sampler at binding 1 (non-MSAA only; see shadergen TODO).
extern const uint32_t k_vram_copy_fs[];
extern const size_t k_vram_copy_fs_size_bytes;

// VRAM write FS. Two blobs - the SSBO vs uniform-texel-buffer split is a
// structural variant (different descriptor type at binding 0:
// STORAGE_BUFFER vs UNIFORM_TEXEL_BUFFER) so it cannot fold into a spec
// constant. The C++ side selects between them based on
// GPU_HW::m_use_ssbos_for_vram_writes. Both blobs share the same spec
// constants:
//   id = 0 RESOLUTION_SCALE (uint) - native-coord downscale + VRAM_SIZE.
//   id = 3 PGXP_DEPTH       (bool) - depth source selection.
extern const uint32_t k_vram_write_ssbo_fs[];
extern const size_t k_vram_write_ssbo_fs_size_bytes;
extern const uint32_t k_vram_write_texbuf_fs[];
extern const size_t k_vram_write_texbuf_fs_size_bytes;

// Display FS - the densest shader in the project. Two blobs handle the
// structural MSAA split (sampler2D vs sampler2DMS); the remaining
// depth_24bit x interlace_mode (None/Interleaved/Separate) x
// smooth_chroma combinations collapse into spec constants. The C++ side
// keeps the existing m_display_pipelines[depth_24][interlace_mode] 2x3
// slot table (smooth_chroma and MSAA are session-constant) and passes
// the rest at pipeline-create time.
//
// Common spec constants on both blobs:
//   id =   0 RESOLUTION_SCALE (uint) - drives VRAM_SIZE.
//   id = 100 DEPTH_24BIT      (bool) - 16bpp vs 24bpp source.
//   id = 101 INTERLACED       (bool) - field-discard logic.
//   id = 102 INTERLEAVED      (bool) - meaningful only when INTERLACED.
//   id = 103 SMOOTH_CHROMA    (bool) - meaningful only when DEPTH_24BIT.
//
// MSAA blob adds:
//   id =   1 MULTISAMPLES     (uint) - LoadVRAM averaging loop bound.
extern const uint32_t k_display_fs[];
extern const size_t k_display_fs_size_bytes;
extern const uint32_t k_display_msaa_fs[];
extern const size_t k_display_msaa_fs_size_bytes;

// VRAM readback FS. Despite the shadergen name (GenerateVRAMReadFragment-
// Shader), this is the CPU-readback path: samples upscaled m_vram_texture,
// downsamples blocks back to PSX-native 16bpp via a box filter, packs two
// 16-bit pixels into one RGBA8 output texel. Two blobs for the structural
// MSAA split.
//
// Spec constants on both blobs:
//   id = 0 RESOLUTION_SCALE (uint) - box-filter inner-loop bounds.
// MSAA blob adds:
//   id = 1 MULTISAMPLES     (uint) - LoadVRAM averaging loop bound.
extern const uint32_t k_vram_readback_fs[];
extern const size_t k_vram_readback_fs_size_bytes;
extern const uint32_t k_vram_readback_msaa_fs[];
extern const size_t k_vram_readback_msaa_fs_size_bytes;

// VRAM update-depth FS. Rebuilds the depth attachment of the upscaled
// VRAM render pass from the colour attachment's alpha after a
// framebuffer recreation (resolution scale / antialiasing / true colour
// toggle). Trivial body - "copy alpha to depth" - so no spec constants
// are required on either blob; the only variance is the sampler type.
// Two blobs for the structural MSAA split.
//
// MSAA blob writes gl_FragDepth from texelFetch(samp0, ..., gl_SampleID),
// which forces per-sample shading per the Vulkan spec.
extern const uint32_t k_vram_update_depth_fs[];
extern const size_t k_vram_update_depth_fs_size_bytes;
extern const uint32_t k_vram_update_depth_msaa_fs[];
extern const size_t k_vram_update_depth_msaa_fs_size_bytes;


// Batch VS - the first of the two batch shaders, and the only non-batch
// pre-baked shader that needs more than two blobs for its structural
// variance. The state space has three SPIR-V-structural axes that
// specialisation constants cannot collapse:
//
//   1. Vertex attribute layout. 2 inputs (untextured), 4 (textured
//      without UV limits) or 5 (textured with UV limits). The 'in'
//      declarations are part of the SPIR-V module interface.
//   2. Output interpolation qualifier. None / centroid / sample,
//      compiled to OpMemberDecorate Centroid / Sample on the
//      VertexData out block. The matching batch FS picks the
//      corresponding qualifier on its inputs.
//   3. Color output perspective. Standard / noperspective, compiled
//      to OpMemberDecorate NoPerspective on v_col0.
//
// 2 x 3 x 2 = 12 blobs. UV_LIMITS used to be a third attribute-layout
// axis (textured-with vs textured-without v_uv_limits) but it has
// been collapsed - the VS always emits a_uv_limits + v_uv_limits when
// textured, with the FS-side u_uv_limits cbuffer scalar deciding
// whether to consume the value. Body-level knobs (RESOLUTION_SCALE,
// PGXP_DEPTH) are common-knob specialisation constants on every
// blob.
//
// The blobs are addressed via the EmbeddedShaderBlob array
// k_batch_vs_blobs and the helper GetBatchVertexShaderBlob below
// rather than via 24 individual extern declarations.
struct EmbeddedShaderBlob
{
  const uint32_t* spv;
  size_t          size_bytes;
};

extern const EmbeddedShaderBlob k_batch_vs_blobs[12];

// Pick the right batch VS blob from the per-call and per-session state
// the C++ side already has at pipeline-creation time. The 'textured'
// argument is the m_batch_vertex_shaders[] index; the rest are direct
// reads of GPU_HW members (derived UsingMSAA() /
// UsingPerSampleShading(), m_disable_color_perspective). uv_limits is
// no longer an axis - the VS always emits a_uv_limits / v_uv_limits
// when textured, the FS gates consumption via u_uv_limits.
const EmbeddedShaderBlob& GetBatchVertexShaderBlob(bool textured,
                                                   bool msaa,
                                                   bool per_sample_shading,
                                                   bool noperspective_color);

// Batch FS, untextured slice (texture_mode == GPUTextureMode::Disabled).
// Three SPIR-V-structural axes:
//
//   - Input interpolation qualifier (must match the batch VS this FS
//     is bound with): none / centroid / sample. 3 variants.
//   - Color input perspective: standard / noperspective. 2 variants.
//   - Dual-source colour output: 1 location-0 output, or 2 outputs at
//     (location 0 index 0) and (location 0 index 1). 2 variants.
//
// 3 x 2 x 2 = 12 blobs. PGXP_DEPTH used to be a fourth axis (gating
// gl_FragDepth declaration) - collapsed to a runtime branch on
// u_pgxp_depth in the FS body. All per-call knobs (TRANSPARENCY tri-
// state, DITHERING, INTERLACING) and the remaining per-session knobs
// (TRUE_COLOR, DITHERING_SCALED, RESOLUTION_SCALE) collapse into
// specialisation constants on every blob.
//
// Textured FS slices (one per texture filter) will land in subsequent
// patches with their own k_batch_textured_*_fs_blobs arrays.
extern const EmbeddedShaderBlob k_batch_untextured_fs_blobs[12];

// Pick the right untextured batch FS blob. dual_source is derived per-
// call from m_supports_dual_source_blend AND render_mode (specifically
// TransparentAndOpaque or OnlyTransparent); the rest are per-session.
const EmbeddedShaderBlob& GetBatchUntexturedFragmentShaderBlob(bool msaa,
                                                               bool per_sample_shading,
                                                               bool noperspective_color,
                                                               bool dual_source);

// Batch FS, textured-with-Nearest-filter slice. Three SPIR-V-structural
// axes (m_texture_filter == GPUTextureFilter::Nearest sessions only;
// the four other filters land in subsequent patches with their own
// blob arrays):
//
//   - Input interpolation qualifier (none / centroid / sample). 3.
//   - Color input perspective (standard / noperspective). 2.
//   - Dual-source output (1 vs 2 outputs). 2.
//
// 3 x 2 x 2 = 12 blobs. New per-call specialisation constants
// relative to the untextured slice:
//
//   id = 107 PALETTE_4_BIT  (bool, actual_texture_mode == 0)
//   id = 108 PALETTE_8_BIT  (bool, actual_texture_mode == 1)
//   id = 109 RAW_TEXTURE    (bool, RawTextureBit set in texture_mode)
//
// PALETTE is derived inside the shader as PALETTE_4_BIT ||
// PALETTE_8_BIT. UV_LIMITS used to be a fifth axis (48 blobs prior) -
// collapsed to the u_uv_limits cbuffer scalar. PGXP_DEPTH used to be
// a fourth axis - collapsed to the u_pgxp_depth cbuffer scalar.
// Brings the Nearest cube into full parity with the Bilinear / JINC2
// / xBR families.
extern const EmbeddedShaderBlob k_batch_textured_nearest_fs_blobs[12];

// Pick the right textured-Nearest blob. Neither uv_limits nor
// pgxp_depth is an axis any more (both lifted to cbuffer scalars);
// the rest mirror the untextured helper.
const EmbeddedShaderBlob& GetBatchTexturedNearestFragmentShaderBlob(bool msaa,
                                                                    bool per_sample_shading,
                                                                    bool noperspective_color,
                                                                    bool dual_source);

// Batch FS, textured-with-Bilinear / BilinearBinAlpha-filter slice.
// Three structural axes (UV_LIMITS is implicit - all non-Nearest filter
// sessions have m_using_uv_limits forced true by ShouldUseUVLimits, so
// there is no non-UV variant; PGXP_DEPTH was a fourth axis but has
// been collapsed to a runtime branch on u_pgxp_depth):
//
//   - Input interpolation qualifier (none / centroid / sample). 3.
//   - Color input perspective (standard / noperspective). 2.
//   - Dual-source output. 2.
//
// 3 x 2 x 2 = 12 blobs. The Bilinear vs BilinearBinAlpha distinction
// is folded into a per-call BINALPHA specialisation constant (id=110).
extern const EmbeddedShaderBlob k_batch_textured_bilinear_fs_blobs[12];

const EmbeddedShaderBlob& GetBatchTexturedBilinearFragmentShaderBlob(bool msaa,
                                                                     bool per_sample_shading,
                                                                     bool noperspective_color,
                                                                     bool dual_source);

// Batch FS, textured-with-JINC2 / JINC2BinAlpha-filter slice. Same
// structural cube as the Bilinear family (3 axes, 12 blobs). JINC2 vs
// JINC2BinAlpha collapsed via the BINALPHA spec const.
extern const EmbeddedShaderBlob k_batch_textured_jinc2_fs_blobs[12];

const EmbeddedShaderBlob& GetBatchTexturedJINC2FragmentShaderBlob(bool msaa,
                                                                  bool per_sample_shading,
                                                                  bool noperspective_color,
                                                                  bool dual_source);

// Batch FS, textured-with-xBR / xBRBinAlpha-filter slice. Same
// structural cube as the Bilinear / JINC2 families (3 axes, 12 blobs).
// xBR vs xBRBinAlpha collapsed via the BINALPHA spec const.
extern const EmbeddedShaderBlob k_batch_textured_xbr_fs_blobs[12];

const EmbeddedShaderBlob& GetBatchTexturedXBRFragmentShaderBlob(bool msaa,
                                                                bool per_sample_shading,
                                                                bool noperspective_color,
                                                                bool dual_source);


// The accurate-blending batch fragment shaders: template 0 untextured, 1
// Nearest, 2 Bilinear, 3 JINC2, 4 xBR (each filter's BinAlpha variant by the
// BINALPHA spec constant, as above).
extern const EmbeddedShaderBlob k_batch_shader_blend_fs_blobs[10];
const EmbeddedShaderBlob& GetBatchShaderBlendFragmentShaderBlob(unsigned template_index, bool noperspective_color);

// Create a VkShaderModule directly from a pre-compiled SPIR-V blob.
//
// This intentionally bypasses Vulkan::ShaderCache: pre-baked SPIR-V is already
// final, so there is nothing to compile and nothing worth caching on disk.
// The caller owns the returned module and must vkDestroyShaderModule it.
// Returns VK_NULL_HANDLE on failure.
VkShaderModule CreateShaderModule(const uint32_t* spv, size_t spv_size_bytes);

} // namespace Vulkan::EmbeddedShaders
