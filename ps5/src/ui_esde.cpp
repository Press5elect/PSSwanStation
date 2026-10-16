/*
	PSSwanStation - ES-DE themes as the library's look: a theme's gamelist
	view drawn with the library's games, and the pages that choose a theme,
	its variant and colours, and fetch more.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	ES-DE (es-de.org) themes describe each screen as elements placed on it, in
	fractions of the screen: pictures, texts, the game's details, and one
	list of the games (a text list, a grid or a carousel). esde.cpp reads a
	theme into those elements; this file draws them over the library's games
	for the system ES-DE calls psx (Sony PlayStation), in the order of their
	zIndex, and moves the list with the pad. The library's own buttons stay
	what they are (Cross plays, Triangle opens the details, Square sorts and
	filters, L1 and R1 go through the console, USB and network games).

	What a game shows comes from what the library knows: its name, the game
	database's developer, publisher, genre, players, release date and
	description, when it was last played, and whether it is a favourite. Its
	pictures are those ES-DE keeps (media/<kind>/<file name>.png or .jpg, as
	ES-DE's downloaded_media/psx folders are laid out, so a folder scraped for
	ES-DE can be copied in), else the cover, and title screens and moments of
	play from the libretro thumbnails collection when downloads are on.
	A video element plays the game's video (media/videos/<file name>.mp4, as
	ES-DE keeps them) after its delay (video.cpp, FFmpeg's decoders), its
	picture until then; animations are not played. Whether videos play and
	how loud is the library's setting (Settings, Library), for every view.

	Themes are not part of the title. Each is its author's, under its own
	licence; they go in <root>themes (or PSSwanStation/themes on a USB drive),
	put there by the person who uses them or fetched here for them from the
	list ES-DE's own theme downloader reads.
*/
#include "ui_internal.h"
#include "esde.h"
#include "display.h"
#include "json.h"

