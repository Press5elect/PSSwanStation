#include "gpu_hw.h"
#include "common/state_wrapper.h"
#include "cpu_core.h"
#include "gpu_sw_backend.h"
#include "pgxp.h"
#include "settings.h"
#include "system.h"
#include <cmath>
#include <cstring>
#include <sstream>
#include <tuple>
#define IS_POW2(value) (((value) & ((value) - 1)) == 0)

template<typename T>
ALWAYS_INLINE static constexpr std::tuple<T, T> MinMax(T v1, T v2)
{
  if (v1 > v2)
    return std::tie(v2, v1);
  return std::tie(v1, v2);
}


ALWAYS_INLINE static bool ShouldUseUVLimits()
{
  // We only need UV limits if PGXP is enabled, or texture filtering is enabled.
  return g_settings.gpu_pgxp_enable || g_settings.gpu_texture_filter != GPUTextureFilter::Nearest;
}

ALWAYS_INLINE static bool ShouldDisableColorPerspective()
{
  return g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_texture_correction && !g_settings.gpu_pgxp_color_correction;
}

GPU_HW::GPU_HW() : GPU()
{
  m_vram_ptr = m_vram_shadow.data();
}

GPU_HW::~GPU_HW()
{
  if (m_sw_renderer)
  {
    m_sw_renderer->Shutdown();
    m_sw_renderer.reset();
  }
}

bool GPU_HW::Initialize(HostDisplay* host_display)
{
  if (!GPU::Initialize(host_display))
    return false;

  m_resolution_scale = CalculateResolutionScale();
  m_multisamples = std::min(g_settings.gpu_multisamples, m_max_multisamples);
  m_render_api = host_display->GetRenderAPI();
  m_per_sample_shading = g_settings.gpu_per_sample_shading && m_supports_per_sample_shading;
  m_true_color = g_settings.gpu_true_color;
  m_scaled_dithering = g_settings.gpu_scaled_dithering;
  m_texture_filtering = g_settings.gpu_texture_filter;
  m_using_uv_limits = ShouldUseUVLimits();
  m_chroma_smoothing = g_settings.gpu_24bit_chroma_smoothing;
  m_downsample_mode = GetDownsampleMode(m_resolution_scale);
  m_disable_color_perspective = m_supports_disable_color_perspective && ShouldDisableColorPerspective();
  m_shader_precompile_mode = g_settings.gpu_shader_precompile_mode;

  if (m_multisamples != g_settings.gpu_multisamples)
  {
    g_host_interface->AddFormattedOSDMessage(
      20.0f, g_host_interface->TranslateString("OSDMessage", "%ux MSAA is not supported, using %ux instead."),
      g_settings.gpu_multisamples, m_multisamples);
  }
  if (!m_per_sample_shading && g_settings.gpu_per_sample_shading)
  {
    g_host_interface->AddOSDMessage(
      g_host_interface->TranslateStdString("OSDMessage", "SSAA is not supported, using MSAA instead."), 20.0f);
  }
  if (!m_supports_dual_source_blend && TextureFilterRequiresDualSourceBlend(m_texture_filtering))
  {
    m_texture_filtering = GPUTextureFilter::Nearest;
  }
  if (!m_supports_adaptive_downsampling && g_settings.gpu_resolution_scale > 1 &&
      g_settings.gpu_downsample_mode == GPUDownsampleMode::Adaptive)
  {
    g_host_interface->AddOSDMessage(
      g_host_interface->TranslateStdString(
        "OSDMessage", "Adaptive downsampling is not supported with the current renderer, using box filter instead."),
      20.0f);
  }

  m_pgxp_depth_buffer = g_settings.UsingPGXPDepthBuffer();

  // Seed the per-session cbuffer fields. m_batch_ubo_dirty defaults
  // to true so the first FlushRender uploads these; subsequent
  // sessions update them in UpdateHWSettings below.
  m_batch_ubo_data.u_resolution_scale = m_resolution_scale;
  m_batch_ubo_data.u_true_color = m_true_color ? 1u : 0u;
  m_batch_ubo_data.u_scaled_dithering = m_scaled_dithering ? 1u : 0u;
  m_batch_ubo_data.u_pgxp_depth = m_pgxp_depth_buffer ? 1u : 0u;
  m_batch_ubo_data.u_uv_limits = m_using_uv_limits ? 1u : 0u;

  UpdateSoftwareRenderer(false);

  return true;
}

void GPU_HW::Reset(bool clear_vram)
{
  GPU::Reset(clear_vram);

  m_batch_current_vertex_ptr = m_batch_start_vertex_ptr;

  m_vram_shadow.fill(0);
  if (m_sw_renderer)
    m_sw_renderer->Reset(clear_vram);

  m_batch = {};
  m_batch_ubo_data = {};

  // Re-seed the per-session cbuffer fields that the line above just
  // zeroed. These three settings live in the batch UBO but are owned
  // by Initialize / UpdateHWSettings, not by the per-draw SetDrawMode
  // path - if they stay at zero, the next FlushRender uploads
  // u_resolution_scale = 0, and every shader expression involving
  // RESOLUTION_SCALE (which the cbuffer-refactor patch macro-aliased
  // to u_resolution_scale) divides by zero. Concretely, RCP_VRAM_SIZE
  // = float2(1.0) / float2(uint2(1024, 512) * 0u) = INF, every
  // textured fragment samples at (INF, INF), every palette lookup
  // hits palette index 0 (transparent), every text glyph is
  // discarded. Untextured polygons render fine because their FS
  // never touches RESOLUTION_SCALE.
  //
  // The original Reset's zero-init was safe pre-refactor because all
  // batch UBO fields were per-draw-owned (texture window, alpha
  // factors, interlace field, mask bit) and got refilled before the
  // next FlushRender. The per-session fields break that invariant,
  // so we restore them explicitly here.
  m_batch_ubo_data.u_resolution_scale = m_resolution_scale;
  m_batch_ubo_data.u_true_color = m_true_color ? 1u : 0u;
  m_batch_ubo_data.u_scaled_dithering = m_scaled_dithering ? 1u : 0u;
  m_batch_ubo_data.u_pgxp_depth = m_pgxp_depth_buffer ? 1u : 0u;
  m_batch_ubo_data.u_uv_limits = m_using_uv_limits ? 1u : 0u;

  m_batch_ubo_dirty = true;
  m_current_depth = 1;

  SetFullVRAMDirtyRectangle();
}

bool GPU_HW::DoState(StateWrapper& sw, HostDisplayTexture** host_texture, bool update_display)
{
  if (!GPU::DoState(sw, host_texture, update_display))
    return false;

  // invalidate the whole VRAM read texture when loading state
  if (sw.IsReading())
  {
    m_batch_current_vertex_ptr = m_batch_start_vertex_ptr;
    SetFullVRAMDirtyRectangle();
    ResetBatchVertexDepth();
  }

  return true;
}

