// Pre-baked Vulkan source template for
// GPU_HW_ShaderGen::GenerateBatchVertexShader.
//
// Batch VS is the first of the two batch shaders, and its state space
// has THREE structurally distinct axes that SPIR-V decorations cannot
// be specialised across:
//
//   - Vertex attribute layout. 2 input attributes (untextured) or 5
//     (textured: pos, col, texcoord, texpage, uv_limits). UV_LIMITS
//     used to be a third attribute-layout variant gating a_uv_limits;
//     it is now routed via the FS-side u_uv_limits cbuffer scalar so
//     the VS unconditionally emits a_uv_limits + v_uv_limits when
//     textured. The 'in <type> <name>' declarations are part of the
//     SPIR-V module interface, not a spec-constant-controlled knob.
//
//   - Output interpolation qualifier. The fragment shader pairs with
//     these and the qualifiers compile to OpMemberDecorate Sample /
//     Centroid in the output VertexData block. There are three
//     mutually exclusive states (no qualifier, centroid, sample),
//     selected per session by UsingMSAA() and UsingPerSampleShading().
//
//   - Color output perspective. The 'noperspective' qualifier on
//     v_col0 compiles to OpMemberDecorate NoPerspective, again not
//     spec-constant-controllable. Two states, selected per session by
//     m_disable_color_perspective.
//
// Total structural variant count: 2 (attribute layouts) x 3
// (interpolation) x 2 (perspective) = 12. Body-level knobs that do
// not affect decorations (PGXP_DEPTH, RESOLUTION_SCALE) are handled
// as specialisation constants on every blob.
//
// This file is the SINGLE GLSL source. The regen tool compiles it
// twelve times with different -D combinations to produce the
// matching .inc blobs; the manifest of variants lives in
// tools/regen_vulkan_spirv.py. The C++ side picks one of the 12 at
// pipeline-create time via the helper in embedded_shaders.cpp.
//
// Spec constants (every blob):
//   constant_id = 0  RESOLUTION_SCALE (uint, common-knob convention).
//                    Drives the 1x-resolution vertex-offset gate, the
//                    upscaled texcoord scaling, and the texpage
//                    base-coordinate scaling.
//
// PGXP_DEPTH used to be a second spec const at constant_id = 3 driving
// the pos_z source selection (a_pos.z mask-bit-Z vs a_pos.w PGXP-
// replayed perspective-correct depth). It is now read from the batch
// UBO (u_pgxp_depth at offset 52) and the body uses a runtime ternary
// on the cbuffer scalar instead. Spec const wasn't a structural axis -
// the SPIR-V was a single blob with two PSO substitutions per VS - so
// the win is small (one less spec const per VS pipeline build, no PSO
// rebuild on PGXP flip for the VS portion). Matches the C++ shadergen
// VS routing (49c0f82) and the Vulkan FS routing in the preceding
// commit so the entire PGXP_DEPTH axis is cbuffer-routed end-to-end on
// both pipeline-emission paths.
//
// Bindings: vertex inputs and the batch UBO at set=0 binding=0 (which
// the pipeline layout m_batch_pipeline_layout already exposes for the
// FS; this VS reads u_pgxp_depth from the same descriptor). No push
// constants.

#version 450 core

// ---- Specialisation constants (body-level knobs) -------------------
layout(constant_id = 0) const uint RESOLUTION_SCALE = 1u;

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

// ---- Vertex inputs (attribute layout axis) -------------------------
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_col0;
#if defined(TEXTURED)
layout(location = 2) in uint a_texcoord;
layout(location = 3) in uint a_texpage;
// a_uv_limits emitted unconditionally when TEXTURED. UV_LIMITS used
// to gate this declaration, matching a separate attribute layout
// variant; routed via the FS-side u_uv_limits cbuffer scalar now,
// so the VS just always passes the value through to v_uv_limits and
// the FS decides at runtime whether to consume it. BatchVertex
// always carries the uv_limits field (gpu_hw.h:47), so the C++
// input layout always has a valid offset to bind ATTR4 to - the
// values are zero when ComputePolygonUVLimits didn't run and the
// FS short-circuits on u_uv_limits in that case.
layout(location = 4) in vec4 a_uv_limits;
#endif

// ---- Vertex outputs (interpolation + perspective axis) -------------
layout(location = 0) out VertexData {
  COLOR_INTERP vec4 v_col0;
#if defined(TEXTURED)
  INTERP vec2 v_tex0;
  flat uvec4 v_texpage;
  flat vec4 v_uv_limits;
#endif
};

