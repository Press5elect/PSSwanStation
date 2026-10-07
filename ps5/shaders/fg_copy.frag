#version 450
// PSSwanStation - frame generation: the game's picture, kept (the part of its
// texture that is picture, over the whole of the target).
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D source;
layout (push_constant) uniform pushBlock
{
	vec4 size;		// xy: one over the target's size
	vec4 part;		// xy: how much of the texture is picture
} pc;
layout (location = 0) out vec4 FragColor;

void main()
{
	FragColor = vec4(texture(source, gl_FragCoord.xy * pc.size.xy * pc.part.xy).rgb, 1.0);
}