#include <miniz.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fe::ui
{
using namespace platform;

namespace
{

// ------------------------------------------------------------- the settings

// themes.cfg beside frontend.cfg: the theme, and what is chosen of each theme.
struct Settings
{
	bool read = false;
	std::string theme;
	std::map<std::string, esde::Selection> chosen;
};
Settings settings;

std::string settingsFile()
{
	return appDir + "themes.cfg";
}

void readSettings()
{
	if (settings.read)
		return;
	settings.read = true;
	FILE *f = fopen(settingsFile().c_str(), "r");
	if (f == nullptr)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		const std::string text = trim(line);
		if (text.empty() || text[0] == '#')
			continue;
		const size_t equals = text.find('=');
		if (equals == std::string::npos)
			continue;
		const std::string key = trim(text.substr(0, equals)), value = trim(text.substr(equals + 1));
		if (key == "theme")
		{
			settings.theme = value;
			continue;
		}
		// Before build 23's test 10 the videos' switches were here; they are
		// the library's now (frontend.cfg), for every view. One that was off
		// stays off.
		if (key == "videos" || key == "video_sound")
		{
			if (value == "0")
			{
				if (key == "videos")
					options::frontend().videos = false;
				else
					options::frontend().videoVolume = 0;
				options::saveFrontend();
			}
			continue;
		}
		const size_t dot = key.rfind('.');
		if (dot == std::string::npos)
			continue;
		esde::Selection& s = settings.chosen[key.substr(0, dot)];
		const std::string field = key.substr(dot + 1);
		if (field == "variant")
			s.variant = value;
		else if (field == "colorScheme")
			s.colorScheme = value;
		else if (field == "fontSize")
			s.fontSize = value;
		else if (field == "aspectRatio")
			s.aspectRatio = value;
		else if (field == "language")
			s.language = value;
	}
	fclose(f);
}

void saveSettings()
{
	std::string text = "# PSSwanStation - the ES-DE theme the library is drawn in (Settings, Library).\n";
	text += "theme = " + settings.theme + "\n";
	for (const auto& [id, s] : settings.chosen)
	{
		const std::pair<const char *, const std::string *> fields[] = { { "variant", &s.variant },
			{ "colorScheme", &s.colorScheme }, { "fontSize", &s.fontSize }, { "aspectRatio", &s.aspectRatio },
			{ "language", &s.language } };
		for (const auto& [name, value] : fields)
			if (!value->empty())
				text += id + "." + name + " = " + *value + "\n";
	}
	writeFile(settingsFile(), text.data(), text.size());
}

// --------------------------------------------------------------- the themes

std::vector<esde::Theme> themes;
bool scanned = false;

std::vector<std::string> themeFolders()
{
	std::vector<std::string> folders = { rootDir + "themes" };
	for (const std::string& drive : usbDrives())
		folders.push_back(drive + "/" + storage::UsbFolder + "/themes");
	return folders;
}

void scan()
{
	if (scanned)
		return;
	scanned = true;
	themes = esde::scan(themeFolders());
	int usable = 0;
	for (const esde::Theme& t : themes)
	{
		if (t.valid)
			usable++;
		else
			diag::mark("themes: %s cannot be used: %s", t.id.c_str(), t.problem.c_str());
	}
	diag::mark("themes: %d ES-DE theme(s) found, %d usable", (int)themes.size(), usable);
}

const esde::Theme *themeById(const std::string& id)
{
	scan();
	for (const esde::Theme& t : themes)
		if (t.id == id && t.valid)
			return &t;
	return nullptr;
}

// ---------------------------------------------------------------- the media

// ES-DE's kinds of picture, and the folders its downloaded_media keeps them in.
const std::pair<const char *, const char *> mediaKinds[] = {
	{ "miximage", "miximages" }, { "marquee", "marquees" }, { "screenshot", "screenshots" },
	{ "titlescreen", "titlescreens" }, { "cover", "covers" }, { "backcover", "backcovers" },
	{ "3dbox", "3dboxes" }, { "physicalmedia", "physicalmedia" }, { "fanart", "fanart" },
};

struct MediaCache
{
	unsigned coversSeen = 0, mediaSeen = 0;
	std::map<std::string, std::string> found;	// game path + kind -> file ("" none)
};
MediaCache mediaCache;

void mediaCacheCheck()
{
	MediaCache& c = mediaCache;
	if (c.coversSeen != covers::generation() || c.mediaSeen != media::generation())
	{
		c.coversSeen = covers::generation();
		c.mediaSeen = media::generation();
		c.found.clear();
	}
}

// The picture of one kind for a game, or empty: ES-DE's media folders
// wherever they are (media.cpp), else the library's cover. `fetch`: a title
// screen or a moment of play is asked for from the libretro collection when
// there is none.
std::string mediaOf(const library::Game& game, const std::string& kind, bool fetch)
{
	if (kind == "image")
	{
		const std::string mix = mediaOf(game, "miximage", fetch);
		return mix.empty() ? mediaOf(game, "screenshot", fetch) : mix;
	}
	const char *folder = nullptr;
	for (const auto& [name, dir] : mediaKinds)
		if (kind == name)
			folder = dir;
	if (folder == nullptr)
		return "";
	mediaCacheCheck();
	MediaCache& c = mediaCache;
	const std::string key = game.path + '\x01' + kind + (fetch ? "+" : "");
	const auto it = c.found.find(key);
	if (it != c.found.end())
		return it->second;
	std::string file = media::find(game, folder);
	if (file.empty() && kind == "cover")
		file = covers::find(game);
	if (file.empty() && fetch && kind == "titlescreen")
		file = covers::findMedia(game, covers::TitleScreen);
	if (file.empty() && fetch && kind == "screenshot")
		file = covers::findMedia(game, covers::InGame);
	c.found[key] = file;
	return file;
}

// A game's video (ES-DE's videos folder, wherever it is), or empty.
std::string videoOf(const library::Game& game)
{
	mediaCacheCheck();
	MediaCache& c = mediaCache;
	const std::string key = game.path + "\x01video";
	const auto it = c.found.find(key);
	if (it != c.found.end())
		return it->second;
	const std::string file = media::find(game, "videos");
	c.found[key] = file;
	return file;
}

// The first picture of the kinds listed ("miximage,screenshot") a game has.
std::string mediaFirst(const library::Game& game, const std::string& kinds)
{
	std::vector<std::string> list;
	std::string current;
	for (const char c : kinds + ",")
	{
		if (c == ',')
		{
			current = trim(current);
			if (!current.empty())
				list.push_back(current);
			current.clear();
		}
		else
			current += c;
	}
	for (const std::string& kind : list)
	{
		const std::string file = mediaOf(game, kind, false);
		if (!file.empty())
			return file;
	}
	// Nothing on the console: what the collection can give, for next time.
	for (const std::string& kind : list)
		if (kind == "screenshot" || kind == "titlescreen" || kind == "image")
		{
			const std::string file = mediaOf(game, kind, true);
			if (!file.empty())
				return file;
		}
	return "";
}

// ------------------------------------------------------------ the theme read

struct Active
{
	std::string id;
	esde::Selection selection;	// as chosen (empty: the theme's first)
	esde::Loaded loaded;
	bool ok = false;
	std::string problem;
	std::vector<const esde::Element *> order;	// the gamelist view's, by zIndex
	const esde::Element *primary = nullptr;	// its list of games
	const esde::Element *help = nullptr;
	int generation = 0;
	unsigned mediaKey = 0;		// the media there were when a variant's fallback was decided
};
Active state;
int wantReload = 1;
double lastMediaReload = -10;

// Whether a theme's files are there, asked once a reading of the theme.
std::map<std::string, bool> existing;

bool exists(const std::string& path)
{
	const auto it = existing.find(path);
	if (it != existing.end())
		return it->second;
	const bool there = fileExists(path);
	existing[path] = there;
	return there;
}

// Text wrapped to a width, kept while it is shown (descriptions are long).
std::map<std::string, std::vector<std::string>> wrapped;

const std::vector<std::string>& wrapCached(int font, const std::string& text, float em, float width)
{
	const std::string key = format("%d|%.1f|%.1f|", font, em, width) + text;
	auto it = wrapped.find(key);
	if (it != wrapped.end())
		return it->second;
	if (wrapped.size() > 256)
		wrapped.clear();
	return wrapped.emplace(key, fontWrap(font, text, em, width)).first->second;
}

float defaultZ(const std::string& type)
{
	if (type == "image" || type == "video")
		return 30;
	if (type == "animation" || type == "badges")
		return 35;
	if (type == "text" || type == "datetime")
		return 40;
	if (type == "gamelistinfo" || type == "rating")
		return 45;
	if (type == "carousel" || type == "grid" || type == "textlist")
		return 50;
	return 60;
}

bool shown(const esde::Element& e)
{
	if (!e.flag("visible", true))
		return false;
	const std::string scope = e.get("scope");
	return scope.empty() || scope == "view" || scope == "shared";
}

// Reads the chosen theme again when it, or what is chosen of it, changed.
bool ensureLoaded(const std::function<bool(const std::string&)>& hasMedia)
{
	readSettings();
	const esde::Theme *theme = themeById(settings.theme);
	if (theme == nullptr)
	{
		state.ok = false;
		state.problem = settings.theme.empty() ? "No ES-DE theme is chosen" : "The theme " + settings.theme
				+ " is not in the themes folder";
		return false;
	}
	esde::Selection sel = settings.chosen[theme->id];
	if (sel.aspectRatio.empty())
		sel.aspectRatio = esde::nearestAspectRatio(*theme, width() / std::max(height(), 1.f));
	const bool same = state.id == theme->id && sel.variant == state.selection.variant
			&& sel.colorScheme == state.selection.colorScheme && sel.fontSize == state.selection.fontSize
			&& sel.aspectRatio == state.selection.aspectRatio && sel.language == state.selection.language;
	// A variant that gives way when the games lack its pictures is decided
	// again when pictures come (fetched, or the media folders listed).
	const unsigned mediaKey = media::generation() * 7919u + covers::generation();
	bool overrides = false;
	for (const esde::Variant& v : theme->variants)
		if (!v.overrides.empty() && (v.name == sel.variant || (sel.variant.empty() && &v == &theme->variants.front())))
			overrides = true;
	if (same && overrides && mediaKey != state.mediaKey && now() - lastMediaReload > 2.0)
	{
		lastMediaReload = now();
		wantReload = 1;
	}
	if (same && !wantReload)
		return state.ok;
	wantReload = 0;
	existing.clear();
	wrapped.clear();
	const double started = now();
	state = Active();
	state.mediaKey = mediaKey;
	state.id = theme->id;
	state.selection = sel;
	state.ok = esde::load(*theme, sel, esde::System(), hasMedia, state.loaded);
	state.generation++;
	for (const std::string& w : state.loaded.warnings)
		diag::mark("themes: %s: %s", theme->id.c_str(), w.c_str());
	if (!state.ok)
	{
		state.problem = state.loaded.warnings.empty() ? "The theme could not be read" : state.loaded.warnings[0];
		return false;
	}
	for (const esde::Element& e : state.loaded.gamelist.elements)
	{
		if (!shown(e))
			continue;
		if (e.type == "helpsystem")
		{
			if (state.help == nullptr)
				state.help = &e;
			continue;
		}
		if (e.type == "carousel" || e.type == "grid" || e.type == "textlist")
		{
			// One list of games: the first shown.
			if (state.primary != nullptr)
				continue;
			state.primary = &e;
		}
		if (e.type == "sound" || e.type == "gameselector" || e.type == "systemstatus")
			continue;
		state.order.push_back(&e);
	}
	std::stable_sort(state.order.begin(), state.order.end(), [](const esde::Element *a, const esde::Element *b) {
		return a->number("zIndex", defaultZ(a->type)) < b->number("zIndex", defaultZ(b->type));
	});
	diag::mark("themes: %s read (%d files, %d elements, variant %s, colours %s, %s, %s) in %.0f ms", theme->id.c_str(),
			state.loaded.files, (int)state.loaded.gamelist.elements.size(), state.loaded.used.variant.c_str(),
			state.loaded.used.colorScheme.c_str(), state.loaded.used.fontSize.c_str(),
			state.loaded.used.aspectRatio.c_str(), (now() - started) * 1000.0);
	if (state.primary == nullptr)
		diag::mark("themes: %s has no list of games in its gamelist view; a plain list is drawn", theme->id.c_str());
	return true;
}

// --------------------------------------------------------------- drawing

constexpr uint32_t White = 0xffffffffu;

ImU32 fade(ImU32 c, float opacity)
{
	const float a = (float)(c >> 24) * std::clamp(opacity, 0.f, 1.f);
	return (c & 0x00ffffffu) | ((ImU32)(a + 0.5f) << 24);
}

struct Box
{
	float x = 0, y = 0, w = 0, h = 0;
	ImVec2 a() const { return ImVec2(x, y); }
	ImVec2 b() const { return ImVec2(x + w, y + h); }
};

float W()
{
	return width();
}

float H()
{
	return height();
}

// The element's place for a size: its pos less its origin's part of the size.
Box place(const esde::Element& e, float w, float h)
{
	float px = 0, py = 0, ox = 0, oy = 0;
	e.pair("pos", px, py);
	e.pair("origin", ox, oy);
	return { px * W() - ox * w, py * H() - oy * h, w, h };
}

bool sizeOf(const esde::Element& e, const char *key, float& w, float& h)
{
	float x = 0, y = 0;
	if (!e.pair(key, x, y))
		return false;
	w = x * W();
	h = y * H();
	return w > 0 || h > 0;
}

// The pixels an element's picture is wanted at (for its reading).
int wantedSide(const esde::Element& e)
{
	float w = 0, h = 0;
	if (sizeOf(e, "cropSize", w, h) || sizeOf(e, "maxSize", w, h) || sizeOf(e, "size", w, h))
		return (int)std::max(std::max(w, h), 32.f);
	return 1024;
}

// Where a picture goes and which part of it shows, for the element's
// size, maxSize or cropSize.
bool fitPicture(const esde::Element& e, float iw, float ih, float& w, float& h, ImVec2& uv0, ImVec2& uv1)
{
	uv0 = ImVec2(0, 0);
	uv1 = ImVec2(1, 1);
	if (iw <= 0 || ih <= 0)
		return false;
	float bw = 0, bh = 0;
	if (sizeOf(e, "cropSize", bw, bh) && bw > 0 && bh > 0)
	{
		// Filled and cut to the box, about its middle.
		w = bw;
		h = bh;
		const float scale = std::max(bw / iw, bh / ih);
		const float uw = bw / (iw * scale), uh = bh / (ih * scale);
		uv0 = ImVec2((1 - uw) * 0.5f, (1 - uh) * 0.5f);
		uv1 = ImVec2(uv0.x + uw, uv0.y + uh);
		return true;
	}
	if (sizeOf(e, "maxSize", bw, bh))
	{
		const float scale = std::min(bw > 0 ? bw / iw : 1e9f, bh > 0 ? bh / ih : 1e9f);
		w = iw * scale;
		h = ih * scale;
		return true;
	}
	if (sizeOf(e, "size", bw, bh))
	{
		w = bw > 0 ? bw : bh * iw / ih;
		h = bh > 0 ? bh : bw * ih / iw;
		return true;
	}
	// As large as it is, on a 1080-line screen.
	w = iw * H() / 1080.f;
	h = ih * H() / 1080.f;
	return true;
}

void picture(const Image& img, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1, ImU32 colour, ImU32 colourEnd,
		bool horizontal, float rounding, float degrees)
{
	if (img.id == nullptr)
		return;
	if (degrees != 0)
		draw()->AddImageRotated((ImTextureID)img.id, a, b, uv0, uv1, colour, rounding, degrees * 3.14159265f / 180.f);
	else if (colourEnd != colour && !horizontal)
		draw()->AddImageGradient((ImTextureID)img.id, a, b, uv0, uv1, colour, colourEnd, rounding);
	else if (colourEnd != colour)
		draw()->AddImageRounded((ImTextureID)img.id, a, b, uv0, uv1, mix(colour, colourEnd, 0.5f), rounding);
	else
		draw()->AddImageRounded((ImTextureID)img.id, a, b, uv0, uv1, colour, rounding);
}

// A picture (read with imagePadded) turned about its middle: drawn over the
// square its turns fit in, its clear border filling what is past its edges.
void turnedPicture(const Image& img, ImVec2 centre, float w, float h, ImVec2 uv0, ImVec2 uv1, ImU32 colour,
		float degrees)
{
	if (img.id == nullptr || img.width <= PictureBorder * 2 || img.height <= PictureBorder * 2 || w <= 0 || h <= 0)
		return;
	const float d = std::sqrt(w * w + h * h);
	// The picture's own pixels (without the border), as a part of the texture.
	const float tw = (float)img.width, th = (float)img.height;
	const float b = (float)PictureBorder;
	const float u0 = (b + uv0.x * (tw - 2 * b)) / tw, u1 = (b + uv1.x * (tw - 2 * b)) / tw;
	const float v0 = (b + uv0.y * (th - 2 * b)) / th, v1 = (b + uv1.y * (th - 2 * b)) / th;
	// The square is d / w times as wide as the picture, about the same middle.
	const float um = (u0 + u1) * 0.5f, vm = (v0 + v1) * 0.5f;
	const float us = (u1 - u0) * d / w * 0.5f, vs = (v1 - v0) * d / h * 0.5f;
	draw()->AddImageRotated((ImTextureID)img.id, ImVec2(centre.x - d * 0.5f, centre.y - d * 0.5f),
			ImVec2(centre.x + d * 0.5f, centre.y + d * 0.5f), ImVec2(um - us, vm - vs), ImVec2(um + us, vm + vs), colour,
			0.f, degrees * 3.14159265f / 180.f);
}

void fill(ImVec2 a, ImVec2 b, ImU32 colour, ImU32 colourEnd, bool horizontal, float rounding)
{
	if ((colour >> 24) == 0 && (colourEnd >> 24) == 0)
		return;
	if (colourEnd == colour)
		draw()->AddRectFilled(a, b, colour, rounding);
	else if (horizontal)
		draw()->AddRectFilledMultiColor(a, b, colour, colourEnd, colourEnd, colour);
	else
		draw()->AddRectFilledMultiColor(a, b, colour, colour, colourEnd, colourEnd);
}

bool horizontalGradient(const esde::Element& e, const char *key = "gradientType")
{
	return e.get(key, "horizontal") == "horizontal";
}

// ------------------------------------------------------------ what is shown

struct Shown
{
	const EsdeGames *games = nullptr;
	int cursor = 0;
	const library::Game *game = nullptr;
	const gamedb::Info *info = nullptr;
	double since = 0;			// seconds the cursor has been on this game
	bool videoTaken = false;	// one video element plays at a time
};

std::string lowerText(std::string s)
{
	for (char& c : s)
		if ((unsigned char)c < 0x80)
			c = (char)std::tolower((unsigned char)c);
	return s;
}

std::string letterCase(const std::string& text, const std::string& how)
{
	if (how == "uppercase")
	{
		std::string s = text;
		for (char& c : s)
			if ((unsigned char)c < 0x80)
				c = (char)std::toupper((unsigned char)c);
		return s;
	}
	if (how == "lowercase")
		return lowerText(text);
	if (how == "capitalize")
	{
		std::string s = text;
		bool start = true;
		for (char& c : s)
		{
			if ((unsigned char)c < 0x80 && start)
				c = (char)std::toupper((unsigned char)c);
			start = c == ' ' || c == '-' || c == '/' || c == '(';
		}
		return s;
	}
	return text;
}

std::string metadata(const Shown& s, const std::string& key)
{
	const esde::System sys;
	if (key == "systemName" || key == "sourceSystemName")
		return sys.name;
	if (key == "systemFullname" || key == "sourceSystemFullname")
		return sys.fullName;
	if (s.game == nullptr)
		return "";
	const library::Game& g = *s.game;
	if (key == "name")
		return g.name;
	if (key == "physicalName")
		return g.fileTitle;
	if (key == "physicalNameExtension")
	{
		const size_t slash = g.path.rfind('/');
		return slash == std::string::npos ? g.path : g.path.substr(slash + 1);
	}
	if (key == "emulator")
		return "PSSwanStation";
	if (key == "favorite")
		return library::favourite(g.path) ? "Yes" : "No";
	if (key == "playtime")
	{
		// How long it has been played in all, as the library keeps it.
		const uint64_t seconds = history::get(g.path).seconds;
		if (seconds == 0)
			return "";
		const uint64_t minutes = seconds / 60;
		return minutes < 1 ? std::string("Under a minute")
				: minutes < 60 ? format("%llum", (unsigned long long)minutes)
				: format("%lluh %02llum", (unsigned long long)(minutes / 60), (unsigned long long)(minutes % 60));
	}
	if (s.info == nullptr)
		return "";
	const gamedb::Info& i = *s.info;
	if (key == "description")
		return i.description;
	if (key == "developer")
		return i.developer;
	if (key == "publisher")
		return i.publisher;
	if (key == "genre")
		return i.genre;
	if (key == "players")
		return i.players <= 0 ? "" : i.players == 1 ? "1" : format("1-%d", i.players);
	return "";
}

std::string systemData(const Shown& s, const std::string& key)
{
	const esde::System sys;
	const int count = s.games != nullptr ? s.games->count : 0;
	int favourites = 0;
	if (key.find("Favorites") != std::string::npos && s.games != nullptr)
		for (int i = 0; i < count; i++)
			if (library::favourite(s.games->game(i).path))
				favourites++;
	if (key == "name")
		return sys.name;
	if (key == "fullname")
		return sys.fullName;
	if (key == "gamecount")
		return format(count == 1 ? "%d game available" : "%d games available", count);
	if (key == "gamecountGames")
		return format(count == 1 ? "%d game" : "%d games", count);
	if (key == "gamecountGamesNoText")
		return format("%d", count);
	if (key == "gamecountFavorites")
		return format(favourites == 1 ? "%d favorite" : "%d favorites", favourites);
	if (key == "gamecountFavoritesNoText")
		return format("%d", favourites);
	return "";
}

// A date in a format as strftime writes one, without the parts not known (a
// release date the database gives to the month, or the year).
std::string dateText(const std::string& format, int year, int month, int day)
{
	std::string out;
	for (size_t i = 0; i < format.size(); i++)
	{
		if (format[i] != '%' || i + 1 >= format.size())
		{
			out += format[i];
			continue;
		}
		const char c = format[++i];
		static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July",
			"August", "September", "October", "November", "December" };
		std::string part;
		bool known = true;
		switch (c)
		{
		case 'Y': part = std::to_string(year); break;
		case 'y': part = ::fe::format("%02d", year % 100); break;
		case 'm': known = month > 0; part = ::fe::format("%02d", month); break;
		case 'b': known = month > 0; part = month > 0 ? std::string(months[month - 1]).substr(0, 3) : ""; break;
		case 'B': known = month > 0; part = month > 0 ? months[month - 1] : ""; break;
		case 'd': case 'e': known = day > 0; part = ::fe::format(c == 'd' ? "%02d" : "%d", day); break;
		case '%': part = "%"; break;
		default: part = ""; break;
		}
		if (known)
			out += part;
		else
		{
			// The separator before it goes with it ("1998-03-" -> "1998-03").
			while (!out.empty() && !std::isalnum((unsigned char)out.back()))
				out.pop_back();
			// And one after it, when it starts the date ("-03-1998").
			if (out.empty())
				while (i + 1 < format.size() && format[i + 1] != '%')
					i++;
		}
	}
	return trim(out);
}

