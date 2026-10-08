#version 450
// PSSwanStation - the picture stretched to the size it has on the screen:
// bilinear, or sharp bilinear (each texel grown by the whole number of times
// it fits, then only the steps between texels blended).
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D source;
layout (push_constant) uniform pushBlock
{
	vec4 picture;	// xy: the picture's size in texels; zw: how much of the texture is picture
	vec4 target;	// xy: the size it is made; z: 1 sharp bilinear
} pc;
layout (location = 0) out vec4 FragColor;

void main()
{
	vec2 texel = gl_FragCoord.xy / pc.target.xy * pc.picture.xy;
	if (pc.target.z > 0.5)
	{
		const vec2 scale = max(floor(pc.target.xy / pc.picture.xy), vec2(1.0));
		const vec2 centre = fract(texel) - 0.5;
		const vec2 region = 0.5 - 0.5 / scale;
		texel = floor(texel) + (centre - clamp(centre, -region, region)) * scale + 0.5;
	}
	FragColor = vec4(textureLod(source, texel / pc.picture.xy * pc.picture.zw, 0.0).rgb, 1.0);
}
