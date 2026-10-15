/*
	PSSwanStation - ES-DE themes read: capabilities.xml, then theme.xml with
	everything it includes, into the elements of the system and gamelist views.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	The order things are read in is the order a theme is written for: in each
	file, its variables first, then the variables of the colour scheme, font
	size and language chosen, then the files it includes (each read the same
	way, all the way down), then its views, then the variant and the aspect
	ratio chosen (each read like a file of its own). A variable written again
	replaces the earlier one; an element written again (in the same view, of
	the same kind and name) keeps its properties and takes the new ones over
	them. Variables are put in where they are used, and a path is made
	absolute from the file it is written in.
*/
#include "esde.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <set>
#include <sys/stat.h>

namespace fe::esde
{
namespace
{

std::string trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && (unsigned char)s[a] <= ' ')
		a++;
	while (b > a && (unsigned char)s[b - 1] <= ' ')
		b--;
	return s.substr(a, b - a);
}

std::string lower(std::string s)
{
	for (char& c : s)
		c = (char)std::tolower((unsigned char)c);
	return s;
}

// "a, b,\n c" -> a, b, c
std::vector<std::string> names(const std::string& list)
{
	std::vector<std::string> out;
	std::string current;
	for (const char c : list + ",")
	{
		if (c == ',')
		{
			current = trim(current);
			if (!current.empty())
				out.push_back(current);
			current.clear();
		}
		else
			current += c;
	}
	return out;
}

