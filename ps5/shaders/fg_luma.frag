#version 450
// PSSwanStation - frame generation: a smaller picture of how bright each part
// is, which is what movement is looked for in. Nine taps over the part of the
// source a pixel here stands for.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D source;
layout (push_constant) uniform pushBlock
{
	vec4 size;		// xy: one over the target's size
	vec4 spread;	// xy: a third of a target pixel, in the source's coordinates; z: 1 when the source is colour
} pc;
layout (location = 0) out vec4 FragColor;

float bright(vec2 uv)
{
	vec4 c = texture(source, uv);
	return pc.spread.z > 0.5 ? dot(c.rgb, vec3(0.299, 0.587, 0.114)) : c.r;
}

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	float sum = 0.0;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
			sum += bright(uv + vec2(x, y) * pc.spread.xy);
	FragColor = vec4(sum / 9.0, 0.0, 0.0, 1.0);
}
