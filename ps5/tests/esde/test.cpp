/*
	PSSwanStation - the ES-DE theme reader's test (esde.cpp), on the PC.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	Reads tests/esde/theme, a small theme written for the test, with each of
	its choices, and checks what the views hold: includes (a per-system one,
	one the theme lacks), variables put in late, the colour scheme, font
	size, language and aspect ratio chosen, a variant's override, elements
	written in several places, paths made absolute from the file they are in.

	    run.sh
*/
#include "esde.h"

#include <cstdio>
#include <string>

using namespace fe::esde;

namespace
{
int failures = 0;

void check(bool ok, const char *what)
{
	std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

std::string prop(const View& view, const char *type, const char *name, const char *key)
{
	const Element *e = view.find(type, name);
	return e == nullptr ? "(none)" : e->get(key);
}

bool endsWith(const std::string& s, const std::string& end)
{
	return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}
}

int main(int argc, char **argv)
{
	const std::string folder = argc > 1 ? argv[1] : "theme";
	Theme theme;
	std::printf("capabilities\n");
	check(readTheme(folder, theme), "the theme is read");
	check(theme.name == "Parser Test", "its name");
	check(theme.variants.size() == 3 && theme.variants[0].label == "List & Mix", "three variants, labels decoded");
	check(theme.variants[0].overrides.size() == 1 && theme.variants[0].overrides[0].mediaTypes.size() == 2,
			"a variant's override with two kinds of picture");
	check(!theme.variants[2].selectable, "a variant that is not to be chosen");
	check(theme.colorSchemes.size() == 2 && theme.colorSchemes[1].label == "Light", "the English label is taken");
	check(theme.aspectRatios.size() == 3 && theme.aspectRatios[2].label == "5:3 (vertical)", "aspect ratios, labelled");
	check(nearestAspectRatio(theme, 16.f / 9.f) == "16:9" && nearestAspectRatio(theme, 1.3f) == "4:3",
			"the display's aspect ratio, nearest");

	const auto read = [&](const Selection& sel, bool mixes, Loaded& out) {
		return load(theme, sel, System(), [mixes](const std::string& kind) { return mixes && kind == "miximage"; }, out);
	};

	std::printf("the first of everything\n");
	Loaded a;
	check(read(Selection(), true, a), "it loads");
	check(a.used.variant == "list" && a.used.colorScheme == "dark" && a.used.fontSize == "medium"
			&& a.used.aspectRatio == "16:9" && a.used.language == "en_US", "the defaults chosen");
	check(a.files == 3, "theme.xml, colours, psx and nothing more read (the missing per-system file quietly)");
	check(a.warnings.empty(), "no warnings");
	check(prop(a.gamelist, "image", "background", "color") == "101010", "the colour scheme's variable");
	check(prop(a.system, "image", "background", "color") == "101010", "a view written for two views is in both");
	check(prop(a.gamelist, "text", "title", "color") == "ffffff", "a variable defined before its value is known");
	check(prop(a.gamelist, "text", "title", "text") == "Sony PlayStation games", "the system's variables");
	check(prop(a.gamelist, "text", "title", "fontSize") == "0.045", "the font size's variable");
	check(prop(a.gamelist, "text", "subtitle", "text") == "left", "an unknown variable is left out");
	check(prop(a.gamelist, "text", "subtitle", "pos") == "0.5 0.1" && prop(a.gamelist, "text", "subtitle", "color") == "ffffff",
			"an element written twice keeps both writings");
	check(endsWith(prop(a.gamelist, "image", "logo", "path"), "/tests/esde/theme/inc/logo.svg")
			|| endsWith(prop(a.gamelist, "image", "logo", "path"), "theme/inc/logo.svg"),
			"a path is from the file it is written in");
	check(endsWith(prop(a.gamelist, "text", "title", "fontPath"), "theme/art/font.ttf"), "a path in a variable");
	check(endsWith(prop(a.gamelist, "badges", "badges", "customBadgeIcon:favorite"), "theme/art/heart.svg"),
			"a property with an attribute");
	check(prop(a.gamelist, "image", "logo", "color") == "3355aa", "the per-system file's variable and element");
	check(prop(a.gamelist, "textlist", "games", "pos") == "0.1 0.2", "the variant's element");
	check(a.gamelist.find("grid", "games") == nullptr, "not another variant's");
	check(prop(a.gamelist, "image", "background", "size") == "1 1", "not another aspect ratio's");

	std::printf("an override\n");
	Loaded b;
	check(read(Selection(), false, b), "it loads");
	check(b.used.variant == "plain" && prop(b.gamelist, "textlist", "games", "pos") == "0.3 0.2",
			"no mixes or screenshots: the override's variant");

	std::printf("other choices\n");
	Selection sel;
	sel.variant = "grid";
	sel.colorScheme = "light";
	sel.fontSize = "small";
	sel.aspectRatio = "4:3";
	sel.language = "de_DE";
	Loaded c;
	check(read(sel, true, c), "it loads");
	check(c.files == 4, "the aspect ratio's include read");
	check(prop(c.gamelist, "image", "background", "color") == "f0f0f0", "the light colours");
	check(prop(c.gamelist, "text", "title", "color") == "000000", "and the variable that names them");
	check(prop(c.gamelist, "text", "title", "fontSize") == "0.03", "the small font");
	check(prop(c.gamelist, "text", "title", "text") == "Spiele", "the language's variable");
	check(prop(c.gamelist, "image", "background", "size") == "1 0.75", "the aspect ratio's element");
	check(prop(c.gamelist, "grid", "games", "pos") == "0.0 0.2" && c.gamelist.find("textlist", "games") == nullptr,
			"the grid variant");

	std::printf("elements\n");
	uint32_t colour = 0;
	check(parseColour("ff8000", colour) && colour == 0xff0080ffu, "RRGGBB");
	check(parseColour("ff800040", colour) && colour == 0x400080ffu, "RRGGBBAA");
	check(!parseColour("ff80", colour) && !parseColour("zzzzzz", colour), "not a colour");
	Element e;
	e.props["pos"] = "0.25 0.75";
	e.props["size"] = "0.5";
	e.props["visible"] = "false";
	float x = 0, y = 0;
	check(e.pair("pos", x, y) && x == 0.25f && y == 0.75f, "a pair");
	check(e.pair("size", x, y) && x == 0.5f && y == 0.5f, "one number for both");
	check(!e.flag("visible", true) && e.flag("other", true), "a flag, and its default");

	std::printf("not a theme\n");
	Theme none;
	check(!readTheme(folder + "/inc", none) && !none.problem.empty(), "a folder without capabilities.xml");
	const std::vector<Theme> found = scan({ folder + "/.." });
	check(found.size() == 1 && found[0].valid && found[0].id == "theme", "the themes in a folder");

	std::printf(failures == 0 ? "all passed\n" : "%d failed\n", failures);
	return failures == 0 ? 0 : 1;
}
