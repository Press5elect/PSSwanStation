#pragma once
#include "common/heap_array.h"
#include "gpu.h"
#include "host_display.h"
#include <cstring>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
class GPU_SW_Backend;
struct GPUBackendCommand;
struct GPUBackendDrawCommand;

class GPU_HW : public GPU
{
public:
#ifdef SWANSTATION_STANDALONE
  void SetDrawJitter(float x, float y) override;
  uint32_t GetDrawResolutionScale() const override { return m_resolution_scale; }
#endif

  enum class BatchRenderMode : uint8_t
  {
    TransparencyDisabled,
    TransparentAndOpaque,
    OnlyOpaque,
    OnlyTransparent
  };

  enum class InterlacedRenderMode : uint8_t
  {
    None,
    InterleavedFields,
    SeparateFields
  };

  GPU_HW();
  virtual ~GPU_HW();

  virtual bool Initialize(HostDisplay* host_display) override;
  virtual void Reset(bool clear_vram) override;
  virtual bool DoState(StateWrapper& sw, HostDisplayTexture** host_texture, bool update_display) override;

protected:
  static constexpr uint32_t VRAM_UPDATE_TEXTURE_BUFFER_SIZE = 4 * 1024 * 1024, VERTEX_BUFFER_SIZE = 4 * 1024 * 1024,
                       UNIFORM_BUFFER_SIZE = 2 * 1024 * 1024, MAX_BATCH_VERTEX_COUNTER_IDS = 65536 - 2,
                       MAX_VERTICES_FOR_RECTANGLE =
                         6 * (((MAX_PRIMITIVE_WIDTH + (TEXTURE_PAGE_WIDTH - 1)) / TEXTURE_PAGE_WIDTH) + 1u) *
                         (((MAX_PRIMITIVE_HEIGHT + (TEXTURE_PAGE_HEIGHT - 1)) / TEXTURE_PAGE_HEIGHT) + 1u);

  struct BatchVertex
  {
    float x;
    float y;
    float z;
    float w;
    uint32_t color;
    uint32_t texpage;
    uint16_t u; // 16-bit texcoords are needed for 256 extent rectangles
    uint16_t v;
    uint32_t uv_limits;

    ALWAYS_INLINE void Set(float x_, float y_, float z_, float w_, uint32_t color_, uint32_t texpage_, uint16_t packed_texcoord,
                           uint32_t uv_limits_)
    {
      x         = x_;
      y         = y_;
      z         = z_;
      w         = w_;
      color     = color_;
      texpage   = texpage_;
      u         = packed_texcoord & 0xFF;
      v         = (packed_texcoord >> 8);
      uv_limits = uv_limits_;
    }

    ALWAYS_INLINE void Set(float x_, float y_, float z_, float w_, uint32_t color_, uint32_t texpage_, uint16_t u_, uint16_t v_,
                           uint32_t uv_limits_)
    {
      x = x_;
      y = y_;
      z = z_;
      w = w_;
      color = color_;
      texpage = texpage_;
      u = u_;
      v = v_;
      uv_limits = uv_limits_;
    }

    ALWAYS_INLINE static uint32_t PackUVLimits(uint32_t min_u, uint32_t max_u, uint32_t min_v, uint32_t max_v)
    {
      return min_u | (min_v << 8) | (max_u << 16) | (max_v << 24);
    }
  };

  struct BatchConfig
  {
    GPUTextureMode texture_mode = GPUTextureMode::Disabled;
    GPUTransparencyMode transparency_mode = GPUTransparencyMode::Disabled;
    bool dithering = false;
    bool interlacing = false;
    bool set_mask_while_drawing = false;
    bool check_mask_before_draw = false;
    bool use_depth_buffer = false;

    // Returns the render mode for this batch.
    BatchRenderMode GetRenderMode() const
    {
      return transparency_mode == GPUTransparencyMode::Disabled ? BatchRenderMode::TransparencyDisabled :
                                                                  BatchRenderMode::TransparentAndOpaque;
    }
  };

