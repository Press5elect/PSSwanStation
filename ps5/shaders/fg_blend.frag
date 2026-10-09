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
// One pixel: r is 1 where the two pictures are of different scenes (fg_cut.frag).
layout (set = 0, binding = 3) uniform sampler2D cut;
layout (push_constant) uniform pushBlock
{
	vec4 size;		// xy: one over the target's size
	vec4 blend;		// x: the phase, 0 the frame before, 1 this one, up to 2 ahead of it; y: 1 to show the movement;
					// z: 1 when `cut` says whether there was a cut
} pc;
layout (location = 0) out vec4 FragColor;

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	float phase = clamp(pc.blend.x, 0.0, 2.0);
	vec3 choice = texture(chosen, uv).rgb;
	vec2 v = choice.xy;
	bool early = phase < 0.5;
	// From the frame before, the pixel has come `phase` of its way; to this
	// frame it has the rest to go (ahead of it: it has gone past by that much).
	vec2 at = early ? uv - v * phase : uv + v * (1.0 - phase);
	float believed = choice.z;
	if (at.x < 0.0 || at.y < 0.0 || at.x > 1.0 || at.y > 1.0)
		believed = 0.0;
	// A cut: nothing is believed, the nearer picture is shown as it is.
	if (pc.blend.z > 0.5 && texelFetch(cut, ivec2(0, 0), 0).r > 0.5)
		believed = 0.0;
	vec3 moved = early ? texture(before, at).rgb : texture(now, at).rgb;
	vec3 nearest = early ? texture(before, uv).rgb : texture(now, uv).rgb;
	vec3 made = mix(nearest, moved, believed);
	if (pc.blend.y > 0.5)
	{
		// The debug view: the movement's way as a colour, its length as how
		// strong; grey where it is not believed.
		vec2 pixels = v / pc.size.xy;
		float angle = atan(pixels.y, pixels.x) / 6.2831853 + 0.5;
		vec3 hue = clamp(abs(fract(angle + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
		float strength = clamp(length(pixels) / 8.0, 0.0, 1.0);
		vec3 shown = mix(vec3(dot(made, vec3(0.299, 0.587, 0.114))), hue, strength * 0.8);
		made = mix(vec3(0.5), shown, max(believed, 0.25));
	}
	FragColor = vec4(made, 1.0);
}
