// Pre-baked Vulkan source template for the BILINEAR FILTER FAMILY
// slice of GPU_HW_ShaderGen::GenerateBatchFragmentShader
// (texture_mode != Disabled AND m_texture_filter == Bilinear or
// BilinearBinAlpha). The two BinAlpha variants share a single
// template with a BINALPHA specialisation constant gating the
// alpha-quantisation step at the end of FilteredSampleFromVRAM; the
// SPIR-V structure is otherwise identical.
//
// Three SPIR-V-structural axes (UV limits is implicit for all non-
// Nearest filters - ShouldUseUVLimits() in gpu_hw.cpp forces
// m_using_uv_limits true when the filter is non-Nearest, so we never
// need a non-UV variant on this template; PGXP_DEPTH was a fourth
// axis but has been collapsed to a runtime branch on u_pgxp_depth):
//
//   - Input interpolation qualifier (none / centroid / sample). 3.
//   - Color input perspective (standard / noperspective). 2.
//   - Dual-source output (1 vs 2 outputs). 2.
//
// 3 x 2 x 2 = 12 blobs.
//
// New per-call specialisation constant relative to textured Nearest:
//
//   id = 110 BINALPHA  (bool, m_texture_filter == BilinearBinAlpha)
//
// All other spec constants are the same set as the textured Nearest
// slice (ids 0, 100-109).
//
// Bindings: same as textured Nearest - pipeline layout
// m_batch_pipeline_layout, BatchUBOData at set=0 binding=0, upscaled
// m_vram_read_texture sampler at set=0 binding=1.

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
layout(constant_id = 107) const bool PALETTE_4_BIT                 = false;
layout(constant_id = 108) const bool PALETTE_8_BIT                 = false;
layout(constant_id = 109) const bool RAW_TEXTURE                   = false;
layout(constant_id = 110) const bool BINALPHA                      = false;

// ---- Interpolation qualifier macros --------------------------------
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
// See batch_textured_nearest.frag.glsl for the offset-based layout
// explanation; same shape, minus u_uv_limits (filter templates always
// use UV limits via the settings-layer coupling, so they read
// v_uv_limits directly without a runtime gate).
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


// ---- VRAM atlas sampler --------------------------------------------
layout(set = 0, binding = 1) uniform sampler2D samp0;

// ---- Inputs from the batch VS --------------------------------------
// UV limits always present on filter shaders (ShouldUseUVLimits forces
// it true for non-Nearest filters).
layout(location = 0) in VertexData {
  COLOR_INTERP vec4  v_col0;
  INTERP       vec2  v_tex0;
  flat         uvec4 v_texpage;
  flat         vec4  v_uv_limits;
};

// ---- Outputs -------------------------------------------------------
#if !defined(SHADER_BLEND)
layout(location = 0, index = 0) out vec4 o_col0;
#if defined(DUAL_SOURCE)
layout(location = 0, index = 1) out vec4 o_col1;
#endif
#endif

// ---- Dithering matrix ----------------------------------------------
const int s_dither_values[16] = int[16](
  -4,  0, -3,  1,
   2, -2,  3, -1,
  -3,  1, -4,  0,
   3, -1,  2, -2
);

uvec3 ApplyDithering(uvec2 coord, uvec3 icol)
{
  uvec2 fc = DITHERING_SCALED
             ? (coord                      & uvec2(3u, 3u))
             : ((coord / RESOLUTION_SCALE) & uvec2(3u, 3u));
  int offset = s_dither_values[fc.y * 4u + fc.x];
  if (TRUE_COLOR)
    return uvec3(clamp(ivec3(icol) + ivec3(offset, offset, offset), 0, 255));
  else
    return uvec3(clamp((ivec3(icol) + ivec3(offset, offset, offset)) >> 3, 0, 31));
}

// ---- Texture-window helpers ----------------------------------------
uvec2 ApplyTextureWindow(uvec2 coords)
{
  uint x = (coords.x & u_texture_window_and.x) | u_texture_window_or.x;
  uint y = (coords.y & u_texture_window_and.y) | u_texture_window_or.y;
  return uvec2(x, y);
}

uvec2 ApplyUpscaledTextureWindow(uvec2 coords)
{
  uvec2 native_coords = coords / RESOLUTION_SCALE;
  uvec2 coords_offset = coords % RESOLUTION_SCALE;
  return (ApplyTextureWindow(native_coords) * RESOLUTION_SCALE) + coords_offset;
}

