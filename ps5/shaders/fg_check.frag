#version 450
// PSSwanStation - frame generation, the quality kind's two-way check. The
// movement was found as seen from half way between the two frames (and tidied,
// fg_tidy.frag); it is found again as seen from each frame alone. A place
// whose movement v is right is where the frame before at x - v/2 has v too
// (seen from that frame) and this frame at x + v/2 has v too (seen from this
// one). Where both say otherwise (and its patches are not alike anyway), the movement is a guess (something came into
// view, or went out of it), and it is marked so (its patches as unlike as can
// be): fg_choose.frag then believes it nowhere, and the nearer frame is shown
// there as it is. Where one says otherwise it is believed a little less.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D tidied;
layout (set = 0, binding = 1) uniform sampler2D fromBefore;
layout (set = 0, binding = 2) uniform sampler2D fromNow;
layout (push_constant) uniform pushBlock
{
	vec4 size;		// xy: one over the movement pictures' size
	vec4 more;
} pc;
layout (location = 0) out vec4 FragColor;

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	vec4 m = texture(tidied, uv);
	vec2 v = m.xy;
	vec2 back = texture(fromBefore, uv - v * 0.5 * pc.size.xy).xy;
	vec2 ahead = texture(fromNow, uv + v * 0.5 * pc.size.xy).xy;
	bool backAgrees = length(back - v) <= 1.5;
	bool aheadAgrees = length(ahead - v) <= 1.5;
	// A flat place matches under many movements, and the two sides may well
	// pick others there: only where the movement's own patches differ is a
	// disagreement taken for one.
	float patches = m.z;
	if (patches > 0.08)
	{
		if (!backAgrees && !aheadAgrees)
			patches = 1.0;
		else if (!backAgrees || !aheadAgrees)
			patches += 0.03;
	}
	FragColor = vec4(v, patches, 1.0);
}
