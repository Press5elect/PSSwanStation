/*
	PSSwanStation - the ES-DE theme reader on the PC: reads each theme folder
	given and prints what the gamelist view (and the system view) holds.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	    dump THEME_FOLDER [variant] [colour scheme]
*/
#include "esde.h"

#include <cstdio>

using namespace fe::esde;

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s THEME_FOLDER [variant] [colour scheme]\n", argv[0]);
		return 2;
	}
	Theme theme;
	if (!readTheme(argv[1], theme))
	{
		std::printf("not a theme: %s\n", theme.problem.c_str());
		return 1;
	}
	std::printf("%s: %zu variants, %zu colour schemes, %zu font sizes, %zu aspect ratios, %zu languages\n",
			theme.name.c_str(), theme.variants.size(), theme.colorSchemes.size(), theme.fontSizes.size(),
			theme.aspectRatios.size(), theme.languages.size());
	Selection sel;
	if (argc > 2)
		sel.variant = argv[2];
	if (argc > 3)
		sel.colorScheme = argv[3];
	sel.aspectRatio = nearestAspectRatio(theme, 16.f / 9.f);
	Loaded loaded;
	const bool ok = load(theme, sel, System(), [](const std::string& type) { return type == "cover"; }, loaded);
	std::printf("load: %s, %d files, variant %s, scheme %s, font %s, ratio %s, language %s\n", ok ? "ok" : "failed",
			loaded.files, loaded.used.variant.c_str(), loaded.used.colorScheme.c_str(), loaded.used.fontSize.c_str(),
			loaded.used.aspectRatio.c_str(), loaded.used.language.c_str());
	for (const std::string& w : loaded.warnings)
		std::printf("  warning: %s\n", w.c_str());
	for (const auto *view : { &loaded.gamelist, &loaded.system })
	{
		std::printf("== %s view: %zu elements\n", view == &loaded.gamelist ? "gamelist" : "system", view->elements.size());
		for (const Element& e : view->elements)
		{
			std::printf("  %s \"%s\"\n", e.type.c_str(), e.name.c_str());
			for (const auto& p : e.props)
				std::printf("      %s = %s\n", p.first.c_str(), p.second.c_str());
		}
	}
	return ok ? 0 : 1;
}