// ---- Batch UBO -----------------------------------------------------
// VS only reads u_pgxp_depth (at offset 52). The pipeline layout has
// the UBO at set=0 binding=0 already - the FS reads more fields from
// the same descriptor. The std140 layout's preceding 32 bytes
// (u_texture_window_*, u_src/dst_alpha_factor, u_interlaced_*,
// u_set_mask_while_drawing) and the spec-const-handled slots between
// offset 32 and 51 are not redeclared here; the explicit
// layout(offset=52) is the GL_ARB_enhanced_layouts core-since-4.40
// way to jump past unused fields.
layout(std140, set = 0, binding = 0) uniform BatchUBOData {
  layout(offset = 52) uint u_pgxp_depth;
  // A sub-pixel shift of everything drawn, in pixels of the scaled
  // framebuffer, set per picture (SetDrawJitter); 0 otherwise.
  layout(offset = 64) float u_jitter_x;
  layout(offset = 68) float u_jitter_y;
};

// --------------------------------------------------------------------

void main()
{
  // 1x-resolution sub-pixel offset matches the shadergen original;
  // the >1x case has the offset pre-applied in CPU-side vertex prep,
  // so the shader uses 0 there.
  float vertex_offset = (RESOLUTION_SCALE == 1u) ? 0.5 : 0.0;

  // PSX VRAM addressing is 0..1023 horizontally, mapped to NDC -1..+1.
  float pos_x = ((a_pos.x + vertex_offset) /  512.0) - 1.0;
  float pos_y = ((a_pos.y + vertex_offset) / -256.0) + 1.0;

  // The sub-pixel jitter: one scaled pixel is 1/(512 S) of NDC across and
  // 1/(256 S) down (a_pos.y grows downwards, pos_y upwards here).
  pos_x += u_jitter_x / (512.0 * float(RESOLUTION_SCALE));
  pos_y -= u_jitter_y / (256.0 * float(RESOLUTION_SCALE));

  // Vulkan flips Y vs GL/D3D; shadergen does this unconditionally
  // under #if API_VULKAN. We are Vulkan-only here so it is also
  // unconditional.
  pos_y = -pos_y;

  // Depth source selection. With PGXP enabled and the depth-buffer
  // option on, the PGXP path replays a perspective-correct depth in
  // a_pos.w and the mask-bit Z slot (a_pos.z) is ignored. Otherwise
  // use the conventional mask-bit Z. u_pgxp_depth is a uniform
  // cbuffer scalar so this branch is uniform across the warp - the
  // driver collapses it to a single conditional move at compile
  // time, same shape as the C++ shadergen VS routing in 49c0f82.
  float pos_z = (u_pgxp_depth != 0u) ? a_pos.w : a_pos.z;
  float pos_w = a_pos.w;

  gl_Position = vec4(pos_x * pos_w, pos_y * pos_w, pos_z * pos_w, pos_w);

  v_col0 = a_col0;

#if defined(TEXTURED)
  // Texture coordinates are packed into a single uint per vertex
  // (lower 16 bits = U, upper 16 bits = V) and scaled by the upscale
  // factor to address into the upscaled VRAM atlas.
  v_tex0 = vec2(float((a_texcoord & 0xFFFFu) * RESOLUTION_SCALE),
                float((a_texcoord >> 16)     * RESOLUTION_SCALE));

  // a_texpage is similarly packed:
  //   bits  0..3   page base X  (in 64-texel native units)
  //   bit   4      page base Y  (256-texel native unit, 0 or 1)
  //   bits 16..21  CLUT X       (in 16-texel native units)
  //   bits 22..30  CLUT Y       (native rows)
  v_texpage.x = ( a_texpage        & 15u ) * 64u  * RESOLUTION_SCALE;
  v_texpage.y = ((a_texpage >>  4) &  1u ) * 256u * RESOLUTION_SCALE;
  v_texpage.z = ((a_texpage >> 16) & 63u ) * 16u  * RESOLUTION_SCALE;
  v_texpage.w = ((a_texpage >> 22) & 511u)        * RESOLUTION_SCALE;

  // UV limits are sent normalised; expand to 0..255 PSX-native here.
  // Always written when textured (UV_LIMITS guard dropped at the
  // collapse commit); the FS-side u_uv_limits gates whether to
  // consume the value.
  v_uv_limits = a_uv_limits * vec4(255.0, 255.0, 255.0, 255.0);
#endif
}