// ---------------------------------------------------------------- the text

struct TextStyle
{
	int font = -1;
	float em = 0, line = 0;
	ImU32 colour = White;
	std::string align = "left", valign = "center", letters;
};

TextStyle textStyle(const esde::Element& e, float defaultSize, const char *colourKey = "color", ImU32 colour = White)
{
	TextStyle t;
	t.font = fontFile(e.get("fontPath"));
	t.em = std::max(e.number("fontSize", defaultSize), 0.005f) * std::min(H(), W());
	t.line = t.em * std::max(e.number("lineSpacing", 1.5f), 0.5f);
	t.colour = e.colour(colourKey, colour);
	t.align = e.get("horizontalAlignment", "left");
	t.valign = e.get("verticalAlignment", "center");
	t.letters = e.get("letterCase", "none");
	return t;
}

void line(const TextStyle& t, float x, float y, float w, const std::string& value, ImU32 colour)
{
	const float tw = fontTextWidth(t.font, value, t.em);
	const float left = t.align == "center" ? x + (w - tw) * 0.5f : t.align == "right" ? x + w - tw : x;
	fontText(t.font, ImVec2(std::floor(left), std::floor(y)), colour, value, t.em, t.line);
}

// Scrolling texts: what each showed, and since when.
struct Scroll
{
	std::string text;
	double since = 0;
};
std::map<std::string, Scroll> scrolls;

double scrollTime(const std::string& key, const std::string& text)
{
	Scroll& s = scrolls[key];
	if (s.text != text)
	{
		s.text = text;
		s.since = clock();
	}
	return clock() - s.since;
}

// A text element (and a datetime's, a gamelistinfo's): `value` in its box.
void textBox(const esde::Element& e, const std::string& value, float opacity)
{
	TextStyle t = textStyle(e, 0.045f);
	const std::string text = letterCase(value, t.letters);
	if (text.empty())
		return;
	float w = 0, h = 0;
	float sx = 0, sy = 0;
	e.pair("size", sx, sy);
	w = sx * W();
	h = sy * H();
	std::vector<std::string> lines;
	if (w <= 0)
	{
		lines.push_back(text);
		for (const std::string& l : lines)
			w = std::max(w, fontTextWidth(t.font, l, t.em));
	}
	else
		lines = wrapCached(t.font, text, t.em, w);
	const bool autoHeight = h <= 0;
	if (autoHeight)
		h = t.line * (float)lines.size();
	const Box box = place(e, w, h);
	const float rotation = e.number("rotation", 0);
	(void)rotation;
	if (e.has("backgroundColor"))
	{
		float mx = 0, my = 0;
		e.pair("backgroundMargins", mx, my);
		const float r = e.number("backgroundCornerRadius", 0) * W();
		fill(ImVec2(box.x - mx * W(), box.y), ImVec2(box.x + box.w + my * W(), box.y + box.h),
				fade(e.colour("backgroundColor", 0), opacity), fade(e.colour("backgroundColor", 0), opacity), false, r);
	}
	const ImU32 colour = fade(t.colour, opacity * e.number("opacity", 1.f));
	const bool container = e.flag("container", true) && !autoHeight;
	const bool horizontal = e.get("containerType", "vertical") == "horizontal";
	const std::string key = e.name + '\x01' + e.type;
	// One line that is too long: scrolled sideways in a horizontal container, else cut.
	if (horizontal || (!autoHeight && h < t.line * 1.6f && lines.size() <= 1) || (sx <= 0))
	{
		const std::string single = lines.size() == 1 ? lines[0] : text;
		const float tw = fontTextWidth(t.font, single, t.em);
		float y = box.y;
		if (t.valign == "center")
			y = box.y + (box.h - t.line) * 0.5f;
		else if (t.valign == "bottom")
			y = box.y + box.h - t.line;
		if (tw > box.w + 0.5f && container && horizontal && motion() != MotionOff)
		{
			const double since = scrollTime(key, single);
			const float delay = e.number("containerStartDelay", 4.5f);
			const float speed = std::max(e.number("containerScrollSpeed", 1.f), 0.1f) * t.em * 2.f;
			const float gap = t.em * 3;
			const float travel = tw + gap;
			float offset = since > delay ? std::fmod((float)(since - delay) * speed, travel) : 0.f;
			draw()->PushClipRect(box.a(), ImVec2(box.x + box.w, box.y + box.h + t.line), true);
			fontText(t.font, ImVec2(std::floor(box.x - offset), std::floor(y)), colour, single, t.em, t.line);
			if (offset > 0)
				fontText(t.font, ImVec2(std::floor(box.x - offset + travel), std::floor(y)), colour, single, t.em, t.line);
			draw()->PopClipRect();
		}
		else
		{
			const std::string cut = tw > box.w + 0.5f ? fontFit(t.font, single, t.em, box.w) : single;
			line(t, box.x, y, box.w, cut, colour);
		}
		return;
	}
	const float total = t.line * (float)lines.size();
	float y = box.y;
	float offset = 0;
	if (total <= box.h || autoHeight)
	{
		if (t.valign == "center")
			y = box.y + (box.h - total) * 0.5f;
		else if (t.valign == "bottom")
			y = box.y + box.h - total;
	}
	else if (container && motion() != MotionOff)
	{
		// Scrolled up after a while, held at the end, and back to the start.
		const double since = scrollTime(key, text);
		const float delay = e.number("containerStartDelay", 4.5f);
		const float speed = std::max(e.number("containerScrollSpeed", 1.f), 0.1f) * t.line * 0.4f;
		const float reset = e.number("containerResetDelay", 7.f);
		const float overflow = total - box.h;
		const float moving = overflow / speed;
		const float cycle = delay + moving + reset;
		const float at = std::fmod((float)since, cycle);
		offset = at < delay ? 0 : at < delay + moving ? (at - delay) * speed : overflow;
	}
	draw()->PushClipRect(box.a(), box.b(), true);
	for (size_t i = 0; i < lines.size(); i++)
	{
		const float ly = y + t.line * (float)i - offset;
		if (ly + t.line < box.y - 1 || ly > box.y + box.h + 1)
			continue;
		line(t, box.x, ly, box.w, lines[i], colour);
	}
	draw()->PopClipRect();
}

// ------------------------------------------------------------------ pictures

// The file an image or video element shows for the game under the cursor.
std::string pictureFile(const esde::Element& e, const Shown& s)
{
	if (e.has("imageType") && s.game != nullptr)
	{
		const std::string file = mediaFirst(*s.game, e.get("imageType"));
		if (!file.empty())
			return file;
		return e.get("default");
	}
	if (e.type == "video")
		return e.get("default");
	const std::string& path = e.get("path");
	if (!path.empty() && exists(path))
		return path;
	return e.get("default");
}

void imageElement(const esde::Element& e, const Shown& s);

// A video element: the game's video (or the element's own) once the cursor
// has stayed on the game for its delay, its picture until then (and after
// its end, when the theme says so).
void videoElement(const esde::Element& e, Shown& s)
{
	std::string file;
	if (!e.get("path").empty() && exists(e.get("path")))
		file = e.get("path");
	else if (s.game != nullptr)
		file = videoOf(*s.game);
	if (file.empty() && !e.get("default").empty() && exists(e.get("default")))
		file = e.get("default");
	const bool wanted = !file.empty() && !s.videoTaken && options::frontend().videos && motion() != MotionOff
			&& s.since >= e.number("delay", 1.5f) && video::available();
	if (wanted)
	{
		s.videoTaken = true;
		const int iterations = (int)e.number("iterationCount", 0);
		video::play(file, iterations <= 0, options::frontend().videoVolume > 0 && e.flag("audio", true));
		int vw = 0, vh = 0;
		void *pic = video::picture(vw, vh);
		const bool showImageAfter = video::ended() && e.get("onIterationsDone", "nothing") == "image";
		if (pic != nullptr && vw > 0 && vh > 0 && !showImageAfter)
		{
			float w = 0, h = 0;
			ImVec2 uv0, uv1;
			if (fitPicture(e, (float)vw, (float)vh, w, h, uv0, uv1))
			{
				const Box box = place(e, w, h);
				const float opacity = e.number("opacity", 1.f);
				const ImU32 colour = fade(e.colour("color", White), opacity);
				const float rounding = e.number("imageCornerRadius", 0) * W();
				// Black bars where the theme's box is wider or taller than the video.
				float bw = 0, bh = 0;
				if (e.flag("pillarboxes", true) && !e.has("cropSize") && sizeOf(e, "maxSize", bw, bh) && bw > 0 && bh > 0)
				{
					float ox = 0, oy = 0;
					e.pair("origin", ox, oy);
					const ImVec2 a(box.x - (bw - w) * ox, box.y - (bh - h) * oy);
					draw()->AddRectFilled(a, ImVec2(a.x + bw, a.y + bh), fade(IM_COL32(0, 0, 0, 255), opacity), rounding);
				}
				draw()->AddImageRounded((ImTextureID)pic, box.a(), box.b(), uv0, uv1, colour, rounding);
				return;
			}
		}
	}
	imageElement(e, s);
}