void GPU_HW::UpdateHWSettings(bool* framebuffer_changed, bool* shaders_changed,
                              bool* only_dim_changed, bool* downsample_changed,
                              bool* shader_source_changed, bool* display_only_source_changed)
{
  const uint32_t resolution_scale = CalculateResolutionScale();
  const uint32_t multisamples = std::min(m_max_multisamples, g_settings.gpu_multisamples);
  const bool per_sample_shading = g_settings.gpu_per_sample_shading && m_supports_per_sample_shading;
  const GPUDownsampleMode downsample_mode = GetDownsampleMode(resolution_scale);
  const bool use_uv_limits = ShouldUseUVLimits();
  const bool disable_color_perspective = m_supports_disable_color_perspective && ShouldDisableColorPerspective();

  *framebuffer_changed =
    (m_resolution_scale != resolution_scale || m_multisamples != multisamples ||
     // Downsample mode only changes framebuffer dimensions when Adaptive is
     // involved (Adaptive uses VRAM_WIDTH * res_scale for the display
     // texture and creates an additional weight texture; Disabled and Box
     // both use GPU_MAX_DISPLAY_WIDTH * res_scale and only differ in
     // whether the downsample texture / framebuffer chain exists at all).
     // The backend's UpdateSettings handler picks up the Disabled <-> Box
     // and other non-Adaptive downsample transitions via the dedicated
     // downsample_changed out-parameter below, which does NOT trigger the
     // full ReadVRAM -> CreateFramebuffer -> UpdateVRAM round-trip.
     (m_downsample_mode != downsample_mode &&
      (m_downsample_mode == GPUDownsampleMode::Adaptive || downsample_mode == GPUDownsampleMode::Adaptive)));

  // Split the shader-affecting setting comparison into "cache-
  // dimensioned settings changed" and "non-dimensioned settings
  // changed". A change confined to dimensioned settings can be
  // served by a backend that keeps a per-(filter, true_color,
  // scaled_dithering) sub-cube of batch pipelines populated across
  // toggles - the previous sub-cube's PSOs are still valid and
  // reachable, so DestroyPipelines can be skipped and
  // CompilePipelines just lazy-populates the new sub-cube on top of
  // whatever was already there. Any non-dimensioned change
  // (resolution scale, MSAA, per-sample shading, UV limits, chroma
  // smoothing, PGXP depth, colour perspective, precompile mode)
  // flips per-session spec constants or structural SPIR-V blob
  // choice applied to EVERY sub-cube and therefore requires a full
  // flush.
  //
  // Downsample mode is intentionally NOT in either diff list - it
  // does not affect the batch matrix at all (no spec const, no
  // structural SPIR-V choice). Downsample mode transitions are
  // surfaced separately via downsample_changed and serviced by the
  // backend without touching the batch pipeline cache.
  //
  // GPUTextureFilter, m_true_color and m_scaled_dithering are the
  // three settings the Vulkan backend currently dimensions over.
  // The set may extend to other 2-value spec consts in the future
  // without changing this API.
  const bool filter_diff           = (m_texture_filtering != g_settings.gpu_texture_filter);
  const bool true_color_diff       = (m_true_color != g_settings.gpu_true_color);
  const bool scaled_dithering_diff = (m_scaled_dithering != g_settings.gpu_scaled_dithering);
  const bool dim_diff              = filter_diff || true_color_diff || scaled_dithering_diff;
  const bool non_dim_diff =
    (m_resolution_scale != resolution_scale || m_multisamples != multisamples ||
     m_per_sample_shading != per_sample_shading ||
     m_chroma_smoothing != g_settings.gpu_24bit_chroma_smoothing ||
     m_pgxp_depth_buffer != g_settings.UsingPGXPDepthBuffer() ||
     m_disable_color_perspective != disable_color_perspective ||
     m_shader_precompile_mode != g_settings.gpu_shader_precompile_mode);
  *shaders_changed = dim_diff || non_dim_diff;
  if (only_dim_changed)
    *only_dim_changed = dim_diff && !non_dim_diff;
  if (downsample_changed)
    *downsample_changed = (m_downsample_mode != downsample_mode);

  // shader_source_changed: the narrower "HLSL / GLSL source string
  // actually changed" signal. Set true only when a setting that the
  // shader generator bakes into the emitted source has flipped.
  // Excludes the three per-session settings routed through the batch
  // UBO by 7b575a3 (resolution_scale, true_color, scaled_dithering) -
  // toggling those is a single 4-byte cbuffer write on the next
  // FlushRender and costs zero shader compilation on D3D11 / D3D12 /
  // OpenGL. Those three backends gate their DestroyShaders /
  // CompileShaders (D3D11) / DestroyPipelines / CompilePipelines
  // (D3D12) / CompilePrograms (OpenGL) round trip on this signal so
  // a cbuffer-only flip becomes a no-op for shader state. Vulkan
  // dimensions its batch pipeline cache over (filter, true_color,
  // scaled_dithering) via spec consts and gates on the broader
  // shaders_changed above instead - those three settings flip
  // shaders_changed without flipping shader_source_changed, and
  // Vulkan's only_dim_changed branch handles them via the dim cache
  // lazy-populate path.
  //
  // Even on D3D11 / D3D12 / OpenGL, where the three cbuffer-routed
  // settings cost zero, only_dim_changed inside shader_source_changed
  // still picks out the filter-only case (the three D3D / GL
  // backends added their own filter dim cache as 00cf11f / 10c53b8 /
  // f8b4a41) so a filter toggle preserves the previous filter's sub-
  // cube of batch shaders / pipelines instead of throwing it away.
  //
  // chroma_smoothing stays in here because it does change the display
  // FS source string on those backends (D3D12 passes it to
  // GenerateDisplayFragmentShader at gpu_hw_d3d12.cpp:~1639, D3D11 at
  // gpu_hw_d3d11.cpp:~1118, OpenGL at gpu_hw_opengl.cpp:~1145).
  // shader_precompile_mode stays in here because flipping from
  // Disabled to Enabled is the request to walk the whole matrix
  // synchronously now - the work is in CompilePipelines, not in
  // shader source, so the existing destroy-and-rebuild path is the
  // right hook even though no source actually changed.
  if (shader_source_changed)
  {
    *shader_source_changed =
      (m_texture_filtering != g_settings.gpu_texture_filter) ||
      (m_multisamples != multisamples) ||
      (m_per_sample_shading != per_sample_shading) ||
      (m_chroma_smoothing != g_settings.gpu_24bit_chroma_smoothing) ||
      (m_pgxp_depth_buffer != g_settings.UsingPGXPDepthBuffer()) ||
      (m_disable_color_perspective != disable_color_perspective) ||
      (m_shader_precompile_mode != g_settings.gpu_shader_precompile_mode);
  }

  // display_only_source_changed: true when shader_source_changed
  // fired SOLELY because chroma_smoothing flipped, and every other
  // shader-source-affecting setting stayed put. chroma_smoothing
  // is the one setting in the shader_source_changed set that
  // never reaches the batch matrix or the VRAM ops PSOs - it's a
  // DefineMacro in GenerateDisplayFragmentShader only, see
  // gpu_hw_shadergen.cpp:1056. Splitting it out lets a backend
  // service a chroma toggle by invalidating just the 6-slot
  // display PSO cache and letting it lazy-fault on the next
  // UpdateDisplay, instead of throwing away the 1164-PSO batch
  // matrix and 12-or-so non-batch VRAM ops PSOs that don't care.
  //
  // The non-batch / batch split here is asymmetric on purpose:
  // multisamples / per_sample_shading also affect non-batch PSOs
  // (VRAM fill / copy / write / update depth - see the
  // gpbuilder.SetMultisamples calls in gpu_hw_d3d12.cpp around
  // line 1439-1627) so a 'non-batch source changed' signal
  // wouldn't carve out a clean partial-rebuild path the way
  // chroma_smoothing does. shader_precompile_mode is similarly
  // excluded here - a precompile mode flip is a request to walk
  // the matrix synchronously NOW, which the full
  // DestroyPipelines + CompilePipelines path handles correctly
  // and a display-only clear obviously does not.
  if (display_only_source_changed)
  {
    const bool chroma_diff = (m_chroma_smoothing != g_settings.gpu_24bit_chroma_smoothing);
    const bool other_source_diff =
      (m_texture_filtering != g_settings.gpu_texture_filter) ||
      (m_multisamples != multisamples) ||
      (m_per_sample_shading != per_sample_shading) ||
      (m_pgxp_depth_buffer != g_settings.UsingPGXPDepthBuffer()) ||
      (m_disable_color_perspective != disable_color_perspective) ||
      (m_shader_precompile_mode != g_settings.gpu_shader_precompile_mode);
    *display_only_source_changed = chroma_diff && !other_source_diff;
  }

  m_resolution_scale = resolution_scale;
  m_multisamples = multisamples;
  m_per_sample_shading = per_sample_shading;
  m_true_color = g_settings.gpu_true_color;
  m_scaled_dithering = g_settings.gpu_scaled_dithering;
  m_texture_filtering = g_settings.gpu_texture_filter;
  m_using_uv_limits = use_uv_limits;
  m_chroma_smoothing = g_settings.gpu_24bit_chroma_smoothing;
  m_downsample_mode = downsample_mode;
  m_disable_color_perspective = disable_color_perspective;
  m_shader_precompile_mode = g_settings.gpu_shader_precompile_mode;

  // Push per-session cbuffer fields into the batch UBO. These three
  // settings used to drive shader recompiles on every flip; they now
  // ride the existing per-session UBO upload that already runs on
  // m_batch_ubo_dirty. Setting the flag here ensures the next
  // FlushRender picks up the new values before any draw uses them.
  // Toggling these settings is effectively free at the shader cache
  // level on D3D11 / D3D12 / OpenGL backends, and on Vulkan it
  // reduces redundant spec const churn.
  m_batch_ubo_data.u_resolution_scale = m_resolution_scale;
  m_batch_ubo_data.u_true_color = m_true_color ? 1u : 0u;
  m_batch_ubo_data.u_scaled_dithering = m_scaled_dithering ? 1u : 0u;
  m_batch_ubo_data.u_uv_limits = m_using_uv_limits ? 1u : 0u;
  m_batch_ubo_dirty = true;

  if (!m_supports_dual_source_blend && TextureFilterRequiresDualSourceBlend(m_texture_filtering))
    m_texture_filtering = GPUTextureFilter::Nearest;

  if (m_pgxp_depth_buffer != g_settings.UsingPGXPDepthBuffer())
  {
    m_pgxp_depth_buffer = g_settings.UsingPGXPDepthBuffer();
    m_batch.use_depth_buffer = false;
    if (m_pgxp_depth_buffer)
      ClearDepthBuffer();
    // The VS reads u_pgxp_depth from the batch UBO to select between
    // a_pos.z and a_pos.w as the depth source. The dirty bit is
    // already set in the bulk push above (line 336), so this update
    // rides the existing upload at the next FlushRender. The FS
    // still recompiles on this flip because PGXP_DEPTH gates
    // SV_Depth in the FS entry-point signature - shader_source_changed
    // upstream of this block has already triggered the rebuild path,
    // and the new shadergen state will pick up the fresh
    // m_pgxp_depth value when it generates the new FS source.
    m_batch_ubo_data.u_pgxp_depth = m_pgxp_depth_buffer ? 1u : 0u;
  }

  UpdateSoftwareRenderer(true);
}

