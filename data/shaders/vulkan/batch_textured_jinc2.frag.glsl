// Pre-baked Vulkan source template for the JINC2 FILTER FAMILY slice
// of GPU_HW_ShaderGen::GenerateBatchFragmentShader (texture_mode !=
// Disabled AND m_texture_filter is JINC2 or JINC2BinAlpha). Same
// structural cube as the Bilinear family (UV_LIMITS implicit, BINALPHA
// per-call spec const, PGXP_DEPTH routed via u_pgxp_depth).
//
// 3 (interp) x 2 (persp) x 2 (dual) = 12 blobs. Spec constants are
// identical to the Bilinear template; the FilteredSampleFromVRAM body
// is the only difference, lifted from gpu_hw_shadergen.cpp lines
// ~229-374 for JINC2 / JINC2BinAlpha.

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


layout(set = 0, binding = 1) uniform sampler2D samp0;

layout(location = 0) in VertexData {
  COLOR_INTERP vec4  v_col0;
  INTERP       vec2  v_tex0;
  flat         uvec4 v_texpage;
  flat         vec4  v_uv_limits;
};

#if !defined(SHADER_BLEND)
layout(location = 0, index = 0) out vec4 o_col0;
#if defined(DUAL_SOURCE)
layout(location = 0, index = 1) out vec4 o_col1;
#endif
#endif

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
    else
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

// ---- JINC2 (sinc-windowed) 4x4-tap weighted sample -----------------
// Lifted from gpu_hw_shadergen.cpp WriteBatchTextureFilter for JINC2 /
// JINC2BinAlpha. BINALPHA spec const collapses the two enum values.
const float JINC2_AR_STRENGTH = 0.8;
const float JINC2_WA          = 1.382300768;
const float JINC2_WB          = 2.576105976;

float dist2d(vec2 pt1, vec2 pt2)
{
  vec2 v = pt2 - pt1;
  return sqrt(dot(v, v));
}

vec4 jinc2_resampler(vec4 x)
{
  vec4 res = mix(sin(x * JINC2_WA) * sin(x * JINC2_WB) / (x * x),
                 vec4(JINC2_WA * JINC2_WB),
                 vec4(equal(x, vec4(0.0))));
  return res;
}

