/*
	PSSwanStation - libretro shader presets brought in from a USB drive and
	compiled on the console.

	SPDX-License-Identifier: GPL-3.0-or-later

	A preset of the libretro slang collection (github.com/libretro/slang-shaders,
	the format RetroArch reads) is a .slangp file naming its passes, each a
	.slang file holding both of a pass's stages in Vulkan GLSL. The title's
	chain runner (chain.cpp) takes what ps5/tools/make-chains.py makes of one
	on a PC: each stage compiled to SPIR-V, what it reads found in the SPIR-V,
	and chain.txt. This is that tool done on the console, with glslang (the
	Khronos reference compiler, built into the title), so any preset can be
	copied to a USB drive (PSSwanStation/shaders, with the files it uses beside
	it, as the collection has them) and brought in from the Picture settings.

	What is done, as make-chains.py does it:
	  - the preset is read (RetroArch's #reference to another preset too, each
	    file's paths being from its own folder);
	  - each pass's #include lines are put in place, its #pragma parameter,
	    name and format lines taken out, and its two stages (#pragma stage)
	    made two sources;
	  - each is compiled for Vulkan 1.0, and its push constants, uniform block
	    and textures read from the SPIR-V by name;
	  - chain.txt, the SPIR-V and the preset's pictures are written to
	    <root>data/shaders/<name>/, which a later start finds again.
*/
#include "slangimport.h"
#include "fe.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fe::slangimport
{
namespace
{
std::mutex mutex;
std::string statusText;
std::atomic<bool> busy{false};
std::thread worker;
std::vector<std::string> names;
bool listed = false;
std::atomic<unsigned> changes{0};

std::string importedRoot()
{
	return rootDir + "data/shaders";
}

std::string directoryOf(const std::string& path)
{
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// "a/b/../c" as "a/c".
std::string normalise(const std::string& path)
{
	std::vector<std::string> parts;
	size_t start = 0;
	const bool absolute = !path.empty() && path[0] == '/';
	while (start <= path.size())
	{
		size_t end = path.find('/', start);
		if (end == std::string::npos)
			end = path.size();
		const std::string part = path.substr(start, end - start);
		if (part == "..")
		{
			if (!parts.empty() && parts.back() != "..")
				parts.pop_back();
			else if (!absolute)
				parts.push_back(part);
		}
		else if (!part.empty() && part != ".")
			parts.push_back(part);
		start = end + 1;
	}
	std::string out = absolute ? "/" : "";
	for (size_t i = 0; i < parts.size(); i++)
		out += (i == 0 ? "" : "/") + parts[i];
	return out;
}

std::string joinPath(const std::string& dir, const std::string& file)
{
	if (!file.empty() && file[0] == '/')
		return normalise(file);
	std::string clean = file;
	for (char& c : clean)
		if (c == '\\')
			c = '/';
	return normalise(dir + "/" + clean);
}

std::string safeName(const std::string& text)
{
	std::string out;
	for (const char c : text)
		out += (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') ? c : '-';
	while (!out.empty() && (out.back() == '-' || out.back() == '.'))
		out.pop_back();
	return out.empty() ? std::string("preset") : out;
}

bool readText(const std::string& path, std::string& text)
{
	std::vector<uint8_t> raw;
	if (!readFile(path, raw))
		return false;
	text.assign(raw.begin(), raw.end());
	return true;
}

std::vector<std::string> lines(const std::string& text)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= text.size())
	{
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(start, end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		out.push_back(line);
		if (end == text.size())
			break;
		start = end + 1;
	}
	return out;
}

// ------------------------------------------------------------ the preset

struct Value
{
	std::string text, dir;	// the value, and the folder of the file that set it
};
using Preset = std::map<std::string, Value>;

bool readPreset(const std::string& path, Preset& preset, std::string& error, int depth = 0)
{
	if (depth > 8)
	{
		error = "presets refer to each other too deeply";
		return false;
	}
	std::string text;
	if (!readText(path, text))
	{
		error = "cannot read " + baseName(path);
		return false;
	}
	const std::string dir = directoryOf(path);
	for (std::string line : lines(text))
	{
		line = trim(line);
		if (line.compare(0, 10, "#reference") == 0)
		{
			std::string inner = trim(line.substr(10));
			if (inner.size() >= 2 && inner.front() == '"' && inner.back() == '"')
				inner = inner.substr(1, inner.size() - 2);
			if (!readPreset(joinPath(dir, inner), preset, error, depth + 1))
				return false;
			continue;
		}
		if (line.empty() || line[0] == '#' || line.compare(0, 2, "//") == 0)
			continue;
		const size_t equals = line.find('=');
		if (equals == std::string::npos)
			continue;
		const std::string key = trim(line.substr(0, equals));
		std::string value = trim(line.substr(equals + 1));
		if (!value.empty() && value[0] == '"')
		{
			const size_t close = value.find('"', 1);
			value = value.substr(1, close == std::string::npos ? std::string::npos : close - 1);
		}
		else
		{
			// A comment after the value.
			size_t cut = value.find(" #");
			if (cut == std::string::npos)
				cut = value.find("//");
			if (cut != std::string::npos)
				value = trim(value.substr(0, cut));
		}
		bool keyOk = !key.empty();
		for (const char c : key)
			keyOk = keyOk && (isalnum((unsigned char)c) || c == '_');
		if (keyOk)
			preset[key] = { value, dir };
	}
	return true;
}

std::string get(const Preset& preset, const std::string& key, const std::string& otherwise = "")
{
	const auto it = preset.find(key);
	return it != preset.end() ? it->second.text : otherwise;
}

bool has(const Preset& preset, const std::string& key)
{
	return preset.find(key) != preset.end();
}

// ------------------------------------------------------------ the passes

bool expand(const std::string& path, std::vector<std::string>& out, std::set<std::string>& seen, std::string& error)
{
	std::string text;
	if (!readText(path, text))
	{
		error = "cannot read " + baseName(path);
		return false;
	}
	for (const std::string& line : lines(text))
	{
		const std::string trimmed = trim(line);
		if (trimmed.compare(0, 8, "#include") == 0)
		{
			const size_t open = trimmed.find('"'), close = trimmed.rfind('"');
			if (open == std::string::npos || close <= open)
			{
				error = "a #include in " + baseName(path) + " is not understood";
				return false;
			}
			// Every time it is named, as RetroArch does (the files keep
			// themselves from being read twice with #ifndef); only a file
			// that includes itself is stopped.
			const std::string inner = joinPath(directoryOf(path), trimmed.substr(open + 1, close - open - 1));
			if (seen.count(inner) != 0)
				continue;
			seen.insert(inner);
			const bool ok = expand(inner, out, seen, error);
			seen.erase(inner);
			if (!ok)
				return false;
		}
		else
			out.push_back(line);
	}
	return true;
}

struct Parameter
{
	std::string name;
	double value, low, high;
};

struct Source
{
	std::string vertex, fragment, name, format;
	std::vector<Parameter> parameters;
};

// "#pragma word rest" as word and rest.
bool pragma(const std::string& line, const char *word, std::string& rest)
{
	std::string text = trim(line);
	if (text.compare(0, 7, "#pragma") != 0)
		return false;
	text = trim(text.substr(7));
	const size_t n = strlen(word);
	if (text.compare(0, n, word) != 0 || (text.size() > n && text[n] != ' ' && text[n] != '\t'))
		return false;
	rest = trim(text.substr(n));
	return true;
}

bool preprocess(const std::string& path, Source& source, std::string& error)
{
	std::vector<std::string> all;
	std::set<std::string> seen;
	if (!expand(path, all, seen, error))
		return false;
	std::vector<std::string> common, vertex, fragment;
	std::vector<std::string> *current = &common;
	for (std::string line : all)
	{
		std::string rest;
		if (pragma(line, "parameter", rest))
		{
			// NAME "description" DEFAULT MIN MAX [STEP]
			Parameter p;
			const size_t space = rest.find_first_of(" \t");
			const size_t open = rest.find('"'), close = rest.rfind('"');
			if (space != std::string::npos && open != std::string::npos && close > open)
			{
				p.name = rest.substr(0, space);
				double values[4] = { 0, 0, 0, 0 };
				const int got = sscanf(rest.c_str() + close + 1, "%lf %lf %lf %lf", &values[0], &values[1], &values[2],
						&values[3]);
				if (got >= 3)
				{
					p.value = values[0];
					p.low = values[1];
					p.high = values[2];
					source.parameters.push_back(p);
				}
			}
			line.clear();
		}
		else if (pragma(line, "name", rest))
		{
			source.name = rest.substr(0, rest.find_first_of(" \t"));
			line.clear();
		}
		else if (pragma(line, "format", rest))
		{
			source.format = rest.substr(0, rest.find_first_of(" \t"));
			line.clear();
		}
		else if (pragma(line, "stage", rest))
		{
			current = rest.compare(0, 6, "vertex") == 0 ? &vertex : rest.compare(0, 8, "fragment") == 0 ? &fragment : &common;
			line.clear();
		}
		current->push_back(line);
	}
	auto join = [](const std::vector<std::string>& a, const std::vector<std::string>& b) {
		std::string text;
		for (const std::string& line : a)
			text += line + "\n";
		for (const std::string& line : b)
			text += line + "\n";
		return text;
	};
	source.vertex = join(common, vertex);
	source.fragment = join(common, fragment);
	if (vertex.empty() || fragment.empty())
	{
		error = baseName(path) + " does not have both stages";
		return false;
	}
	return true;
}

bool compile(const std::string& text, bool vertex, std::vector<uint32_t>& spirv, std::string& error)
{
	static std::once_flag once;
	std::call_once(once, [] { glslang::InitializeProcess(); });
	const EShLanguage stage = vertex ? EShLangVertex : EShLangFragment;
	glslang::TShader shader(stage);
	const char *strings[1] = { text.c_str() };
	shader.setStrings(strings, 1);
	shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
	const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
	if (!shader.parse(GetDefaultResources(), 100, false, messages))
	{
		error = shader.getInfoLog();
		return false;
	}
	glslang::TProgram program;
	program.addShader(&shader);
	if (!program.link(messages))
	{
		error = program.getInfoLog();
		return false;
	}
	spirv.clear();
	glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
	return !spirv.empty();
}

// ------------------------------------------------------- SPIR-V reflection

struct Member
{
	uint32_t offset;
	std::string kind, name;
};

struct Block
{
	bool present = false;
	uint32_t binding = 0, size = 0;
	std::vector<Member> members;
};

struct Reflected
{
	Block push, ubo;
	std::vector<std::pair<uint32_t, std::string>> samplers;
};

std::string spirvString(const uint32_t *words, size_t count)
{
	std::string out;
	for (size_t i = 0; i < count; i++)
		for (int b = 0; b < 4; b++)
		{
			const char c = (char)((words[i] >> (8 * b)) & 0xFF);
			if (c == '\0')
				return out;
			out += c;
		}
	return out;
}

bool reflect(const std::vector<uint32_t>& words, Reflected& out, std::string& error)
{
	struct Type
	{
		std::string kind;
		uint32_t size = 0;
		std::vector<uint32_t> members;
		uint32_t storage = 0, pointee = 0;
	};
	std::map<uint32_t, std::string> names;
	std::map<std::pair<uint32_t, uint32_t>, std::string> memberNames;
	std::map<std::pair<uint32_t, uint32_t>, uint32_t> memberOffsets;
	std::map<uint32_t, uint32_t> bindings;
	std::map<uint32_t, Type> types;
	struct Variable
	{
		uint32_t type, id, storage;
	};
	std::vector<Variable> variables;
	size_t i = 5;
	while (i < words.size())
	{
		const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
		if (count == 0 || i + count > words.size())
			break;
		const uint32_t *a = words.data() + i + 1;
		const size_t n = count - 1;
		if (op == 5 && n >= 2)
			names[a[0]] = spirvString(a + 1, n - 1);
		else if (op == 6 && n >= 3)
			memberNames[{ a[0], a[1] }] = spirvString(a + 2, n - 2);
		else if (op == 71 && n >= 3 && a[1] == 33)
			bindings[a[0]] = a[2];
		else if (op == 72 && n >= 4 && a[2] == 35)
			memberOffsets[{ a[0], a[1] }] = a[3];
		else if (op == 21 && n >= 3)
			types[a[0]] = { a[2] ? "int" : "uint", 4, {}, 0, 0 };
		else if (op == 22 && n >= 2)
			types[a[0]] = { "float", 4, {}, 0, 0 };
		else if (op == 23 && n >= 3)
			types[a[0]] = { "vec" + std::to_string(a[2]), 4 * a[2], {}, 0, 0 };
		else if (op == 24 && n >= 3)
			types[a[0]] = { "mat" + std::to_string(a[2]), 16 * a[2], {}, 0, 0 };
		else if ((op == 25 || op == 26 || op == 27) && n >= 1)
			types[a[0]] = { "texture", 0, {}, 0, 0 };
		else if (op == 30 && n >= 1)
			types[a[0]] = { "struct", 0, std::vector<uint32_t>(a + 1, a + n), 0, 0 };
		else if (op == 32 && n >= 3)
			types[a[0]] = { "pointer", 0, {}, a[1], a[2] };
		else if (op == 59 && n >= 3)
			variables.push_back({ a[0], a[1], a[2] });
		i += count;
	}
	auto members = [&](uint32_t structId, Block& block) {
		const Type& type = types[structId];
		for (uint32_t index = 0; index < type.members.size(); index++)
		{
			const std::string kind = types[type.members[index]].kind;
			if (kind != "float" && kind != "int" && kind != "uint" && kind != "vec4" && kind != "mat4")
			{
				error = "a uniform of type " + kind + " is not handled";
				return false;
			}
			const uint32_t offset = memberOffsets[{ structId, index }];
			block.members.push_back({ offset, kind, memberNames[{ structId, index }] });
			block.size = std::max(block.size, offset + (kind == "mat4" ? 64u : kind == "vec4" ? 16u : 4u));
		}
		block.present = true;
		return true;
	};
	for (const Variable& v : variables)
	{
		const auto pointer = types.find(v.type);
		if (pointer == types.end() || pointer->second.kind != "pointer")
			continue;
		const uint32_t target = pointer->second.pointee;
		if (v.storage == 9)
		{
			if (!members(target, out.push))
				return false;
		}
		else if (v.storage == 2 && types[target].kind == "struct")
		{
			out.ubo.binding = bindings.count(v.id) ? bindings[v.id] : 0;
			if (!members(target, out.ubo))
				return false;
		}
		else if (v.storage == 0 && types[target].kind == "texture")
			out.samplers.push_back({ bindings.count(v.id) ? bindings[v.id] : 0, names[v.id] });
	}
	return true;
}

std::string number(double value)
{
	char text[64];
	snprintf(text, sizeof(text), "%g", value);
	return text;
}

bool writeWords(const std::string& path, const std::vector<uint32_t>& words)
{
	return writeFile(path, words.data(), words.size() * 4);
}

void say(const std::string& text)
{
	std::lock_guard<std::mutex> lock(mutex);
	statusText = text;
}

void removeTree(const std::string& path)
{
	DIR *dir = opendir(path.c_str());
	if (dir != nullptr)
	{
		while (const dirent *entry = readdir(dir))
		{
			const std::string name = entry->d_name;
			if (name == "." || name == "..")
				continue;
			const std::string inside = path + "/" + name;
			if (dirExists(inside))
				removeTree(inside);
			else
				unlink(inside.c_str());
		}
		closedir(dir);
	}
	rmdir(path.c_str());
}

void list()
{
	std::vector<std::string> found;
	DIR *dir = opendir(importedRoot().c_str());
	if (dir != nullptr)
	{
		while (const dirent *entry = readdir(dir))
		{
			const std::string name = entry->d_name;
			if (name[0] != '.' && fileExists(importedRoot() + "/" + name + "/chain.txt"))
				found.push_back(name);
		}
		closedir(dir);
	}
	std::sort(found.begin(), found.end());
	std::lock_guard<std::mutex> lock(mutex);
	names = found;
	listed = true;
	changes++;
}

void findIn(const std::string& folder, int depth, std::vector<Found>& out)
{
	if (depth > 4)
		return;
	DIR *dir = opendir(folder.c_str());
	if (dir == nullptr)
		return;
	std::vector<std::string> folders;
	while (const dirent *entry = readdir(dir))
	{
		const std::string name = entry->d_name;
		if (name[0] == '.')
			continue;
		const std::string path = folder + "/" + name;
		if (extension(name) == ".slangp")
			out.push_back({ safeName(fileTitle(name)), path, false });
		else if (dirExists(path))
			folders.push_back(path);
	}
	closedir(dir);
	for (const std::string& inner : folders)
		findIn(inner, depth + 1, out);
}
}

bool importNow(const std::string& presetPath, const std::string& wantedName, std::string& error)
{
	const std::string name = safeName(wantedName);
	Preset preset;
	if (!readPreset(presetPath, preset, error))
		return false;
	const int passes = atoi(get(preset, "shaders", "0").c_str());
	if (passes <= 0 || passes > 64)
	{
		error = "the preset names no passes";
		return false;
	}
	makeDir(importedRoot());
	const std::string out = importedRoot() + "/" + name + ".new";
	removeTree(out);
	makeDir(out);
	std::vector<std::string> passLines;
	std::vector<Parameter> parameters;
	std::set<std::string> parameterNames;
	static const std::set<std::string> formats = { "R8G8B8A8_UNORM", "R8G8B8A8_SRGB", "R16G16B16A16_SFLOAT",
		"R32G32B32A32_SFLOAT", "A2B10G10R10_UNORM_PACK32", "R16G16_SFLOAT", "R32_SFLOAT", "R8_UNORM", "R16_SFLOAT",
		"R32G32_SFLOAT" };
	for (int n = 0; n < passes; n++)
	{
		say(format("Bringing in %s: pass %d of %d", name.c_str(), n + 1, passes));
		const std::string key = format("shader%d", n);
		const auto shaderIt = preset.find(key);
		if (shaderIt == preset.end())
		{
			error = format("the preset has no %s", key.c_str());
			removeTree(out);
			return false;
		}
		const std::string path = joinPath(shaderIt->second.dir, shaderIt->second.text);
		Source source;
		if (!preprocess(path, source, error))
		{
			removeTree(out);
			return false;
		}
		for (const Parameter& p : source.parameters)
			if (parameterNames.insert(p.name).second)
				parameters.push_back(p);
		Reflected stages[2];
		for (int s = 0; s < 2; s++)
		{
			std::vector<uint32_t> spirv;
			std::string why;
			if (!compile(s == 0 ? source.vertex : source.fragment, s == 0, spirv, why))
			{
				error = format("pass %d (%s, %s stage) did not compile: ", n, baseName(path).c_str(), s == 0 ? "vertex" : "fragment")
						+ trim(why.substr(0, 600));
				removeTree(out);
				return false;
			}
			if (!reflect(spirv, stages[s], error) || !writeWords(out + format("/p%d.%s.spv", n, s == 0 ? "vert" : "frag"), spirv))
			{
				if (error.empty())
					error = "could not write the compiled pass";
				removeTree(out);
				return false;
			}
		}
		std::string fmt = source.format;
		if (get(preset, format("float_framebuffer%d", n)) == "true")
			fmt = "R16G16B16A16_SFLOAT";
		else if (get(preset, format("srgb_framebuffer%d", n)) == "true")
			fmt = "R8G8B8A8_SRGB";
		if (fmt.empty())
			fmt = "R8G8B8A8_UNORM";
		if (formats.count(fmt) == 0)
		{
			error = format("pass %d asks for a format (%s) the chain runner does not have", n, fmt.c_str());
			removeTree(out);
			return false;
		}
		const bool last = n == passes - 1;
		std::string types[2];
		double values[2];
		for (int axis = 0; axis < 2; axis++)
		{
			const char *a = axis == 0 ? "x" : "y";
			const std::string typeKey = format("scale_type_%s%d", a, n), valueKey = format("scale_%s%d", a, n);
			std::string type = has(preset, typeKey) ? get(preset, typeKey) : get(preset, format("scale_type%d", n));
			std::string value = has(preset, valueKey) ? get(preset, valueKey) : get(preset, format("scale%d", n));
			if (type.empty())
			{
				type = last ? "viewport" : "source";
				value = "1.0";
			}
			types[axis] = type;
			values[axis] = value.empty() ? 1.0 : atof(value.c_str());
		}
		std::string alias = get(preset, format("alias%d", n));
		if (alias.empty())
			alias = source.name;
		if (alias.empty())
			alias = "-";
		const std::string wrap = get(preset, format("wrap_mode%d", n), "clamp_to_border");
		passLines.push_back(format("pass %d p%d.vert.spv p%d.frag.spv %s %d %s %d %d %s %s %s %s %s", n, n, n, fmt.c_str(),
				get(preset, format("filter_linear%d", n)) == "true" ? 1 : 0, wrap.c_str(),
				get(preset, format("mipmap_input%d", n)) == "true" ? 1 : 0, atoi(get(preset, format("frame_count_mod%d", n), "0").c_str()),
				alias.c_str(), types[0].c_str(), number(values[0]).c_str(), types[1].c_str(), number(values[1]).c_str()));
		// The two stages together: a member either reads is set for both.
		std::map<std::string, std::pair<uint32_t, std::string>> push, ubo;
		uint32_t pushSize = 0, uboSize = 0;
		int uboBinding = -1;
		std::map<uint32_t, std::string> samplers;
		for (const Reflected& r : stages)
		{
			if (r.push.present)
			{
				pushSize = std::max(pushSize, r.push.size);
				for (const Member& m : r.push.members)
					push[m.name] = { m.offset, m.kind };
			}
			if (r.ubo.present)
			{
				uboBinding = (int)r.ubo.binding;
				uboSize = std::max(uboSize, r.ubo.size);
				for (const Member& m : r.ubo.members)
					ubo[m.name] = { m.offset, m.kind };
			}
			for (const auto& [binding, sampler] : r.samplers)
				samplers[binding] = sampler;
		}
		if (pushSize != 0)
			passLines.push_back(format("push %u", pushSize));
		if (uboBinding >= 0)
			passLines.push_back(format("ubo %d %u", uboBinding, uboSize));
		for (int which = 0; which < 2; which++)
		{
			std::vector<std::pair<uint32_t, std::pair<std::string, std::string>>> sorted;
			for (const auto& [member, place] : which == 0 ? push : ubo)
				sorted.push_back({ place.first, { place.second, member } });
			std::sort(sorted.begin(), sorted.end());
			for (const auto& entry : sorted)
				passLines.push_back(format("member %s %u %s %s", which == 0 ? "push" : "ubo", entry.first,
						entry.second.first.c_str(), entry.second.second.c_str()));
		}
		for (const auto& [binding, sampler] : samplers)
			passLines.push_back(format("sampler %u %s", binding, sampler.c_str()));
	}
	std::string chain = "chain " + name + "\n";
	// The preset's own values for the parameters it lists.
	std::map<std::string, double> overrides;
	{
		const std::string listed = get(preset, "parameters");
		size_t start = 0;
		while (start <= listed.size())
		{
			size_t end = listed.find(';', start);
			if (end == std::string::npos)
				end = listed.size();
			const std::string key = trim(listed.substr(start, end - start));
			if (!key.empty() && has(preset, key))
				overrides[key] = atof(get(preset, key).c_str());
			start = end + 1;
		}
	}
	for (const Parameter& p : parameters)
		chain += "param " + p.name + " " + number(overrides.count(p.name) ? overrides[p.name] : p.value) + " " + number(p.low)
				+ " " + number(p.high) + "\n";
	// The preset's pictures.
	{
		const std::string textures = get(preset, "textures");
		size_t start = 0;
		int index = 0;
		while (start <= textures.size())
		{
			size_t end = textures.find(';', start);
			if (end == std::string::npos)
				end = textures.size();
			const std::string lut = trim(textures.substr(start, end - start));
			start = end + 1;
			if (lut.empty())
				continue;
			const auto it = preset.find(lut);
			if (it == preset.end())
			{
				error = "the preset names a picture (" + lut + ") without its file";
				removeTree(out);
				return false;
			}
			const std::string from = joinPath(it->second.dir, it->second.text);
			std::vector<uint8_t> data;
			const std::string file = format("lut%d%s", index++, extension(from).c_str());
			if (!readFile(from, data) || !writeFile(out + "/" + file, data.data(), data.size()))
			{
				error = "the picture " + baseName(from) + " could not be copied";
				removeTree(out);
				return false;
			}
			chain += "lut " + lut + " " + file + " " + (get(preset, lut + "_linear") == "true" ? "1" : "0") + " "
					+ (get(preset, lut + "_mipmap") == "true" ? "1" : "0") + " " + get(preset, lut + "_wrap_mode", "clamp_to_border")
					+ "\n";
		}
	}
	for (const std::string& line : passLines)
		chain += line + "\n";
	if (!writeFile(out + "/chain.txt", chain.data(), chain.size()))
	{
		error = "chain.txt could not be written";
		removeTree(out);
		return false;
	}
	// In place of one brought in before under that name.
	const std::string final = importedRoot() + "/" + name;
	removeTree(final);
	if (rename(out.c_str(), final.c_str()) != 0)
	{
		error = "the folder could not be put in place";
		removeTree(out);
		return false;
	}
	diag::mark("shaders: brought in %s from %s (%d passes, %zu parameters)", name.c_str(), presetPath.c_str(), passes,
			parameters.size());
	list();
	return true;
}

std::vector<Found> find()
{
	std::vector<Found> out;
	for (const std::string& drive : platform::usbDrives())
		findIn(drive + "/" + storage::UsbFolder + "/shaders", 0, out);
	findIn(rootDir + "shaders", 0, out);
	const std::vector<std::string> have = imported();
	for (Found& found : out)
		found.imported = std::find(have.begin(), have.end(), found.name) != have.end();
	std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return lowercase(a.name) < lowercase(b.name); });
	return out;
}

bool start(const Found& preset)
{
	if (busy.exchange(true))
		return false;
	if (worker.joinable())
		worker.join();
	say("Bringing in " + preset.name + "\xe2\x80\xa6");
	worker = std::thread([preset] {
		std::string error;
		const bool ok = importNow(preset.path, preset.name, error);
		say(ok ? preset.name + " is brought in: choose it under Picture tube." : preset.name + " was not brought in: " + error);
		if (!ok)
			diag::mark("shaders: %s was not brought in: %s", preset.name.c_str(), error.c_str());
		busy = false;
	});
	return true;
}

bool working()
{
	return busy;
}

std::string status()
{
	std::lock_guard<std::mutex> lock(mutex);
	return statusText;
}

std::vector<std::string> imported()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (listed)
			return names;
	}
	list();
	std::lock_guard<std::mutex> lock(mutex);
	return names;
}

std::string folder(const std::string& name)
{
	return importedRoot() + "/" + name;
}

void remove(const std::string& name)
{
	removeTree(folder(safeName(name)));
	diag::mark("shaders: removed %s", name.c_str());
	list();
}

unsigned generation()
{
	return changes;
}

}