bool isFile(const std::string& path)
{
	struct stat st;
	return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool isFolder(const std::string& path)
{
	struct stat st;
	return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string folderOf(const std::string& file)
{
	const size_t slash = file.rfind('/');
	return slash == std::string::npos ? std::string("./") : file.substr(0, slash + 1);
}

// "/a/b/../c/./d" -> "/a/c/d"
std::string normalise(const std::string& path)
{
	std::vector<std::string> parts;
	std::string part;
	const bool absolute = !path.empty() && path[0] == '/';
	for (const char c : path + "/")
	{
		if (c != '/')
		{
			part += c;
			continue;
		}
		if (part == "..")
		{
			if (!parts.empty() && parts.back() != "..")
				parts.pop_back();
			else if (!absolute)
				parts.push_back(part);
		}
		else if (!part.empty() && part != ".")
			parts.push_back(part);
		part.clear();
	}
	std::string out = absolute ? "/" : "";
	for (size_t i = 0; i < parts.size(); i++)
		out += (i ? "/" : "") + parts[i];
	return out;
}

// A label for the screen: the English one, the one with no language, else the first.
std::string labelOf(const pugi::xml_node& node, const std::string& otherwise)
{
	std::string first, plain, english;
	for (pugi::xml_node label : node.children("label"))
	{
		const std::string text = trim(label.child_value());
		const std::string language = label.attribute("language").value();
		if (first.empty())
			first = text;
		if (language.empty() && plain.empty())
			plain = text;
		if ((language == "en_US" || language == "en_GB") && english.empty())
			english = text;
	}
	if (!english.empty())
		return english;
	if (!plain.empty())
		return plain;
	return first.empty() ? otherwise : first;
}

bool truthy(const std::string& value, bool otherwise)
{
	const std::string v = lower(trim(value));
	if (v == "true" || v == "1" || v == "yes")
		return true;
	if (v == "false" || v == "0" || v == "no")
		return false;
	return otherwise;
}

const std::set<std::string>& elementKinds()
{
	static const std::set<std::string> kinds = { "image", "video", "animation", "badges", "text", "datetime",
		"gamelistinfo", "rating", "carousel", "grid", "textlist", "helpsystem", "clock", "systemstatus", "gameselector",
		"sound" };
	return kinds;
}

// Properties that name a file.
bool isPathProperty(const std::string& key)
{
	static const std::set<std::string> paths = { "path", "default", "fontPath", "filledPath", "unfilledPath",
		"selectorImagePath", "defaultImage", "defaultFolderImage", "staticImage", "backgroundImage", "selectorImage",
		"imagePath", "iconPath" };
	if (paths.count(key) != 0)
		return true;
	const size_t colon = key.find(':');
	return colon != std::string::npos && key.compare(0, 6, "custom") == 0;
}

// ------------------------------------------------------------------ reading

struct Reader
{
	const Theme& theme;
	Selection sel;
	const System& system;
	Loaded& out;
	std::map<std::string, std::string> vars;
	std::map<std::string, size_t> systemIndex, gamelistIndex;
	std::set<std::string> including;	// the files being read, against a file including itself
	int order = 0;

	Reader(const Theme& t, const Selection& s, const System& sys, Loaded& o) : theme(t), sel(s), system(sys), out(o) {}

	void warn(const std::string& message)
	{
		if (out.warnings.size() < 64)
			out.warnings.push_back(message);
	}

	// ${name} put in; an unknown one stays as it is written (it may be defined later).
	std::string resolve(const std::string& value, int depth = 0) const
	{
		if (value.find("${") == std::string::npos || depth > 8)
			return value;
		std::string result;
		size_t i = 0;
		while (i < value.size())
		{
			const size_t start = value.find("${", i);
			if (start == std::string::npos)
			{
				result += value.substr(i);
				break;
			}
			const size_t end = value.find('}', start + 2);
			if (end == std::string::npos)
			{
				result += value.substr(i);
				break;
			}
			result += value.substr(i, start - i);
			const std::string name = trim(value.substr(start + 2, end - start - 2));
			const auto found = vars.find(name);
			if (found != vars.end())
				result += resolve(found->second, depth + 1);
			else
				result += value.substr(start, end + 1 - start);
			i = end + 1;
		}
		return result;
	}

	// What is left of an unknown variable is taken out when a property is used.
	static std::string dropUnknown(const std::string& value)
	{
		std::string result;
		size_t i = 0;
		while (i < value.size())
		{
			const size_t start = value.find("${", i);
			if (start == std::string::npos)
			{
				result += value.substr(i);
				break;
			}
			const size_t end = value.find('}', start + 2);
			result += value.substr(i, start - i);
			if (end == std::string::npos)
				break;
			i = end + 1;
		}
		return result;
	}

	std::string path(const std::string& written, const std::string& dir) const
	{
		std::string value = trim(dropUnknown(resolve(written)));
		if (value.empty())
			return "";
		if (value[0] == ':' || value[0] == '~')
			return "";		// ES-DE's own resources, the home folder: neither is here
		if (value[0] == '/')
			return normalise(value);
		const std::string relative = value.compare(0, 2, "./") == 0 ? value.substr(2) : value;
		const std::string here = normalise(dir + relative);
		if (isFile(here) || isFolder(here))
			return here;
		// Written in an included file about the theme's own folder.
		const std::string fromRoot = normalise(theme.folder + relative);
		return isFile(fromRoot) ? fromRoot : here;
	}

	static bool chosen(const pugi::xml_node& node, const std::string& selected)
	{
		if (selected.empty())
			return false;
		for (const std::string& name : names(node.attribute("name").value()))
			if (name == selected || name == "all")
				return true;
		return false;
	}

	void readVariables(const pugi::xml_node& node)
	{
		for (pugi::xml_node v : node.children())
			if (v.type() == pugi::node_element)
				vars[v.name()] = resolve(trim(v.child_value()));
	}

	void include(const std::string& written, const std::string& dir, int depth)
	{
		const std::string file = path(written, dir);
		if (file.empty() || !isFile(file))
		{
			// A per-system file the theme does not have for this system is usual.
			if (written.find("${") == std::string::npos && file.find("${") == std::string::npos)
				warn("include not found: " + trim(written));
			return;
		}
		if (depth > 24 || !including.insert(file).second)
		{
			warn("include skipped (it includes itself): " + file);
			return;
		}
		readFile(file, depth);
		including.erase(file);
	}

	bool readFile(const std::string& file, int depth)
	{
		pugi::xml_document doc;
		const pugi::xml_parse_result parsed = doc.load_file(file.c_str(), pugi::parse_default | pugi::parse_trim_pcdata);
		if (!parsed)
		{
			warn(file + ": " + parsed.description());
			return false;
		}
		out.files++;
		const pugi::xml_node root = doc.child("theme");
		if (!root)
		{
			warn(file + ": no <theme>");
			return false;
		}
		process(root, folderOf(file), depth);
		return true;
	}

	void process(const pugi::xml_node& node, const std::string& dir, int depth)
	{
		for (pugi::xml_node child : node.children("variables"))
			readVariables(child);
		for (pugi::xml_node child : node.children("colorScheme"))
			if (chosen(child, sel.colorScheme))
				for (pugi::xml_node v : child.children("variables"))
					readVariables(v);
		for (pugi::xml_node child : node.children("fontSize"))
			if (chosen(child, sel.fontSize))
				for (pugi::xml_node v : child.children("variables"))
					readVariables(v);
		for (pugi::xml_node child : node.children("language"))
			if (chosen(child, sel.language))
				for (pugi::xml_node v : child.children("variables"))
					readVariables(v);
		for (pugi::xml_node child : node.children("include"))
			include(child.child_value(), dir, depth + 1);
		for (pugi::xml_node child : node.children("view"))
			readView(child, dir);
		for (pugi::xml_node child : node.children("variant"))
			if (chosen(child, sel.variant))
				process(child, dir, depth + 1);
		for (pugi::xml_node child : node.children("aspectRatio"))
			if (chosen(child, sel.aspectRatio))
				process(child, dir, depth + 1);
	}

	void readView(const pugi::xml_node& node, const std::string& dir)
	{
		for (const std::string& viewName : names(node.attribute("name").value()))
		{
			View *view = viewName == "system" ? &out.system : viewName == "gamelist" ? &out.gamelist : nullptr;
			if (view == nullptr)
				continue;
			auto& index = view == &out.system ? systemIndex : gamelistIndex;
			for (pugi::xml_node e : node.children())
			{
				if (e.type() != pugi::node_element)
					continue;
				const std::string type = e.name();
				if (elementKinds().count(type) == 0)
					continue;
				for (const std::string& name : names(e.attribute("name").value()))
				{
					const std::string key = type + '\x01' + name;
					auto found = index.find(key);
					if (found == index.end())
					{
						Element fresh;
						fresh.type = type;
						fresh.name = name;
						fresh.order = order++;
						view->elements.push_back(fresh);
						found = index.emplace(key, view->elements.size() - 1).first;
					}
					Element& element = view->elements[found->second];
					for (pugi::xml_node p : e.children())
					{
						if (p.type() != pugi::node_element)
							continue;
						std::string prop = p.name();
						// customIcon icon="x", customBadgeIcon badge="x", customButtonIcon button="x"...
						if (pugi::xml_attribute a = p.first_attribute())
							prop += std::string(":") + a.value();
						const std::string written = p.child_value();
						element.props[prop] = isPathProperty(prop) ? path(written, dir)
								: trim(dropUnknown(resolve(written)));
					}
				}
			}
		}
	}
};

const std::string& pick(const std::vector<Choice>& choices, const std::string& wanted,
		std::initializer_list<const char *> preferred)
{
	static const std::string none;
	for (const Choice& c : choices)
		if (c.name == wanted)
			return c.name;
	for (const char *p : preferred)
		for (const Choice& c : choices)
			if (c.name == p)
				return c.name;
	for (const Choice& c : choices)
		if (c.selectable)
			return c.name;
	return choices.empty() ? none : choices.front().name;
}

} // namespace

// ------------------------------------------------------------------ Element

const std::string& Element::get(const std::string& key) const
{
	static const std::string none;
	const auto it = props.find(key);
	return it == props.end() ? none : it->second;
}

std::string Element::get(const std::string& key, const std::string& otherwise) const
{
	const auto it = props.find(key);
	return it == props.end() || it->second.empty() ? otherwise : it->second;
}

float Element::number(const std::string& key, float otherwise) const
{
	const auto it = props.find(key);
	if (it == props.end() || it->second.empty())
		return otherwise;
	char *end = nullptr;
	const float v = std::strtof(it->second.c_str(), &end);
	return end == it->second.c_str() || !std::isfinite(v) ? otherwise : v;
}

bool Element::pair(const std::string& key, float& x, float& y) const
{
	const auto it = props.find(key);
	if (it == props.end() || it->second.empty())
		return false;
	const char *s = it->second.c_str();
	char *end = nullptr;
	const float a = std::strtof(s, &end);
	if (end == s || !std::isfinite(a))
		return false;
	const char *rest = end;
	const float b = std::strtof(rest, &end);
	x = a;
	y = end == rest || !std::isfinite(b) ? a : b;
	return true;
}

bool Element::flag(const std::string& key, bool otherwise) const
{
	const auto it = props.find(key);
	return it == props.end() ? otherwise : truthy(it->second, otherwise);
}

uint32_t Element::colour(const std::string& key, uint32_t otherwise) const
{
	uint32_t c = otherwise;
	const auto it = props.find(key);
	if (it != props.end())
		parseColour(it->second, c);
	return c;
}

const Element *View::find(const std::string& type, const std::string& name) const
{
	for (const Element& e : elements)
		if (e.type == type && e.name == name)
			return &e;
	return nullptr;
}

bool parseColour(const std::string& value, uint32_t& out)
{
	std::string v = trim(value);
	if (!v.empty() && v[0] == '#')
		v = v.substr(1);
	if (v.size() != 6 && v.size() != 8)
		return false;
	for (const char c : v)
		if (!std::isxdigit((unsigned char)c))
			return false;
	const unsigned long n = std::strtoul(v.c_str(), nullptr, 16);
	uint32_t r, g, b, a = 255;
	if (v.size() == 6)
	{
		r = (n >> 16) & 255;
		g = (n >> 8) & 255;
		b = n & 255;
	}
	else
	{
		r = (n >> 24) & 255;
		g = (n >> 16) & 255;
		b = (n >> 8) & 255;
		a = n & 255;
	}
	out = (a << 24) | (b << 16) | (g << 8) | r;
	return true;
}

// ------------------------------------------------------------------ themes

bool readTheme(const std::string& folderIn, Theme& out)
{
	out = Theme();
	out.folder = folderIn.empty() || folderIn.back() == '/' ? folderIn : folderIn + "/";
	{
		std::string f = out.folder.substr(0, out.folder.size() - 1);
		const size_t slash = f.rfind('/');
		out.id = slash == std::string::npos ? f : f.substr(slash + 1);
	}
	out.name = out.id;
	const std::string capabilities = out.folder + "capabilities.xml";
	if (!isFile(capabilities))
	{
		out.problem = isFile(out.folder + "theme.xml")
				? "no capabilities.xml: a theme for an older EmulationStation, which ES-DE themes replaced"
				: "no capabilities.xml";
		return false;
	}
	pugi::xml_document doc;
	const pugi::xml_parse_result parsed = doc.load_file(capabilities.c_str());
	if (!parsed)
	{
		out.problem = std::string("capabilities.xml: ") + parsed.description();
		return false;
	}
	const pugi::xml_node root = doc.child("themeCapabilities");
	if (!root)
	{
		out.problem = "capabilities.xml has no <themeCapabilities>";
		return false;
	}
	const std::string themeName = trim(root.child_value("themeName"));
	if (!themeName.empty())
		out.name = themeName;
	for (pugi::xml_node v : root.children("variant"))
	{
		Variant variant;
		variant.name = trim(v.attribute("name").value());
		if (variant.name.empty())
			continue;
		variant.label = labelOf(v, variant.name);
		variant.selectable = truthy(v.child_value("selectable"), true);
		for (pugi::xml_node o : v.children("override"))
		{
			Override ov;
			ov.trigger = trim(o.child_value("trigger"));
			for (pugi::xml_node m : o.children("mediaType"))
				for (const std::string& type : names(m.child_value()))
					ov.mediaTypes.push_back(type);
			ov.useVariant = trim(o.child_value("useVariant"));
			if (!ov.trigger.empty() && !ov.useVariant.empty())
				variant.overrides.push_back(ov);
		}
		out.variants.push_back(variant);
	}
	for (pugi::xml_node c : root.children("colorScheme"))
	{
		Choice choice;
		choice.name = trim(c.attribute("name").value());
		if (choice.name.empty())
			continue;
		choice.label = labelOf(c, choice.name);
		out.colorSchemes.push_back(choice);
	}
	const auto plainList = [&](const char *tag, std::vector<Choice>& list) {
		for (pugi::xml_node n : root.children(tag))
		{
			Choice choice;
			choice.name = trim(n.child_value());
			if (choice.name.empty())
				continue;
			choice.label = choice.name;
			list.push_back(choice);
		}
	};
	plainList("fontSize", out.fontSizes);
	plainList("aspectRatio", out.aspectRatios);
	plainList("language", out.languages);
	for (Choice& c : out.aspectRatios)
	{
		std::string label = c.name;
		const size_t underscore = label.find('_');
		if (underscore != std::string::npos)
			label = label.substr(0, underscore) + " (" + label.substr(underscore + 1) + ")";
		c.label = label;
	}
	for (Choice& c : out.fontSizes)
	{
		std::string label = c.name;
		if (!label.empty())
			label[0] = (char)std::toupper((unsigned char)label[0]);
		c.label = label;
	}
	if (!isFile(out.folder + "theme.xml"))
	{
		out.problem = "no theme.xml";
		return false;
	}
	out.valid = true;
	return true;
}

std::vector<Theme> scan(const std::vector<std::string>& folders)
{
	std::vector<Theme> themes;
	std::set<std::string> seen;
	for (std::string base : folders)
	{
		if (base.empty())
			continue;
		if (base.back() != '/')
			base += '/';
		DIR *dir = opendir(base.c_str());
		if (dir == nullptr)
			continue;
		while (dirent *entry = readdir(dir))
		{
			const std::string name = entry->d_name;
			if (name.empty() || name[0] == '.')
				continue;
			const std::string folder = base + name;
			if (!isFolder(folder) || !seen.insert(lower(name)).second)
				continue;
			Theme theme;
			readTheme(folder, theme);
			// A folder without a theme in it (a download being unpacked) is left out.
			if (theme.valid || isFile(folder + "/theme.xml") || isFile(folder + "/capabilities.xml"))
				themes.push_back(theme);
		}
		closedir(dir);
	}
	std::sort(themes.begin(), themes.end(),
			[](const Theme& a, const Theme& b) { return lower(a.name) < lower(b.name); });
	return themes;
}

std::string nearestAspectRatio(const Theme& theme, float ratio)
{
	std::string best;
	float bestDistance = 1e9f;
	for (const Choice& c : theme.aspectRatios)
	{
		float w = 0, h = 0;
		if (std::sscanf(c.name.c_str(), "%f:%f", &w, &h) != 2 || w <= 0 || h <= 0)
			continue;
		const bool vertical = c.name.find("vert") != std::string::npos;
		const float r = vertical ? h / w : w / h;
		const float distance = std::fabs(std::log(r / ratio));
		if (distance < bestDistance - 1e-4f)
		{
			bestDistance = distance;
			best = c.name;
		}
	}
	return best;
}

bool load(const Theme& theme, const Selection& chosen, const System& system,
		const std::function<bool(const std::string&)>& hasMedia, Loaded& out)
{
	out = Loaded();
	if (!theme.valid)
	{
		out.warnings.push_back(theme.problem.empty() ? "not a theme" : theme.problem);
		return false;
	}
	Selection sel;
	{
		std::vector<Choice> variants(theme.variants.begin(), theme.variants.end());
		sel.variant = pick(variants, chosen.variant, {});
		// A chosen variant may give way to another when the system lacks
		// what it shows (videos are never here).
		for (const Variant& v : theme.variants)
		{
			if (v.name != sel.variant)
				continue;
			for (const Override& o : v.overrides)
			{
				bool applies = false;
				if (o.trigger == "noVideos")
					applies = true;
				else if (o.trigger == "noMedia")
				{
					applies = true;
					for (const std::string& type : o.mediaTypes)
						if (hasMedia && hasMedia(type))
							applies = false;
				}
				if (applies)
				{
					sel.variant = o.useVariant;
					break;
				}
			}
			break;
		}
	}
	sel.colorScheme = pick(theme.colorSchemes, chosen.colorScheme, {});
	sel.fontSize = pick(theme.fontSizes, chosen.fontSize, { "medium" });
	sel.aspectRatio = pick(theme.aspectRatios, chosen.aspectRatio, { "16:9" });
	sel.language = pick(theme.languages, chosen.language, { "en_US", "en_GB" });
	if (sel.language.empty())
		sel.language = "en_US";
	out.used = sel;

	Reader reader(theme, sel, system, out);
	const std::pair<const char *, std::string> systemVars[] = {
		{ "system.name", system.name }, { "system.fullName", system.fullName }, { "system.theme", system.theme },
		{ "system.sortName", system.fullName },
		{ "system.name.autoCollections", system.name }, { "system.fullName.autoCollections", system.fullName },
		{ "system.theme.autoCollections", system.theme },
		{ "system.name.customCollections", system.name }, { "system.fullName.customCollections", system.fullName },
		{ "system.theme.customCollections", system.theme },
		{ "system.name.noCollections", system.name }, { "system.fullName.noCollections", system.fullName },
		{ "system.theme.noCollections", system.theme },
		{ "lang", sel.language },
	};
	for (const auto& v : systemVars)
		reader.vars[v.first] = v.second;

	std::string file = theme.folder + system.theme + "/theme.xml";
	if (!isFile(file))
		file = theme.folder + "theme.xml";
	out.themeFile = file;
	reader.including.insert(file);
	if (!reader.readFile(file, 0))
		return false;
	return !out.gamelist.elements.empty() || !out.system.elements.empty();
}

} // namespace fe::esde