void FilteredSampleFromVRAM(uvec4 texpage, vec2 coords, vec4 uv_limits,
                            out vec4 texcol, out float ialpha)
{
  vec4 weights[4];
  vec2 dx = vec2(1.0, 0.0);
  vec2 dy = vec2(0.0, 1.0);
  vec2 pc = coords.xy;
  vec2 tc = (floor(pc - vec2(0.5, 0.5)) + vec2(0.5, 0.5));

  weights[0] = jinc2_resampler(vec4(dist2d(pc, tc        - dx       - dy),
                                    dist2d(pc, tc                   - dy),
                                    dist2d(pc, tc        + dx       - dy),
                                    dist2d(pc, tc + 2.0 * dx        - dy)));
  weights[1] = jinc2_resampler(vec4(dist2d(pc, tc        - dx           ),
                                    dist2d(pc, tc                       ),
                                    dist2d(pc, tc        + dx           ),
                                    dist2d(pc, tc + 2.0 * dx            )));
  weights[2] = jinc2_resampler(vec4(dist2d(pc, tc        - dx       + dy),
                                    dist2d(pc, tc                   + dy),
                                    dist2d(pc, tc        + dx       + dy),
                                    dist2d(pc, tc + 2.0 * dx        + dy)));
  weights[3] = jinc2_resampler(vec4(dist2d(pc, tc        - dx + 2.0 * dy),
                                    dist2d(pc, tc            + 2.0 * dy),
                                    dist2d(pc, tc        + dx + 2.0 * dy),
                                    dist2d(pc, tc + 2.0 * dx + 2.0 * dy)));

  #define SAMP(c) SampleFromVRAM(texpage, clamp((c), uv_limits.xy, uv_limits.zw))
  vec4 c00 = SAMP(tc         - dx        - dy); float a00 = any(notEqual(c00, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c10 = SAMP(tc                     - dy); float a10 = any(notEqual(c10, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c20 = SAMP(tc         + dx        - dy); float a20 = any(notEqual(c20, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c30 = SAMP(tc + 2.0 * dx          - dy); float a30 = any(notEqual(c30, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c01 = SAMP(tc         - dx            ); float a01 = any(notEqual(c01, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c11 = SAMP(tc                         ); float a11 = any(notEqual(c11, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c21 = SAMP(tc         + dx            ); float a21 = any(notEqual(c21, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c31 = SAMP(tc + 2.0 * dx              ); float a31 = any(notEqual(c31, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c02 = SAMP(tc         - dx        + dy); float a02 = any(notEqual(c02, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c12 = SAMP(tc                     + dy); float a12 = any(notEqual(c12, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c22 = SAMP(tc         + dx        + dy); float a22 = any(notEqual(c22, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c32 = SAMP(tc + 2.0 * dx          + dy); float a32 = any(notEqual(c32, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c03 = SAMP(tc         - dx + 2.0 * dy ); float a03 = any(notEqual(c03, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c13 = SAMP(tc              + 2.0 * dy ); float a13 = any(notEqual(c13, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c23 = SAMP(tc         + dx + 2.0 * dy ); float a23 = any(notEqual(c23, vec4(0.0))) ? 1.0 : 0.0;
  vec4 c33 = SAMP(tc + 2.0 * dx + 2.0 * dy   ); float a33 = any(notEqual(c33, vec4(0.0))) ? 1.0 : 0.0;
  #undef SAMP

  // Min/max samples around the centre cell (for anti-ringing).
  vec4 min_sample = min(min(c11, c21), min(c12, c22));
  float min_sample_alpha = min(min(a11, a21), min(a12, a22));
  vec4 max_sample = max(max(c11, c21), max(c12, c22));
  float max_sample_alpha = max(max(a11, a21), max(a12, a22));

  vec4 color;
  color  = vec4(dot(weights[0], vec4(c00.x, c10.x, c20.x, c30.x)),
                dot(weights[0], vec4(c00.y, c10.y, c20.y, c30.y)),
                dot(weights[0], vec4(c00.z, c10.z, c20.z, c30.z)),
                dot(weights[0], vec4(c00.w, c10.w, c20.w, c30.w)));
  color += vec4(dot(weights[1], vec4(c01.x, c11.x, c21.x, c31.x)),
                dot(weights[1], vec4(c01.y, c11.y, c21.y, c31.y)),
                dot(weights[1], vec4(c01.z, c11.z, c21.z, c31.z)),
                dot(weights[1], vec4(c01.w, c11.w, c21.w, c31.w)));
  color += vec4(dot(weights[2], vec4(c02.x, c12.x, c22.x, c32.x)),
                dot(weights[2], vec4(c02.y, c12.y, c22.y, c32.y)),
                dot(weights[2], vec4(c02.z, c12.z, c22.z, c32.z)),
                dot(weights[2], vec4(c02.w, c12.w, c22.w, c32.w)));
  color += vec4(dot(weights[3], vec4(c03.x, c13.x, c23.x, c33.x)),
                dot(weights[3], vec4(c03.y, c13.y, c23.y, c33.y)),
                dot(weights[3], vec4(c03.z, c13.z, c23.z, c33.z)),
                dot(weights[3], vec4(c03.w, c13.w, c23.w, c33.w)));
  float wsum = dot(weights[0], vec4(1.0)) + dot(weights[1], vec4(1.0))
             + dot(weights[2], vec4(1.0)) + dot(weights[3], vec4(1.0));
  color = color / wsum;

  float alpha;
  alpha  = dot(weights[0], vec4(a00, a10, a20, a30));
  alpha += dot(weights[1], vec4(a01, a11, a21, a31));
  alpha += dot(weights[2], vec4(a02, a12, a22, a32));
  alpha += dot(weights[3], vec4(a03, a13, a23, a33));
  alpha = alpha / wsum;

  // Anti-ringing clamp followed by lerp back to the unclamped value.
  vec4 aux = color;
  float aux_alpha = alpha;
  color = clamp(color, min_sample, max_sample);
  alpha = clamp(alpha, min_sample_alpha, max_sample_alpha);
  color = mix(aux, color, JINC2_AR_STRENGTH);
  alpha = mix(aux_alpha, alpha, JINC2_AR_STRENGTH);

  ialpha = alpha;
  texcol = color;

  if (ialpha > 0.0)
    texcol.rgb /= vec3(ialpha, ialpha, ialpha);

  if (BINALPHA)
    ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
}

void main()
{
  uvec3 vertcol = uvec3(v_col0.rgb * vec3(255.0, 255.0, 255.0));

  if (INTERLACING)
  {
    if ((uint(gl_FragCoord.y) & 1u) == u_interlaced_displayed_field)
      discard;
  }

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
