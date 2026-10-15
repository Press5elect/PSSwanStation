/*
	PSSwanStation - ES-DE themes: what a theme offers, and a theme read into
	the elements of its two views for one system.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	ES-DE (es-de.org, the EmulationStation Desktop Edition) themes are folders
	of XML files, pictures and fonts: capabilities.xml says what a theme
	offers (its variants, colour schemes, font sizes, aspect ratios and
	languages), theme.xml (and what it includes) places elements on the
	screen, each a picture, a text, a list of games and so on with its
	properties, for the system view and the gamelist view. This file reads
	both the way ES-DE's theme format is written: includes, variables (the
	theme's and the system's), the chosen variant, colour scheme, font size,
	aspect ratio and language, a variant's overrides, and elements whose
	properties are merged from every place they are written. What the
	elements look like on the screen is ui_esde.cpp's.

	No part of ES-DE is copied here; the format is read from what themes
	written for it contain. Themes are not shipped with the title: each one
	is its author's, under its own licence, and is put on the console by the
	person who uses it (or fetched by the title for them, ui_esde.cpp).
*/
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fe::esde
{

struct Choice
{
	std::string name;	// as theme.xml names it
	std::string label;	// for the screen (the name when the theme gives none)
	bool selectable = true;
};

// A variant used in place of the chosen one when the system lacks something.
struct Override
{
	std::string trigger;					// "noVideos", "noMedia"
	std::vector<std::string> mediaTypes;	// noMedia's kinds ("miximage", "screenshot")
	std::string useVariant;
};

struct Variant : Choice
{
	std::vector<Override> overrides;
};

// A theme as capabilities.xml describes it.
struct Theme
{
	std::string folder;		// absolute, ending in '/'
	std::string id;			// the folder's name
	std::string name;		// themeName, or the folder's name
	std::vector<Variant> variants;
	std::vector<Choice> colorSchemes, fontSizes, aspectRatios, languages;
	bool valid = false;
	std::string problem;	// why not, when not
};

// The themes in these folders (each a folder with capabilities.xml), by name.
std::vector<Theme> scan(const std::vector<std::string>& folders);
bool readTheme(const std::string& folder, Theme& out);

// One element of a view: its kind ("image", "textlist"...), its name, and its
// properties as written, with variables put in and paths made absolute
// (a property with an attribute, such as customBadgeIcon badge="favorite",
// is "customBadgeIcon:favorite").
struct Element
{
	std::string type, name;
	std::map<std::string, std::string> props;
	int order = 0;		// where it was first written (for equal zIndex)

	bool has(const std::string& key) const { return props.count(key) != 0; }
	const std::string& get(const std::string& key) const;
	std::string get(const std::string& key, const std::string& otherwise) const;
	float number(const std::string& key, float otherwise) const;
	// "x y"; a single number for both.
	bool pair(const std::string& key, float& x, float& y) const;
	bool flag(const std::string& key, bool otherwise) const;
	// RRGGBB or RRGGBBAA, as 0xAABBGGRR (the interface's).
	uint32_t colour(const std::string& key, uint32_t otherwise) const;
};

struct View
{
	std::vector<Element> elements;	// in the order first written
	const Element *find(const std::string& type, const std::string& name) const;
};

// What is chosen of what a theme offers; empty: the theme's first (16:9 and
// en_US where the theme has them, "medium" for the font size).
struct Selection
{
	std::string variant, colorScheme, fontSize, aspectRatio, language;
};

// The system the views are read for, as ES-DE names it.
struct System
{
	std::string name = "psx";
	std::string fullName = "Sony PlayStation";
	std::string theme = "psx";
};

struct Loaded
{
	View system, gamelist;
	Selection used;			// what was used, the defaults filled in
	std::string themeFile;	// theme.xml read
	std::vector<std::string> warnings;
	int files = 0;			// XML files read
};

// Converts a colour as themes write it (RRGGBB or RRGGBBAA) to 0xAABBGGRR.
bool parseColour(const std::string& value, uint32_t& out);

// Reads the theme for the system. `hasMedia` answers whether any game has
// pictures of a kind (a variant's noMedia override); videos never are.
bool load(const Theme& theme, const Selection& chosen, const System& system,
		const std::function<bool(const std::string& mediaType)>& hasMedia, Loaded& out);

// The display's aspect ratio as theme.xml names it, nearest of those offered.
std::string nearestAspectRatio(const Theme& theme, float ratio);

} // namespace fe::esde