  struct BatchUBOData
  {
    uint32_t u_texture_window_and[2];
    uint32_t u_texture_window_or[2];
    float u_src_alpha_factor;
    float u_dst_alpha_factor;
    uint32_t u_interlaced_displayed_field;
    uint32_t u_set_mask_while_drawing;
    // Per-session fields. These used to be baked into the HLSL/GLSL
    // source at shadergen time via DefineMacro / CONSTANT-literal
    // emission, which forced a fresh shader compile for every value
    // (every resolution scale, every true_color flip, every
    // scaled_dithering flip). Routing them through the batch
    // uniform buffer instead means a toggle is a single 4-byte
    // cbuffer write picked up on the next FlushRender - no DXBC
    // recompile, no PSO rebuild, no batch matrix flush. Mirrors
    // what Vulkan spec consts do for the same settings on the
    // pre-baked SPIR-V path.
    uint32_t u_resolution_scale;
    uint32_t u_true_color;
    uint32_t u_scaled_dithering;
    // u_dithering: per-batch ordered dithering toggle (the PSX
    // GP0(E1).dither_enable bit). Used to live in the shader source
    // as a compile-time DITHERING macro and as a dim in the batch
    // FS / PSO matrices; now a runtime branch on the cbuffer field.
    // Set in SetDrawMode alongside m_batch.dithering. Mirror of
    // u_scaled_dithering's per-session shape, but the per-batch
    // granularity matches the underlying register write rate.
    uint32_t u_dithering;
    // u_interlacing: per-batch interlaced-rendering gate (0 = off,
    // 1 = on). Used to live as a compile-time INTERLACING macro
    // gating a discard block; now a runtime short-circuit branch on
    // the cbuffer field. u_interlaced_displayed_field above carries
    // the active-field LSB and is updated independently in the same
    // SetDrawMode block - the two fields are split so each can be
    // updated on its own cadence (field LSB per frame, interlacing
    // on/off on display-mode changes).
    uint32_t u_interlacing;
    // u_pgxp_depth: per-session PGXP-depth-buffer mode toggle (0 = off,
    // 1 = on). Used to live as a compile-time PGXP_DEPTH macro in the
    // batch VS source, selecting between a_pos.z (legacy mask-Z) and
    // a_pos.w (PGXP-computed Z) as the depth source. Now a runtime
    // ternary on the cbuffer field. The FS-side PGXP_DEPTH usage
    // (gating SV_Depth declaration) is NOT yet routed - it still
    // affects the FS entry-point signature - so toggling
    // m_pgxp_depth_buffer mid-session still triggers a FS / PSO
    // matrix rebuild via shader_source_changed. VS, however, stays
    // bound across the flip.
    uint32_t u_pgxp_depth;
    // u_uv_limits: per-session UV-clamping gate (0 = off, 1 = on).
    // Used to live as a compile-time UV_LIMITS macro driving the
    // a_uv_limits vertex input + v_uv_limits varying + body
    // population. Now a runtime branch on the cbuffer field. The
    // VS source is invariant under the bit (always writes
    // v_uv_limits, always declares a_uv_limits); the FS source is
    // invariant too (the texture-filtering arm uses v_uv_limits
    // unconditionally thanks to the settings-layer coupling between
    // TEXTURE_FILTERING != Nearest and m_using_uv_limits=true via
    // ShouldUseUVLimits(); the non-filtered arm is the cbuffer
    // runtime branch). D3D11 / D3D12 input layouts always bind
    // ATTR4 when textured; OpenGL already did; Vulkan untouched
    // (its own pre-baked SPIR-V path needs a separate regen).
    uint32_t u_uv_limits;
    // u_render_mode: per-batch BatchRenderMode enum value (0/1/2/3).
    // 0=TransparencyDisabled, 1=TransparentAndOpaque, 2=OnlyOpaque,
    // 3=OnlyTransparent. The shader body branches on this scalar
    // instead of compile-time TRANSPARENCY / TRANSPARENCY_ONLY_OPAQUE
    // / TRANSPARENCY_ONLY_TRANSPARENT macros - the three booleans
    // are derived inside the FS body as (u_render_mode != 0),
    // (u_render_mode == 2), and (u_render_mode == 3) respectively.
    // The two-pass-rendering case in FlushRender re-uploads the UBO
    // with a different u_render_mode value between the OnlyOpaque
    // and OnlyTransparent DrawInstanced calls so each draw sees its
    // matching enum value. PSO blend state still varies by render
    // mode at the GraphicsPipelineBuilder level - the [render_mode]
    // matrix dim in m_batch_pipelines stays. Only the FS bytecode
    // is invariant across the flip post-routing, which is the win
    // for the D3D12 pre-bake: 288 -> 72 variants per textured
    // filter template (4x reduction).
    uint32_t u_render_mode;
#ifdef SWANSTATION_STANDALONE
    // u_jitter_x/y: a sub-pixel shift of everything drawn, in pixels of
    // the scaled framebuffer, set per picture (SetDrawJitter).
    float u_jitter_x;
    float u_jitter_y;
    float u_jitter_pad[2];
#endif
  };

