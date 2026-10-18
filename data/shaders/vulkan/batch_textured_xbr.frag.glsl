// Pre-baked Vulkan source template for the xBR FILTER FAMILY slice of
// GPU_HW_ShaderGen::GenerateBatchFragmentShader (texture_mode !=
// Disabled AND m_texture_filter is xBR or xBRBinAlpha). Same
// structural cube as the Bilinear / JINC2 families (UV_LIMITS implicit,
// BINALPHA per-call spec const, PGXP_DEPTH routed via u_pgxp_depth).
//
// 3 (interp) x 2 (persp) x 2 (dual) = 12 blobs. Spec constants are
// identical to the other filter families; the FilteredSampleFromVRAM
// body is lifted from gpu_hw_shadergen.cpp lines ~381-661 for xBR /
// xBRBinAlpha and is the largest body of any of the filter templates.
//
// The xBR algorithm samples a 5x5 neighbourhood around the source
// texel, computes YCbCr distances between adjacent samples, and
// decides per-quadrant whether to blend using the BLEND_NONE /
// BLEND_NORMAL / BLEND_DOMINANT directions. The big P() helper is the
// per-tap VRAM sample with UV-limit clamping.

#version 450 core
#extension GL_GOOGLE_include_directive : require
#if defined(SHADER_BLEND)
#extension GL_ARB_fragment_shader_interlock : require
#endif

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
  flat         uvec2 v_replacement;
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

#include "batch_texture_blocks.glslinc"

vec4 SampleFromVRAMBase(uvec4 texpage, vec2 coords)
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

// ---- xBR helpers ---------------------------------------------------
const int   BLEND_NONE                = 0;
const int   BLEND_NORMAL              = 1;
const int   BLEND_DOMINANT            = 2;
const float LUMINANCE_WEIGHT          = 1.0;
const float EQUAL_COLOR_TOLERANCE     = 0.1176470588235294;
const float STEEP_DIRECTION_THRESHOLD = 2.2;
const float DOMINANT_DIRECTION_THRESHOLD = 3.6;
const vec4  W_YCBCR                   = vec4(0.2627, 0.6780, 0.0593, 0.5);

float DistYCbCr(vec4 pixA, vec4 pixB)
{
  const float scaleB = 0.5 / (1.0 - W_YCBCR.b);
  const float scaleR = 0.5 / (1.0 - W_YCBCR.r);
  vec4 diff = pixA - pixB;
  float Y  = dot(diff, W_YCBCR);
  float Cb = scaleB * (diff.b - Y);
  float Cr = scaleR * (diff.r - Y);
  return sqrt(((LUMINANCE_WEIGHT * Y) * (LUMINANCE_WEIGHT * Y)) + (Cb * Cb) + (Cr * Cr));
}

bool IsPixEqual(vec4 pixA, vec4 pixB)
{
  return (DistYCbCr(pixA, pixB) < EQUAL_COLOR_TOLERANCE);
}

float get_left_ratio(vec2 center, vec2 origin, vec2 direction, vec2 scale_in)
{
  vec2 P0    = center - origin;
  vec2 proj  = direction * (dot(P0, direction) / dot(direction, direction));
  vec2 distv = P0 - proj;
  vec2 orth  = vec2(-direction.y, direction.x);
  float side = sign(dot(P0, orth));
  float v    = side * length(distv * scale_in);
  return smoothstep(-sqrt(2.0) / 2.0, sqrt(2.0) / 2.0, v);
}

#define P(coord, xoffs, yoffs) SampleFromVRAM(texpage, clamp(coord + vec2(float(xoffs), float(yoffs)), uv_limits.xy, uv_limits.zw))

