#version 450
#extension GL_GOOGLE_include_directive : require
// PSSwanStation - AMD's Contrast Adaptive Sharpening of the stretched picture.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D source;
layout (push_constant) uniform pushBlock
{
	uvec4 const0;
	uvec4 const1;
} pc;
layout (location = 0) out vec4 FragColor;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
AF3 CasLoad(ASU2 p) { return texelFetch(source, clamp(p, ASU2(0), textureSize(source, 0) - ASU2(1)), 0).rgb; }
void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_cas.h"

void main()
{
	AF3 c;
	CasFilter(c.r, c.g, c.b, AU2(gl_FragCoord.xy), pc.const0, pc.const1, true);
	FragColor = vec4(c, 1.0);
}
