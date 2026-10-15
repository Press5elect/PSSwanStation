/*
	PSSwanStation - a small JSON reader: GitHub's release answers (update.cpp),
	ES-DE's list of themes (ui_esde.cpp).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fe
{
namespace jsonlimits
{
constexpr int MaxJsonDepth = 48;
}
using jsonlimits::MaxJsonDepth;

// As much of it as the releases page needs: every kind of value is read, an
// answer that is cut short or not quite JSON gives what was read up to there.

struct Json
{
	enum Type { Null, Bool, Number, String, Array, Object };
	Type type = Null;
	bool boolean = false;
	double number = 0;
	std::string text;
	std::vector<std::string> keys;	// an object's, beside its items
	std::vector<Json> items;

	const Json *find(const char *key) const
	{
		if (type == Object)
			for (size_t i = 0; i < keys.size() && i < items.size(); i++)
				if (keys[i] == key)
					return &items[i];
		return nullptr;
	}
	std::string string(const char *key) const
	{
		const Json *value = find(key);
		return value != nullptr && value->type == String ? value->text : "";
	}
};

class JsonReader
{
public:
	JsonReader(const char *data, size_t size) : at(data), end(data + size)
	{
	}

	bool read(Json& out, int depth = 0)
	{
		skipSpace();
		if (at >= end || depth > MaxJsonDepth)
			return false;
		const char c = *at;
		if (c == '{')
			return readObject(out, depth);
		if (c == '[')
			return readArray(out, depth);
		if (c == '"')
		{
			out.type = Json::String;
			return readString(out.text);
		}
		if (c == '-' || (c >= '0' && c <= '9'))
			return readNumber(out);
		if (word("true"))
		{
			out.type = Json::Bool;
			out.boolean = true;
			return true;
		}
		if (word("false"))
		{
			out.type = Json::Bool;
			return true;
		}
		return word("null");
	}

private:
	void skipSpace()
	{
		while (at < end && (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n'))
			at++;
	}

	bool word(const char *text)
	{
		const size_t length = strlen(text);
		if ((size_t)(end - at) < length || memcmp(at, text, length) != 0)
			return false;
		at += length;
		return true;
	}

	bool readNumber(Json& out)
	{
		const char *start = at;
		while (at < end && ((*at >= '0' && *at <= '9') || *at == '-' || *at == '+' || *at == '.' || *at == 'e'
				|| *at == 'E'))
			at++;
		out.type = Json::Number;
		out.number = strtod(std::string(start, (size_t)(at - start)).c_str(), nullptr);
		return true;
	}

	static void appendUtf8(std::string& out, uint32_t code)
	{
		if (code < 0x80)
			out += (char)code;
		else if (code < 0x800)
		{
			out += (char)(0xc0 | (code >> 6));
			out += (char)(0x80 | (code & 0x3f));
		}
		else if (code < 0x10000)
		{
			out += (char)(0xe0 | (code >> 12));
			out += (char)(0x80 | ((code >> 6) & 0x3f));
			out += (char)(0x80 | (code & 0x3f));
		}
		else
		{
			out += (char)(0xf0 | (code >> 18));
			out += (char)(0x80 | ((code >> 12) & 0x3f));
			out += (char)(0x80 | ((code >> 6) & 0x3f));
			out += (char)(0x80 | (code & 0x3f));
		}
	}

	// The four hex digits of a \u escape, the cursor on the first of them.
	bool readHex4(uint32_t& code)
	{
		if (end - at < 4)
			return false;
		code = 0;
		for (int i = 0; i < 4; i++)
		{
			const char c = at[i];
			uint32_t digit;
			if (c >= '0' && c <= '9')
				digit = (uint32_t)(c - '0');
			else if (c >= 'a' && c <= 'f')
				digit = (uint32_t)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				digit = (uint32_t)(c - 'A' + 10);
			else
				return false;
			code = code * 16 + digit;
		}
		at += 4;
		return true;
	}

	bool readString(std::string& out)
	{
		out.clear();
		at++;	// the opening quote
		while (at < end)
		{
			const char c = *at++;
			if (c == '"')
				return true;
			if (c != '\\')
			{
				out += c;
				continue;
			}
			if (at >= end)
				return false;
			const char escape = *at++;
			switch (escape)
			{
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'u':
			{
				uint32_t code;
				if (!readHex4(code))
					return false;
				if (code >= 0xd800 && code < 0xdc00)
				{
					// The first half of a pair: the second follows as its own escape.
					uint32_t low = 0;
					const char *back = at;
					if (end - at >= 6 && at[0] == '\\' && at[1] == 'u' && (at += 2, readHex4(low)) && low >= 0xdc00
							&& low < 0xe000)
						code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
					else
					{
						at = back;
						code = 0xfffd;
					}
				}
				else if (code >= 0xdc00 && code < 0xe000)
					code = 0xfffd;
				appendUtf8(out, code);
				break;
			}
			default:	// '"', '\\', '/', and anything unknown as itself
				out += escape;
				break;
			}
		}
		return false;
	}

	bool readArray(Json& out, int depth)
	{
		out.type = Json::Array;
		at++;
		for (;;)
		{
			skipSpace();
			if (at >= end)
				return false;
			if (*at == ']')
			{
				at++;
				return true;
			}
			if (*at == ',')
			{
				at++;
				continue;
			}
			Json item;
			const bool whole = read(item, depth + 1);
			out.items.push_back(std::move(item));
			if (!whole)
				return false;
		}
	}

	bool readObject(Json& out, int depth)
	{
		out.type = Json::Object;
		at++;
		for (;;)
		{
			skipSpace();
			if (at >= end)
				return false;
			if (*at == '}')
			{
				at++;
				return true;
			}
			if (*at == ',')
			{
				at++;
				continue;
			}
			if (*at != '"')
				return false;
			std::string key;
			if (!readString(key))
				return false;
			skipSpace();
			if (at >= end || *at != ':')
				return false;
			at++;
			Json value;
			const bool whole = read(value, depth + 1);
			out.keys.push_back(std::move(key));
			out.items.push_back(std::move(value));
			if (!whole)
				return false;
		}
	}

	const char *at, *end;
};

} // namespace fe