void imageElement(const esde::Element& e, const Shown& s)
{
	const std::string file = pictureFile(e, s);
	if (file.empty())
		return;
	const bool tile = e.flag("tile", false);
	const Image img = image(file, tile ? 256 : wantedSide(e));
	if (img.id == nullptr)
		return;
	const float opacity = e.number("opacity", 1.f);
	const ImU32 colour = fade(e.colour("color", White), opacity);
	const ImU32 colourEnd = fade(e.colour("colorEnd", e.colour("color", White)), opacity);
	const float rounding = e.number(e.type == "video" ? "imageCornerRadius" : "cornerRadius", 0) * W();
	if (tile)
	{
		float w = 0, h = 0;
		if (!sizeOf(e, "size", w, h))
		{
			w = W();
			h = H();
		}
		if (w <= 0)
			w = W();
		if (h <= 0)
			h = H();
		const Box box = place(e, w, h);
		float tw = 0, th = 0;
		if (!sizeOf(e, "tileSize", tw, th) || tw <= 0 || th <= 0)
		{
			tw = (float)img.width * H() / 1080.f;
			th = (float)img.height * H() / 1080.f;
		}
		// A tile as large as the box, or one tiny tile (a spacer): one quad.
		if (tw >= box.w || th >= box.h || (box.w / tw) * (box.h / th) > 4096)
		{
			picture(img, box.a(), box.b(), ImVec2(0, 0), ImVec2(1, 1), colour, colourEnd, horizontalGradient(e),
					rounding, 0);
			return;
		}
		draw()->PushClipRect(box.a(), box.b(), true);
		for (float y = box.y; y < box.y + box.h; y += th)
			for (float x = box.x; x < box.x + box.w; x += tw)
				picture(img, ImVec2(x, y), ImVec2(x + tw, y + th), ImVec2(0, 0), ImVec2(1, 1), colour, colourEnd,
						horizontalGradient(e), 0, 0);
		draw()->PopClipRect();
		return;
	}
	float w = 0, h = 0;
	ImVec2 uv0, uv1;
	if (!fitPicture(e, (float)img.width, (float)img.height, w, h, uv0, uv1))
		return;
	if (e.flag("flipHorizontal", false))
		std::swap(uv0.x, uv1.x);
	if (e.flag("flipVertical", false))
		std::swap(uv0.y, uv1.y);
	const Box box = place(e, w, h);
	const float degrees = e.number("rotation", 0);
	if (degrees != 0)
	{
		// About rotationOrigin (its middle when not given).
		float rx = 0.5f, ry = 0.5f;
		e.pair("rotationOrigin", rx, ry);
		const ImVec2 pivot(box.x + rx * w, box.y + ry * h);
		const float rad = degrees * 3.14159265f / 180.f;
		const float cx = box.x + w * 0.5f - pivot.x, cy = box.y + h * 0.5f - pivot.y;
		const ImVec2 centre(pivot.x + cx * std::cos(rad) - cy * std::sin(rad), pivot.y + cx * std::sin(rad) + cy * std::cos(rad));
		turnedPicture(imagePadded(file, wantedSide(e)), centre, w, h, uv0, uv1, colour, degrees);
		return;
	}
	picture(img, box.a(), box.b(), uv0, uv1, colour, colourEnd, horizontalGradient(e), rounding, 0);
}

void ratingElement(const esde::Element& e, const Shown&)
{
	// The game database gives no rating: nothing is drawn rather than a
	// rating of none.
	(void)e;
}

void badgesElement(const esde::Element& e, const Shown& s)
{
	if (s.game == nullptr || !library::favourite(s.game->path))
		return;
	float w = 0, h = 0;
	if (!sizeOf(e, "size", w, h))
		return;
	const int lines = std::max((int)e.number("lines", 2), 1);
	const float side = h > 0 ? h / (float)lines : w * 0.2f;
	const Box box = place(e, w > 0 ? w : side, h > 0 ? h : side);
	float x = box.x;
	if (e.get("horizontalAlignment", "left") == "right")
		x = box.x + box.w - side;
	const ImVec2 a(x, box.y), b(x + side, box.y + side);
	const std::string& icon = e.get("customBadgeIcon:favorite");
	if (!icon.empty())
	{
		const Image img = image(icon, (int)side);
		if (img.id != nullptr)
		{
			draw()->AddImage((ImTextureID)img.id, a, b, ImVec2(0, 0), ImVec2(1, 1), e.colour("badgeIconColor", White));
			return;
		}
	}
	const float em = side * 0.7f;
	fontText(-1, ImVec2(a.x + (side - fontTextWidth(-1, icon::Heart, em)) * 0.5f, a.y), IM_COL32(255, 120, 140, 255),
			icon::Heart, em, side);
}

void datetimeElement(const esde::Element& e, const Shown& s)
{
	const std::string key = e.get("metadata", "releasedate");
	std::string value;
	if (s.game != nullptr && key == "releasedate" && s.info != nullptr && s.info->year > 0)
		value = dateText(e.get("format", "%Y-%m-%d"), s.info->year, s.info->month, 0);
	else if (s.game != nullptr && key == "lastplayed")
	{
		const history::Entry played = history::get(s.game->path);
		if (played.lastPlayed > 0)
		{
			if (e.flag("displayRelative", false))
				value = agoText(played.lastPlayed);
			else
			{
				// The console's own time zone (its libc's is UTC).
				const time_t when = (time_t)played.lastPlayed + localTimeOffset();
				struct tm local{};
				gmtime_r(&when, &local);
				char buffer[128] = "";
				strftime(buffer, sizeof(buffer), e.get("format", "%Y-%m-%d").c_str(), &local);
				value = buffer;
			}
		}
	}
	if (value.empty())
		value = e.get("defaultValue", key == "lastplayed" ? "Never" : "Unknown");
	textBox(e, value, 1.f);
}

void clockElement(const esde::Element& e)
{
	if (!e.has("pos"))
		return;
	const time_t t = time(nullptr) + localTimeOffset();
	struct tm local{};
	gmtime_r(&t, &local);
	char buffer[64] = "";
	strftime(buffer, sizeof(buffer), e.get("format", "%H:%M").c_str(), &local);
	TextStyle st = textStyle(e, 0.035f, "color", White);
	const float tw = fontTextWidth(st.font, buffer, st.em);
	float hl = 0, hr = 0, vt = 0, vb = 0;
	e.pair("backgroundHorizontalPadding", hl, hr);
	e.pair("backgroundVerticalPadding", vt, vb);
	const float w = tw + (hl + hr) * W(), h = st.em * 1.25f + (vt + vb) * H();
	const Box box = place(e, w, h);
	if (e.has("backgroundColor"))
		fill(box.a(), box.b(), e.colour("backgroundColor", 0), e.colour("backgroundColor", 0), false,
				e.number("backgroundCornerRadius", 0) * W());
	fontText(st.font, ImVec2(box.x + hl * W(), box.y + vt * H()), st.colour, buffer, st.em, st.em * 1.25f);
}

// ------------------------------------------------------------ the list's items

struct ItemLook
{
	std::string kinds, defaultImage;
	std::string fit = "contain";	// contain, fill, cover
	float relative = 1;				// the picture's part of the item
	ImU32 tint = White, tintEnd = White;
	bool horizontal = true;
	float rounding = 0;
	TextStyle text;
	ImU32 textBackground = 0;
};

// A game's picture in an item's box, or its name when it has none.
void drawItem(const ItemLook& look, const library::Game& game, ImVec2 a, ImVec2 b, float opacity, bool selected,
		ImU32 selectedTextColour)
{
	const float bw = b.x - a.x, bh = b.y - a.y;
	const float iw = bw * look.relative, ih = bh * look.relative;
	const ImVec2 ia(a.x + (bw - iw) * 0.5f, a.y + (bh - ih) * 0.5f), ib(ia.x + iw, ia.y + ih);
	std::string file = mediaFirst(game, look.kinds);
	if (file.empty())
		file = look.defaultImage;
	const Image img = file.empty() ? Image() : image(file, (int)std::max(iw, ih));
	if (img.id != nullptr && img.width > 0 && img.height > 0)
	{
		ImVec2 p0 = ia, p1 = ib, uv0(0, 0), uv1(1, 1);
		const float ar = (float)img.width / (float)img.height, boxAr = iw / std::max(ih, 1.f);
		if (look.fit == "contain")
		{
			if (ar > boxAr)
			{
				const float h = iw / ar;
				p0.y = ia.y + (ih - h) * 0.5f;
				p1.y = p0.y + h;
			}
			else
			{
				const float w = ih * ar;
				p0.x = ia.x + (iw - w) * 0.5f;
				p1.x = p0.x + w;
			}
		}
		else if (look.fit == "cover")
		{
			if (ar > boxAr)
			{
				const float u = boxAr / ar;
				uv0.x = (1 - u) * 0.5f;
				uv1.x = uv0.x + u;
			}
			else
			{
				const float v = ar / boxAr;
				uv0.y = (1 - v) * 0.5f;
				uv1.y = uv0.y + v;
			}
		}
		picture(img, p0, p1, uv0, uv1, fade(look.tint, opacity), fade(look.tintEnd, opacity), look.horizontal,
				look.rounding, 0);
		return;
	}
	// No picture: the name, on its background.
	if ((look.textBackground >> 24) != 0)
		draw()->AddRectFilled(ia, ib, fade(look.textBackground, opacity), look.rounding);
	const TextStyle& t = look.text;
	const std::string name = letterCase(game.name, t.letters);
	const std::vector<std::string>& lines = wrapCached(t.font, name, t.em, iw * 0.92f);
	const size_t most = std::max<size_t>(1, (size_t)(ih / std::max(t.line, 1.f)));
	const size_t count = std::min(lines.size(), most);
	float y = ia.y + (ih - t.line * (float)count) * 0.5f;
	TextStyle centred = t;
	centred.align = "center";
	for (size_t i = 0; i < count; i++, y += t.line)
		line(centred, ia.x, y, iw, i + 1 == count && count < lines.size() ? fontFit(t.font, lines[i] + " " + lines[i + 1], t.em, iw * 0.92f)
				: lines[i], fade(selected ? selectedTextColour : t.colour, opacity));
}

ItemLook itemLook(const esde::Element& e)
{
	ItemLook look;
	look.kinds = e.get("imageType", "marquee");
	look.defaultImage = e.get("defaultImage");
	look.fit = e.get("imageFit", "contain");
	look.relative = std::clamp(e.number("imageRelativeScale", 1.f), 0.1f, 1.f);
	look.tint = e.colour("imageColor", White);
	look.tintEnd = e.colour("imageColorEnd", look.tint);
	look.horizontal = horizontalGradient(e, "imageGradientType");
	look.rounding = e.number("imageCornerRadius", 0) * W();
	look.text = textStyle(e, 0.045f, "textColor", IM_COL32(0, 0, 0, 255));
	look.textBackground = e.colour("textBackgroundColor", IM_COL32(255, 255, 255, 64));
	return look;
}

// ------------------------------------------------------------------ the lists

struct ListState
{
	float shown = 0;		// the cursor as drawn, moving towards it
	int generation = -1;
	int count = -1;
};
ListState list;