static uint32_t PreviousPow2(uint32_t value)
{
  value |= (value >> 1);
  value |= (value >> 2);
  value |= (value >> 4);
  value |= (value >> 8);
  value |= (value >> 16);
  return value - (value >> 1);
}

uint32_t GPU_HW::CalculateResolutionScale() const
{
  uint32_t scale;
  if (g_settings.gpu_resolution_scale != 0)
    scale = std::clamp<uint32_t>(g_settings.gpu_resolution_scale, 1, m_max_resolution_scale);
  else
  {
    // Auto scaling. When the system is starting and all borders crop is enabled, the registers are zero, and
    // display_height therefore is also zero. Use the default size from the region in this case.
    const int32_t height = (m_crtc_state.display_height != 0) ?
                         static_cast<int32_t>(m_crtc_state.display_height) :
                         (m_console_is_pal ? (PAL_VERTICAL_ACTIVE_END - PAL_VERTICAL_ACTIVE_START) :
                                             (NTSC_VERTICAL_ACTIVE_END - NTSC_VERTICAL_ACTIVE_START));
    const int32_t preferred_scale =
      static_cast<int32_t>(std::ceil(static_cast<float>(m_host_display->GetWindowHeight()) / height));
    scale = static_cast<uint32_t>(std::clamp<int32_t>(preferred_scale, 1, m_max_resolution_scale));
  }
  if (g_settings.gpu_downsample_mode == GPUDownsampleMode::Adaptive && m_supports_adaptive_downsampling && scale > 1 &&
      !(IS_POW2(scale)))
    return PreviousPow2(scale);
  return scale;
}

GPUDownsampleMode GPU_HW::GetDownsampleMode(uint32_t resolution_scale) const
{
  if (resolution_scale == 1)
    return GPUDownsampleMode::Disabled;

  if (g_settings.gpu_downsample_mode == GPUDownsampleMode::Adaptive)
    return m_supports_adaptive_downsampling ? GPUDownsampleMode::Adaptive : GPUDownsampleMode::Box;

  return g_settings.gpu_downsample_mode;
}

void GPU_HW::UpdateVRAMReadTexture()
{
  ClearVRAMDirtyRectangle();
}

void GPU_HW::HandleFlippedQuadTextureCoordinates(BatchVertex* vertices)
{
  // Taken from beetle-psx gpu_polygon.cpp
  // For X/Y flipped 2D sprites, PSX games rely on a very specific rasterization behavior. If U or V is decreasing in X
  // or Y, and we use the provided U/V as is, we will sample the wrong texel as interpolation covers an entire pixel,
  // while PSX samples its interpolation essentially in the top-left corner and splats that interpolant across the
  // entire pixel. While we could emulate this reasonably well in native resolution by shifting our vertex coords by
  // 0.5, this breaks in upscaling scenarios, because we have several samples per native sample and we need NN rules to
  // hit the same UV every time. One approach here is to use interpolate at offset or similar tricks to generalize the
  // PSX interpolation patterns, but the problem is that vertices sharing an edge will no longer see the same UV (due to
  // different plane derivatives), we end up sampling outside the intended boundary and artifacts are inevitable, so the
  // only case where we can apply this fixup is for "sprites" or similar which should not share edges, which leads to
  // this unfortunate code below.

  // It might be faster to do more direct checking here, but the code below handles primitives in any order and
  // orientation, and is far more SIMD-friendly if needed.
  const float abx = vertices[1].x - vertices[0].x;
  const float aby = vertices[1].y - vertices[0].y;
  const float bcx = vertices[2].x - vertices[1].x;
  const float bcy = vertices[2].y - vertices[1].y;
  const float cax = vertices[0].x - vertices[2].x;
  const float cay = vertices[0].y - vertices[2].y;

  // Compute static derivatives, just assume W is uniform across the primitive and that the plane equation remains the
  // same across the quad. (which it is, there is no Z.. yet).
  const float dudx = -aby * static_cast<float>(vertices[2].u) - bcy * static_cast<float>(vertices[0].u) -
                     cay * static_cast<float>(vertices[1].u);
  const float dvdx = -aby * static_cast<float>(vertices[2].v) - bcy * static_cast<float>(vertices[0].v) -
                     cay * static_cast<float>(vertices[1].v);
  const float dudy = +abx * static_cast<float>(vertices[2].u) + bcx * static_cast<float>(vertices[0].u) +
                     cax * static_cast<float>(vertices[1].u);
  const float dvdy = +abx * static_cast<float>(vertices[2].v) + bcx * static_cast<float>(vertices[0].v) +
                     cax * static_cast<float>(vertices[1].v);
  const float area = bcx * cay - bcy * cax;

  // Detect and reject any triangles with 0 size texture area
  const int32_t texArea = (vertices[1].u - vertices[0].u) * (vertices[2].v - vertices[0].v) -
                      (vertices[2].u - vertices[0].u) * (vertices[1].v - vertices[0].v);

  // Leverage PGXP to further avoid 3D polygons that just happen to align this way after projection
  const bool is_3d = (vertices[0].w != vertices[1].w || vertices[0].w != vertices[2].w);

  // Shouldn't matter as degenerate primitives will be culled anyways.
  if (area == 0.0f || texArea == 0 || is_3d)
    return;

  // Use floats here as it'll be faster than integer divides.
  const float rcp_area = 1.0f / area;
  const float dudx_area = dudx * rcp_area;
  const float dudy_area = dudy * rcp_area;
  const float dvdx_area = dvdx * rcp_area;
  const float dvdy_area = dvdy * rcp_area;
  const bool neg_dudx = dudx_area < 0.0f;
  const bool neg_dudy = dudy_area < 0.0f;
  const bool neg_dvdx = dvdx_area < 0.0f;
  const bool neg_dvdy = dvdy_area < 0.0f;
  const bool zero_dudx = dudx_area == 0.0f;
  const bool zero_dudy = dudy_area == 0.0f;
  const bool zero_dvdx = dvdx_area == 0.0f;
  const bool zero_dvdy = dvdy_area == 0.0f;

  // If we have negative dU or dV in any direction, increment the U or V to work properly with nearest-neighbor in
  // this impl. If we don't have 1:1 pixel correspondence, this creates a slight "shift" in the sprite, but we
  // guarantee that we don't sample garbage at least. Overall, this is kinda hacky because there can be legitimate,
  // rare cases where 3D meshes hit this scenario, and a single texel offset can pop in, but this is way better than
  // having borked 2D overall.
  //
  // TODO: If perf becomes an issue, we can probably SIMD the 8 comparisons above,
  // create an 8-bit code, and use a LUT to get the offsets.
  // Case 1: U is decreasing in X, but no change in Y.
  // Case 2: U is decreasing in Y, but no change in X.
  // Case 3: V is decreasing in X, but no change in Y.
  // Case 4: V is decreasing in Y, but no change in X.
  if ((neg_dudx && zero_dudy) || (neg_dudy && zero_dudx))
  {
    vertices[0].u++;
    vertices[1].u++;
    vertices[2].u++;
    vertices[3].u++;
  }

  if ((neg_dvdx && zero_dvdy) || (neg_dvdy && zero_dvdx))
  {
    vertices[0].v++;
    vertices[1].v++;
    vertices[2].v++;
    vertices[3].v++;
  }
}

void GPU_HW::ComputePolygonUVLimits(BatchVertex* vertices, uint32_t num_vertices)
{
  uint16_t min_u = vertices[0].u, max_u = vertices[0].u, min_v = vertices[0].v, max_v = vertices[0].v;
  for (uint32_t i = 1; i < num_vertices; i++)
  {
    min_u = std::min<uint16_t>(min_u, vertices[i].u);
    max_u = std::max<uint16_t>(max_u, vertices[i].u);
    min_v = std::min<uint16_t>(min_v, vertices[i].v);
    max_v = std::max<uint16_t>(max_v, vertices[i].v);
  }

  if (min_u != max_u)
    max_u--;
  if (min_v != max_v)
    max_v--;

  for (uint32_t i = 0; i < num_vertices; i++)
    vertices[i].uv_limits = BatchVertex::PackUVLimits(min_u, max_u, min_v, max_v);
}

void GPU_HW::SetBatchDepthBuffer(bool enabled)
{
  if (GetBatchVertexCount() > 0)
  {
    FlushRender();
    EnsureVertexBufferSpaceForCurrentCommand();
  }
  m_batch.use_depth_buffer = enabled;
}

void GPU_HW::CheckForDepthClear(const BatchVertex* vertices, uint32_t num_vertices)
{
  float average_z;
  if (num_vertices == 3)
    average_z = std::min((vertices[0].w + vertices[1].w + vertices[2].w) / 3.0f, 1.0f);
  else
    average_z = std::min((vertices[0].w + vertices[1].w + vertices[2].w + vertices[3].w) / 4.0f, 1.0f);

  if ((average_z - m_last_depth_z) >= g_settings.gpu_pgxp_depth_clear_threshold)
  {
    if (GetBatchVertexCount() > 0)
    {
      FlushRender();
      EnsureVertexBufferSpaceForCurrentCommand();
    }

    ClearDepthBuffer();
  }

  m_last_depth_z = average_z;
}

