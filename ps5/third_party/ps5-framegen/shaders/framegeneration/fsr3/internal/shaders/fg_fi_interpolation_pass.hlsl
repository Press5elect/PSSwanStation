// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// RPCS3-PS5 (m73): FSR3's interpolation pass (ffx_frameinterpolation_pass.hlsl), optical flow only (FFX_FG_OF_ONLY): no game
// motion vector field, disocclusion mask or inpainting pyramid. m73b: a 2x2 quad of pixels a thread (computeFrameinterpolationQuad,
// which also reads the optical flow and keeps each pixel's still history), so dispatched for (width + 1) / 2 x (height + 1) / 2.

#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW                            1
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_X      2
#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y      3
#define FFX_FRAMEINTERPOLATION_BIND_SRV_PREVIOUS_INTERPOLATION_SOURCE           4
#define FFX_FRAMEINTERPOLATION_BIND_SRV_CURRENT_INTERPOLATION_SOURCE            5
#define FFX_FRAMEINTERPOLATION_BIND_SRV_COUNTERS                                8

#define FFX_FRAMEINTERPOLATION_BIND_UAV_OUTPUT                                  0
#define FFX_FRAMEINTERPOLATION_BIND_UAV_STATIC_HISTORY                          1
#if !defined(FG_NO_TEMPORAL)
#define FFX_FRAMEINTERPOLATION_BIND_UAV_QUAD_MOTION                             2 // m74
#endif

#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION                       0

#include "frameinterpolation/ffx_frameinterpolation_callbacks_hlsl.h"
#include "frameinterpolation/ffx_frameinterpolation_common.h"
#include "frameinterpolation/ffx_frameinterpolation.h"

[numthreads(8, 8, 1)]
void CS(FfxInt32x2 iQuad : SV_DispatchThreadID)
{
    computeFrameinterpolationQuad(iQuad);
}