void textlistElement(const esde::Element& e, const EsdeGames& games, int& cursor, bool active)
{
	float w = W(), h = H() * 0.8f;
	sizeOf(e, "size", w, h);
	if (w <= 0)
		w = W();
	if (h <= 0)
		h = H() * 0.8f;
	const Box box = place(e, w, h);
	TextStyle t = textStyle(e, 0.045f, "primaryColor", IM_COL32(0x0f, 0x0f, 0x0f, 255));
	const int count = games.count;
	const int visible = std::max((int)(box.h / std::max(t.line, 1.f)), 1);
	if (active)
	{
		if (nav(Down))
			cursor = cursor + 1 < count ? cursor + 1 : (hit(Down) ? 0 : cursor);
		if (nav(Up))
			cursor = cursor > 0 ? cursor - 1 : (hit(Up) ? count - 1 : cursor);
		if (nav(Right))
			cursor = std::min(cursor + visible, count - 1);
		if (nav(Left))
			cursor = std::max(cursor - visible, 0);
	}
	// The cursor's line in the middle, but at the ends.
	float first = (float)cursor - (float)(visible - 1) * 0.5f;
	first = std::clamp(first, 0.f, std::max((float)(count - visible), 0.f));
	list.shown = list.generation != state.generation ? first : approach(list.shown, first, 20.f);
	list.generation = state.generation;
	const float scroll = list.shown;
	const ImU32 selected = e.colour("selectedColor", t.colour);
	const ImU32 selectedBack = e.colour("selectedBackgroundColor", 0);
	float sbx = 0, sby = 0;
	e.pair("selectedBackgroundMargins", sbx, sby);
	const float sbr = e.number("selectedBackgroundCornerRadius", 0) * W();
	const float margin = e.number("horizontalMargin", 0) * W();
	draw()->PushClipRect(box.a(), box.b(), true);
	// The selector: a bar (or a picture) behind the line under the cursor.
	{
		const float y = box.y + ((float)cursor - scroll) * t.line;
		float selH = t.line;
		float sh = 0;
		if (e.pair("selectorHeight", sh, sh) && sh > 0)
			selH = sh * H();
		float sw = box.w;
		float swx = 0;
		if (e.pair("selectorWidth", swx, swx) && swx > 0)
			sw = swx * W();
		float ox = 0, oy = 0;
		e.pair("selectorHorizontalOffset", ox, ox);
		e.pair("selectorVerticalOffset", oy, oy);
		const ImVec2 a(box.x + ox * W(), y + (t.line - selH) * 0.5f + oy * H()), b(a.x + sw, a.y + selH);
		const std::string& selectorImage = e.get("selectorImagePath");
		const Image img = selectorImage.empty() ? Image() : image(selectorImage, (int)sw);
		const ImU32 sc = e.colour("selectorColor", IM_COL32(0x33, 0x33, 0x33, 255));
		if (img.id != nullptr)
			picture(img, a, b, ImVec2(0, 0), ImVec2(1, 1), sc, e.colour("selectorColorEnd", sc),
					horizontalGradient(e, "selectorGradientType"), 0, 0);
		else
			fill(a, b, sc, e.colour("selectorColorEnd", sc), horizontalGradient(e, "selectorGradientType"), 0);
	}
	const int from = std::max((int)scroll - 1, 0), to = std::min((int)scroll + visible + 2, count);
	for (int i = from; i < to; i++)
	{
		const library::Game& game = games.game(i);
		const float y = box.y + ((float)i - scroll) * t.line;
		const bool isCursor = i == cursor;
		std::string name = letterCase(game.name, t.letters);
		const float room = box.w - margin * 2;
		name = fontFit(t.font, name, t.em, room);
		if (isCursor && (selectedBack >> 24) != 0)
		{
			const float tw = fontTextWidth(t.font, name, t.em);
			const float left = t.align == "center" ? box.x + margin + (room - tw) * 0.5f
					: t.align == "right" ? box.x + box.w - margin - tw : box.x + margin;
			draw()->AddRectFilled(ImVec2(left - sbx * W(), y + (t.line - t.em * 1.3f) * 0.5f - sby * H()),
					ImVec2(left + tw + sbx * W(), y + (t.line + t.em * 1.3f) * 0.5f + sby * H()), selectedBack, sbr);
		}
		line(t, box.x + margin, y, room, name, isCursor ? selected : t.colour);
	}
	draw()->PopClipRect();
}

void gridElement(const esde::Element& e, const EsdeGames& games, int& cursor, bool active)
{
	float w = W(), h = H() * 0.8f;
	sizeOf(e, "size", w, h);
	if (w <= 0)
		w = W();
	if (h <= 0)
		h = H() * 0.8f;
	const Box box = place(e, w, h);
	float iw = 0.15f * W(), ih = 0.25f * H();
	{
		float x = 0, y = 0;
		if (e.pair("itemSize", x, y))
		{
			iw = x > 0 ? x * W() : (y > 0 ? y * H() : iw);
			ih = y > 0 ? y * H() : iw;
			if (x < 0)
				iw = ih;
		}
	}
	float sx = -1, sy = -1;
	const bool spaced = e.pair("itemSpacing", sx, sy);
	int columns = 1;
	float gapX = 0, gapY = 0;
	if (spaced && sx >= 0)
	{
		gapX = sx * W();
		gapY = (sy >= 0 ? sy : sx) * H();
		columns = std::max((int)((box.w + gapX) / (iw + gapX)), 1);
	}
	else
	{
		columns = std::max((int)(box.w / (iw * 1.08f)), 1);
		gapX = columns > 1 ? (box.w - iw * columns) / (float)(columns - 1) : 0;
		gapY = gapX * H() / W() * 1.6f;
		if (gapY <= 0)
			gapY = ih * 0.08f;
	}
	const int count = games.count;
	if (active)
	{
		if (nav(Right) && cursor + 1 < count)
			cursor++;
		if (nav(Left) && cursor > 0)
			cursor--;
		if (nav(Down))
			cursor = cursor + columns < count ? cursor + columns
					: (cursor / columns < (count - 1) / columns ? count - 1 : cursor);
		if (nav(Up) && cursor - columns >= 0)
			cursor -= columns;
	}
	const float rowH = ih + gapY;
	const int rows = (count + columns - 1) / columns;
	const int visibleRows = std::max((int)((box.h + gapY) / rowH), 1);
	const int row = cursor / columns;
	float first = (float)row - (float)(visibleRows - 1) * 0.5f;
	first = std::clamp(std::floor(first + 0.5f), 0.f, std::max((float)(rows - visibleRows), 0.f));
	list.shown = list.generation != state.generation ? first : approach(list.shown, first, 14.f);
	list.generation = state.generation;
	const float scroll = list.shown;
	const float scale = std::max(e.number("itemScale", 1.05f), 0.5f);
	const float unfocused = std::clamp(e.number("unfocusedItemOpacity", 1.f), 0.f, 1.f);
	const bool inwards = e.flag("scaleInwards", false);
	const ItemLook look = itemLook(e);
	const ImU32 back = e.colour("backgroundColor", 0);
	const std::string& backImage = e.get("backgroundImage");
	const std::string& selectorImage = e.get("selectorImage");
	const ImU32 selectorColour = e.colour("selectorColor", 0);
	const float selectorScale = e.number("selectorRelativeScale", 1.f);
	const bool selectorOnTop = e.get("selectorLayer", "top") == "top";
	const ImU32 selectedText = e.colour("textSelectedColor", look.text.colour);
	// Rows fully out of the box are left out; the item under the cursor is drawn last.
	draw()->PushClipRect(ImVec2(box.x - iw * 0.2f, box.y - ih * 0.2f), ImVec2(box.x + box.w + iw * 0.2f,
			box.y + box.h + ih * 0.2f), true);
	for (int pass = 0; pass < 2; pass++)
		for (int r = std::max((int)scroll - 1, 0); r < std::min((int)scroll + visibleRows + 2, rows); r++)
			for (int c = 0; c < columns; c++)
			{
				const int i = r * columns + c;
				if (i >= count)
					break;
				const bool isCursor = i == cursor;
				if (isCursor != (pass == 1))
					continue;
				const float cx = box.x + (float)c * (iw + gapX) + iw * 0.5f;
				const float cy = box.y + ((float)r - scroll) * rowH + ih * 0.5f;
				const float k = isCursor ? scale : 1.f;
				ImVec2 a(cx - iw * 0.5f * k, cy - ih * 0.5f * k), b(cx + iw * 0.5f * k, cy + ih * 0.5f * k);
				// Made larger towards the grid's middle, not past its edges.
				if (inwards && isCursor)
				{
					const float rowTop = box.y + ((float)r - scroll) * rowH, rowBottom = rowTop + ih;
					const float dx = a.x < box.x ? box.x - a.x : b.x > box.x + box.w ? box.x + box.w - b.x : 0.f;
					const float dy = a.y < std::max(box.y, rowTop - gapY) && rowTop >= box.y - 1 ? rowTop - a.y
							: b.y > box.y + box.h && rowBottom <= box.y + box.h + 1 ? box.y + box.h - b.y : 0.f;
					a.x += dx;
					b.x += dx;
					a.y += dy;
					b.y += dy;
				}
				const float opacity = isCursor ? 1.f : unfocused;
				const auto selector = [&] {
					if (!isCursor)
						return;
					const float sw = iw * k * selectorScale, sh = ih * k * selectorScale;
					const ImVec2 sa(cx - sw * 0.5f, cy - sh * 0.5f), sb(cx + sw * 0.5f, cy + sh * 0.5f);
					const Image si = selectorImage.empty() ? Image() : image(selectorImage, (int)std::max(sw, sh));
					if (si.id != nullptr)
						draw()->AddImage((ImTextureID)si.id, sa, sb, ImVec2(0, 0), ImVec2(1, 1),
								selectorColour != 0 ? selectorColour : White);
					else if ((selectorColour >> 24) != 0)
						draw()->AddRectFilled(sa, sb, selectorColour, look.rounding);
				};
				if (!backImage.empty())
				{
					const Image bi = image(backImage, (int)std::max(iw, ih));
					if (bi.id != nullptr)
						draw()->AddImage((ImTextureID)bi.id, a, b, ImVec2(0, 0), ImVec2(1, 1), fade(back != 0 ? back : White, opacity));
				}
				else if ((back >> 24) != 0)
					draw()->AddRectFilled(a, b, fade(back, opacity), look.rounding);
				if (!selectorOnTop)
					selector();
				drawItem(look, games.game(i), a, b, opacity, isCursor, selectedText);
				if (selectorOnTop)
					selector();
			}
	draw()->PopClipRect();
}

