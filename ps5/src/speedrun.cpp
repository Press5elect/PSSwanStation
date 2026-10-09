/*
	PSSwanStation - a speedrun timer with splits.

	SPDX-License-Identifier: GPL-3.0-or-later

	The timer counts the emulator's frames (game time: the same on every
	console, whatever the screen's rate, and it stops while a menu is open) or
	the clock (real time). A run is split by hand (OPTIONS and Cross) or by
	itself, when a value in the game's memory becomes what a split's condition
	says: the level number, a flag that a boss is beaten. The values are found
	with Find in memory (memsearch.cpp). A run done without loading a state,
	rewinding or fast forward that ends faster than the personal best becomes
	it; the fastest each segment was ever done is kept too.

	The game's file, <root>data/splits/<serial>.txt, can be written by hand:

	  start when 0x8001A2B4 u8 == 1        (optional: when the run starts by itself)
	  split First level | when 0x8007A3B4 u8 == 2
	  split Boss
	  best 83450 165000                    (the title's: ms at each split)
	  gold 83450 80000                     (the title's: each segment's fastest)

	A condition is an address in the main memory (0x80000000 and up), a size
	(u8, u16, u32), a comparison (== != > < >= <=) and a number; it acts when
	it becomes true.
*/
#include "speedrun.h"
#include "memsearch.h"
#include "fe.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fe::speedrun
{
namespace
{
std::vector<Split> list;
std::string serial_, title_, path_, startCondition, practiceWhy;
State runState = Idle;
size_t at = 0;
uint64_t frames = 0;
double fpsNow = 60.0;
double startedAt = 0;
int64_t finishedAt = 0;
bool isPractice = false;
bool startHeld = false, splitHeld = false;

struct Condition
{
	uint32_t address = 0;
	int bytes = 1;
	int op = 0;		// 0 ==, 1 !=, 2 >, 3 <, 4 >=, 5 <=
	uint32_t value = 0;
};

bool parse(const std::string& text, Condition& out)
{
	char size[8] = {}, op[4] = {};
	char addressText[32] = {}, valueText[32] = {};
	if (sscanf(text.c_str(), "%31s %7s %3s %31s", addressText, size, op, valueText) != 4)
		return false;
	char *end = nullptr;
	const unsigned long address = strtoul(addressText, &end, 0);
	if (end == addressText)
		return false;
	out.address = (uint32_t)(address & 0x00FFFFFFu);
	if (!strcmp(size, "u8"))
		out.bytes = 1;
	else if (!strcmp(size, "u16"))
		out.bytes = 2;
	else if (!strcmp(size, "u32"))
		out.bytes = 4;
	else
		return false;
	static const char *const ops[6] = { "==", "!=", ">", "<", ">=", "<=" };
	out.op = -1;
	for (int i = 0; i < 6; i++)
		if (!strcmp(op, ops[i]))
			out.op = i;
	if (out.op < 0)
		return false;
	const unsigned long value = strtoul(valueText, &end, 0);
	if (end == valueText)
		return false;
	out.value = (uint32_t)value;
	return true;
}

bool evaluate(const Condition& c)
{
	const uint32_t now = memsearch::read(c.address, c.bytes);
	switch (c.op)
	{
	case 0: return now == c.value;
	case 1: return now != c.value;
	case 2: return now > c.value;
	case 3: return now < c.value;
	case 4: return now >= c.value;
	case 5: return now <= c.value;
	default: return false;
	}
}

int64_t now_()
{
	if (runState == Finished)
		return finishedAt;
	if (runState == Idle)
		return 0;
	if (options::frontend().speedrunClock == 1)
		return (int64_t)((fe::now() - startedAt) * 1000.0);
	return (int64_t)((double)frames * 1000.0 / (fpsNow > 1.0 ? fpsNow : 60.0));
}

void save()
{
	if (path_.empty())
		return;
	std::string text = "# PSSwanStation's speedrun splits for " + serial_ + " (" + title_ + ")\n"
			"# split NAME [| when ADDRESS u8|u16|u32 ==|!=|>|<|>=|<= VALUE]; start when CONDITION; best and gold are the "
			"title's\n";
	if (!startCondition.empty())
		text += "start when " + startCondition + "\n";
	bool anyBest = false, anyGold = false;
	for (const Split& split : list)
	{
		text += "split " + split.name + (split.when.empty() ? std::string() : " | when " + split.when) + "\n";
		anyBest = anyBest || split.best >= 0;
		anyGold = anyGold || split.gold >= 0;
	}
	if (anyBest)
	{
		text += "best";
		for (const Split& split : list)
			text += " " + std::to_string(split.best);
		text += "\n";
	}
	if (anyGold)
	{
		text += "gold";
		for (const Split& split : list)
			text += " " + std::to_string(split.gold);
		text += "\n";
	}
	makeDir(rootDir + "data/splits");
	writeFile(path_, text.data(), text.size());
}

void finish()
{
	finishedAt = now_();
	runState = Finished;
	at = list.size();
	if (isPractice)
	{
		diag::mark("speedrun: finished in %s (practice: %s)", timeText(finishedAt).c_str(), practiceWhy.c_str());
		return;
	}
	// Each segment's fastest.
	int64_t previous = 0;
	bool changed = false;
	for (Split& split : list)
	{
		if (split.time < 0)
			continue;
		const int64_t segment = split.time - previous;
		previous = split.time;
		if (segment > 0 && (split.gold < 0 || segment < split.gold))
		{
			split.gold = segment;
			changed = true;
		}
	}
	// A new personal best: faster at the end, every split done.
	const bool complete = !list.empty() && std::all_of(list.begin(), list.end(), [](const Split& s) { return s.time >= 0; });
	const int64_t bestEnd = list.empty() ? -1 : list.back().best;
	const bool record = complete && (bestEnd < 0 || finishedAt < bestEnd);
	if (record)
	{
		for (Split& split : list)
			split.best = split.time;
		changed = true;
	}
	diag::mark("speedrun: finished in %s%s", timeText(finishedAt).c_str(), record ? " (a personal best)" : "");
	host::addMessage(record ? "A personal best: " + timeText(finishedAt) : "Finished in " + timeText(finishedAt), 5.0);
	if (changed)
		save();
}

void begin()
{
	if (list.empty())
		list.push_back({ "The end", "", -1, -1, -1 });
	for (Split& split : list)
		split.time = -1;
	runState = Running;
	at = 0;
	frames = 0;
	startedAt = fe::now();
	finishedAt = 0;
	isPractice = false;
	practiceWhy.clear();
	// A split that holds already waits for its condition to become true again.
	Condition c;
	splitHeld = !list.empty() && parse(list[0].when, c) && evaluate(c);
	diag::mark("speedrun: started");
}

void advance()
{
	if (at >= list.size())
		return;
	list[at].time = now_();
	at++;
	if (at >= list.size())
	{
		finish();
		return;
	}
	Condition c;
	splitHeld = parse(list[at].when, c) && evaluate(c);
}
}

void loadFor(const std::string& serial, const std::string& title)
{
	unload();
	serial_ = serial;
	title_ = title;
	if (serial.empty())
		return;
	path_ = rootDir + "data/splits/" + serial + ".txt";
	std::vector<uint8_t> raw;
	if (!readFile(path_, raw))
		return;
	const std::string text(raw.begin(), raw.end());
	std::vector<int64_t> best, gold;
	size_t start = 0;
	while (start < text.size())
	{
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = trim(text.substr(start, end - start));
		start = end + 1;
		if (line.empty() || line[0] == '#')
			continue;
		if (line.compare(0, 11, "start when ") == 0)
			startCondition = trim(line.substr(11));
		else if (line.compare(0, 6, "split ") == 0)
		{
			Split split;
			std::string rest = line.substr(6);
			const size_t bar = rest.find("| when ");
			if (bar != std::string::npos)
			{
				split.when = trim(rest.substr(bar + 7));
				rest = rest.substr(0, bar);
			}
			split.name = trim(rest);
			list.push_back(split);
		}
		else if (line.compare(0, 5, "best ") == 0 || line.compare(0, 5, "gold ") == 0)
		{
			std::vector<int64_t>& into = line[0] == 'b' ? best : gold;
			const char *p = line.c_str() + 5;
			while (*p != '\0')
			{
				char *end2 = nullptr;
				const long long value = strtoll(p, &end2, 10);
				if (end2 == p)
					break;
				into.push_back((int64_t)value);
				p = end2;
			}
		}
	}
	for (size_t i = 0; i < list.size(); i++)
	{
		list[i].best = i < best.size() ? best[i] : -1;
		list[i].gold = i < gold.size() ? gold[i] : -1;
	}
	Condition c;
	startHeld = parse(startCondition, c) && evaluate(c);
}

void unload()
{
	list.clear();
	serial_.clear();
	title_.clear();
	path_.clear();
	startCondition.clear();
	runState = Idle;
	at = 0;
	frames = 0;
	isPractice = false;
}

const std::string& file()
{
	return path_;
}

void frame(double fps)
{
	fpsNow = fps;
	if (runState == Running)
		frames++;
	Condition c;
	if (runState != Running && !startCondition.empty() && parse(startCondition, c))
	{
		const bool holdsNow = evaluate(c);
		if (holdsNow && !startHeld)
			begin();
		startHeld = holdsNow;
	}
	if (runState == Running && at < list.size() && !list[at].when.empty() && parse(list[at].when, c))
	{
		const bool holdsNow = evaluate(c);
		if (holdsNow && !splitHeld)
			advance();
		else
			splitHeld = holdsNow;
	}
}

void startOrSplit()
{
	if (runState == Running)
		advance();
	else
		begin();
}

void undo()
{
	if (runState == Finished && !list.empty())
	{
		runState = Running;
		at = list.size() - 1;
		list[at].time = -1;
		return;
	}
	if (runState == Running && at > 0)
	{
		at--;
		list[at].time = -1;
	}
}

void skip()
{
	if (runState != Running || at >= list.size())
		return;
	list[at].time = -1;
	at++;
	if (at >= list.size())
		finish();
}

void reset()
{
	if (runState == Running)
		diag::mark("speedrun: reset at %s", timeText(now_()).c_str());
	runState = Idle;
	at = 0;
	frames = 0;
	for (Split& split : list)
		split.time = -1;
	isPractice = false;
	practiceWhy.clear();
}

void markPractice(const char *why)
{
	if (runState != Running || isPractice)
		return;
	isPractice = true;
	practiceWhy = why;
	diag::mark("speedrun: practice from here (%s)", why);
}

State state()
{
	return runState;
}

int64_t elapsed()
{
	return now_();
}

size_t current()
{
	return at;
}

bool practice()
{
	return isPractice;
}

const std::string& practiceReason()
{
	return practiceWhy;
}

std::vector<Split>& splits()
{
	return list;
}

void addSplit(const std::string& name)
{
	if (trim(name).empty())
		return;
	list.push_back({ trim(name), "", -1, -1, -1 });
	save();
}

void removeSplit(size_t index)
{
	if (index >= list.size())
		return;
	list.erase(list.begin() + (long)index);
	if (runState != Idle)
		reset();
	save();
}

void renameSplit(size_t index, const std::string& name)
{
	if (index >= list.size() || trim(name).empty())
		return;
	list[index].name = trim(name);
	save();
}

void setWhen(size_t index, const std::string& condition)
{
	if (index >= list.size())
		return;
	list[index].when = trim(condition);
	save();
}

const std::string& startWhen()
{
	return startCondition;
}

void setStartWhen(const std::string& condition)
{
	startCondition = trim(condition);
	Condition c;
	startHeld = parse(startCondition, c) && evaluate(c);
	save();
}

void forgetBest()
{
	for (Split& split : list)
		split.best = split.gold = -1;
	save();
}

bool validCondition(const std::string& condition)
{
	Condition c;
	return parse(condition, c);
}

bool holds(const std::string& condition)
{
	Condition c;
	return parse(condition, c) && evaluate(c);
}

std::string timeText(int64_t ms)
{
	if (ms < 0)
		return "\xe2\x80\x94";
	const int64_t hundredths = ms / 10;
	const int64_t seconds = hundredths / 100, minutes = seconds / 60, hours = minutes / 60;
	if (hours > 0)
		return format("%lld:%02lld:%02lld.%02lld", (long long)hours, (long long)(minutes % 60), (long long)(seconds % 60),
				(long long)(hundredths % 100));
	return format("%lld:%02lld.%02lld", (long long)minutes, (long long)(seconds % 60), (long long)(hundredths % 100));
}

std::string deltaText(int64_t ms)
{
	const int64_t magnitude = ms < 0 ? -ms : ms;
	const int64_t hundredths = magnitude / 10;
	const int64_t seconds = hundredths / 100;
	if (seconds >= 60)
		return std::string(ms < 0 ? "-" : "+") + format("%lld:%02lld.%01lld", (long long)(seconds / 60),
				(long long)(seconds % 60), (long long)((hundredths % 100) / 10));
	return std::string(ms < 0 ? "-" : "+") + format("%lld.%02lld", (long long)seconds, (long long)(hundredths % 100));
}

}