  struct VRAMFillUBOData
  {
    uint32_t u_dst_x;
    uint32_t u_dst_y;
    uint32_t u_end_x;
    uint32_t u_end_y;
    float u_fill_color[4];
    uint32_t u_interlaced_displayed_field;
  };

  struct VRAMWriteUBOData
  {
    uint32_t u_dst_x;
    uint32_t u_dst_y;
    uint32_t u_end_x;
    uint32_t u_end_y;
    uint32_t u_width;
    uint32_t u_height;
    uint32_t u_buffer_base_offset;
    uint32_t u_mask_or_bits;
    float u_depth_value;
    // Appended for the cbuffer-routed RESOLUTION_SCALE refactor.
    // Matches the trailing "uint u_resolution_scale, uint u_pad0"
    // members on the HLSL side in GenerateVRAMWriteFragmentShader.
    // u_pad0 brings total to 44 bytes (the HLSL compiler rounds the
    // cbuffer up to the next 16-byte multiple internally, but we
    // upload exactly sizeof(struct) bytes on every backend and the
    // shader never reads beyond u_resolution_scale).
    uint32_t u_resolution_scale;
    uint32_t u_pad0;
  };

  struct VRAMCopyUBOData
  {
    uint32_t u_src_x;
    uint32_t u_src_y;
    uint32_t u_dst_x;
    uint32_t u_dst_y;
    uint32_t u_end_x;
    uint32_t u_end_y;
    uint32_t u_width;
    uint32_t u_height;
    uint32_t u_set_mask_bit;
    float u_depth_value;
    // Appended for the cbuffer-routed RESOLUTION_SCALE refactor (this
    // commit). Matches the trailing "uint u_resolution_scale, uint u_pad0"
    // members on the HLSL side in GenerateVRAMCopyFragmentShader -
    // u_pad0 keeps the cbuffer 16-byte-aligned (HLSL cbuffers pack to
    // 16-byte rows; after u_depth_value at offset 36, adding one uint
    // would end at offset 40, but the row ends at 48 so we pad to
    // bring the total to 48 bytes).
    uint32_t u_resolution_scale;
    uint32_t u_pad0;
  };

  class ShaderCompileProgressTracker
  {
  public:
    ShaderCompileProgressTracker(std::string title, uint32_t total);

    void Increment();

  private:
    std::string m_title;
    uint64_t m_min_time;
    uint64_t m_update_interval;
    uint64_t m_start_time;
    uint64_t m_last_update_time;
    uint32_t m_progress;
    uint32_t m_total;
  };