void carouselElement(const esde::Element& e, const EsdeGames& games, int& cursor, bool active)
{
	const std::string type = e.get("type", "horizontal");
	const bool vertical = type == "vertical" || type == "verticalWheel";
	const bool wheel = type == "horizontalWheel" || type == "verticalWheel";
	float w = W(), h = H() * 0.2325f;
	sizeOf(e, "size", w, h);
	if (w <= 0)
		w = W();
	if (h <= 0)
		h = H() * 0.2325f;
	const Box box = place(e, w, h);
	const int count = games.count;
	const int maxItems = std::max((int)e.number("maxItemCount", 3), 1);
	if (active)
	{
		const uint32_t next = vertical ? Down : Right, previous = vertical ? Up : Left;
		const uint32_t pageNext = vertical ? Right : Down, pagePrevious = vertical ? Left : Up;
		if (nav(next))
			cursor = cursor + 1 < count ? cursor + 1 : (hit(next) ? 0 : cursor);
		if (nav(previous))
			cursor = cursor > 0 ? cursor - 1 : (hit(previous) ? count - 1 : cursor);
		if (nav(pageNext))
			cursor = std::min(cursor + maxItems, count - 1);
		if (nav(pagePrevious))
			cursor = std::max(cursor - maxItems, 0);
	}
	list.shown = list.generation != state.generation || list.count != count ? (float)cursor
			: approach(list.shown, (float)cursor, 12.f);
	if (std::fabs(list.shown - (float)cursor) > (float)count * 0.5f)
		list.shown = (float)cursor;
	list.generation = state.generation;
	list.count = count;
	// The background bar.
	{
		const ImU32 c = e.colour("color", IM_COL32(255, 255, 255, 0xd8));
		fill(box.a(), box.b(), c, e.colour("colorEnd", c), horizontalGradient(e), 0);
	}
	float iw = 0.25f * W(), ih = 0.155f * H();
	{
		float x = 0, y = 0;
		if (e.pair("itemSize", x, y))
		{
			iw = x * W();
			ih = y * H();
		}
	}
	const float scale = std::max(e.number("itemScale", 1.2f), 0.2f);
	const float unfocused = std::clamp(e.number("unfocusedItemOpacity", 0.5f), 0.f, 1.f);
	const ItemLook look = itemLook(e);
	const ImU32 selectedText = e.colour("textSelectedColor", look.text.colour);
	// Where the item under the cursor is: the middle, or the side the theme puts it.
	float cx = box.x + box.w * 0.5f, cy = box.y + box.h * 0.5f;
	const std::string halign = e.get("itemHorizontalAlignment", "center"), valign = e.get("itemVerticalAlignment", "center");
	if (!vertical && !wheel)
	{
		if (halign == "left")
			cx = box.x + iw * scale * 0.5f;
		else if (halign == "right")
			cx = box.x + box.w - iw * scale * 0.5f;
		if (valign == "top")
			cy = box.y + ih * scale * 0.5f;
		else if (valign == "bottom")
			cy = box.y + box.h - ih * scale * 0.5f;
	}
	else if (vertical && !wheel)
	{
		if (valign == "top")
			cy = box.y + ih * scale * 0.5f;
		else if (valign == "bottom")
			cy = box.y + box.h - ih * scale * 0.5f;
		if (halign == "left")
			cx = box.x + iw * scale * 0.5f;
		else if (halign == "right")
			cx = box.x + box.w - iw * scale * 0.5f;
	}
	float ox = 0, oy = 0;
	e.pair("horizontalOffset", ox, ox);
	e.pair("verticalOffset", oy, oy);
	cx += ox * box.w;
	cy += oy * box.h;
	const float step = vertical ? box.h / (float)maxItems : box.w / (float)maxItems;
	const float rotationStep = e.number("itemRotation", 7.5f);
	float rox = -3.f, roy = 0.5f;
	e.pair("itemRotationOrigin", rox, roy);
	const int reach = wheel ? std::max((int)(180.f / std::max(std::fabs(rotationStep), 1.f)), 3) : maxItems / 2 + 2;
	draw()->PushClipRect(box.a(), box.b(), true);
	for (int pass = 0; pass < 2; pass++)
		for (int d = -reach; d <= reach; d++)
		{
			const int i = (int)std::floor(list.shown + 0.5f) + d;
			if (i < 0 || i >= count)
				continue;
			const bool isCursor = i == cursor;
			if (isCursor != (pass == 1))
				continue;
			const float offset = (float)i - list.shown;
			const float nearness = std::clamp(1.f - std::fabs(offset), 0.f, 1.f);
			const float k = 1.f + (scale - 1.f) * nearness;
			const float opacity = unfocused + (1.f - unfocused) * nearness;
			float x = cx, y = cy, angle = 0;
			if (wheel)
			{
				// Turned about a point beside the item, a step of angle each.
				angle = offset * rotationStep;
				const float rad = angle * 3.14159265f / 180.f;
				const float px0 = (rox - 0.5f) * iw, py0 = (roy - 0.5f) * ih;
				const float rx = -px0, ry = -py0;
				x = cx + px0 + rx * std::cos(rad) - ry * std::sin(rad);
				y = cy + py0 + rx * std::sin(rad) + ry * std::cos(rad);
			}
			else if (vertical)
				y = cy + offset * step;
			else
				x = cx + offset * step;
			const ImVec2 a(x - iw * 0.5f * k, y - ih * 0.5f * k), b(x + iw * 0.5f * k, y + ih * 0.5f * k);
			if (angle != 0)
			{
				std::string file = mediaFirst(games.game(i), look.kinds);
				if (file.empty())
					file = look.defaultImage;
				const Image img = file.empty() ? Image() : imagePadded(file, (int)std::max(iw, ih));
				if (img.id != nullptr && img.width > PictureBorder * 2)
				{
					// Fitted, then turned about its middle.
					const float ar = (float)(img.width - PictureBorder * 2) / (float)(img.height - PictureBorder * 2);
					const float bw = b.x - a.x, bh = b.y - a.y;
					float fw = bw, fh = bw / ar;
					if (fh > bh)
					{
						fh = bh;
						fw = bh * ar;
					}
					turnedPicture(img, ImVec2(x, y), fw, fh, ImVec2(0, 0), ImVec2(1, 1), fade(look.tint, opacity), angle);
					continue;
				}
			}
			drawItem(look, games.game(i), a, b, opacity, isCursor, selectedText);
		}
	draw()->PopClipRect();
}

// The library's hints in the theme's help style.
void helpElement(const esde::Element& e, const std::vector<Hint>& hints)
{
	if (hints.empty())
		return;
	TextStyle t = textStyle(e, 0.035f, "textColor", IM_COL32(0x77, 0x77, 0x77, 255));
	// The whole entry (its symbol and its text) is scaled by entryRelativeScale.
	t.em *= std::clamp(e.number("entryRelativeScale", 1.f), 0.3f, 2.f);
	const float iconUnits = toUnits(t.em * 1.3f);
	const float spacing = e.number("entrySpacing", 0.00833f) * W();
	const float iconGap = e.number("iconTextSpacing", 0.00416f) * W();
	float hl = 0, hr = 0, vt = 0, vb = 0;
	e.pair("backgroundHorizontalPadding", hl, hr);
	e.pair("backgroundVerticalPadding", vt, vb);
	// The theme's "entries" (ES-DE's names for the buttons: a south, b east,
	// x west, y north, start, back, lr the shoulders, ltrt the triggers):
	// only those of the library's hints are shown.
	std::vector<Hint> shownHints;
	{
		std::set<std::string> wanted;
		std::string current;
		for (const char c : e.get("entries", "all") + ",")
		{
			if (c == ',')
			{
				current = trim(current);
				if (!current.empty())
					wanted.insert(lowerText(current));
				current.clear();
			}
			else
				current += c;
		}
		const auto nameOf = [](uint32_t button) -> std::string {
			if (button == Cross)
				return "a";
			if (button == Circle)
				return "b";
			if (button == Square)
				return "x";
			if (button == Triangle)
				return "y";
			if (button == Options)
				return "start";
			if (button == (TouchLeft | TouchRight) || button == TouchLeft || button == TouchRight)
				return "back";
			if (button == (L1 | R1) || button == L1 || button == R1)
				return "lr";
			if (button == (L2 | R2) || button == L2 || button == R2)
				return "ltrt";
			return "";
		};
		for (const Hint& h : hints)
		{
			const std::string name = nameOf(h.button);
			if (wanted.count("all") != 0 || wanted.empty() || wanted.count(name) != 0
					|| (name == "ltrt" && (wanted.count("lt") != 0 || wanted.count("rt") != 0))
					|| (name == "lr" && (wanted.count("l") != 0 || wanted.count("r") != 0)))
				shownHints.push_back(h);
		}
		if (shownHints.empty())
			return;
	}
	std::vector<std::string> labels;
	std::vector<float> glyphs;
	float total = 0;
	for (const Hint& h : shownHints)
	{
		labels.push_back(letterCase(h.label, "uppercase"));
		glyphs.push_back(glyphWidth(iconUnits, h.button));
		total += glyphs.back() + iconGap + fontTextWidth(t.font, labels.back(), t.em) + spacing;
	}
	total -= spacing;
	const float rowH = std::max(px(iconUnits), t.em * 1.3f);
	float px0 = 0.012f, py0 = 0.9515f;
	e.pair("pos", px0, py0);
	float ox = 0, oy = 0;
	e.pair("origin", ox, oy);
	const float w = total + (hl + hr) * W(), h = rowH + (vt + vb) * H();
	const Box box{ px0 * W() - ox * w, py0 * H() - oy * h, w, h };
	if (e.has("backgroundColor"))
		fill(box.a(), box.b(), e.colour("backgroundColor", 0), e.colour("backgroundColor", 0), false,
				e.number("backgroundCornerRadius", 0) * W());
	float x = box.x + hl * W();
	const float y = box.y + vt * H();
	for (size_t i = 0; i < shownHints.size(); i++)
	{
		buttonGlyph(ImVec2(x + glyphs[i] * 0.5f, y + rowH * 0.5f), iconUnits, shownHints[i].button);
		x += glyphs[i] + iconGap;
		fontText(t.font, ImVec2(x, y), t.colour, labels[i], t.em, rowH);
		x += fontTextWidth(t.font, labels[i], t.em) + spacing;
	}
}

// A view with no list of games of its own: a plain one at the left.
const esde::Element& fallbackList()
{
	static esde::Element e = [] {
		esde::Element made;
		made.type = "textlist";
		made.name = "gamelist";
		made.props["pos"] = "0.05 0.15";
		made.props["size"] = "0.45 0.7";
		made.props["primaryColor"] = "cccccc";
		made.props["selectedColor"] = "ffffff";
		made.props["selectorColor"] = "ffffff22";
		made.props["fontSize"] = "0.035";
		return made;
	}();
	return e;
}

}

// ------------------------------------------------------------ the library's

std::vector<std::string> esdeViewNames()
{
	scan();
	std::vector<std::string> names;
	for (const esde::Theme& t : themes)
		if (t.valid)
			names.push_back("ES-DE: " + t.name);
	return names;
}

int esdeViewIndex()
{
	readSettings();
	scan();
	int k = 0;
	for (const esde::Theme& t : themes)
	{
		if (!t.valid)
			continue;
		if (t.id == settings.theme)
			return k;
		k++;
	}
	return -1;
}

void esdeChooseView(int index)
{
	readSettings();
	scan();
	int k = 0;
	for (const esde::Theme& t : themes)
	{
		if (!t.valid)
			continue;
		if (k++ == index)
		{
			settings.theme = t.id;
			saveSettings();
			wantReload = 1;
			return;
		}
	}
}

void esdeRescan()
{
	scanned = false;
	wantReload = 1;
}

bool esdeChosen()
{
	return options::frontend().view == EsdeView && themeById((readSettings(), settings.theme)) != nullptr;
}

bool esdeView(const EsdeGames& games, int& cursor, bool active, bool fresh, const std::vector<Hint>& hints)
{
	if (games.count <= 0)
		return false;
	const bool ok = ensureLoaded([&games](const std::string& kind) {
		// A variant shows a kind of picture: whether the first games have it.
		for (int i = 0; i < std::min(games.count, 40); i++)
			if (!mediaOf(games.game(i), kind, false).empty())
				return true;
		return false;
	});
	if (!ok)
		return false;
	if (fresh)
		list.generation = -1;
	cursor = std::clamp(cursor, 0, games.count - 1);
	// ES-DE's screen is black under the theme.
	draw()->AddRectFilled(ImVec2(0, 0), ImVec2(W(), H()), IM_COL32(0, 0, 0, 255));
	// The list moves first, so everything else shows the game it is on now.
	const esde::Element &primary = state.primary != nullptr ? *state.primary : fallbackList();
	Shown shownGame;
	shownGame.games = &games;
	// How long the cursor has stayed on the game (a video waits for it).
	static std::string focusedPath;
	static double focusedSince = 0;
	const auto focus = [&] {
		shownGame.cursor = cursor;
		shownGame.game = &games.game(cursor);
		shownGame.info = games.info ? games.info(cursor) : nullptr;
		if (shownGame.game->path != focusedPath)
		{
			focusedPath = shownGame.game->path;
			focusedSince = clock();
		}
		shownGame.since = clock() - focusedSince;
	};
	focus();
	bool listDrawn = false;
	const auto drawList = [&] {
		if (listDrawn)
			return;
		listDrawn = true;
		const int before = cursor;
		if (primary.type == "grid")
			gridElement(primary, games, cursor, active);
		else if (primary.type == "carousel")
			carouselElement(primary, games, cursor, active);
		else
			textlistElement(primary, games, cursor, active);
		if (cursor != before)
			focus();
	};
	for (const esde::Element *e : state.order)
	{
		if (e == state.primary)
		{
			drawList();
			continue;
		}
		const std::string& type = e->type;
		if (type == "image")
			imageElement(*e, shownGame);
		else if (type == "video")
			videoElement(*e, shownGame);
		else if (type == "text")
		{
			std::string value;
			if (e->has("metadata"))
				value = metadata(shownGame, e->get("metadata"));
			else if (e->has("systemdata"))
				value = systemData(shownGame, e->get("systemdata"));
			else
				value = e->get("text");
			if (value.empty())
				value = e->get("defaultValue");
			textBox(*e, value, 1.f);
		}
		else if (type == "datetime")
			datetimeElement(*e, shownGame);
		else if (type == "gamelistinfo")
			textBox(*e, format("%d / %d", cursor + 1, games.count), 1.f);
		else if (type == "rating")
			ratingElement(*e, shownGame);
		else if (type == "badges")
			badgesElement(*e, shownGame);
		else if (type == "clock")
			clockElement(*e);
	}
	drawList();
	if (state.help != nullptr)
		helpElement(*state.help, hints);
	return true;
}