uvec2 FloatToIntegerCoords(vec2 coords)
{
  return uvec2((RESOLUTION_SCALE == 1u) ? roundEven(coords) : floor(coords));
}

uint RGBA8ToRGBA5551(vec4 v)
{
  uint r = uint(roundEven(v.r * 31.0));
  uint g = uint(roundEven(v.g * 31.0));
  uint b = uint(roundEven(v.b * 31.0));
  uint a = (v.a != 0.0) ? 1u : 0u;
  return r | (g << 5) | (b << 10) | (a << 15);
}

// ---- VRAM atlas sample (per-texel, no filtering) -------------------
vec4 SampleFromVRAM(uvec4 texpage, vec2 coords)
{
  const vec2 rcp_vram_size = vec2(1.0) / vec2(uvec2(1024u, 512u) * RESOLUTION_SCALE);
  bool palette = PALETTE_4_BIT || PALETTE_8_BIT;
  if (palette)
  {
    uvec2 icoord = ApplyTextureWindow(FloatToIntegerCoords(coords));
    uvec2 index_coord = icoord;
    if (PALETTE_4_BIT)
      index_coord.x /= 4u;
    else  // PALETTE_8_BIT
      index_coord.x /= 2u;

    uvec2 vicoord = uvec2(texpage.x + index_coord.x * RESOLUTION_SCALE,
                          texpage.y + index_coord.y * RESOLUTION_SCALE);
    vec4 texel       = texture(samp0, vec2(vicoord) * rcp_vram_size);
    uint vram_value  = RGBA8ToRGBA5551(texel);

    uint palette_index;
    if (PALETTE_4_BIT)
    {
      uint subpixel = icoord.x & 3u;
      palette_index = (vram_value >> (subpixel * 4u)) & 0x0Fu;
    }
    else
    {
      uint subpixel = icoord.x & 1u;
      palette_index = (vram_value >> (subpixel * 8u)) & 0xFFu;
    }
    uvec2 palette_icoord = uvec2(texpage.z + (palette_index * RESOLUTION_SCALE),
                                 texpage.w);
    return texture(samp0, vec2(palette_icoord) * rcp_vram_size);
  }
  else
  {
    uvec2 icoord = ApplyUpscaledTextureWindow(FloatToIntegerCoords(coords));
    uvec2 direct_icoord = uvec2(texpage.x + icoord.x, texpage.y + icoord.y);
    return texture(samp0, vec2(direct_icoord) * rcp_vram_size);
  }
}

// ---- Bilinear-filter four-tap weighted sample ----------------------
// Body lifted from gpu_hw_shadergen.cpp::WriteBatchTextureFilter for
// Bilinear / BilinearBinAlpha. BINALPHA collapses the two enum values
// into a single template (originally a #if BINALPHA preprocessor gate
// on the ialpha quantisation step).
void FilteredSampleFromVRAM(uvec4 texpage, vec2 coords, vec4 uv_limits,
                            out vec4 texcol, out float ialpha)
{
  // Coordinates of the four texels we interpolate between, clamped to
  // the polygon's UV bounds.
  vec2 texel_top_left = fract(coords) - vec2(0.5, 0.5);
  vec2 texel_offset   = sign(texel_top_left);
  vec4 fcoords        = max(coords.xyxy + vec4(0.0, 0.0, texel_offset.x, texel_offset.y),
                            vec4(0.0, 0.0, 0.0, 0.0));

  vec4 s00 = SampleFromVRAM(texpage, clamp(fcoords.xy, uv_limits.xy, uv_limits.zw));
  vec4 s10 = SampleFromVRAM(texpage, clamp(fcoords.zy, uv_limits.xy, uv_limits.zw));
  vec4 s01 = SampleFromVRAM(texpage, clamp(fcoords.xw, uv_limits.xy, uv_limits.zw));
  vec4 s11 = SampleFromVRAM(texpage, clamp(fcoords.zw, uv_limits.xy, uv_limits.zw));

  // Per-corner alpha: 1 if the texel is non-transparent, 0 if it is the
  // VRAM "0000h" sentinel.
  float a00 = (any(notEqual(s00, vec4(0.0)))) ? 1.0 : 0.0;
  float a10 = (any(notEqual(s10, vec4(0.0)))) ? 1.0 : 0.0;
  float a01 = (any(notEqual(s01, vec4(0.0)))) ? 1.0 : 0.0;
  float a11 = (any(notEqual(s11, vec4(0.0)))) ? 1.0 : 0.0;

  // Bilinear weights.
  vec2 weights = abs(texel_top_left);
  texcol = mix(mix(s00, s10, weights.x), mix(s01, s11, weights.x), weights.y);
  ialpha = mix(mix(a00, a10, weights.x), mix(a01, a11, weights.x), weights.y);

  // Compensate for partially transparent sampling.
  if (ialpha > 0.0)
    texcol.rgb /= vec3(ialpha, ialpha, ialpha);

  if (BINALPHA)
    ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
}

