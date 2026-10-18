// Pre-baked Vulkan source template for the UNTEXTURED slice of
// GPU_HW_ShaderGen::GenerateBatchFragmentShader (texture_mode ==
// GPUTextureMode::Disabled). The textured slice is several times
// larger and will land in subsequent patches, one or two texture
// filter values at a time; this template only emits SPIR-V valid for
// pipelines whose lookup_mode is Disabled (the renderer's untextured
// path - solid-shaded polygons, no VRAM sampling).
//
// Structural axes for the untextured batch FS:
//
//   - Input interpolation qualifier on v_col0. None / centroid /
//     sample, matching the batch VS pair this FS is bound with. (3
//     variants.)
//   - Color input perspective. Standard / noperspective. (2 variants.)
//   - Dual-source color output. With dual-source-blend hardware the
//     transparency=TransparentAndOpaque or =OnlyTransparent render
//     modes also write a second colour value at location 0 index 1
//     that drives SRC1_COLOR / SRC1_ALPHA in the blend equation. The
//     output declaration count is structural - the pipeline blend
//     state references location-0-index-1 or it does not. (2 variants.)
//
// Total untextured structural blobs: 3 x 2 x 2 = 12.
//
// PGXP_DEPTH used to be a fourth axis (writes gl_FragDepth or omits
// it) but has been collapsed to a runtime branch on the u_pgxp_depth
// cbuffer scalar - gl_FragDepth is always written now, with the
// expression chosen at runtime. Brings the untextured cube into
// parity with the other 4 FS templates after their parallel
// collapses.
//
// Per-call and per-session knobs that DO NOT affect SPIR-V structure
// collapse into specialisation constants on every blob:
//
//   constant_id =   0  RESOLUTION_SCALE             (uint)
//   constant_id = 103  DITHERING                    (bool)
//   constant_id = 104  INTERLACING                  (bool)
//   constant_id = 105  DITHERING_SCALED             (bool)
//   constant_id = 106  TRUE_COLOR                   (bool)
//
// TRANSPARENCY used to live as 3 spec consts at constant_id =
// 100/101/102 (TRANSPARENCY, TRANSPARENCY_ONLY_OPAQUE,
// TRANSPARENCY_ONLY_TRANSPARENT) encoding the 4-value
// BatchRenderMode enum. They have been collapsed to a runtime
// branch on the u_render_mode cbuffer scalar (offset 60 - the
// former u_pad2 slot). The C++ side re-uploads the UBO between
// two-pass DrawIndexed calls so each draw sees its matching
// u_render_mode value (see gpu_hw.cpp FlushRender). Mirrors the
// prior cbuffer-routing arc (DITHERING / INTERLACING / UV_LIMITS
// / PGXP_DEPTH).
//
// Bindings: pipeline layout m_batch_pipeline_layout. The dynamic UBO
// at set=0 binding=0 supplies the BatchUBOData fields read here
// (u_src_alpha_factor, u_dst_alpha_factor, u_interlaced_displayed_field,
// u_set_mask_while_drawing, u_render_mode). u_texture_window_* are
// present in the UBO but unused in the untextured path.

#version 450 core
#if defined(SHADER_BLEND)
#extension GL_ARB_fragment_shader_interlock : require
#extension GL_GOOGLE_include_directive : require
#endif

// ---- Specialisation constants --------------------------------------
layout(constant_id =   0) const uint RESOLUTION_SCALE              = 1u;
layout(constant_id = 103) const bool DITHERING                     = false;
layout(constant_id = 104) const bool INTERLACING                   = false;
layout(constant_id = 105) const bool DITHERING_SCALED              = false;
layout(constant_id = 106) const bool TRUE_COLOR                    = false;

// ---- Interpolation qualifier macros (structural) -------------------
#if defined(INTERP_SAMPLE)
#  define INTERP sample
#elif defined(INTERP_CENTROID)
#  define INTERP centroid
#else
#  define INTERP
#endif

#if defined(NOPERSP)
#  define COLOR_INTERP noperspective INTERP
#else
#  define COLOR_INTERP INTERP
#endif

// ---- Batch UBO -----------------------------------------------------
// Layout matches WriteBatchUniformBuffer in the shadergen. Field order
// is significant: the C++ side packs in this exact order. u_pgxp_depth
// at offset 52 is read at runtime to decide the gl_FragDepth write
// expression; the explicit layout(offset=52) skips past the spec-
// const-handled fields between offset 32 and 51 (u_resolution_scale,
// u_true_color, u_scaled_dithering, u_dithering, u_interlacing).
layout(std140, set = 0, binding = 0) uniform BatchUBOData {
  uvec2 u_texture_window_and;
  uvec2 u_texture_window_or;
  float u_src_alpha_factor;
  float u_dst_alpha_factor;
  uint  u_interlaced_displayed_field;
  bool  u_set_mask_while_drawing;
  layout(offset = 52) uint u_pgxp_depth;
  layout(offset = 60) uint u_render_mode;
  layout(offset = 64) uint u_transparency_mode;
  layout(offset = 68) uint u_check_mask;
};

