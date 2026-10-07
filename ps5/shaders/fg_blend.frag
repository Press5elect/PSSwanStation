#version 450
// PSSwanStation - frame generation: the picture between the frame before and
// this one, a part `phase` of the way from the one to the other.
//
// Each pixel is taken from where it was in the frame before and where it is
// in this one, along the movement found (fg_search.frag), and the two are
// mixed. Where they do not look alike the movement was not found (something
// came into view, or the pictures have nothing to do with each other): there
// the nearer of the two real pictures is shown, which is a moment's stutter
// in that place and no ghost. What did not change between the two is this
// frame's, untouched: writing over a moving background stays sharp.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D before;
layout (set = 0, binding = 1) uniform sampler2D now;
layout (set = 0, binding = 2) uniform sampler2D movement;
layout (push_constant) uniform pushBlock
{
	vec4 size;		// xy: one over the target's size; zw: one over the movement picture's size
	vec4 blend;		// x: the phase, 0 the frame before, 1 this one
} pc;
layout (location = 0) out vec4 FragColor;

float most(vec3 v)
{
	return max(v.r, max(v.g, v.b));
}

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	float phase = clamp(pc.blend.x, 0.0, 1.0);
	// The movement, a little smoothed: found block by block, it has steps
	// that an edge would show.
	vec2 around = pc.size.zw * 0.75;
	vec2 v = (texture(movement, uv).rg * 2.0
			+ texture(movement, uv + vec2(around.x, 0.0)).rg + texture(movement, uv - vec2(around.x, 0.0)).rg
			+ texture(movement, uv + vec2(0.0, around.y)).rg + texture(movement, uv - vec2(0.0, around.y)).rg) / 6.0;
	v *= pc.size.zw;
	vec3 here = texture(now, uv).rgb, was = texture(before, uv).rgb;
	vec2 from = uv - v * phase, to = uv + v * (1.0 - phase);
	vec3 a = texture(before, from).rgb, b = texture(now, to).rgb;
	// The two are alike when they differ little, and much less than the same
	// place of the two frames does: a movement that brings them no nearer than
	// they were is no movement found, however faint the picture.
	float apart = most(abs(a - b)), unmoved = most(abs(here - was));
	float alike = (1.0 - smoothstep(0.10, 0.28, apart)) * (1.0 - smoothstep(0.35, 0.80, apart / (unmoved + 0.004)));
	// Taken from beyond the picture's edge, it is no picture.
	vec2 low = min(from, to), high = max(from, to);
	if (low.x < 0.0 || low.y < 0.0 || high.x > 1.0 || high.y > 1.0)
		alike = 0.0;
	vec3 colour = mix(phase < 0.5 ? was : here, mix(a, b, phase), alike);
	float still = 1.0 - smoothstep(0.012, 0.045, unmoved);
	FragColor = vec4(mix(colour, here, still), 1.0);
}
