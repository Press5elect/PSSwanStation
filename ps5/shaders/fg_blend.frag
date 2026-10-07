#version 450
// PSSwanStation - frame generation: the picture between the frame before and
// this one, a part `phase` of the way from the one to the other.
//
// Each pixel has a movement and how far it is believed (fg_choose.frag). The
// pixel is taken from the nearer of the two frames only, at where it was
// there: mixing the two frames would show any small error in the movement as
// a doubled, blurred picture, and this shows it as nothing. Where the
// movement is not believed (something came into view, or the two pictures
// have nothing to do with each other) the nearer frame is shown as it is,
// which is a moment's stutter in that place and no ghost.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D before;
layout (set = 0, binding = 1) uniform sampler2D now;
layout (set = 0, binding = 2) uniform sampler2D chosen;
layout (push_constant) uniform pushBlock
{
	vec4 size;		// xy: one over the target's size
	vec4 blend;		// x: the phase, 0 the frame before, 1 this one
} pc;
layout (location = 0) out vec4 FragColor;

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	float phase = clamp(pc.blend.x, 0.0, 1.0);
	vec3 choice = texture(chosen, uv).rgb;
	vec2 v = choice.xy;
	bool early = phase < 0.5;
	// From the frame before, the pixel has come `phase` of its way; to this
	// frame it has the rest to go.
	vec2 at = early ? uv - v * phase : uv + v * (1.0 - phase);
	float believed = choice.z;
	if (at.x < 0.0 || at.y < 0.0 || at.x > 1.0 || at.y > 1.0)
		believed = 0.0;
	vec3 moved = early ? texture(before, at).rgb : texture(now, at).rgb;
	vec3 nearest = early ? texture(before, uv).rgb : texture(now, uv).rgb;
	FragColor = vec4(mix(nearest, moved, believed), 1.0);
}
