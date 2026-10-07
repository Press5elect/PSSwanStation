#version 450
#extension GL_GOOGLE_include_directive : require
// PSSwanStation - FSR 1, the sharpening (RCAS) of the upscaled picture.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D tex;
layout (push_constant) uniform pushBlock
{
	uvec4 con;
} pc;
layout (location = 0) out vec4 FragColor;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(tex, clamp(p, ASU2(0), textureSize(tex, 0) - ASU2(1)), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

void main()
{
	AF3 c;
	FsrRcasF(c.r, c.g, c.b, AU2(gl_FragCoord.xy), pc.con);
	FragColor = vec4(c, 1.0);
}
