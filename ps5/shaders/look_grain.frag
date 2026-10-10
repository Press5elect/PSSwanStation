#version 450
// PSSwanStation - film grain over the picture, at the size it has on the
// screen: each grain a small square of a few screen pixels, a new pattern
// every frame, strongest in the middle tones (where film's is), fainter in
// the blacks and whites.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D source;
layout (push_constant) uniform pushBlock
{
	vec4 picture;	// xy: the picture's size in pixels (the target's too)
	vec4 grain;		// x: strength; y: a grain's side in pixels; z: the frame's seed; w: 1 coloured grain
} pc;
layout (location = 0) out vec4 FragColor;

// A hash of a grain's place and the frame, to 0..1.
float hash(vec3 p)
{
	p = fract(p * vec3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yxz + 33.33);
	return fract((p.x + p.y) * p.z);
}

void main()
{
	const vec2 p = gl_FragCoord.xy;
	const vec3 c = textureLod(source, p / pc.picture.xy, 0.0).rgb;
	const vec2 cell = floor(p / max(pc.grain.y, 1.0));
	// Two draws added: a triangular spread, which looks like grain rather
	// than static.
	const float seed = pc.grain.z;
	float n = hash(vec3(cell, seed)) + hash(vec3(cell + 71.0, seed + 0.37)) - 1.0;
	vec3 noise = vec3(n);
	if (pc.grain.w > 0.5)
		noise = vec3(n, hash(vec3(cell + 13.0, seed + 0.11)) + hash(vec3(cell + 97.0, seed + 0.59)) - 1.0,
				hash(vec3(cell + 29.0, seed + 0.23)) + hash(vec3(cell + 53.0, seed + 0.83)) - 1.0);
	const float luma = dot(c, vec3(0.299, 0.587, 0.114));
	const float midtone = 0.35 + 0.65 * (1.0 - abs(2.0 * luma - 1.0));
	FragColor = vec4(clamp(c + noise * pc.grain.x * midtone, 0.0, 1.0), 1.0);
}
