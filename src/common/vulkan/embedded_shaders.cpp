// Copyright 2026 LibretroAdmin
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#include "embedded_shaders.h"
#include "../log.h"
#include "context.h"
#include "util.h"
Log_SetChannel(Vulkan::EmbeddedShaders);

// Storage for the embedded SPIR-V blobs. Each .inc supplies a definition of
// the array and the matching size constant declared in embedded_shaders.h.
// The .inc files MUST be included inside the namespace block below so the
// names resolve to the header's extern declarations.
namespace Vulkan::EmbeddedShaders {

#include "embedded_spirv/adaptive_downsample_blur_fs.inc"
#include "embedded_spirv/adaptive_downsample_composite_fs.inc"
#include "embedded_spirv/adaptive_downsample_mip_fs.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_centroid_noperp_dual.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_centroid_noperp_nodual.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_centroid_persp_dual.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_centroid_persp_nodual.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_none_noperp_dual.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_none_noperp_nodual.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_none_persp_dual.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_none_persp_nodual.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_sample_noperp_dual.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_sample_noperp_nodual.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_sample_persp_dual.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_sample_persp_nodual.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_centroid_noperp_dual.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_centroid_noperp_nodual.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_centroid_persp_dual.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_centroid_persp_nodual.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_none_noperp_dual.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_none_noperp_nodual.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_none_persp_dual.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_none_persp_nodual.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_sample_noperp_dual.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_sample_noperp_nodual.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_sample_persp_dual.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_sample_persp_nodual.inc"
#include "embedded_spirv/batch_textured_nearest_fs_centroid_noperp_dual.inc"
#include "embedded_spirv/batch_textured_nearest_fs_centroid_noperp_nodual.inc"
#include "embedded_spirv/batch_textured_nearest_fs_centroid_persp_dual.inc"
#include "embedded_spirv/batch_textured_nearest_fs_centroid_persp_nodual.inc"
#include "embedded_spirv/batch_textured_nearest_fs_none_noperp_dual.inc"
#include "embedded_spirv/batch_textured_nearest_fs_none_noperp_nodual.inc"
#include "embedded_spirv/batch_textured_nearest_fs_none_persp_dual.inc"
#include "embedded_spirv/batch_textured_nearest_fs_none_persp_nodual.inc"
#include "embedded_spirv/batch_textured_nearest_fs_sample_noperp_dual.inc"
#include "embedded_spirv/batch_textured_nearest_fs_sample_noperp_nodual.inc"
#include "embedded_spirv/batch_textured_nearest_fs_sample_persp_dual.inc"
#include "embedded_spirv/batch_textured_nearest_fs_sample_persp_nodual.inc"
#include "embedded_spirv/batch_textured_xbr_fs_centroid_noperp_dual.inc"
#include "embedded_spirv/batch_textured_xbr_fs_centroid_noperp_nodual.inc"
#include "embedded_spirv/batch_textured_xbr_fs_centroid_persp_dual.inc"
#include "embedded_spirv/batch_textured_xbr_fs_centroid_persp_nodual.inc"
#include "embedded_spirv/batch_textured_xbr_fs_none_noperp_dual.inc"
#include "embedded_spirv/batch_textured_xbr_fs_none_noperp_nodual.inc"
#include "embedded_spirv/batch_textured_xbr_fs_none_persp_dual.inc"
#include "embedded_spirv/batch_textured_xbr_fs_none_persp_nodual.inc"
#include "embedded_spirv/batch_textured_xbr_fs_sample_noperp_dual.inc"
#include "embedded_spirv/batch_textured_xbr_fs_sample_noperp_nodual.inc"
#include "embedded_spirv/batch_textured_xbr_fs_sample_persp_dual.inc"
#include "embedded_spirv/batch_textured_xbr_fs_sample_persp_nodual.inc"
#include "embedded_spirv/batch_untextured_fs_centroid_noperp_dual.inc"
#include "embedded_spirv/batch_untextured_fs_centroid_noperp_nodual.inc"
#include "embedded_spirv/batch_untextured_fs_centroid_persp_dual.inc"
#include "embedded_spirv/batch_untextured_fs_centroid_persp_nodual.inc"
#include "embedded_spirv/batch_untextured_fs_none_noperp_dual.inc"
#include "embedded_spirv/batch_untextured_fs_none_noperp_nodual.inc"
#include "embedded_spirv/batch_untextured_fs_none_persp_dual.inc"
#include "embedded_spirv/batch_untextured_fs_none_persp_nodual.inc"
#include "embedded_spirv/batch_untextured_fs_sample_noperp_dual.inc"
#include "embedded_spirv/batch_untextured_fs_sample_noperp_nodual.inc"
#include "embedded_spirv/batch_untextured_fs_sample_persp_dual.inc"
#include "embedded_spirv/batch_untextured_fs_sample_persp_nodual.inc"
#include "embedded_spirv/batch_vs_textured_centroid_noperp.inc"
#include "embedded_spirv/batch_vs_textured_centroid_persp.inc"
#include "embedded_spirv/batch_vs_textured_none_noperp.inc"
#include "embedded_spirv/batch_vs_textured_none_persp.inc"
#include "embedded_spirv/batch_vs_textured_sample_noperp.inc"
#include "embedded_spirv/batch_vs_textured_sample_persp.inc"
#include "embedded_spirv/batch_vs_untextured_centroid_noperp.inc"
#include "embedded_spirv/batch_vs_untextured_centroid_persp.inc"
#include "embedded_spirv/batch_vs_untextured_none_noperp.inc"
#include "embedded_spirv/batch_vs_untextured_none_persp.inc"
#include "embedded_spirv/batch_vs_untextured_sample_noperp.inc"
#include "embedded_spirv/batch_vs_untextured_sample_persp.inc"
#include "embedded_spirv/box_sample_downsample_fs.inc"
#include "embedded_spirv/display_fs.inc"
#include "embedded_spirv/display_msaa_fs.inc"
#include "embedded_spirv/present_cursor_fs.inc"
#include "embedded_spirv/present_display_fs.inc"
#include "embedded_spirv/present_fullscreen_vs.inc"
#include "embedded_spirv/screen_quad_vs.inc"
#include "embedded_spirv/uv_quad_vs.inc"
#include "embedded_spirv/vram_copy_fs.inc"
#include "embedded_spirv/vram_fill_fs.inc"
#include "embedded_spirv/vram_readback_fs.inc"
#include "embedded_spirv/vram_readback_msaa_fs.inc"
#include "embedded_spirv/vram_update_depth_fs.inc"
#include "embedded_spirv/vram_update_depth_msaa_fs.inc"
#include "embedded_spirv/vram_write_ssbo_fs.inc"
#include "embedded_spirv/vram_write_texbuf_fs.inc"
#include "embedded_spirv/batch_untextured_fs_none_persp_blend.inc"
#include "embedded_spirv/batch_untextured_fs_none_noperp_blend.inc"
#include "embedded_spirv/batch_textured_nearest_fs_none_persp_blend.inc"
#include "embedded_spirv/batch_textured_nearest_fs_none_noperp_blend.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_none_persp_blend.inc"
#include "embedded_spirv/batch_textured_bilinear_fs_none_noperp_blend.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_none_persp_blend.inc"
#include "embedded_spirv/batch_textured_jinc2_fs_none_noperp_blend.inc"
#include "embedded_spirv/batch_textured_xbr_fs_none_persp_blend.inc"
#include "embedded_spirv/batch_textured_xbr_fs_none_noperp_blend.inc"

// Batch VS blob lookup table. Index encoding (must match
// GetBatchVertexShaderBlob below):
//
//   attr   = textured ? 1 : 0                          // 0..1
//   interp = per_sample_shading ? 2 : (msaa ? 1 : 0)   // 0..2
//   persp  = noperspective_color ? 1 : 0               // 0..1
//
//   index  = attr * 6 + interp * 2 + persp             // 0..11
//
// Macro keeps the table readable and ensures the symbol and size
// stay in lock-step.
#define BLOB(name) { k_##name, k_##name##_size_bytes }
const EmbeddedShaderBlob k_batch_vs_blobs[12] = {
  // attr = 0 (untextured)
  BLOB(batch_vs_untextured_none_persp),         // [ 0] interp=none,     persp=true
  BLOB(batch_vs_untextured_none_noperp),        // [ 1] interp=none,     persp=false
  BLOB(batch_vs_untextured_centroid_persp),     // [ 2] interp=centroid, persp=true
  BLOB(batch_vs_untextured_centroid_noperp),    // [ 3] interp=centroid, persp=false
  BLOB(batch_vs_untextured_sample_persp),       // [ 4] interp=sample,   persp=true
  BLOB(batch_vs_untextured_sample_noperp),      // [ 5] interp=sample,   persp=false
  // attr = 1 (textured, always with UV limits since the UV_LIMITS-
  // routing commit lifted the axis to the FS-side u_uv_limits
  // cbuffer scalar - the VS now unconditionally emits a_uv_limits +
  // v_uv_limits when textured, the FS decides at runtime whether to
  // consume it).
  BLOB(batch_vs_textured_none_persp),           // [ 6]
  BLOB(batch_vs_textured_none_noperp),          // [ 7]
  BLOB(batch_vs_textured_centroid_persp),       // [ 8]
  BLOB(batch_vs_textured_centroid_noperp),      // [ 9]
  BLOB(batch_vs_textured_sample_persp),         // [10]
  BLOB(batch_vs_textured_sample_noperp),        // [11]
};
#undef BLOB

const EmbeddedShaderBlob& GetBatchVertexShaderBlob(bool textured,
                                                   bool msaa,
                                                   bool per_sample_shading,
                                                   bool noperspective_color)
{
  const unsigned attr   = textured ? 1u : 0u;
  const unsigned interp = per_sample_shading ? 2u : (msaa ? 1u : 0u);
  const unsigned persp  = noperspective_color ? 1u : 0u;
  const unsigned index  = attr * 6u + interp * 2u + persp;
  return k_batch_vs_blobs[index];
}

// Batch FS untextured blob table. Index encoding (must match
// GetBatchUntexturedFragmentShaderBlob below):
//
//   interp = per_sample_shading ? 2 : (msaa ? 1 : 0)   (0..2)
//   persp  = noperspective_color ? 1 : 0               (0..1)
//   dual   = dual_source ? 1 : 0                       (0..1)
//   pgxp   = pgxp_depth ? 1 : 0                        (0..1)
//
//   index  = interp * 8 + persp * 4 + dual * 2 + pgxp  (0..23)
#define BLOB(name) { k_##name, k_##name##_size_bytes }
const EmbeddedShaderBlob k_batch_untextured_fs_blobs[12] = {
  BLOB(batch_untextured_fs_none_persp_nodual), // [ 0]
  BLOB(batch_untextured_fs_none_persp_dual), // [ 1]
  BLOB(batch_untextured_fs_none_noperp_nodual), // [ 2]
  BLOB(batch_untextured_fs_none_noperp_dual), // [ 3]
  BLOB(batch_untextured_fs_centroid_persp_nodual), // [ 4]
  BLOB(batch_untextured_fs_centroid_persp_dual), // [ 5]
  BLOB(batch_untextured_fs_centroid_noperp_nodual), // [ 6]
  BLOB(batch_untextured_fs_centroid_noperp_dual), // [ 7]
  BLOB(batch_untextured_fs_sample_persp_nodual), // [ 8]
  BLOB(batch_untextured_fs_sample_persp_dual), // [ 9]
  BLOB(batch_untextured_fs_sample_noperp_nodual), // [10]
  BLOB(batch_untextured_fs_sample_noperp_dual), // [11]
};
#undef BLOB

const EmbeddedShaderBlob& GetBatchUntexturedFragmentShaderBlob(bool msaa,
                                                               bool per_sample_shading,
                                                               bool noperspective_color,
                                                               bool dual_source)
{
  const unsigned interp = per_sample_shading ? 2u : (msaa ? 1u : 0u);
  const unsigned persp  = noperspective_color ? 1u : 0u;
  const unsigned dual   = dual_source ? 1u : 0u;
  const unsigned index  = interp * 4u + persp * 2u + dual;
  return k_batch_untextured_fs_blobs[index];
}


// Batch FS textured-Nearest blob table. Index encoding (must match
// GetBatchTexturedNearestFragmentShaderBlob below):
//
//   interp = per_sample_shading ? 2 : (msaa ? 1 : 0)        (0..2)
//   persp  = noperspective_color ? 1 : 0                    (0..1)
//   dual   = dual_source ? 1 : 0                            (0..1)
//   pgxp   = pgxp_depth ? 1 : 0                             (0..1)
//
//   index  = interp*8 + persp*4 + dual*2 + pgxp             (0..23)
//
// UV_LIMITS was a fifth axis (24 blobs prior) - collapsed to the
// u_uv_limits cbuffer scalar so the Nearest FS source has a single
// shape and runtime-branches on the cbuffer value. Brings the Nearest
// cube into parity shape with the Bilinear / JINC2 / xBR families.
#define BLOB(name) { k_##name, k_##name##_size_bytes }
const EmbeddedShaderBlob k_batch_textured_nearest_fs_blobs[12] = {
  BLOB(batch_textured_nearest_fs_none_persp_nodual), // [ 0]
  BLOB(batch_textured_nearest_fs_none_persp_dual), // [ 1]
  BLOB(batch_textured_nearest_fs_none_noperp_nodual), // [ 2]
  BLOB(batch_textured_nearest_fs_none_noperp_dual), // [ 3]
  BLOB(batch_textured_nearest_fs_centroid_persp_nodual), // [ 4]
  BLOB(batch_textured_nearest_fs_centroid_persp_dual), // [ 5]
  BLOB(batch_textured_nearest_fs_centroid_noperp_nodual), // [ 6]
  BLOB(batch_textured_nearest_fs_centroid_noperp_dual), // [ 7]
  BLOB(batch_textured_nearest_fs_sample_persp_nodual), // [ 8]
  BLOB(batch_textured_nearest_fs_sample_persp_dual), // [ 9]
  BLOB(batch_textured_nearest_fs_sample_noperp_nodual), // [10]
  BLOB(batch_textured_nearest_fs_sample_noperp_dual), // [11]
};
#undef BLOB

const EmbeddedShaderBlob& GetBatchTexturedNearestFragmentShaderBlob(bool msaa,
                                                               bool per_sample_shading,
                                                               bool noperspective_color,
                                                               bool dual_source)
{
  const unsigned interp = per_sample_shading ? 2u : (msaa ? 1u : 0u);
  const unsigned persp  = noperspective_color ? 1u : 0u;
  const unsigned dual   = dual_source ? 1u : 0u;
  const unsigned index  = interp * 4u + persp * 2u + dual;
  return k_batch_textured_nearest_fs_blobs[index];
}

// Batch FS textured-Bilinear / BilinearBinAlpha blob table. Index
// encoding (must match GetBatchTexturedBilinearFragmentShaderBlob
// below):
//
//   interp = per_sample_shading ? 2 : (msaa ? 1 : 0)    (0..2)
//   persp  = noperspective_color ? 1 : 0                (0..1)
//   dual   = dual_source ? 1 : 0                        (0..1)
//   pgxp   = pgxp_depth ? 1 : 0                         (0..1)
//
//   index  = interp*8 + persp*4 + dual*2 + pgxp         (0..23)
//
// BINALPHA (id=110) is a per-call spec constant, not a structural
// axis - it just toggles the final ialpha quantisation step.
#define BLOB(name) { k_##name, k_##name##_size_bytes }
const EmbeddedShaderBlob k_batch_textured_bilinear_fs_blobs[12] = {
  BLOB(batch_textured_bilinear_fs_none_persp_nodual), // [ 0]
  BLOB(batch_textured_bilinear_fs_none_persp_dual), // [ 1]
  BLOB(batch_textured_bilinear_fs_none_noperp_nodual), // [ 2]
  BLOB(batch_textured_bilinear_fs_none_noperp_dual), // [ 3]
  BLOB(batch_textured_bilinear_fs_centroid_persp_nodual), // [ 4]
  BLOB(batch_textured_bilinear_fs_centroid_persp_dual), // [ 5]
  BLOB(batch_textured_bilinear_fs_centroid_noperp_nodual), // [ 6]
  BLOB(batch_textured_bilinear_fs_centroid_noperp_dual), // [ 7]
  BLOB(batch_textured_bilinear_fs_sample_persp_nodual), // [ 8]
  BLOB(batch_textured_bilinear_fs_sample_persp_dual), // [ 9]
  BLOB(batch_textured_bilinear_fs_sample_noperp_nodual), // [10]
  BLOB(batch_textured_bilinear_fs_sample_noperp_dual), // [11]
};
#undef BLOB

const EmbeddedShaderBlob& GetBatchTexturedBilinearFragmentShaderBlob(bool msaa,
                                                               bool per_sample_shading,
                                                               bool noperspective_color,
                                                               bool dual_source)
{
  const unsigned interp = per_sample_shading ? 2u : (msaa ? 1u : 0u);
  const unsigned persp  = noperspective_color ? 1u : 0u;
  const unsigned dual   = dual_source ? 1u : 0u;
  const unsigned index  = interp * 4u + persp * 2u + dual;
  return k_batch_textured_bilinear_fs_blobs[index];
}

// Batch FS textured-JINC2 / JINC2BinAlpha blob table. Index encoding (same as
// Bilinear / matches GetJinc2... helper below):
//
//   interp = per_sample_shading ? 2 : (msaa ? 1 : 0)    (0..2)
//   persp  = noperspective_color ? 1 : 0                (0..1)
//   dual   = dual_source ? 1 : 0                        (0..1)
//   pgxp   = pgxp_depth ? 1 : 0                         (0..1)
//
//   index  = interp*8 + persp*4 + dual*2 + pgxp         (0..23)
#define BLOB(name) { k_##name, k_##name##_size_bytes }
const EmbeddedShaderBlob k_batch_textured_jinc2_fs_blobs[12] = {
  BLOB(batch_textured_jinc2_fs_none_persp_nodual), // [ 0]
  BLOB(batch_textured_jinc2_fs_none_persp_dual), // [ 1]
  BLOB(batch_textured_jinc2_fs_none_noperp_nodual), // [ 2]
  BLOB(batch_textured_jinc2_fs_none_noperp_dual), // [ 3]
  BLOB(batch_textured_jinc2_fs_centroid_persp_nodual), // [ 4]
  BLOB(batch_textured_jinc2_fs_centroid_persp_dual), // [ 5]
  BLOB(batch_textured_jinc2_fs_centroid_noperp_nodual), // [ 6]
  BLOB(batch_textured_jinc2_fs_centroid_noperp_dual), // [ 7]
  BLOB(batch_textured_jinc2_fs_sample_persp_nodual), // [ 8]
  BLOB(batch_textured_jinc2_fs_sample_persp_dual), // [ 9]
  BLOB(batch_textured_jinc2_fs_sample_noperp_nodual), // [10]
  BLOB(batch_textured_jinc2_fs_sample_noperp_dual), // [11]
};
#undef BLOB

const EmbeddedShaderBlob& GetBatchTexturedJINC2FragmentShaderBlob(bool msaa,
                                                               bool per_sample_shading,
                                                               bool noperspective_color,
                                                               bool dual_source)
{
  const unsigned interp = per_sample_shading ? 2u : (msaa ? 1u : 0u);
  const unsigned persp  = noperspective_color ? 1u : 0u;
  const unsigned dual   = dual_source ? 1u : 0u;
  const unsigned index  = interp * 4u + persp * 2u + dual;
  return k_batch_textured_jinc2_fs_blobs[index];
}

// Batch FS textured-xBR / xBRBinAlpha blob table. Index encoding (same as
// Bilinear / matches GetXbr... helper below):
//
//   interp = per_sample_shading ? 2 : (msaa ? 1 : 0)    (0..2)
//   persp  = noperspective_color ? 1 : 0                (0..1)
//   dual   = dual_source ? 1 : 0                        (0..1)
//   pgxp   = pgxp_depth ? 1 : 0                         (0..1)
//
//   index  = interp*8 + persp*4 + dual*2 + pgxp         (0..23)
#define BLOB(name) { k_##name, k_##name##_size_bytes }
const EmbeddedShaderBlob k_batch_textured_xbr_fs_blobs[12] = {
  BLOB(batch_textured_xbr_fs_none_persp_nodual), // [ 0]
  BLOB(batch_textured_xbr_fs_none_persp_dual), // [ 1]
  BLOB(batch_textured_xbr_fs_none_noperp_nodual), // [ 2]
  BLOB(batch_textured_xbr_fs_none_noperp_dual), // [ 3]
  BLOB(batch_textured_xbr_fs_centroid_persp_nodual), // [ 4]
  BLOB(batch_textured_xbr_fs_centroid_persp_dual), // [ 5]
  BLOB(batch_textured_xbr_fs_centroid_noperp_nodual), // [ 6]
  BLOB(batch_textured_xbr_fs_centroid_noperp_dual), // [ 7]
  BLOB(batch_textured_xbr_fs_sample_persp_nodual), // [ 8]
  BLOB(batch_textured_xbr_fs_sample_persp_dual), // [ 9]
  BLOB(batch_textured_xbr_fs_sample_noperp_nodual), // [10]
  BLOB(batch_textured_xbr_fs_sample_noperp_dual), // [11]
};
#undef BLOB

const EmbeddedShaderBlob& GetBatchTexturedXBRFragmentShaderBlob(bool msaa,
                                                               bool per_sample_shading,
                                                               bool noperspective_color,
                                                               bool dual_source)
{
  const unsigned interp = per_sample_shading ? 2u : (msaa ? 1u : 0u);
  const unsigned persp  = noperspective_color ? 1u : 0u;
  const unsigned dual   = dual_source ? 1u : 0u;
  const unsigned index  = interp * 4u + persp * 2u + dual;
  return k_batch_textured_xbr_fs_blobs[index];
}

// PSSwanStation: the accurate-blending batch fragment shaders (SHADER_BLEND;
// data/shaders/vulkan/batch_shader_blend.glslinc). Index: template * 2 +
// noperspective colour, the templates being untextured, Nearest, Bilinear,
// JINC2, xBR.
#define BLOB(name) { k_##name, k_##name##_size_bytes }
const EmbeddedShaderBlob k_batch_shader_blend_fs_blobs[10] = {
  BLOB(batch_untextured_fs_none_persp_blend),
  BLOB(batch_untextured_fs_none_noperp_blend),
  BLOB(batch_textured_nearest_fs_none_persp_blend),
  BLOB(batch_textured_nearest_fs_none_noperp_blend),
  BLOB(batch_textured_bilinear_fs_none_persp_blend),
  BLOB(batch_textured_bilinear_fs_none_noperp_blend),
  BLOB(batch_textured_jinc2_fs_none_persp_blend),
  BLOB(batch_textured_jinc2_fs_none_noperp_blend),
  BLOB(batch_textured_xbr_fs_none_persp_blend),
  BLOB(batch_textured_xbr_fs_none_noperp_blend),
};
#undef BLOB

const EmbeddedShaderBlob& GetBatchShaderBlendFragmentShaderBlob(unsigned template_index, bool noperspective_color)
{
  return k_batch_shader_blend_fs_blobs[(template_index < 5u ? template_index : 0u) * 2u + (noperspective_color ? 1u : 0u)];
}

VkShaderModule CreateShaderModule(const uint32_t* spv, size_t spv_size_bytes)
{
  if (!spv || spv_size_bytes == 0)
  {
    Log_ErrorPrint("CreateShaderModule called with empty SPIR-V blob");
    return VK_NULL_HANDLE;
  }
  if ((spv_size_bytes % sizeof(uint32_t)) != 0)
  {
    Log_ErrorPrintf("CreateShaderModule SPIR-V size %zu is not a multiple of 4", spv_size_bytes);
    return VK_NULL_HANDLE;
  }

  const VkShaderModuleCreateInfo ci = {
    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    nullptr,
    0,
    spv_size_bytes,
    spv,
  };

  VkShaderModule mod = VK_NULL_HANDLE;
  VkResult res = vkCreateShaderModule(g_vulkan_context->GetDevice(), &ci, nullptr, &mod);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateShaderModule() failed for embedded SPIR-V: ");
    return VK_NULL_HANDLE;
  }
  return mod;
}

} // namespace Vulkan::EmbeddedShaders