uint32_t GPU_HW::GetAdaptiveDownsamplingMipLevels() const
{
  uint32_t levels = 0;
  uint32_t current_width = VRAM_WIDTH * m_resolution_scale;
  while (current_width >= VRAM_WIDTH)
  {
    levels++;
    current_width /= 2;
  }

  return levels;
}

GPU_HW::SmoothingUBOData GPU_HW::GetSmoothingUBO(uint32_t level, uint32_t left, uint32_t top, uint32_t width, uint32_t height, uint32_t tex_width,
                                                 uint32_t tex_height) const
{
  const float rcp_width = 1.0f / static_cast<float>(tex_width >> level);
  const float rcp_height = 1.0f / static_cast<float>(tex_height >> level);

  SmoothingUBOData data;
  data.min_uv[0] = static_cast<float>(left >> level) * rcp_width;
  data.min_uv[1] = static_cast<float>(top >> level) * rcp_height;
  data.max_uv[0] = static_cast<float>((left + width) >> level) * rcp_width;
  data.max_uv[1] = static_cast<float>((top + height) >> level) * rcp_height;
  data.rcp_size[0] = rcp_width;
  data.rcp_size[1] = rcp_height;

  return data;
}

void GPU_HW::DrawLine(float x0, float y0, uint32_t col0, float x1, float y1, uint32_t col1, float depth)
{
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  std::array<BatchVertex, 4> output;
  if (dx == 0.0f && dy == 0.0f)
  {
    // Degenerate, render a point.
    output[0].Set(x0, y0, depth, 1.0f, col0, 0, 0, 0);
    output[1].Set(x0 + 1.0f, y0, depth, 1.0f, col0, 0, 0, 0);
    output[2].Set(x1, y1 + 1.0f, depth, 1.0f, col0, 0, 0, 0);
    output[3].Set(x1 + 1.0f, y1 + 1.0f, depth, 1.0f, col0, 0, 0, 0);
  }
  else
  {
    const float abs_dx = std::fabs(dx);
    const float abs_dy = std::fabs(dy);
    float fill_dx, fill_dy;
    float dxdk, dydk;
    float pad_x0 = 0.0f;
    float pad_x1 = 0.0f;
    float pad_y0 = 0.0f;
    float pad_y1 = 0.0f;

    // Check for vertical or horizontal major lines.
    // When expanding to a rect, do so in the appropriate direction.
    // FIXME: This scheme seems to kinda work, but it seems very hard to find a method
    // that looks perfect on every game.
    // Vagrant Story speech bubbles are a very good test case here!
    if (abs_dx > abs_dy)
    {
      fill_dx = 0.0f;
      fill_dy = 1.0f;
      dxdk = 1.0f;
      dydk = dy / abs_dx;

      if (dx > 0.0f)
      {
        // Right
        pad_x1 = 1.0f;
        pad_y1 = dydk;
      }
      else
      {
        // Left
        pad_x0 = 1.0f;
        pad_y0 = -dydk;
      }
    }
    else
    {
      fill_dx = 1.0f;
      fill_dy = 0.0f;
      dydk = 1.0f;
      dxdk = dx / abs_dy;

      if (dy > 0.0f)
      {
        // Down
        pad_y1 = 1.0f;
        pad_x1 = dxdk;
      }
      else
      {
        // Up
        pad_y0 = 1.0f;
        pad_x0 = -dxdk;
      }
    }

    const float ox0 = x0 + pad_x0;
    const float oy0 = y0 + pad_y0;
    const float ox1 = x1 + pad_x1;
    const float oy1 = y1 + pad_y1;

    output[0].Set(ox0, oy0, depth, 1.0f, col0, 0, 0, 0);
    output[1].Set(ox0 + fill_dx, oy0 + fill_dy, depth, 1.0f, col0, 0, 0, 0);
    output[2].Set(ox1, oy1, depth, 1.0f, col1, 0, 0, 0);
    output[3].Set(ox1 + fill_dx, oy1 + fill_dy, depth, 1.0f, col1, 0, 0, 0);
  }

  AddVertex(output[0]);
  AddVertex(output[1]);
  AddVertex(output[2]);
  AddVertex(output[3]);
  AddVertex(output[2]);
  AddVertex(output[1]);
}