  static constexpr std::tuple<float, float, float, float> RGBA8ToFloat(uint32_t rgba)
  {
    return std::make_tuple(static_cast<float>(rgba & UINT32_C(0xFF)) * (1.0f / 255.0f),
                           static_cast<float>((rgba >> 8) & UINT32_C(0xFF)) * (1.0f / 255.0f),
                           static_cast<float>((rgba >> 16) & UINT32_C(0xFF)) * (1.0f / 255.0f),
                           static_cast<float>(rgba >> 24) * (1.0f / 255.0f));
  }

  // The optional out-parameter only_dim_changed is set to true iff
  // *shaders_changed is true AND the change is confined to the
  // settings the backend's batch pipeline cache is dimensioned over
  // (currently: texture filter, true colour, scaled dithering).
  // Backends with a dimensioned batch cache (Vulkan) can read this
  // to skip DestroyPipelines on such a toggle, preserving previously-
  // built sub-cubes for instant return when the user cycles back.
  // Backends without the dimension (D3D11 / D3D12 today) can ignore
  // this and continue doing a full destroy + recompile on any
  // shaders_changed event.
  //
  // The optional out-parameter downsample_changed is set to true iff
  // the downsample mode setting has changed from the cached session
  // value. Downsample mode does NOT participate in either
  // *framebuffer_changed (unless Adaptive is involved on either
  // side of the transition, since Adaptive uses a different
  // display-texture width and an additional weight texture) or
  // *shaders_changed (downsample mode never affects batch
  // pipelines / batch fragment shaders). Backends opt into a
  // targeted downsample-only rebuild path by reading this flag;
  // Disabled <-> Box transitions cost a couple of small VkPipeline
  // / texture / framebuffer creates rather than a full VRAM
  // round-trip.
  void UpdateHWSettings(bool* framebuffer_changed, bool* shaders_changed,
                        bool* only_dim_changed = nullptr,
                        bool* downsample_changed = nullptr,
                        bool* shader_source_changed = nullptr,
                        bool* display_only_source_changed = nullptr);

  virtual void UpdateVRAMReadTexture();
  virtual void UpdateDepthBufferFromMaskBit() = 0;
  virtual void ClearDepthBuffer() = 0;
  virtual void SetScissorFromDrawingArea() = 0;
  virtual void MapBatchVertexPointer(uint32_t required_vertices) = 0;
  virtual void UnmapBatchVertexPointer(uint32_t used_vertices) = 0;
  virtual void UploadUniformBuffer(const void* uniforms, uint32_t uniforms_size) = 0;
  virtual void DrawBatchVertices(BatchRenderMode render_mode, uint32_t base_vertex, uint32_t num_vertices) = 0;

  uint32_t CalculateResolutionScale() const;
  GPUDownsampleMode GetDownsampleMode(uint32_t resolution_scale) const;

  ALWAYS_INLINE bool IsUsingMultisampling() const { return m_multisamples > 1; }
  ALWAYS_INLINE bool IsUsingDownsampling() const
  {
    return (m_downsample_mode != GPUDownsampleMode::Disabled && !m_GPUSTAT.display_area_color_depth_24);
  }