std::string esdeProblem()
{
	return state.problem;
}

int libraryViewChoice()
{
	const int own = 3 + (int)flowNames().size();
	const int view = options::frontend().view;
	if (view == EsdeView)
	{
		const int k = esdeViewIndex();
		return k >= 0 ? own + k : 0;
	}
	return std::clamp(view, 0, own - 1);
}

void chooseLibraryView(int index)
{
	const int own = 3 + (int)flowNames().size();
	if (index >= own)
	{
		esdeChooseView(index - own);
		options::frontend().view = EsdeView;
	}
	else
		options::frontend().view = index;
	options::saveFrontend();
}

// --------------------------------------------------------------- the fetching

namespace
{

// ES-DE's own theme downloader reads this list (a git repository on GitLab).
constexpr const char *ThemesListUrl = "https://gitlab.com/es-de/themes/themes-list/-/raw/master/themes.json";
constexpr uint64_t MaxThemeZip = 3ull << 30;
constexpr uint64_t MaxThemeUnpacked = 4ull << 30;
constexpr size_t MaxThemeFiles = 60000;

struct Offer
{
	std::string name, author, url, repo;	// url: the git repository's
	bool known = false;						// from the list built in, not the list fetched
};

struct Fetcher
{
	std::mutex mutex;
	// The list.
	int listState = 0;		// 0 not asked, 1 asking, 2 read, 3 failed
	std::string listError;
	std::vector<Offer> offers;
	// One theme at a time.
	std::atomic<bool> busy{false}, stop{false}, rescan{false};
	std::string working;	// the theme's name
	std::string stage;		// "Downloading", "Unpacking"
	uint64_t done = 0, total = 0;
	std::string result;		// what came of the last one
	bool resultGood = false;
	std::string installedId;
};
Fetcher fetcher;

// Themes known to be ES-DE themes, offered when the list cannot be read.
const Offer knownThemes[] = {
	{ "Art Book Next", "Anthony Caccese", "https://github.com/anthonycaccese/art-book-next-es-de", "art-book-next-es-de", true },
	{ "Epic Noir (Revisited)", "Anthony Caccese", "https://github.com/anthonycaccese/epic-noir-revisited-es-de",
		"epic-noir-revisited-es-de", true },
};

bool localTest(const std::string& url);

std::string repoNameOf(std::string url)
{
	while (!url.empty() && url.back() == '/')
		url.pop_back();
	if (url.size() > 4 && url.compare(url.size() - 4, 4, ".git") == 0)
		url.resize(url.size() - 4);
	const size_t slash = url.rfind('/');
	return slash == std::string::npos ? url : url.substr(slash + 1);
}

// Any object in the list with a name and a repository's address is a theme
// (the list's other fields are not needed, nor relied on).
void collectOffers(const Json& j, std::vector<Offer>& out, std::set<std::string>& seen)
{
	if (j.type == Json::Object)
	{
		std::string url = j.string("url");
		if (url.empty())
			url = j.string("repository");
		const std::string name = j.string("name");
		if (!name.empty() && (url.find("github.com/") != std::string::npos || url.find("gitlab.com/") != std::string::npos
				|| url.find("codeberg.org/") != std::string::npos || localTest(url)))
		{
			Offer o;
			o.name = name;
			o.url = url;
			o.author = j.string("author");
			o.repo = j.string("reponame");
			if (o.repo.empty())
				o.repo = repoNameOf(url);
			if (seen.insert(lowerText(o.repo)).second)
				out.push_back(o);
			return;
		}
	}
	for (const Json& item : j.items)
		collectOffers(item, out, seen);
}

// The PC's test serves its own list and themes from this machine.
bool localTest(const std::string& url)
{
#if defined(SWANSTATION_HOST)
	return url.compare(0, 17, "http://127.0.0.1:") == 0;
#else
	(void)url;
	return false;
#endif
}

void fetchList()
{
	std::vector<uint8_t> answer;
	std::string listUrl = ThemesListUrl;
#if defined(SWANSTATION_HOST)
	if (const char *test = getenv("SWANSTATION_THEMES_LIST"))
		listUrl = test;
#endif
	// GitLab's raw file, then the same file through GitLab's interface.
	std::vector<std::string> urls = { listUrl,
		"https://gitlab.com/api/v4/projects/es-de%2Fthemes%2Fthemes-list/repository/files/themes.json/raw?ref=master" };
	int status = -1;
	for (const std::string& url : urls)
	{
		status = httpGet(url, answer, 30);
		diag::mark("themes: the list from %s: %s", url.substr(0, url.find('/', 8)).c_str(),
				status == 200 ? format("%zu bytes", answer.size()).c_str()
				: status < 0 ? "no answer (no network, or the server's certificate was refused: see the tls: lines)"
				: format("the server answered %d", status).c_str());
		if (status == 200)
			break;
	}
	std::vector<Offer> offers;
	std::string error;
	if (status == 200)
	{
		Json page;
		JsonReader((const char *)answer.data(), answer.size()).read(page);
		std::set<std::string> seen;
		collectOffers(page, offers, seen);
		if (offers.empty())
			error = "ES-DE's list of themes was read, but no theme was found in it";
	}
	else
		error = status < 0 ? "ES-DE's list of themes (on gitlab.com) could not be reached" : format("ES-DE's list of themes: gitlab.com answered %d", status);
	std::sort(offers.begin(), offers.end(), [](const Offer& a, const Offer& b) { return lowerText(a.name) < lowerText(b.name); });
	diag::mark("themes: the list: %s", error.empty() ? format("%d themes", (int)offers.size()).c_str() : error.c_str());
	std::lock_guard<std::mutex> lock(fetcher.mutex);
	if (offers.empty())
	{
		for (const Offer& o : knownThemes)
			offers.push_back(o);
		fetcher.listError = error;
		fetcher.listState = 3;
	}
	else
		fetcher.listState = 2;
	fetcher.offers = offers;
}

// The addresses a repository's files can be fetched from as a ZIP.
std::vector<std::string> zipUrls(const std::string& gitUrl)
{
	std::string base = gitUrl;
	while (!base.empty() && base.back() == '/')
		base.pop_back();
	if (base.size() > 4 && base.compare(base.size() - 4, 4, ".git") == 0)
		base.resize(base.size() - 4);
	const std::string repo = repoNameOf(base);
	if (localTest(base))
		return { base + ".zip" };
	if (base.find("github.com/") != std::string::npos)
	{
		// GitHub's archive (it sends the request on to codeload.github.com),
		// and codeload itself, for the default branch and the usual names.
		const std::string path = base.substr(base.find("github.com/") + 11);
		return { base + "/archive/HEAD.zip", "https://codeload.github.com/" + path + "/zip/HEAD",
			"https://codeload.github.com/" + path + "/zip/refs/heads/main",
			"https://codeload.github.com/" + path + "/zip/refs/heads/master" };
	}
	if (base.find("gitlab.com/") != std::string::npos)
		return { base + "/-/archive/main/" + repo + "-main.zip", base + "/-/archive/master/" + repo + "-master.zip" };
	if (base.find("codeberg.org/") != std::string::npos)
		return { base + "/archive/main.zip", base + "/archive/master.zip" };
	return {};
}

void removeTree(const std::string& path)
{
	struct stat st;
	if (lstat(path.c_str(), &st) != 0)
		return;
	if (S_ISDIR(st.st_mode))
	{
		if (DIR *dir = opendir(path.c_str()))
		{
			while (const dirent *entry = readdir(dir))
				if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
					removeTree(path + "/" + entry->d_name);
			closedir(dir);
		}
		rmdir(path.c_str());
	}
	else
		unlink(path.c_str());
}

size_t readZip(void *opaque, mz_uint64 offset, void *buffer, size_t bytes)
{
	const int fd = *(const int *)opaque;
	size_t got = 0;
	while (got < bytes)
	{
		const ssize_t n = pread(fd, (char *)buffer + got, bytes - got, (off_t)(offset + got));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	return got;
}

size_t writeZipEntry(void *opaque, mz_uint64, const void *buffer, size_t bytes)
{
	return fwrite(buffer, 1, bytes, (FILE *)opaque);
}

// What a theme needs; videos, sounds and the authors' working files are left out.
bool wanted(const std::string& path)
{
	const size_t dot = path.rfind('.');
	const std::string ext = dot == std::string::npos ? "" : lowerText(path.substr(dot));
	static const std::set<std::string> skip = { ".mp4", ".mkv", ".webm", ".avi", ".mov", ".wav", ".ogg", ".mp3",
		".flac", ".psd", ".xcf", ".afdesign", ".ai", ".sketch", ".kra", ".zip", ".7z" };
	return skip.count(ext) == 0;
}

void setStage(const char *stage, uint64_t done, uint64_t total)
{
	std::lock_guard<std::mutex> lock(fetcher.mutex);
	fetcher.stage = stage;
	fetcher.done = done;
	fetcher.total = total;
}

// Unpacks the ZIP into `into`, without the folder every entry is under.
std::string unpackTheme(const std::string& zipPath, const std::string& into, size_t& files)
{
	const int fd = open(zipPath.c_str(), O_RDONLY);
	if (fd < 0)
		return "the download cannot be read";
	struct stat st{};
	stat(zipPath.c_str(), &st);
	mz_zip_archive zip;
	mz_zip_zero_struct(&zip);
	zip.m_pRead = readZip;
	zip.m_pIO_opaque = (void *)&fd;
	if (!mz_zip_reader_init(&zip, (mz_uint64)st.st_size, 0))
	{
		close(fd);
		return format("the download is not a ZIP that can be read (%s)", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
	}
	std::string error;
	const mz_uint count = mz_zip_reader_get_num_files(&zip);
	uint64_t unpacked = 0, total = 0;
	std::string top;
	for (mz_uint i = 0; i < count && error.empty(); i++)
	{
		mz_zip_archive_file_stat entry;
		if (!mz_zip_reader_file_stat(&zip, i, &entry))
			error = "an entry of the ZIP cannot be read";
		const std::string name = entry.m_filename;
		const size_t slash = name.find('/');
		const std::string first = slash == std::string::npos ? name : name.substr(0, slash);
		if (top.empty())
			top = first;
		else if (first != top)
			top = "\x01";	// not all under one folder
		total += entry.m_uncomp_size;
	}
	if (count > MaxThemeFiles)
		error = "the theme has too many files";
	else if (total > MaxThemeUnpacked)
		error = "the theme is too large";
	const size_t strip = top.empty() || top == "\x01" ? 0 : top.size() + 1;
	files = 0;
	for (mz_uint i = 0; i < count && error.empty(); i++)
	{
		if (fetcher.stop)
		{
			error = "stopped";
			break;
		}
		mz_zip_archive_file_stat entry;
		if (!mz_zip_reader_file_stat(&zip, i, &entry))
			continue;
		std::string name = entry.m_filename;
		if (name.size() <= strip)
			continue;
		name = name.substr(strip);
		// Nothing that leads out of the folder, no hidden folders (.git, .github).
		if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos || name.find('\\') != std::string::npos
				|| name[0] == '.' || name.find("/.") != std::string::npos)
			continue;
		const std::string path = into + name;
		if (mz_zip_reader_is_file_a_directory(&zip, i))
		{
			makeDir(path);
			continue;
		}
		if (!wanted(name))
			continue;
		makeDir(path.substr(0, path.rfind('/') + 1));
		FILE *out = fopen(path.c_str(), "wb");
		if (out == nullptr)
		{
			error = "the themes folder cannot be written";
			break;
		}
		const bool ok = mz_zip_reader_extract_to_callback(&zip, i, writeZipEntry, out, 0) != MZ_FALSE;
		const bool closed = fclose(out) == 0;
		if (!ok || !closed)
		{
			error = "a file could not be unpacked (the storage may be full)";
			break;
		}
		files++;
		unpacked += entry.m_uncomp_size;
		if ((files & 31) == 0)
			setStage("Unpacking", unpacked, total);
	}
	mz_zip_reader_end(&zip);
	close(fd);
	return error;
}

void fetchTheme(Offer offer)
{
	const std::string themesDir = rootDir + "themes/";
	makeDir(themesDir);
	const std::string zipPath = themesDir + ".download.zip";
	const std::string staging = themesDir + ".unpacking/";
	removeTree(staging);
	unlink(zipPath.c_str());
	std::string error;
	int status = -1;
	for (const std::string& url : zipUrls(offer.url))
	{
		setStage("Downloading", 0, 0);
		diag::mark("themes: fetching %s", url.c_str());
		status = httpDownload(url, zipPath, MaxThemeZip, [](uint64_t done, uint64_t total) {
			setStage("Downloading", done, total);
			return !fetcher.stop.load();
		});
		diag::mark("themes: %s", status == 200 ? "downloaded" : status < 0 ? "no answer" : format("the server answered %d", status).c_str());
		if (status == 200 || fetcher.stop)
			break;
	}
	if (fetcher.stop)
		error = "stopped";
	else if (status != 200)
		error = status < 0 ? "the theme's files could not be reached (the boot log's themes: and tls: lines say more)"
				: format("the server answered %d", status);
	size_t files = 0;
	if (error.empty())
	{
		setStage("Unpacking", 0, 0);
		error = unpackTheme(zipPath, staging, files);
	}
	unlink(zipPath.c_str());
	std::string id = offer.repo.empty() ? repoNameOf(offer.url) : offer.repo;
	if (error.empty())
	{
		esde::Theme check;
		if (!esde::readTheme(staging, check))
			error = "what was fetched is not an ES-DE theme (" + check.problem + ")";
	}
	if (error.empty())
	{
		// The theme takes the place of the one fetched before.
		const std::string target = themesDir + id;
		removeTree(target + ".old");
		rename(target.c_str(), (target + ".old").c_str());
		if (rename(staging.substr(0, staging.size() - 1).c_str(), target.c_str()) != 0)
		{
			rename((target + ".old").c_str(), target.c_str());
			error = "the theme could not be put in the themes folder";
		}
		else
			removeTree(target + ".old");
	}
	removeTree(staging);
	diag::mark("themes: %s: %s", offer.name.c_str(), error.empty() ? format("installed, %zu files", files).c_str() : error.c_str());
	std::lock_guard<std::mutex> lock(fetcher.mutex);
	fetcher.resultGood = error.empty();
	fetcher.result = error.empty() ? offer.name + " is in the themes folder (" + format("%zu files", files) + ")."
			: offer.name + " was not fetched: " + error + ".";
	fetcher.installedId = error.empty() ? id : "";
	fetcher.rescan = error.empty();
	fetcher.working.clear();
	fetcher.busy = false;
	fetcher.stop = false;
}

std::string megabytes(uint64_t bytes)
{
	return format("%.0f MB", (double)bytes / (1024.0 * 1024.0));
}

} // namespace

// ------------------------------------------------------------------ the pages

void themesPage(Frame& f)
{
	if (f.fresh)
		esdeRescan();
	scan();
	readSettings();
	std::vector<Item> items;
	std::vector<const esde::Theme *> usable;
	for (const esde::Theme& t : themes)
		if (t.valid)
			usable.push_back(&t);
	const esde::Theme *theme = themeById(settings.theme);
	const bool inUse = options::frontend().view == EsdeView && theme != nullptr;
	{
		std::vector<std::string> names = { "None: the library's own views" };
		int current = 0;
		for (size_t i = 0; i < usable.size(); i++)
		{
			names.push_back(usable[i]->name);
			if (inUse && usable[i] == theme)
				current = (int)i + 1;
		}
		items.push_back(choice("Theme", current, names, "", [usable](int i) {
			if (i <= 0)
			{
				if (options::frontend().view == EsdeView)
					options::frontend().view = 0;
			}
			else
			{
				settings.theme = usable[(size_t)i - 1]->id;
				saveSettings();
				options::frontend().view = EsdeView;
				wantReload = 1;
			}
			options::saveFrontend();
		}));
	}
	if (theme != nullptr)
	{
		esde::Selection& sel = settings.chosen[theme->id];
		const auto pick = [&items, theme](const std::string& label, const std::vector<esde::Choice>& choices,
				std::string esde::Selection::*field, bool automatic) {
			std::vector<std::string> names;
			std::vector<std::string> values;
			if (automatic)
			{
				names.push_back("Automatic");
				values.push_back("");
			}
			for (const esde::Choice& c : choices)
				if (c.selectable)
				{
					names.push_back(c.label);
					values.push_back(c.name);
				}
			if (values.size() < 2)
				return;
			const std::string now = settings.chosen[theme->id].*field;
			int current = 0;
			for (size_t i = 0; i < values.size(); i++)
				if (values[i] == now)
					current = (int)i;
			// The theme's first is what an empty choice gives.
			if (now.empty() && !automatic)
			{
				const std::string& used = field == &esde::Selection::variant ? state.loaded.used.variant
						: field == &esde::Selection::colorScheme ? state.loaded.used.colorScheme
						: field == &esde::Selection::fontSize ? state.loaded.used.fontSize : state.loaded.used.language;
				for (size_t i = 0; i < values.size(); i++)
					if (values[i] == used && state.id == theme->id)
						current = (int)i;
			}
			const std::string id = theme->id;
			items.push_back(choice(label, current, names, "", [values, field, id](int i) {
				settings.chosen[id].*field = values[(size_t)std::clamp(i, 0, (int)values.size() - 1)];
				saveSettings();
				wantReload = 1;
			}));
		};
		std::vector<esde::Choice> variants(theme->variants.begin(), theme->variants.end());
		pick("Variant", variants, &esde::Selection::variant, false);
		pick("Colour scheme", theme->colorSchemes, &esde::Selection::colorScheme, false);
		pick("Font size", theme->fontSizes, &esde::Selection::fontSize, false);
		pick("Aspect ratio", theme->aspectRatios, &esde::Selection::aspectRatio, true);
		pick("Language", theme->languages, &esde::Selection::language, false);
		(void)sel;
	}
	// The same switches as Settings, Library: they are for every view.
	videoSettingItems(items, false);
	{
		Item get = action(icon::Download, "Get themes", "", [] { push(Page::ThemeDownload); });
		items.push_back(get);
	}
	items.push_back(fact("Pictures and videos found", media::status()));
	items.push_back(action(icon::Sync, "Look through the media folders again", "", [] { media::rescan(); }));
	items.push_back(action(icon::Sync, "Look in the themes folders again", "", [] { esdeRescan(); }));
	std::string problems;
	for (const esde::Theme& t : themes)
		if (!t.valid)
			problems += "\n" + t.id + ": " + t.problem + ".";
	if (inUse && !state.ok && !state.problem.empty())
		problems += "\n" + state.problem + ".";
	const std::string note = "An ES-DE theme draws the whole library as it does in ES-DE: the games in its list, "
			"their pictures and details where the theme puts them. Themes go in " + shownRoot() + "themes (or "
			"PSSwanStation/themes on a USB drive), each in its own folder. Pictures ES-DE has scraped go in "
			+ shownRoot() + "media (its downloaded_media/psx folders as they are: covers, screenshots, marquees, "
			"miximages..., and videos/<file name>.mp4). Each theme is its author's work, under its own "
			"licence, and is not part of PSSwanStation." + problems;
	const Item *focused = panelList(f, "ES-DE themes", items, 900, (float)std::min((int)items.size(), 7) * 60 + 10, note);
	standardHints(focused);
}

void themeDownloadPage(Frame& f)
{
	if (f.fresh)
	{
		std::lock_guard<std::mutex> lock(fetcher.mutex);
		if (fetcher.listState == 0 || fetcher.listState == 3)
		{
			fetcher.listState = 1;
			std::thread(fetchList).detach();
		}
	}
	std::vector<Item> items;
	std::string note;
	bool busy = fetcher.busy.load();
	{
		std::lock_guard<std::mutex> lock(fetcher.mutex);
		scan();
		std::set<std::string> installed;
		for (const esde::Theme& t : themes)
			installed.insert(lowerText(t.id));
		for (const Offer& o : fetcher.offers)
		{
			const bool have = installed.count(lowerText(o.repo)) != 0;
			Item item = action(have ? icon::Sync : icon::Download, o.name, "", [o] {
				if (fetcher.busy.exchange(true))
					return;
				{
					std::lock_guard<std::mutex> lock(fetcher.mutex);
					fetcher.working = o.name;
					fetcher.result.clear();
					fetcher.stage = "Downloading";
					fetcher.done = fetcher.total = 0;
				}
				fetcher.stop = false;
				std::thread(fetchTheme, o).detach();
			}, !busy && httpAvailable());
			item.value = have ? "Installed" : o.author;
			item.confirmHint = have ? "Fetch again" : "Fetch";
			items.push_back(item);
		}
		if (busy)
		{
			std::string amount;
			if (fetcher.total > 0)
				amount = ": " + megabytes(fetcher.done) + " of " + megabytes(fetcher.total);
			else if (fetcher.done > 0)
				amount = ": " + megabytes(fetcher.done);
			note = fetcher.stage + " " + fetcher.working + amount + "\xe2\x80\xa6  (Square stops it.)";
		}
		else if (!fetcher.result.empty())
			note = fetcher.result;
		else if (!httpAvailable())
			note = "The console is not on a network: themes cannot be fetched.";
		else if (fetcher.listState == 1)
			note = "Reading ES-DE's list of themes\xe2\x80\xa6";
		else if (fetcher.listState == 3)
			note = fetcher.listError + ". These themes are known to be ES-DE themes.";
		else
			note = format("%d themes, from the list ES-DE's own theme downloader reads. ", (int)fetcher.offers.size());
		if (!busy && fetcher.result.empty())
			note += " A theme is fetched from its author's repository into " + shownRoot() + "themes; it is its "
					"author's work, under its own licence. Themes are large: some are hundreds of megabytes.";
		if (!busy && fetcher.resultGood && !fetcher.installedId.empty())
		{
			const std::string id = fetcher.installedId;
			// The cursor on "Use it now" when it first appears.
			static std::string offered;
			if (offered != id)
			{
				offered = id;
				f.cursor = 0;
			}
			items.insert(items.begin(), action(icon::Check, "Use it now", "", [id] {
				esdeRescan();
				settings.theme = id;
				saveSettings();
				options::frontend().view = EsdeView;
				options::saveFrontend();
				wantReload = 1;
				stack.clear();
			}));
		}
	}
	if (busy && hit(Square))
		fetcher.stop = true;
	if (fetcher.rescan.exchange(false))
		esdeRescan();
	if (items.empty())
		items.push_back(fact("Themes", fetcher.listState == 1 ? "Reading the list\xe2\x80\xa6" : "None"));
	const Item *focused = panelList(f, "Get ES-DE themes", items, 900, 7 * 60 + 10, note);
	standardHints(focused);
}


}
