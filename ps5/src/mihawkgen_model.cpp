/*
	PSSwanStation - MihawkGen's trained network, assembled into the title.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	third_party/mihawkgen/model/network.fgw is Mihawk's network (432,308
	parameters, GPL-3.0-or-later, https://github.com/mihawk-99/PS5_MihawkGen);
	it is read by MihawkGen's own loader, from this blob.
*/
#include <cstddef>

#ifndef MIHAWKGEN_MODEL_PATH
#error "MIHAWKGEN_MODEL_PATH is the network's file (CMakeLists.txt)"
#endif

__asm__(".section .rodata\n"
		".balign 16\n"
		".global fe_mihawkgen_model\n"
		"fe_mihawkgen_model:\n"
		".incbin \"" MIHAWKGEN_MODEL_PATH "\"\n"
		".global fe_mihawkgen_model_end\n"
		"fe_mihawkgen_model_end:\n"
		".byte 0\n"
		".previous\n");

extern "C" const unsigned char fe_mihawkgen_model[];
extern "C" const unsigned char fe_mihawkgen_model_end[];

namespace fe::mihawkgen
{
const void *model()
{
	return fe_mihawkgen_model;
}

size_t modelSize()
{
	return (size_t)(fe_mihawkgen_model_end - fe_mihawkgen_model);
}
}