// --------------------------------------------------------------------

void main()
{
  uvec3 vertcol = uvec3(v_col0.rgb * vec3(255.0, 255.0, 255.0));

  if (INTERLACING)
  {
    if ((uint(gl_FragCoord.y) & 1u) == u_interlaced_displayed_field)
      discard;
  }

  // Texture coords. Palette textures index the VRAM atlas with native-
  // resolution coords; direct-16bpp textures use upscaled coords. The
  // UV-limits extension (direct only) compensates for upscaling.
  bool palette = PALETTE_4_BIT || PALETTE_8_BIT;
  vec2 coords = v_tex0;
  if (palette)
    coords /= float(RESOLUTION_SCALE);

  vec4 uv_limits = v_uv_limits;
  if (!palette)
  {
    uv_limits = uv_limits * float(RESOLUTION_SCALE);
    uv_limits.zw += float(RESOLUTION_SCALE - 1u);
  }

  vec4 texcol;
  float ialpha;
  FilteredSampleFromVRAM(v_texpage, coords, uv_limits, texcol, ialpha);
  if (ialpha < 0.5)
    discard;

  bool semitransparent = (texcol.a >= 0.5);

  uvec3 icolor;
  if (!TRUE_COLOR)
  {
    icolor = uvec3(texcol.rgb * vec3(255.0, 255.0, 255.0)) >> 3;
    if (!RAW_TEXTURE)
    {
      icolor = (icolor * vertcol) >> 4;
      if (DITHERING)
        icolor = ApplyDithering(uvec2(gl_FragCoord.xy), icolor);
      else
        icolor = min(icolor >> 3, uvec3(31u, 31u, 31u));
    }
  }
  else
  {
    icolor = uvec3(texcol.rgb * vec3(255.0, 255.0, 255.0));
    if (!RAW_TEXTURE)
    {
      icolor = (icolor * vertcol) >> 7;
      if (DITHERING)
        icolor = ApplyDithering(uvec2(gl_FragCoord.xy), icolor);
      else
        icolor = min(icolor, uvec3(255u, 255u, 255u));
    }
  }

  float oalpha = u_set_mask_while_drawing ? 1.0 : (semitransparent ? 1.0 : 0.0);

  float premultiply_alpha = (u_render_mode != 0u)
                            ? (ialpha * (semitransparent ? u_src_alpha_factor : 1.0))
                            : ialpha;

  vec3 color = TRUE_COLOR
               ? ((vec3(icolor) * premultiply_alpha) / vec3(255.0, 255.0, 255.0))
               : (floor(vec3(icolor) * premultiply_alpha) /  vec3(31.0,  31.0,  31.0));

#if !defined(SHADER_BLEND)
  if ((u_render_mode != 0u))
  {
    if (semitransparent)
    {
      o_col0 = vec4(color, oalpha);
#if defined(DUAL_SOURCE)
      o_col1 = vec4(0.0, 0.0, 0.0, u_dst_alpha_factor / ialpha);
#endif
      if ((u_render_mode == 2u))
        discard;
    }
    else
    {
      o_col0 = vec4(color, oalpha);
#if defined(DUAL_SOURCE)
      o_col1 = vec4(0.0, 0.0, 0.0, 1.0 - ialpha);
#endif
      if ((u_render_mode == 3u))
        discard;
    }
  }
  else
  {
    o_col0 = vec4(color, oalpha);
#if defined(DUAL_SOURCE)
    o_col1 = vec4(0.0, 0.0, 0.0, 1.0 - ialpha);
#endif
  }

  gl_FragDepth = (u_pgxp_depth != 0u) ? gl_FragCoord.z : (oalpha * gl_FragCoord.z);
#else
  // The console's own blending and mask test (batch_shader_blend.glslinc).
  beginInvocationInterlockARB();
  ShaderBlendPixel(true, icolor, (u_render_mode != 0u) && semitransparent, oalpha, ialpha);
  endInvocationInterlockARB();
#endif
}
