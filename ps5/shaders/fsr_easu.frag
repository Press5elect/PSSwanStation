#version 450
#extension GL_GOOGLE_include_directive : require
// PSSwanStation - FSR 1, the edge-adaptive upscale (EASU): the game's picture
// into an image the size it has on the screen.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D tex;
layout (push_constant) uniform pushBlock
{
	uvec4 con0;
	uvec4 con1;
	uvec4 con2;
	uvec4 con3;
	// The last place a gather may be centred: the picture can be a part of its
	// texture, and what is beyond it is not picture.
	vec4 limit;
} pc;
layout (location = 0) out vec4 FragColor;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(tex, min(p, pc.limit.xy), 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(tex, min(p, pc.limit.xy), 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(tex, min(p, pc.limit.xy), 2); }
#include "ffx_fsr1.h"

void main()
{
	AF3 c;
	FsrEasuF(c, AU2(gl_FragCoord.xy), pc.con0, pc.con1, pc.con2, pc.con3);
	FragColor = vec4(c, 1.0);
}