// ---- xBR FilteredSampleFromVRAM ------------------------------------
void FilteredSampleFromVRAM(uvec4 texpage, vec2 coords, vec4 uv_limits,
                            out vec4 texcol, out float ialpha)
{
  vec2 scale_v = vec2(8.0, 8.0);
  vec2 pos = fract(coords.xy) - vec2(0.5, 0.5);
  vec2 coord = coords.xy - pos;

  // Sample 3x3 neighbourhood; W field carries the non-transparent flag.
  vec4 A = P(coord, -1, -1); float Aw = A.w; A.w = any(notEqual(A, vec4(0.0))) ? 1.0 : 0.0;
  vec4 B = P(coord,  0, -1); float Bw = B.w; B.w = any(notEqual(B, vec4(0.0))) ? 1.0 : 0.0;
  vec4 C = P(coord,  1, -1); float Cw = C.w; C.w = any(notEqual(C, vec4(0.0))) ? 1.0 : 0.0;
  vec4 D = P(coord, -1,  0); float Dw = D.w; D.w = any(notEqual(D, vec4(0.0))) ? 1.0 : 0.0;
  vec4 E = P(coord,  0,  0); float Ew = E.w; E.w = any(notEqual(E, vec4(0.0))) ? 1.0 : 0.0;
  vec4 F = P(coord,  1,  0); float Fw = F.w; F.w = any(notEqual(F, vec4(0.0))) ? 1.0 : 0.0;
  vec4 G = P(coord, -1,  1); float Gw = G.w; G.w = any(notEqual(G, vec4(0.0))) ? 1.0 : 0.0;
  vec4 H = P(coord,  0,  1); float Hw = H.w; H.w = any(notEqual(H, vec4(0.0))) ? 1.0 : 0.0;
  // NOTE: the original shadergen uses VECTOR_NEQ(H,..) for the Iw flag,
  // which is a documented quirk; we preserve it byte-for-byte to keep
  // the visual output identical.
  vec4 I = P(coord,  1,  1); float Iw = I.w; I.w = any(notEqual(H, vec4(0.0))) ? 1.0 : 0.0;

  // blendResult mapping: x|y|
  //                      w|z|
  ivec4 blendResult = ivec4(BLEND_NONE, BLEND_NONE, BLEND_NONE, BLEND_NONE);

  // Quadrant z (lower-right)
  if (!((all(equal(E, F)) && all(equal(H, I))) || (all(equal(E, H)) && all(equal(F, I)))))
  {
    float dist_H_F = DistYCbCr(G, E) + DistYCbCr(E, C) + DistYCbCr(P(coord, 0, 2), I) + DistYCbCr(I, P(coord, 2, 0)) + (4.0 * DistYCbCr(H, F));
    float dist_E_I = DistYCbCr(D, H) + DistYCbCr(H, P(coord, 1, 2)) + DistYCbCr(B, F) + DistYCbCr(F, P(coord, 2, 1)) + (4.0 * DistYCbCr(E, I));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_H_F) < dist_E_I;
    blendResult.z = ((dist_H_F < dist_E_I) && any(notEqual(E, F)) && any(notEqual(E, H))) ? (dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  // Quadrant w (lower-left)
  if (!((all(equal(D, E)) && all(equal(G, H))) || (all(equal(D, G)) && all(equal(E, H)))))
  {
    float dist_G_E = DistYCbCr(P(coord, -2, 1), D) + DistYCbCr(D, B) + DistYCbCr(P(coord, -1, 2), H) + DistYCbCr(H, F) + (4.0 * DistYCbCr(G, E));
    float dist_D_H = DistYCbCr(P(coord, -2, 0), G) + DistYCbCr(G, P(coord, 0, 2)) + DistYCbCr(A, E) + DistYCbCr(E, I) + (4.0 * DistYCbCr(D, H));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_D_H) < dist_G_E;
    blendResult.w = ((dist_G_E > dist_D_H) && any(notEqual(E, D)) && any(notEqual(E, H))) ? (dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  // Quadrant y (upper-right)
  if (!((all(equal(B, C)) && all(equal(E, F))) || (all(equal(B, E)) && all(equal(C, F)))))
  {
    float dist_E_C = DistYCbCr(D, B) + DistYCbCr(B, P(coord, 1, -2)) + DistYCbCr(H, F) + DistYCbCr(F, P(coord, 2, -1)) + (4.0 * DistYCbCr(E, C));
    float dist_B_F = DistYCbCr(A, E) + DistYCbCr(E, I) + DistYCbCr(P(coord, 0, -2), C) + DistYCbCr(C, P(coord, 2, 0)) + (4.0 * DistYCbCr(B, F));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_B_F) < dist_E_C;
    blendResult.y = ((dist_E_C > dist_B_F) && any(notEqual(E, B)) && any(notEqual(E, F))) ? (dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  // Quadrant x (upper-left)
  if (!((all(equal(A, B)) && all(equal(D, E))) || (all(equal(A, D)) && all(equal(B, E)))))
  {
    float dist_D_B = DistYCbCr(P(coord, -2, 0), A) + DistYCbCr(A, P(coord, 0, -2)) + DistYCbCr(G, E) + DistYCbCr(E, C) + (4.0 * DistYCbCr(D, B));
    float dist_A_E = DistYCbCr(P(coord, -2, -1), D) + DistYCbCr(D, H) + DistYCbCr(P(coord, -1, -2), B) + DistYCbCr(B, F) + (4.0 * DistYCbCr(A, E));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_D_B) < dist_A_E;
    blendResult.x = ((dist_D_B < dist_A_E) && any(notEqual(E, D)) && any(notEqual(E, B))) ? (dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  vec4 res = E;
  float resW = Ew;

  // Quadrant z (lower-right) blend
  if (blendResult.z != BLEND_NONE)
  {
    float dist_F_G = DistYCbCr(F, G);
    float dist_H_C = DistYCbCr(H, C);
    bool doLineBlend = (blendResult.z == BLEND_DOMINANT ||
      !((blendResult.y != BLEND_NONE && !IsPixEqual(E, G)) || (blendResult.w != BLEND_NONE && !IsPixEqual(E, C)) ||
        (IsPixEqual(G, H) && IsPixEqual(H, I) && IsPixEqual(I, F) && IsPixEqual(F, C) && !IsPixEqual(E, I))));

    vec2 origin = vec2(0.0, 1.0 / sqrt(2.0));
    vec2 direction = vec2(1.0, -1.0);
    if (doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_F_G <= dist_H_C) && any(notEqual(E, G)) && any(notEqual(D, G));
      bool haveSteepLine   = (STEEP_DIRECTION_THRESHOLD * dist_H_C <= dist_F_G) && any(notEqual(E, C)) && any(notEqual(B, C));
      origin = haveShallowLine ? vec2(0.0, 0.25) : vec2(0.0, 0.5);
      direction.x += haveShallowLine ? 1.0 : 0.0;
      direction.y -= haveSteepLine   ? 1.0 : 0.0;
    }

    vec4  blendPix = mix(H, F, step(DistYCbCr(E, F), DistYCbCr(E, H)));
    float blendW   = mix(Hw, Fw, step(DistYCbCr(E, F), DistYCbCr(E, H)));
    res  = mix(res,  blendPix, get_left_ratio(pos, origin, direction, scale_v));
    resW = mix(resW, blendW,   get_left_ratio(pos, origin, direction, scale_v));
  }

  // Quadrant w (lower-left) blend
  if (blendResult.w != BLEND_NONE)
  {
    float dist_H_A = DistYCbCr(H, A);
    float dist_D_I = DistYCbCr(D, I);
    bool doLineBlend = (blendResult.w == BLEND_DOMINANT ||
      !((blendResult.z != BLEND_NONE && !IsPixEqual(E, A)) || (blendResult.x != BLEND_NONE && !IsPixEqual(E, I)) ||
        (IsPixEqual(A, D) && IsPixEqual(D, G) && IsPixEqual(G, H) && IsPixEqual(H, I) && !IsPixEqual(E, G))));

    vec2 origin = vec2(-1.0 / sqrt(2.0), 0.0);
    vec2 direction = vec2(1.0, 1.0);
    if (doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_H_A <= dist_D_I) && any(notEqual(E, A)) && any(notEqual(B, A));
      bool haveSteepLine   = (STEEP_DIRECTION_THRESHOLD * dist_D_I <= dist_H_A) && any(notEqual(E, I)) && any(notEqual(F, I));
      origin = haveShallowLine ? vec2(-0.25, 0.0) : vec2(-0.5, 0.0);
      direction.y += haveShallowLine ? 1.0 : 0.0;
      direction.x += haveSteepLine   ? 1.0 : 0.0;
    }

    vec4  blendPix = mix(H, D, step(DistYCbCr(E, D), DistYCbCr(E, H)));
    float blendW   = mix(Hw, Dw, step(DistYCbCr(E, D), DistYCbCr(E, H)));
    res  = mix(res,  blendPix, get_left_ratio(pos, origin, direction, scale_v));
    resW = mix(resW, blendW,   get_left_ratio(pos, origin, direction, scale_v));
  }

  // Quadrant y (upper-right) blend
  if (blendResult.y != BLEND_NONE)
  {
    float dist_B_I = DistYCbCr(B, I);
    float dist_F_A = DistYCbCr(F, A);
    bool doLineBlend = (blendResult.y == BLEND_DOMINANT ||
      !((blendResult.x != BLEND_NONE && !IsPixEqual(E, I)) || (blendResult.z != BLEND_NONE && !IsPixEqual(E, A)) ||
        (IsPixEqual(I, F) && IsPixEqual(F, C) && IsPixEqual(C, B) && IsPixEqual(B, A) && !IsPixEqual(E, C))));

    vec2 origin = vec2(1.0 / sqrt(2.0), 0.0);
    vec2 direction = vec2(-1.0, -1.0);
    if (doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_B_I <= dist_F_A) && any(notEqual(E, I)) && any(notEqual(H, I));
      bool haveSteepLine   = (STEEP_DIRECTION_THRESHOLD * dist_F_A <= dist_B_I) && any(notEqual(E, A)) && any(notEqual(D, A));
      origin = haveShallowLine ? vec2(0.25, 0.0) : vec2(0.5, 0.0);
      direction.y -= haveShallowLine ? 1.0 : 0.0;
      direction.x -= haveSteepLine   ? 1.0 : 0.0;
    }

    vec4  blendPix = mix(F, B, step(DistYCbCr(E, B), DistYCbCr(E, F)));
    float blendW   = mix(Fw, Bw, step(DistYCbCr(E, B), DistYCbCr(E, F)));
    res  = mix(res,  blendPix, get_left_ratio(pos, origin, direction, scale_v));
    resW = mix(resW, blendW,   get_left_ratio(pos, origin, direction, scale_v));
  }

  // Quadrant x (upper-left) blend
  if (blendResult.x != BLEND_NONE)
  {
    float dist_D_C = DistYCbCr(D, C);
    float dist_B_G = DistYCbCr(B, G);
    bool doLineBlend = (blendResult.x == BLEND_DOMINANT ||
      !((blendResult.w != BLEND_NONE && !IsPixEqual(E, C)) || (blendResult.y != BLEND_NONE && !IsPixEqual(E, G)) ||
        (IsPixEqual(C, B) && IsPixEqual(B, A) && IsPixEqual(A, D) && IsPixEqual(D, G) && !IsPixEqual(E, A))));

    vec2 origin = vec2(0.0, -1.0 / sqrt(2.0));
    vec2 direction = vec2(-1.0, 1.0);
    if (doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_D_C <= dist_B_G) && any(notEqual(E, C)) && any(notEqual(F, C));
      bool haveSteepLine   = (STEEP_DIRECTION_THRESHOLD * dist_B_G <= dist_D_C) && any(notEqual(E, G)) && any(notEqual(H, G));
      origin = haveShallowLine ? vec2(0.0, -0.25) : vec2(0.0, -0.5);
      direction.x -= haveShallowLine ? 1.0 : 0.0;
      direction.y += haveSteepLine   ? 1.0 : 0.0;
    }

    vec4  blendPix = mix(D, B, step(DistYCbCr(E, B), DistYCbCr(E, D)));
    float blendW   = mix(Dw, Bw, step(DistYCbCr(E, B), DistYCbCr(E, D)));
    res  = mix(res,  blendPix, get_left_ratio(pos, origin, direction, scale_v));
    resW = mix(resW, blendW,   get_left_ratio(pos, origin, direction, scale_v));
  }

  ialpha = res.w;
  texcol = vec4(res.xyz, resW);

  if (ialpha > 0.0)
    texcol.rgb /= vec3(ialpha, ialpha, ialpha);

  if (BINALPHA)
    ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
}

#undef P

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