  void SetFullVRAMDirtyRectangle()
  {
    m_vram_dirty_rect.Set(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    m_draw_mode.SetTexturePageChanged();
  }
  void ClearVRAMDirtyRectangle()
  {
    // Sets the rectangle to invalid coordinates (right < left, top < bottom).
    m_vram_dirty_rect.Set(m_vram_dirty_rect.InvalidMinCoord, m_vram_dirty_rect.InvalidMinCoord, m_vram_dirty_rect.InvalidMaxCoord, m_vram_dirty_rect.InvalidMaxCoord);
  }
  void IncludeVRAMDirtyRectangle(const Common::Rectangle<uint32_t>& rect);

  uint32_t GetBatchVertexSpace() const { return static_cast<uint32_t>(m_batch_end_vertex_ptr - m_batch_current_vertex_ptr); }
  uint32_t GetBatchVertexCount() const { return static_cast<uint32_t>(m_batch_current_vertex_ptr - m_batch_start_vertex_ptr); }
  void EnsureVertexBufferSpaceForCurrentCommand();
  void ResetBatchVertexDepth();

  /// Returns the value to be written to the depth buffer for the current operation for mask bit emulation.
  ALWAYS_INLINE float GetCurrentNormalizedVertexDepth() const
  {
    return 1.0f - (static_cast<float>(m_current_depth) / 65535.0f);
  }

  /// Returns the interlaced mode to use when scanning out/displaying.
  ALWAYS_INLINE InterlacedRenderMode GetInterlacedRenderMode() const
  {
    if (IsInterlacedDisplayEnabled())
      return m_GPUSTAT.vertical_resolution ? InterlacedRenderMode::InterleavedFields :
                                             InterlacedRenderMode::SeparateFields;
    return InterlacedRenderMode::None;
  }

  /// Returns true if the specified texture filtering mode requires dual-source blending.
  ALWAYS_INLINE bool TextureFilterRequiresDualSourceBlend(GPUTextureFilter filter)
  {
    return (filter == GPUTextureFilter::Bilinear || filter == GPUTextureFilter::JINC2 ||
            filter == GPUTextureFilter::xBR);
  }

  /// Returns true if alpha blending should be enabled for drawing the current batch.
  ALWAYS_INLINE bool UseAlphaBlending(GPUTransparencyMode transparency_mode, BatchRenderMode render_mode) const
  {
    if (m_texture_filtering == GPUTextureFilter::Bilinear || m_texture_filtering == GPUTextureFilter::JINC2 ||
        m_texture_filtering == GPUTextureFilter::xBR)
      return true;
    if (transparency_mode == GPUTransparencyMode::Disabled || render_mode == BatchRenderMode::OnlyOpaque)
      return false;
    return true;
  }

  /// We need two-pass rendering when using BG-FG blending and texturing, as the transparency can be enabled
  /// on a per-pixel basis, and the opaque pixels shouldn't be blended at all.
  ALWAYS_INLINE bool NeedsTwoPassRendering() const
  {
    return (m_batch.texture_mode != GPUTextureMode::Disabled &&
            (m_batch.transparency_mode == GPUTransparencyMode::BackgroundMinusForeground ||
             (!m_supports_dual_source_blend && m_batch.transparency_mode != GPUTransparencyMode::Disabled)));
  }

  /// Returns true if a given (render_mode, texture_mode) batch shader slot can
  /// actually be selected by the runtime at draw time. Used by the precompile
  /// loop and the background-compile worker to skip cells that the runtime
  /// would never bind, so we don't burn D3DCompile / glslang time on shaders
  /// that can't ever be used.
  ///
  /// Three classes of cell are structurally unreachable:
  ///
  ///   - texture_mode 3 (Reserved_Direct16Bit) and 7 (Reserved_RawDirect16Bit)
  ///     dedupe to canonical modes 2 and 6 inside GetBatchPixelShader /
  ///     GetBatchPipeline. The first runtime fault on the reserved slot just
  ///     aliases the canonical slot's ComPtr; no fresh compile is needed.
  ///
  ///   - OnlyOpaque / OnlyTransparent with texture_mode == Disabled.
  ///     NeedsTwoPassRendering short-circuits on (texture_mode != Disabled),
  ///     so untextured polys never go through the two-pass fallback path.
  ///
  ///   - TransparentAndOpaque with texture_mode != Disabled on hardware
  ///     WITHOUT dual-source blend support. NeedsTwoPassRendering returns
  ///     true for textured transparent draws on this hardware, so the
  ///     single-pass dual-source path is never selected. (Untextured
  ///     transparent draws on the same hardware still use
  ///     TransparentAndOpaque - NeedsTwoPassRendering returns false for
  ///     them - so this skip is texture_mode-dependent.)
  ALWAYS_INLINE static bool IsBatchShaderReachable(BatchRenderMode render_mode, uint8_t texture_mode,
                                                   bool supports_dual_source_blend)
  {
    if (texture_mode == static_cast<uint8_t>(GPUTextureMode::Reserved_Direct16Bit) ||
        texture_mode == static_cast<uint8_t>(GPUTextureMode::Reserved_RawDirect16Bit))
      return false;

    if ((render_mode == BatchRenderMode::OnlyOpaque || render_mode == BatchRenderMode::OnlyTransparent) &&
        texture_mode == static_cast<uint8_t>(GPUTextureMode::Disabled))
      return false;

    if (!supports_dual_source_blend && render_mode == BatchRenderMode::TransparentAndOpaque &&
        texture_mode != static_cast<uint8_t>(GPUTextureMode::Disabled))
      return false;

    return true;
  }

  /// Total reachable cell count in the batch shader matrix, for sizing the
  /// progress tracker upfront in Enabled mode.
  static uint32_t CountReachableBatchShaders(bool supports_dual_source_blend)
  {
    uint32_t count = 0;
    for (uint8_t rm = 0; rm < 4; rm++)
    {
      for (uint8_t tm = 0; tm < 9; tm++)
      {
        for (uint8_t d = 0; d < 2; d++)
        {
          for (uint8_t i = 0; i < 2; i++)
          {
            (void)d;
            (void)i;
            if (IsBatchShaderReachable(static_cast<BatchRenderMode>(rm), tm, supports_dual_source_blend))
              count++;
          }
        }
      }
    }
    return count;
  }

  /// Returns true if the specified VRAM fill is oversized.
  ALWAYS_INLINE static bool IsVRAMFillOversized(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
  {
    return ((x + width) > VRAM_WIDTH || (y + height) > VRAM_HEIGHT);
  }

  ALWAYS_INLINE bool IsUsingSoftwareRendererForReadbacks() { return static_cast<bool>(m_sw_renderer); }

  void FillBackendCommandParameters(GPUBackendCommand* cmd) const;
  void FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const;
  void UpdateSoftwareRenderer(bool copy_vram_from_hw);
  void ReadSoftwareRendererVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  void UpdateSoftwareRendererVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, bool set_mask,
                                  bool check_mask);
  void FillSoftwareRendererVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
  void CopySoftwareRendererVRAM(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height);

  void FillVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) override;
  void UpdateVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, bool set_mask, bool check_mask) override;
  void CopyVRAM(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height) override;
  void DispatchRenderCommand() override;
  void FlushRender() override;

  void CalcScissorRect(int* left, int* top, int* right, int* bottom);

  /// Computes the area affected by a VRAM transfer, including wrap-around of X.
  Common::Rectangle<uint32_t> GetVRAMTransferBounds(uint32_t x, uint32_t y, uint32_t width, uint32_t height) const;

  /// Returns true if the VRAM copy shader should be used (oversized copies, masking).
  bool UseVRAMCopyShader(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height) const;

  VRAMFillUBOData GetVRAMFillUBOData(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) const;
  VRAMWriteUBOData GetVRAMWriteUBOData(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t buffer_offset, bool set_mask,
                                       bool check_mask) const;
  VRAMCopyUBOData GetVRAMCopyUBOData(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height) const;

  /// Expands a line into two triangles.
  void DrawLine(float x0, float y0, uint32_t col0, float x1, float y1, uint32_t col1, float depth);

  /// Handles quads with flipped texture coordinate directions.
  static void HandleFlippedQuadTextureCoordinates(BatchVertex* vertices);

  /// Computes polygon U/V boundaries.
  static void ComputePolygonUVLimits(BatchVertex* vertices, uint32_t num_vertices);
  // Writes one triangle of a polygon to the batch, its texture coordinates in
  // sixteenths of a texel; with expand, a quarter of a native pixel larger.
  void AddPolygonTriangle(const BatchVertex& a, const BatchVertex& b, const BatchVertex& c, bool expand);

  /// Sets the depth test flag for PGXP depth buffering.
  void SetBatchDepthBuffer(bool enabled);
  void CheckForDepthClear(const BatchVertex* vertices, uint32_t num_vertices);

  /// UBO data for adaptive smoothing.
  struct SmoothingUBOData
  {
    float min_uv[2];
    float max_uv[2];
    float rcp_size[2];
  };

  /// Returns the number of mipmap levels used for adaptive smoothing.
  uint32_t GetAdaptiveDownsamplingMipLevels() const;

  /// Returns the UBO data for an adaptive smoothing pass.
  SmoothingUBOData GetSmoothingUBO(uint32_t level, uint32_t left, uint32_t top, uint32_t width, uint32_t height, uint32_t tex_width,
                                   uint32_t tex_height) const;

  HeapArray<uint16_t, VRAM_WIDTH * VRAM_HEIGHT> m_vram_shadow;
  std::unique_ptr<GPU_SW_Backend> m_sw_renderer;

  BatchVertex* m_batch_start_vertex_ptr = nullptr;
  BatchVertex* m_batch_end_vertex_ptr = nullptr;
  BatchVertex* m_batch_current_vertex_ptr = nullptr;
  uint32_t m_batch_base_vertex = 0;
  int32_t m_current_depth = 0;
  float m_last_depth_z = 1.0f;

  uint32_t m_resolution_scale = 1;
  uint32_t m_multisamples = 1;
  uint32_t m_max_resolution_scale = 1;
  uint32_t m_max_multisamples = 1;
  HostDisplay::RenderAPI m_render_api = HostDisplay::RenderAPI::None;
  bool m_true_color = true;

  union
  {
    BitField<uint8_t, bool, 0, 1> m_supports_per_sample_shading;
    BitField<uint8_t, bool, 1, 1> m_supports_dual_source_blend;
    BitField<uint8_t, bool, 2, 1> m_supports_adaptive_downsampling;
    BitField<uint8_t, bool, 3, 1> m_supports_disable_color_perspective;
    BitField<uint8_t, bool, 4, 1> m_per_sample_shading;
    BitField<uint8_t, bool, 5, 1> m_scaled_dithering;
    BitField<uint8_t, bool, 6, 1> m_chroma_smoothing;
    BitField<uint8_t, bool, 7, 1> m_disable_color_perspective;

    uint8_t bits = 0;
  };

  GPUTextureFilter m_texture_filtering = GPUTextureFilter::Nearest;
  GPUDownsampleMode m_downsample_mode = GPUDownsampleMode::Disabled;
  bool m_using_uv_limits = false;
  bool m_close_gaps = false;
  bool m_pgxp_depth_buffer = false;

  // Previous-value cache of g_settings.gpu_shader_precompile_mode
  // so UpdateHWSettings can detect a runtime flip and route it
  // through the same destroy/recompile path the other shader-
  // affecting settings use. Without this, switching the mode at
  // runtime would silently fail to apply: the existing Lazy
  // worker would keep running (or not be spawned) regardless of
  // the new value, because shaders_changed wouldn't fire on a
  // pure precompile-mode change.
  GPUShaderPrecompileMode m_shader_precompile_mode = GPUShaderPrecompileMode::Lazy;

  BatchConfig m_batch;
  BatchUBOData m_batch_ubo_data = {};

  // Bounding box of VRAM area that the GPU has drawn into.
  Common::Rectangle<uint32_t> m_vram_dirty_rect;

  // Changed state
  bool m_batch_ubo_dirty = true;

private:
  static constexpr uint32_t MIN_BATCH_VERTEX_COUNT = 6, MAX_BATCH_VERTEX_COUNT = VERTEX_BUFFER_SIZE / sizeof(BatchVertex);

  void LoadVertices();

  ALWAYS_INLINE void AddVertex(const BatchVertex& v)
  {
    std::memcpy(m_batch_current_vertex_ptr, &v, sizeof(BatchVertex));
    m_batch_current_vertex_ptr++;
  }

  template<typename... Args>
  ALWAYS_INLINE void AddNewVertex(Args&&... args)
  {
    m_batch_current_vertex_ptr->Set(std::forward<Args>(args)...);
    m_batch_current_vertex_ptr++;
  }
};