void GPU_HW::LoadVertices()
{
  if (m_GPUSTAT.check_mask_before_draw)
    m_current_depth++;

  const GPURenderCommand rc{m_render_command.bits};
  const uint32_t texpage = static_cast<uint32_t>(m_draw_mode.mode_reg.bits) | (static_cast<uint32_t>(m_draw_mode.palette_reg) << 16);
  const float depth = GetCurrentNormalizedVertexDepth();

  switch (rc.primitive)
  {
    case GPUPrimitive::Polygon:
    {
      uint16_t native_texcoords[4];
      const uint32_t first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;
      const bool pgxp = g_settings.gpu_pgxp_enable;

      const uint32_t num_vertices = rc.quad_polygon ? 4 : 3;
      std::array<BatchVertex, 4> vertices;
      std::array<std::array<int32_t, 2>, 4> native_vertex_positions;
      bool valid_w = g_settings.gpu_pgxp_texture_correction;
      for (uint32_t i = 0; i < num_vertices; i++)
      {
        const uint32_t color = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
        const uint64_t maddr_and_pos = m_fifo.Pop();
        const GPUVertexPosition vp{static_cast<uint32_t>(maddr_and_pos)};
        const uint16_t texcoord = textured ? static_cast<uint16_t>(FifoPop()) : 0;
        const int32_t native_x = m_drawing_offset.x + vp.x;
        const int32_t native_y = m_drawing_offset.y + vp.y;
        native_vertex_positions[i][0] = native_x;
        native_vertex_positions[i][1] = native_y;
        native_texcoords[i] = texcoord;
        vertices[i].Set(static_cast<float>(native_x), static_cast<float>(native_y), depth, 1.0f, color, texpage,
                        texcoord, 0xFFFF0000u);

        if (pgxp)
        {
          valid_w &=
            PGXP::GetPreciseVertex(static_cast<uint32_t>(maddr_and_pos >> 32), vp.bits, native_x, native_y, m_drawing_offset.x,
                                   m_drawing_offset.y, &vertices[i].x, &vertices[i].y, &vertices[i].w);
        }
      }
      if (pgxp)
      {
        if (!valid_w)
        {
          if (m_batch.use_depth_buffer)
            SetBatchDepthBuffer(false);
          for (BatchVertex& v : vertices)
            v.w = 1.0f;
        }
        else if (g_settings.gpu_pgxp_depth_buffer)
        {
          const bool use_depth = (m_batch.transparency_mode == GPUTransparencyMode::Disabled);
          if (m_batch.use_depth_buffer != use_depth)
            SetBatchDepthBuffer(use_depth);
          if (use_depth)
            CheckForDepthClear(vertices.data(), num_vertices);
        }
      }

      if (rc.quad_polygon && m_resolution_scale > 1)
        HandleFlippedQuadTextureCoordinates(vertices.data());

      if (m_using_uv_limits && textured)
        ComputePolygonUVLimits(vertices.data(), num_vertices);

      if (!IsDrawingAreaIsValid())
        return;

      // Cull polygons which are too large.
      const auto [min_x_12, max_x_12] = MinMax(native_vertex_positions[1][0], native_vertex_positions[2][0]);
      const auto [min_y_12, max_y_12] = MinMax(native_vertex_positions[1][1], native_vertex_positions[2][1]);
      const int32_t min_x = std::min(min_x_12, native_vertex_positions[0][0]);
      const int32_t max_x = std::max(max_x_12, native_vertex_positions[0][0]);
      const int32_t min_y = std::min(min_y_12, native_vertex_positions[0][1]);
      const int32_t max_y = std::max(max_y_12, native_vertex_positions[0][1]);

      if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
      {
      }
      else
      {
        const uint32_t clip_left = static_cast<uint32_t>(std::clamp<int32_t>(min_x, m_drawing_area.left, m_drawing_area.right));
        const uint32_t clip_right = static_cast<uint32_t>(std::clamp<int32_t>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
        const uint32_t clip_top = static_cast<uint32_t>(std::clamp<int32_t>(min_y, m_drawing_area.top, m_drawing_area.bottom));
        const uint32_t clip_bottom =
          static_cast<uint32_t>(std::clamp<int32_t>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

        m_vram_dirty_rect.Include(clip_left, clip_right, clip_top, clip_bottom);
        AddDrawTriangleTicks(native_vertex_positions[0][0], native_vertex_positions[0][1],
                             native_vertex_positions[1][0], native_vertex_positions[1][1],
                             native_vertex_positions[2][0], native_vertex_positions[2][1], rc.shading_enable,
                             rc.texture_enable, rc.transparency_enable);

        std::memcpy(m_batch_current_vertex_ptr, vertices.data(), sizeof(BatchVertex) * 3);
        m_batch_current_vertex_ptr += 3;
      }

      // quads
      if (rc.quad_polygon)
      {
        const int32_t min_x_123 = std::min(min_x_12, native_vertex_positions[3][0]);
        const int32_t max_x_123 = std::max(max_x_12, native_vertex_positions[3][0]);
        const int32_t min_y_123 = std::min(min_y_12, native_vertex_positions[3][1]);
        const int32_t max_y_123 = std::max(max_y_12, native_vertex_positions[3][1]);

        // Cull polygons which are too large.
        if ((max_x_123 - min_x_123) >= MAX_PRIMITIVE_WIDTH || (max_y_123 - min_y_123) >= MAX_PRIMITIVE_HEIGHT)
        {
        }
        else
        {
          const uint32_t clip_left = static_cast<uint32_t>(std::clamp<int32_t>(min_x_123, m_drawing_area.left, m_drawing_area.right));
          const uint32_t clip_right =
            static_cast<uint32_t>(std::clamp<int32_t>(max_x_123, m_drawing_area.left, m_drawing_area.right)) + 1u;
          const uint32_t clip_top = static_cast<uint32_t>(std::clamp<int32_t>(min_y_123, m_drawing_area.top, m_drawing_area.bottom));
          const uint32_t clip_bottom =
            static_cast<uint32_t>(std::clamp<int32_t>(max_y_123, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

          m_vram_dirty_rect.Include(clip_left, clip_right, clip_top, clip_bottom);
          AddDrawTriangleTicks(native_vertex_positions[2][0], native_vertex_positions[2][1],
                               native_vertex_positions[1][0], native_vertex_positions[1][1],
                               native_vertex_positions[3][0], native_vertex_positions[3][1], rc.shading_enable,
                               rc.texture_enable, rc.transparency_enable);

          AddVertex(vertices[2]);
          AddVertex(vertices[1]);
          AddVertex(vertices[3]);
        }
      }

      if (m_sw_renderer)
      {
        GPUBackendDrawPolygonCommand* cmd = m_sw_renderer->NewDrawPolygonCommand(num_vertices);
        FillDrawCommand(cmd, rc);

        for (uint32_t i = 0; i < num_vertices; i++)
        {
          GPUBackendDrawPolygonCommand::Vertex* vert = &cmd->vertices[i];
          vert->x = native_vertex_positions[i][0];
          vert->y = native_vertex_positions[i][1];
          vert->texcoord = native_texcoords[i];
          vert->color = vertices[i].color;
        }

        m_sw_renderer->PushCommand(cmd);
      }
    }
    break;

    case GPUPrimitive::Rectangle:
    {
      const uint32_t color = rc.color_for_first_vertex;
      const GPUVertexPosition vp{FifoPop()};
      const int32_t pos_x = TruncateGPUVertexPosition(m_drawing_offset.x + vp.x);
      const int32_t pos_y = TruncateGPUVertexPosition(m_drawing_offset.y + vp.y);

      const auto [texcoord_x, texcoord_y] = UnpackTexcoord(rc.texture_enable ? static_cast<uint16_t>(FifoPop()) : 0);
      uint16_t orig_tex_left = static_cast<uint16_t>(texcoord_x);
      uint16_t orig_tex_top = static_cast<uint16_t>(texcoord_y);
      int32_t rectangle_width;
      int32_t rectangle_height;
      switch (rc.rectangle_size)
      {
        case GPUDrawRectangleSize::R1x1:
          rectangle_width = 1;
          rectangle_height = 1;
          break;
        case GPUDrawRectangleSize::R8x8:
          rectangle_width = 8;
          rectangle_height = 8;
          break;
        case GPUDrawRectangleSize::R16x16:
          rectangle_width = 16;
          rectangle_height = 16;
          break;
        default:
        {
          const uint32_t width_and_height = FifoPop();
          rectangle_width = static_cast<int32_t>(width_and_height & VRAM_WIDTH_MASK);
          rectangle_height = static_cast<int32_t>((width_and_height >> 16) & VRAM_HEIGHT_MASK);

          if (rectangle_width >= MAX_PRIMITIVE_WIDTH || rectangle_height >= MAX_PRIMITIVE_HEIGHT)
            return;
        }
        break;
      }

      if (!IsDrawingAreaIsValid())
        return;

      // we can split the rectangle up into potentially 8 quads
      if (m_batch.use_depth_buffer)
        SetBatchDepthBuffer(false);

      // Split the rectangle into multiple quads if it's greater than 256x256, as the texture page should repeat.
      uint16_t tex_top = orig_tex_top;
      for (int32_t y_offset = 0; y_offset < rectangle_height;)
      {
        const int32_t quad_height = std::min<int32_t>(rectangle_height - y_offset, TEXTURE_PAGE_WIDTH - tex_top);
        const float quad_start_y = static_cast<float>(pos_y + y_offset);
        const float quad_end_y = quad_start_y + static_cast<float>(quad_height);
        const uint16_t tex_bottom = tex_top + static_cast<uint16_t>(quad_height);

        uint16_t tex_left = orig_tex_left;
        for (int32_t x_offset = 0; x_offset < rectangle_width;)
        {
          const int32_t quad_width = std::min<int32_t>(rectangle_width - x_offset, TEXTURE_PAGE_HEIGHT - tex_left);
          const float quad_start_x = static_cast<float>(pos_x + x_offset);
          const float quad_end_x = quad_start_x + static_cast<float>(quad_width);
          const uint16_t tex_right = tex_left + static_cast<uint16_t>(quad_width);
          const uint32_t uv_limits = BatchVertex::PackUVLimits(tex_left, tex_right - 1, tex_top, tex_bottom - 1);

          AddNewVertex(quad_start_x, quad_start_y, depth, 1.0f, color, texpage, tex_left, tex_top, uv_limits);
          AddNewVertex(quad_end_x, quad_start_y, depth, 1.0f, color, texpage, tex_right, tex_top, uv_limits);
          AddNewVertex(quad_start_x, quad_end_y, depth, 1.0f, color, texpage, tex_left, tex_bottom, uv_limits);

          AddNewVertex(quad_start_x, quad_end_y, depth, 1.0f, color, texpage, tex_left, tex_bottom, uv_limits);
          AddNewVertex(quad_end_x, quad_start_y, depth, 1.0f, color, texpage, tex_right, tex_top, uv_limits);
          AddNewVertex(quad_end_x, quad_end_y, depth, 1.0f, color, texpage, tex_right, tex_bottom, uv_limits);

          x_offset += quad_width;
          tex_left = 0;
        }

        y_offset += quad_height;
        tex_top = 0;
      }

      const uint32_t clip_left = static_cast<uint32_t>(std::clamp<int32_t>(pos_x, m_drawing_area.left, m_drawing_area.right));
      const uint32_t clip_right =
        static_cast<uint32_t>(std::clamp<int32_t>(pos_x + rectangle_width, m_drawing_area.left, m_drawing_area.right)) + 1u;
      const uint32_t clip_top = static_cast<uint32_t>(std::clamp<int32_t>(pos_y, m_drawing_area.top, m_drawing_area.bottom));
      const uint32_t clip_bottom =
        static_cast<uint32_t>(std::clamp<int32_t>(pos_y + rectangle_height, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

      m_vram_dirty_rect.Include(clip_left, clip_right, clip_top, clip_bottom);
      AddDrawRectangleTicks(clip_right - clip_left, clip_bottom - clip_top, rc.texture_enable, rc.transparency_enable);

      if (m_sw_renderer)
      {
        GPUBackendDrawRectangleCommand* cmd = m_sw_renderer->NewDrawRectangleCommand();
        FillDrawCommand(cmd, rc);
        cmd->color = color;
        cmd->x = pos_x;
        cmd->y = pos_y;
        cmd->width = static_cast<uint16_t>(rectangle_width);
        cmd->height = static_cast<uint16_t>(rectangle_height);
        cmd->texcoord = (static_cast<uint16_t>(texcoord_y) << 8) | static_cast<uint16_t>(texcoord_x);
        m_sw_renderer->PushCommand(cmd);
      }
    }
    break;

    case GPUPrimitive::Line:
    {
      if (m_batch.use_depth_buffer)
        SetBatchDepthBuffer(false);

      if (!rc.polyline)
      {
        uint32_t start_color, end_color;
        GPUVertexPosition start_pos, end_pos;
        if (rc.shading_enable)
        {
          start_color = rc.color_for_first_vertex;
          start_pos.bits = FifoPop();
          end_color = FifoPop() & UINT32_C(0x00FFFFFF);
          end_pos.bits = FifoPop();
        }
        else
        {
          start_color = end_color = rc.color_for_first_vertex;
          start_pos.bits = FifoPop();
          end_pos.bits = FifoPop();
        }

        if (!IsDrawingAreaIsValid())
          return;

        int32_t start_x = start_pos.x + m_drawing_offset.x;
        int32_t start_y = start_pos.y + m_drawing_offset.y;
        int32_t end_x = end_pos.x + m_drawing_offset.x;
        int32_t end_y = end_pos.y + m_drawing_offset.y;
        const auto [min_x, max_x] = MinMax(start_x, end_x);
        const auto [min_y, max_y] = MinMax(start_y, end_y);
        if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
          return;

        const uint32_t clip_left = static_cast<uint32_t>(std::clamp<int32_t>(min_x, m_drawing_area.left, m_drawing_area.right));
        const uint32_t clip_right = static_cast<uint32_t>(std::clamp<int32_t>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
        const uint32_t clip_top = static_cast<uint32_t>(std::clamp<int32_t>(min_y, m_drawing_area.top, m_drawing_area.bottom));
        const uint32_t clip_bottom =
          static_cast<uint32_t>(std::clamp<int32_t>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

        m_vram_dirty_rect.Include(clip_left, clip_right, clip_top, clip_bottom);
        AddDrawLineTicks(clip_right - clip_left, clip_bottom - clip_top, rc.shading_enable);

        // TODO: Should we do a PGXP lookup here? Most lines are 2D.
        DrawLine(static_cast<float>(start_x), static_cast<float>(start_y), start_color, static_cast<float>(end_x),
                 static_cast<float>(end_y), end_color, depth);

        if (m_sw_renderer)
        {
          GPUBackendDrawLineCommand* cmd = m_sw_renderer->NewDrawLineCommand(2);
          FillDrawCommand(cmd, rc);
          cmd->vertices[0].Set(start_x, start_y, start_color);
          cmd->vertices[1].Set(end_x, end_y, end_color);
          m_sw_renderer->PushCommand(cmd);
        }
      }
      else
      {
        // Multiply by two because we don't use line strips.
        const uint32_t num_vertices = GetPolyLineVertexCount();

        if (!IsDrawingAreaIsValid())
          return;

        const bool shaded = rc.shading_enable;

        uint32_t buffer_pos = 0;
        const GPUVertexPosition start_vp{m_blit_buffer[buffer_pos++]};
        int32_t start_x = start_vp.x + m_drawing_offset.x;
        int32_t start_y = start_vp.y + m_drawing_offset.y;
        uint32_t start_color = rc.color_for_first_vertex;

        GPUBackendDrawLineCommand* cmd;
        if (m_sw_renderer)
        {
          cmd = m_sw_renderer->NewDrawLineCommand(num_vertices);
          FillDrawCommand(cmd, rc);
          cmd->vertices[0].Set(start_x, start_y, start_color);
        }
        else
        {
          cmd = nullptr;
        }

        for (uint32_t i = 1; i < num_vertices; i++)
        {
          const uint32_t end_color = shaded ? (m_blit_buffer[buffer_pos++] & UINT32_C(0x00FFFFFF)) : start_color;
          const GPUVertexPosition vp{m_blit_buffer[buffer_pos++]};
          const int32_t end_x = m_drawing_offset.x + vp.x;
          const int32_t end_y = m_drawing_offset.y + vp.y;

          const auto [min_x, max_x] = MinMax(start_x, end_x);
          const auto [min_y, max_y] = MinMax(start_y, end_y);
          if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
          {
          }
          else
          {
            const uint32_t clip_left = static_cast<uint32_t>(std::clamp<int32_t>(min_x, m_drawing_area.left, m_drawing_area.right));
            const uint32_t clip_right =
              static_cast<uint32_t>(std::clamp<int32_t>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
            const uint32_t clip_top = static_cast<uint32_t>(std::clamp<int32_t>(min_y, m_drawing_area.top, m_drawing_area.bottom));
            const uint32_t clip_bottom =
              static_cast<uint32_t>(std::clamp<int32_t>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

            m_vram_dirty_rect.Include(clip_left, clip_right, clip_top, clip_bottom);
            AddDrawLineTicks(clip_right - clip_left, clip_bottom - clip_top, rc.shading_enable);

            // TODO: Should we do a PGXP lookup here? Most lines are 2D.
            DrawLine(static_cast<float>(start_x), static_cast<float>(start_y), start_color, static_cast<float>(end_x),
                     static_cast<float>(end_y), end_color, depth);
          }

          start_x = end_x;
          start_y = end_y;
          start_color = end_color;

          if (cmd)
            cmd->vertices[i].Set(end_x, end_y, end_color);
        }

        if (cmd)
          m_sw_renderer->PushCommand(cmd);
      }
    }
    break;

    default:
      break;
  }
}

void GPU_HW::CalcScissorRect(int* left, int* top, int* right, int* bottom)
{
  *left = m_drawing_area.left * m_resolution_scale;
  *right = std::max<uint32_t>((m_drawing_area.right + 1) * m_resolution_scale, *left + 1);
  *top = m_drawing_area.top * m_resolution_scale;
  *bottom = std::max<uint32_t>((m_drawing_area.bottom + 1) * m_resolution_scale, *top + 1);
}

GPU_HW::VRAMFillUBOData GPU_HW::GetVRAMFillUBOData(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) const
{
  // drop precision unless true colour is enabled
  if (!m_true_color)
    color = VRAMRGBA5551ToRGBA8888(VRAMRGBA8888ToRGBA5551(color));

  VRAMFillUBOData uniforms;
  uniforms.u_dst_x = (x % VRAM_WIDTH) * m_resolution_scale;
  uniforms.u_dst_y = (y % VRAM_HEIGHT) * m_resolution_scale;
  uniforms.u_end_x = ((x + width) % VRAM_WIDTH) * m_resolution_scale;
  uniforms.u_end_y = ((y + height) % VRAM_HEIGHT) * m_resolution_scale;
  std::tie(uniforms.u_fill_color[0], uniforms.u_fill_color[1], uniforms.u_fill_color[2], uniforms.u_fill_color[3]) =
    RGBA8ToFloat(color);

  uniforms.u_interlaced_displayed_field = GetActiveLineLSB();
  return uniforms;
}

Common::Rectangle<uint32_t> GPU_HW::GetVRAMTransferBounds(uint32_t x, uint32_t y, uint32_t width, uint32_t height) const
{
  Common::Rectangle<uint32_t> out_rc = Common::Rectangle<uint32_t>::FromExtents(x % VRAM_WIDTH, y % VRAM_HEIGHT, width, height);
  if (out_rc.right > VRAM_WIDTH)
  {
    out_rc.left = 0;
    out_rc.right = VRAM_WIDTH;
  }
  if (out_rc.bottom > VRAM_HEIGHT)
  {
    out_rc.top = 0;
    out_rc.bottom = VRAM_HEIGHT;
  }
  return out_rc;
}

GPU_HW::VRAMWriteUBOData GPU_HW::GetVRAMWriteUBOData(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t buffer_offset,
                                                     bool set_mask, bool check_mask) const
{
  const VRAMWriteUBOData uniforms = {
    (x % VRAM_WIDTH), (y % VRAM_HEIGHT), ((x + width) % VRAM_WIDTH),  ((y + height) % VRAM_HEIGHT),     width,
    height,           buffer_offset,     (set_mask) ? 0x8000u : 0x00, GetCurrentNormalizedVertexDepth(),
    // Cbuffer-routed RESOLUTION_SCALE - see GenerateVRAMWriteFragmentShader
    // for the alias plumbing. The body uses RESOLUTION_SCALE +
    // VRAM_SIZE which now derive from u_resolution_scale at runtime
    // instead of being baked at shadergen time.
    m_resolution_scale,
    0u /* u_pad0 */};
  return uniforms;
}

bool GPU_HW::UseVRAMCopyShader(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height) const
{
  // masking enabled, oversized, or overlapping
  return (m_GPUSTAT.IsMaskingEnabled() || ((src_x % VRAM_WIDTH) + width) > VRAM_WIDTH ||
          ((src_y % VRAM_HEIGHT) + height) > VRAM_HEIGHT || ((dst_x % VRAM_WIDTH) + width) > VRAM_WIDTH ||
          ((dst_y % VRAM_HEIGHT) + height) > VRAM_HEIGHT ||
          Common::Rectangle<uint32_t>::FromExtents(src_x, src_y, width, height)
            .Intersects(Common::Rectangle<uint32_t>::FromExtents(dst_x, dst_y, width, height)));
}

GPU_HW::VRAMCopyUBOData GPU_HW::GetVRAMCopyUBOData(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width,
                                                   uint32_t height) const
{
  const VRAMCopyUBOData uniforms = {(src_x % VRAM_WIDTH) * m_resolution_scale,
                                    (src_y % VRAM_HEIGHT) * m_resolution_scale,
                                    (dst_x % VRAM_WIDTH) * m_resolution_scale,
                                    (dst_y % VRAM_HEIGHT) * m_resolution_scale,
                                    ((dst_x + width) % VRAM_WIDTH) * m_resolution_scale,
                                    ((dst_y + height) % VRAM_HEIGHT) * m_resolution_scale,
                                    width * m_resolution_scale,
                                    height * m_resolution_scale,
                                    m_GPUSTAT.set_mask_while_drawing ? 1u : 0u,
                                    GetCurrentNormalizedVertexDepth(),
                                    // Cbuffer-routed RESOLUTION_SCALE - the shader's VRAM_SIZE / RCP_VRAM_SIZE
                                    // aliases now derive from this field instead of having m_resolution_scale
                                    // baked compile-time. See GenerateVRAMCopyFragmentShader.
                                    m_resolution_scale,
                                    0u /* u_pad0 */};

  return uniforms;
}

void GPU_HW::IncludeVRAMDirtyRectangle(const Common::Rectangle<uint32_t>& rect)
{
  m_vram_dirty_rect.Include(rect);

  // the vram area can include the texture page, but the game can leave it as-is. in this case, set it as dirty so the
  // shadow texture is updated
  if (!m_draw_mode.IsTexturePageChanged() &&
      (m_draw_mode.mode_reg.GetTexturePageRectangle().Intersects(rect) ||
       (m_draw_mode.mode_reg.IsUsingPalette() && m_draw_mode.GetTexturePaletteRectangle().Intersects(rect))))
  {
    m_draw_mode.SetTexturePageChanged();
  }
}

void GPU_HW::EnsureVertexBufferSpaceForCurrentCommand()
{
  uint32_t required_vertices;
  switch (m_render_command.primitive)
  {
    case GPUPrimitive::Polygon:
      required_vertices = m_render_command.quad_polygon ? 6 : 3;
      break;
    case GPUPrimitive::Rectangle:
      required_vertices = MAX_VERTICES_FOR_RECTANGLE;
      break;
    case GPUPrimitive::Line:
    default:
      required_vertices = m_render_command.polyline ? (GetPolyLineVertexCount() * 6u) : 6u;
      break;
  }

  // can we fit these vertices in the current depth buffer range?
  if ((m_current_depth + required_vertices) > MAX_BATCH_VERTEX_COUNTER_IDS)
  {
    // implies FlushRender()
    ResetBatchVertexDepth();
  }
  else if (m_batch_current_vertex_ptr)
  {
    if (GetBatchVertexSpace() >= required_vertices)
      return;

    FlushRender();
  }

  MapBatchVertexPointer(required_vertices);
}

void GPU_HW::ResetBatchVertexDepth()
{
  if (m_pgxp_depth_buffer)
    return;

  FlushRender();
  UpdateDepthBufferFromMaskBit();

  m_current_depth = 1;
}

void GPU_HW::UpdateSoftwareRenderer(bool copy_vram_from_hw)
{
  const bool current_enabled = (m_sw_renderer != nullptr);
  const bool new_enabled = g_settings.gpu_use_software_renderer_for_readbacks;
  if (current_enabled == new_enabled)
    return;

  m_vram_ptr = m_vram_shadow.data();

  if (!new_enabled)
  {
    if (m_sw_renderer)
      m_sw_renderer->Shutdown();
    m_sw_renderer.reset();
    return;
  }

  std::unique_ptr<GPU_SW_Backend> sw_renderer = std::make_unique<GPU_SW_Backend>();
  if (!sw_renderer->Initialize(true))
    return;

  // We need to fill in the SW renderer's VRAM with the current state for hot toggles.
  if (copy_vram_from_hw)
  {
    FlushRender();
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    std::memcpy(sw_renderer->GetVRAM(), m_vram_ptr, sizeof(uint16_t) * VRAM_WIDTH * VRAM_HEIGHT);

    // Sync the drawing area.
    GPUBackendSetDrawingAreaCommand* cmd = sw_renderer->NewSetDrawingAreaCommand();
    cmd->new_area = m_drawing_area;
    sw_renderer->PushCommand(cmd);
  }

  m_sw_renderer = std::move(sw_renderer);
  m_vram_ptr = m_sw_renderer->GetVRAM();
}

void GPU_HW::FillBackendCommandParameters(GPUBackendCommand* cmd) const
{
  cmd->params.bits = 0;
  cmd->params.check_mask_before_draw = m_GPUSTAT.check_mask_before_draw;
  cmd->params.set_mask_while_drawing = m_GPUSTAT.set_mask_while_drawing;
  cmd->params.active_line_lsb = m_crtc_state.active_line_lsb;
  cmd->params.interlaced_rendering = m_GPUSTAT.SkipDrawingToActiveField();
}

void GPU_HW::FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const
{
  FillBackendCommandParameters(cmd);
  cmd->rc.bits = rc.bits;
  cmd->draw_mode.bits = m_draw_mode.mode_reg.bits;
  cmd->palette.bits = m_draw_mode.palette_reg;
  cmd->window = m_draw_mode.texture_window;
}

void GPU_HW::ReadSoftwareRendererVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
  m_sw_renderer->Sync(false);
}

void GPU_HW::UpdateSoftwareRendererVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, bool set_mask,
                                        bool check_mask)
{
  const uint32_t num_words = width * height;
  GPUBackendUpdateVRAMCommand* cmd = m_sw_renderer->NewUpdateVRAMCommand(num_words);
  FillBackendCommandParameters(cmd);
  cmd->params.set_mask_while_drawing = set_mask;
  cmd->params.check_mask_before_draw = check_mask;
  cmd->x = static_cast<uint16_t>(x);
  cmd->y = static_cast<uint16_t>(y);
  cmd->width = static_cast<uint16_t>(width);
  cmd->height = static_cast<uint16_t>(height);
  std::memcpy(cmd->data, data, sizeof(uint16_t) * num_words);
  m_sw_renderer->PushCommand(cmd);
}

void GPU_HW::FillSoftwareRendererVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color)
{
  GPUBackendFillVRAMCommand* cmd = m_sw_renderer->NewFillVRAMCommand();
  FillBackendCommandParameters(cmd);
  cmd->x = static_cast<uint16_t>(x);
  cmd->y = static_cast<uint16_t>(y);
  cmd->width = static_cast<uint16_t>(width);
  cmd->height = static_cast<uint16_t>(height);
  cmd->color = color;
  m_sw_renderer->PushCommand(cmd);
}

void GPU_HW::CopySoftwareRendererVRAM(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height)
{
  GPUBackendCopyVRAMCommand* cmd = m_sw_renderer->NewCopyVRAMCommand();
  FillBackendCommandParameters(cmd);
  cmd->src_x = static_cast<uint16_t>(src_x);
  cmd->src_y = static_cast<uint16_t>(src_y);
  cmd->dst_x = static_cast<uint16_t>(dst_x);
  cmd->dst_y = static_cast<uint16_t>(dst_y);
  cmd->width = static_cast<uint16_t>(width);
  cmd->height = static_cast<uint16_t>(height);
  m_sw_renderer->PushCommand(cmd);
}

void GPU_HW::FillVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color)
{
  IncludeVRAMDirtyRectangle(
    Common::Rectangle<uint32_t>::FromExtents(x, y, width, height).Clamped(0, 0, VRAM_WIDTH, VRAM_HEIGHT));
}

void GPU_HW::UpdateVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, bool set_mask, bool check_mask)
{
  IncludeVRAMDirtyRectangle(Common::Rectangle<uint32_t>::FromExtents(x, y, width, height));

  if (check_mask)
  {
    // set new vertex counter since we want this to take into consideration previous masked pixels
    m_current_depth++;
  }
}

void GPU_HW::CopyVRAM(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height)
{
  IncludeVRAMDirtyRectangle(
    Common::Rectangle<uint32_t>::FromExtents(dst_x, dst_y, width, height).Clamped(0, 0, VRAM_WIDTH, VRAM_HEIGHT));

  if (m_GPUSTAT.check_mask_before_draw)
  {
    // set new vertex counter since we want this to take into consideration previous masked pixels
    m_current_depth++;
  }
}

void GPU_HW::DispatchRenderCommand()
{
  const GPURenderCommand rc{m_render_command.bits};

  GPUTextureMode texture_mode;
  if (rc.IsTexturingEnabled())
  {
    // texture page changed - check that the new page doesn't intersect the drawing area
    if (m_draw_mode.IsTexturePageChanged())
    {
      m_draw_mode.ClearTexturePageChangedFlag();
      if (m_vram_dirty_rect.Valid() && (m_draw_mode.mode_reg.GetTexturePageRectangle().Intersects(m_vram_dirty_rect) ||
                                        (m_draw_mode.mode_reg.IsUsingPalette() &&
                                         m_draw_mode.GetTexturePaletteRectangle().Intersects(m_vram_dirty_rect))))
      {
        if (m_batch_current_vertex_ptr != m_batch_start_vertex_ptr)
          FlushRender();

        UpdateVRAMReadTexture();
      }
    }

    texture_mode = m_draw_mode.mode_reg.texture_mode;
    if (rc.raw_texture_enable)
    {
      texture_mode =
        static_cast<GPUTextureMode>(static_cast<uint8_t>(texture_mode) | static_cast<uint8_t>(GPUTextureMode::RawTextureBit));
    }
  }
  else
  {
    texture_mode = GPUTextureMode::Disabled;
  }

  // has any state changed which requires a new batch?
  const GPUTransparencyMode transparency_mode =
    rc.transparency_enable ? m_draw_mode.mode_reg.transparency_mode : GPUTransparencyMode::Disabled;
  const bool dithering_enable = (!m_true_color && rc.IsDitheringEnabled()) ? m_GPUSTAT.dither_enable : false;
  if (texture_mode != m_batch.texture_mode || transparency_mode != m_batch.transparency_mode ||
      transparency_mode == GPUTransparencyMode::BackgroundMinusForeground || dithering_enable != m_batch.dithering)
  {
    FlushRender();
  }

  EnsureVertexBufferSpaceForCurrentCommand();

  // transparency mode change
  if (m_batch.transparency_mode != transparency_mode && transparency_mode != GPUTransparencyMode::Disabled)
  {
    static constexpr float transparent_alpha[4][2] = {{0.5f, 0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {0.25f, 1.0f}};

    const float src_alpha_factor = transparent_alpha[static_cast<uint32_t>(transparency_mode)][0];
    const float dst_alpha_factor = transparent_alpha[static_cast<uint32_t>(transparency_mode)][1];
    m_batch_ubo_dirty |= (m_batch_ubo_data.u_src_alpha_factor != src_alpha_factor ||
                          m_batch_ubo_data.u_dst_alpha_factor != dst_alpha_factor);
    m_batch_ubo_data.u_src_alpha_factor = src_alpha_factor;
    m_batch_ubo_data.u_dst_alpha_factor = dst_alpha_factor;
  }

  const bool check_mask_before_draw = m_GPUSTAT.check_mask_before_draw;
  const bool set_mask_while_drawing = m_GPUSTAT.set_mask_while_drawing;
  if (m_batch.check_mask_before_draw != check_mask_before_draw ||
      m_batch.set_mask_while_drawing != set_mask_while_drawing)
  {
    m_batch.check_mask_before_draw = check_mask_before_draw;
    m_batch.set_mask_while_drawing = set_mask_while_drawing;
    m_batch_ubo_dirty |= (m_batch_ubo_data.u_set_mask_while_drawing != static_cast<uint32_t>(set_mask_while_drawing));
    m_batch_ubo_data.u_set_mask_while_drawing = static_cast<uint32_t>(set_mask_while_drawing);
  }

  m_batch.interlacing = IsInterlacedRenderingEnabled();
  // u_interlacing gates the discard in the FS body; the value is
  // updated on every SetDrawMode so a display-mode flip mid-frame
  // is picked up by the next FlushRender. The displayed-field LSB
  // is only meaningful when interlacing is on (the FS discard short-
  // circuits on u_interlacing == 0), but pushing it unconditionally
  // when on keeps the existing per-frame field-flip handling -
  // the C++ alternation cost is one branch + one cbuffer compare,
  // negligible compared to the FlushRender that follows.
  const uint32_t new_interlacing = m_batch.interlacing ? 1u : 0u;
  m_batch_ubo_dirty |= (m_batch_ubo_data.u_interlacing != new_interlacing);
  m_batch_ubo_data.u_interlacing = new_interlacing;
  if (m_batch.interlacing)
  {
    const uint32_t displayed_field = GetActiveLineLSB();
    m_batch_ubo_dirty |= (m_batch_ubo_data.u_interlaced_displayed_field != displayed_field);
    m_batch_ubo_data.u_interlaced_displayed_field = displayed_field;
  }

  // update state
  m_batch.texture_mode = texture_mode;
  m_batch.transparency_mode = transparency_mode;
  m_batch.dithering = dithering_enable;
  // Push the new dithering bit to the batch UBO so the next
  // FlushRender's shader sees it. Was a compile-time #define
  // baked into the FS source (driving a dim of the batch FS / PSO
  // matrix); now a runtime branch on u_dithering. Toggling this
  // mid-frame - which happens on every PSX GP0(E1).dither_enable
  // write - is a single 4-byte cbuffer write, no shader recompile
  // and no PSO churn.
  const uint32_t new_dithering = dithering_enable ? 1u : 0u;
  m_batch_ubo_dirty |= (m_batch_ubo_data.u_dithering != new_dithering);
  m_batch_ubo_data.u_dithering = new_dithering;

  if (m_draw_mode.IsTextureWindowChanged())
  {
    m_draw_mode.ClearTextureWindowChangedFlag();

    m_batch_ubo_data.u_texture_window_and[0] = static_cast<uint32_t>(m_draw_mode.texture_window.and_x);
    m_batch_ubo_data.u_texture_window_and[1] = static_cast<uint32_t>(m_draw_mode.texture_window.and_y);
    m_batch_ubo_data.u_texture_window_or[0] = static_cast<uint32_t>(m_draw_mode.texture_window.or_x);
    m_batch_ubo_data.u_texture_window_or[1] = static_cast<uint32_t>(m_draw_mode.texture_window.or_y);
    m_batch_ubo_dirty = true;
  }

  if (m_drawing_area_changed)
  {
    m_drawing_area_changed = false;
    SetScissorFromDrawingArea();

    if (m_pgxp_depth_buffer && m_last_depth_z < 1.0f)
      ClearDepthBuffer();

    if (m_sw_renderer)
    {
      GPUBackendSetDrawingAreaCommand* cmd = m_sw_renderer->NewSetDrawingAreaCommand();
      cmd->new_area = m_drawing_area;
      m_sw_renderer->PushCommand(cmd);
    }
  }

  LoadVertices();
}

void GPU_HW::FlushRender()
{
  if (!m_batch_current_vertex_ptr)
    return;

  const uint32_t vertex_count = GetBatchVertexCount();
  UnmapBatchVertexPointer(vertex_count);

  if (vertex_count == 0)
    return;

  // Single-pass: stamp u_render_mode with the per-batch enum value
  // before the (possibly cached) UBO upload below.
  // Two-pass: leave m_batch_ubo_data untouched here - the per-draw
  // re-upload happens between the OnlyOpaque and OnlyTransparent
  // DrawBatchVertices calls.
  if (!NeedsTwoPassRendering())
  {
    const uint32_t new_render_mode = static_cast<uint32_t>(m_batch.GetRenderMode());
    m_batch_ubo_dirty |= (m_batch_ubo_data.u_render_mode != new_render_mode);
    m_batch_ubo_data.u_render_mode = new_render_mode;
  }

  if (m_batch_ubo_dirty)
  {
    UploadUniformBuffer(&m_batch_ubo_data, sizeof(m_batch_ubo_data));
    m_batch_ubo_dirty = false;
  }

  if (NeedsTwoPassRendering())
  {
    // Two-pass: re-upload the UBO between the OnlyOpaque and
    // OnlyTransparent draws so the FS sees the matching
    // u_render_mode value. Was a no-op pre-routing - both passes
    // shared one UBO upload because the per-pass discard logic was
    // baked into the FS bytecode via TRANSPARENCY_ONLY_OPAQUE /
    // TRANSPARENCY_ONLY_TRANSPARENT macros that produced 2 DIFFERENT
    // FS variants. Post-routing the FS bytecode is invariant across
    // the flip; the cost is +1 ~64-byte cbuffer write per
    // NeedsTwoPassRendering() FlushRender. Worth it: routing
    // collapses the FS variant matrix by 4x on the D3D12 pre-bake
    // side and matches the prior cbuffer-routing arc's shape.
    m_batch_ubo_data.u_render_mode = static_cast<uint32_t>(BatchRenderMode::OnlyOpaque);
    UploadUniformBuffer(&m_batch_ubo_data, sizeof(m_batch_ubo_data));
    DrawBatchVertices(BatchRenderMode::OnlyOpaque, m_batch_base_vertex, vertex_count);
    m_batch_ubo_data.u_render_mode = static_cast<uint32_t>(BatchRenderMode::OnlyTransparent);
    UploadUniformBuffer(&m_batch_ubo_data, sizeof(m_batch_ubo_data));
    DrawBatchVertices(BatchRenderMode::OnlyTransparent, m_batch_base_vertex, vertex_count);
  }
  else
    DrawBatchVertices(m_batch.GetRenderMode(), m_batch_base_vertex, vertex_count);
}

GPU_HW::ShaderCompileProgressTracker::ShaderCompileProgressTracker(std::string title, uint32_t total)
  : m_title(std::move(title)), m_min_time(Common::Timer::ConvertSecondsToValue(1.0)),
    m_update_interval(Common::Timer::ConvertSecondsToValue(0.1)), m_start_time(Common::Timer::GetValue()),
    m_last_update_time(0), m_progress(0), m_total(total)
{
}

void GPU_HW::ShaderCompileProgressTracker::Increment()
{
  m_progress++;

  const uint64_t tv = Common::Timer::GetValue();
  if ((tv - m_start_time) >= m_min_time && (tv - m_last_update_time) >= m_update_interval)
  {
    g_host_interface->DisplayLoadingScreen(m_title.c_str(), 0, static_cast<int>(m_total), static_cast<int>(m_progress));
    m_last_update_time = tv;
  }
}

#ifdef SWANSTATION_STANDALONE
void GPU_HW::SetDrawJitter(float x, float y)
{
  if (m_batch_ubo_data.u_jitter_x == x && m_batch_ubo_data.u_jitter_y == y)
    return;
  // What is batched so far was drawn with the jitter before.
  FlushRender();
  m_batch_ubo_data.u_jitter_x = x;
  m_batch_ubo_data.u_jitter_y = y;
  m_batch_ubo_dirty = true;
}
#endif