#if defined(SHADER_BLEND)
#include "batch_shader_blend.glslinc"
#endif


// ---- Inputs from the batch VS --------------------------------------
layout(location = 0) in VertexData {
  COLOR_INTERP vec4 v_col0;
};

// ---- Outputs (DUAL_SOURCE axis) ------------------------------------
#if !defined(SHADER_BLEND)
layout(location = 0, index = 0) out vec4 o_col0;
#if defined(DUAL_SOURCE)
layout(location = 0, index = 1) out vec4 o_col1;
#endif
#endif

// PGXP_DEPTH axis collapsed: gl_FragDepth is now always written, with
// a runtime branch on u_pgxp_depth at the write site below choosing
// between v_pos.z pass-through (PGXP mode) and oalpha * v_pos.z (mask-
// bit encoding for legacy non-PGXP depth use).

// ---- Dithering matrix (CONSTANT in shadergen) ----------------------
// Same 4x4 Bayer values as DITHER_MATRIX in core/types.h.
const int s_dither_values[16] = int[16](
  -4,  0, -3,  1,
   2, -2,  3, -1,
  -3,  1, -4,  0,
   3, -1,  2, -2
);

uvec3 ApplyDithering(uvec2 coord, uvec3 icol)
{
  uvec2 fc = DITHERING_SCALED
             ? (coord                     & uvec2(3u, 3u))
             : ((coord / RESOLUTION_SCALE) & uvec2(3u, 3u));
  int offset = s_dither_values[fc.y * 4u + fc.x];

  if (TRUE_COLOR)
    return uvec3(clamp(ivec3(icol) + ivec3(offset, offset, offset), 0, 255));
  else
    return uvec3(clamp((ivec3(icol) + ivec3(offset, offset, offset)) >> 3, 0, 31));
}

void main()
{
  uvec3 vertcol = uvec3(v_col0.rgb * vec3(255.0, 255.0, 255.0));

  // INTERLACING discard (untextured can still hit a field-strobed draw).
  // fixYCoord on Vulkan is the identity function so we use gl_FragCoord.y
  // directly.
  if (INTERLACING)
  {
    if ((uint(gl_FragCoord.y) & 1u) == u_interlaced_displayed_field)
      discard;
  }

  // Untextured fragment: all pixels are semitransparent by convention,
  // colour comes from the interpolated vertex colour, ialpha is unity.
  uvec3 icolor = vertcol;
  float ialpha = 1.0;

  if (DITHERING)
  {
    icolor = ApplyDithering(uvec2(gl_FragCoord.xy), icolor);
  }
  else
  {
    if (!TRUE_COLOR)
      icolor >>= 3;
  }

  // Mask bit: untextured polygons clear the mask bit unless the
  // 'set mask while drawing' command bit is set.
  float oalpha = float(u_set_mask_while_drawing);

  // Premultiply alpha so the colour output can carry the mask in alpha.
  float premultiply_alpha = (u_render_mode != 0u) ? (ialpha * u_src_alpha_factor) : ialpha;

  vec3 color = TRUE_COLOR
               ? ((vec3(icolor) * premultiply_alpha) / vec3(255.0, 255.0, 255.0))
               : (floor(vec3(icolor) * premultiply_alpha) /  vec3(31.0,  31.0,  31.0));

  // Output. With dual-source blending o_col1 carries the destination
  // alpha factor; without, only o_col0 is written. The matching
  // pipeline blend state references SRC1_* or it does not, set by
  // GetBatchPipeline on the C++ side.
#if !defined(SHADER_BLEND)
  o_col0 = vec4(color, oalpha);
#if defined(DUAL_SOURCE)
  if ((u_render_mode != 0u))
    o_col1 = vec4(0.0, 0.0, 0.0, u_dst_alpha_factor / ialpha);
  else
    o_col1 = vec4(0.0, 0.0, 0.0, 1.0 - ialpha);
#endif

  // Untextured + transparency-only-opaque should never produce
  // visible pixels - the original discards them after the colour
  // output. This branch should be unreachable for untextured (the
  // shadergen documents it as "We shouldn't be rendering opaque
  // geometry only when untextured"), but the original code did not
  // assert this so we mirror its silence rather than redirect.

  // gl_FragDepth: always written (PGXP_DEPTH was a compile-time
  // axis pre-routing). u_pgxp_depth != 0 picks gl_FragCoord.z
  // pass-through (the rasterizer-interpolated VS depth IS the
  // PGXP-replayed depth in this mode); u_pgxp_depth == 0 picks
  // oalpha * gl_FragCoord.z to encode the PSX mask bit into the
  // depth buffer for the legacy non-PGXP path.
  gl_FragDepth = (u_pgxp_depth != 0u) ? gl_FragCoord.z : (oalpha * gl_FragCoord.z);
#else
  // The console's own blending and mask test (batch_shader_blend.glslinc).
  beginInvocationInterlockARB();
  ShaderBlendPixel(true, icolor, (u_render_mode != 0u), oalpha, ialpha);
  endInvocationInterlockARB();
#endif
}
