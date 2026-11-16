#include "automation/automation.h"

#include <core/sh2/sh2_bus.h>
#include <core/watch.h>
#include <input/input.h>
#include <log/log.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace Automation
{

namespace
{

enum class Op
{
	At,
	Wait,
	WaitUntil,
	Set,
	Hold,
	Release,
	Press,
	Plug,
	MouseMove,
	MouseDown,
	MouseUp,
	MouseClick,
	MouseTo,
	Shot,
	Reboot,
	Exit,
	Echo,
	Speed,
	Assert,
	Pause,
	Resume,
	Step,
	Status,
	SaveState,
	LoadState,
	Dump,
};

enum class CondKind
{
	Write,
	Serial,
	Mem,
};

enum class Cmp
{
	EQ,
	NE,
	LT,
	GT,
	LE,
	GE,
	AND,
};

struct Condition
{
	CondKind kind = CondKind::Mem;
	uint32_t addr = 0;
	int size = 4;
	bool has_value = false;
	uint32_t value = 0;
	Cmp cmp = Cmp::EQ;
	int port = 0;
	std::string text;
};

struct Command
{
	Op op = Op::Echo;
	int buttons = 0;
	int64_t x = 0, y = 0;
	uint64_t n = 0;
	double d = 0;
	std::string text;
	Condition cond;
	uint64_t timeout = 0;
	// The text of the line, for replies and messages; set on the command
	// that completes the line.
	std::string source;
	bool last_of_line = false;
	int line = 0;
};

struct Program
{
	std::string name;
	bool is_control = false;
	std::vector<Command> cmds;
	size_t pc = 0;

	// State of the command at pc while it blocks
	bool started = false;
	uint64_t until = 0;
	uint64_t deadline = 0;
	bool cond_hit = false;
	int watch_id = 0;
	std::string serial_seen;
	bool finished_logged = false;
};

constexpr uint64_t DEFAULT_PRESS_FRAMES = 6;
constexpr size_t SERIAL_KEEP = 64 * 1024;

Hooks hooks;
Program script;
Program control;
bool have_script = false;
bool have_control = false;
bool exiting = false;
bool is_paused = false;
int serial_watch = 0;

// Scripted input
int held_pad = 0;
struct Timed
{
	int mask;
	uint64_t until;
};
std::vector<Timed> timed_pad;
int held_mouse = 0;
std::vector<Timed> timed_mouse;
int applied_mouse = 0;

// Scheduled shots
uint64_t shot_every = 0;
std::set<uint64_t> shot_at;
int64_t last_scheduled_shot = -1;

// Control channel input
std::mutex incoming_mutex;
std::deque<std::string> incoming;
std::atomic<bool> stdin_closed{false};

// ---------------------------------------------------------------- parsing

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

bool tokenize(const std::string& line, std::vector<std::string>& out, std::string& error)
{
	out.clear();
	size_t i = 0;
	while (i < line.size())
	{
		char c = line[i];
		if (std::isspace((unsigned char)c))
		{
			i++;
			continue;
		}
		if (c == '#') break;
		if (c == '"')
		{
			std::string tok;
			i++;
			while (i < line.size() && line[i] != '"')
			{
				if (line[i] == '\\' && i + 1 < line.size())
				{
					char e = line[i + 1];
					tok += (e == 'n') ? '\n' : e;
					i += 2;
					continue;
				}
				tok += line[i++];
			}
			if (i >= line.size())
			{
				error = "unterminated quote";
				return false;
			}
			i++;
			out.push_back(tok);
			continue;
		}
		size_t start = i;
		while (i < line.size() && !std::isspace((unsigned char)line[i])) i++;
		out.push_back(line.substr(start, i - start));
	}
	return true;
}

bool parse_u64(const std::string& s, uint64_t& out)
{
	if (s.empty()) return false;
	try
	{
		size_t used = 0;
		out = std::stoull(s, &used, 0);
		return used == s.size() && s[0] != '-';
	}
	catch (std::exception&)
	{
		return false;
	}
}

bool parse_i64(const std::string& s, int64_t& out)
{
	if (s.empty()) return false;
	try
	{
		size_t used = 0;
		out = std::stoll(s, &used, 0);
		return used == s.size();
	}
	catch (std::exception&)
	{
		return false;
	}
}

bool parse_double(const std::string& s, double& out)
{
	try
	{
		size_t used = 0;
		out = std::stod(s, &used);
		return used == s.size();
	}
	catch (std::exception&)
	{
		return false;
	}
}

bool parse_buttons(const std::string& text, int& mask, std::string& error)
{
	static const std::pair<const char*, int> NAMES[] = {
		{"up", Input::PAD_UP},	 {"down", Input::PAD_DOWN}, {"left", Input::PAD_LEFT}, {"right", Input::PAD_RIGHT},
		{"a", Input::PAD_A},	 {"b", Input::PAD_B},		{"c", Input::PAD_C},	   {"d", Input::PAD_D},
		{"start", Input::PAD_START}, {"l", Input::PAD_L1},	{"r", Input::PAD_R1},	   {"none", 0},
	};
	mask = 0;
	std::stringstream names(lower(text));
	std::string name;
	while (std::getline(names, name, '+'))
	{
		bool found = false;
		for (const auto& [n, bit] : NAMES)
		{
			if (name == n)
			{
				mask |= bit;
				found = true;
				break;
			}
		}
		if (!found)
		{
			error = "unknown button '" + name + "' (up down left right a b c d start l r none)";
			return false;
		}
	}
	return true;
}

bool parse_mouse_button(const std::string& text, int& mask, std::string& error)
{
	std::string t = lower(text);
	if (t == "left" || t == "l")
	{
		mask = Input::MOUSE_L;
		return true;
	}
	if (t == "right" || t == "r")
	{
		mask = Input::MOUSE_R;
		return true;
	}
	error = "expected left or right, got '" + text + "'";
	return false;
}

bool parse_cmp(const std::string& t, Cmp& cmp)
{
	static const std::pair<const char*, Cmp> OPS[] = {
		{"==", Cmp::EQ}, {"!=", Cmp::NE}, {"<", Cmp::LT},  {">", Cmp::GT},
		{"<=", Cmp::LE}, {">=", Cmp::GE}, {"&", Cmp::AND},
	};
	for (const auto& [name, c] : OPS)
	{
		if (t == name)
		{
			cmp = c;
			return true;
		}
	}
	return false;
}

// mem[.b|.w|.l] ADDR OP VALUE, starting at tokens[i]; advances i.
bool parse_mem(const std::vector<std::string>& t, size_t& i, Condition& c, std::string& error)
{
	std::string head = lower(t[i]);
	c.kind = CondKind::Mem;
	c.size = 4;
	if (head == "mem.b") c.size = 1;
	else if (head == "mem.w") c.size = 2;
	else if (head == "mem.l" || head == "mem") c.size = 4;
	else
	{
		error = "expected mem, mem.b, mem.w or mem.l";
		return false;
	}
	uint64_t addr, value;
	if (i + 3 >= t.size() || !parse_u64(t[i + 1], addr) || !parse_cmp(t[i + 2], c.cmp) || !parse_u64(t[i + 3], value))
	{
		error = "expected mem ADDR OP VALUE (OP is == != < > <= >= &)";
		return false;
	}
	c.addr = (uint32_t)addr;
	c.value = (uint32_t)value;
	c.has_value = true;
	i += 4;
	return true;
}

bool parse_condition(const std::vector<std::string>& t, size_t& i, Condition& c, std::string& error)
{
	if (i >= t.size())
	{
		error = "expected a condition (write, serial or mem)";
		return false;
	}
	std::string kind = lower(t[i]);
	if (kind == "write")
	{
		uint64_t addr;
		if (i + 1 >= t.size() || !parse_u64(t[i + 1], addr))
		{
			error = "expected write ADDR [VALUE]";
			return false;
		}
		c.kind = CondKind::Write;
		c.addr = (uint32_t)addr;
		i += 2;
		uint64_t value;
		if (i < t.size() && lower(t[i]) != "timeout" && parse_u64(t[i], value))
		{
			c.has_value = true;
			c.value = (uint32_t)value;
			i++;
		}
		return true;
	}
	if (kind == "serial")
	{
		uint64_t port;
		if (i + 2 >= t.size() || !parse_u64(t[i + 1], port) || port > 1)
		{
			error = "expected serial PORT \"TEXT\" (PORT 0 or 1)";
			return false;
		}
		c.kind = CondKind::Serial;
		c.port = (int)port;
		c.text = t[i + 2];
		i += 3;
		return true;
	}
	if (kind.rfind("mem", 0) == 0)
	{
		return parse_mem(t, i, c, error);
	}
	error = "unknown condition '" + t[i] + "' (write, serial or mem)";
	return false;
}

bool need_args(const std::vector<std::string>& t, size_t i, size_t count, const char* usage, std::string& error)
{
	if (t.size() - i < count + 1)
	{
		error = std::string("usage: ") + usage;
		return false;
	}
	return true;
}

// Parse one line into zero, one or two commands (a frame prefix becomes its
// own command).
bool parse_line(const std::string& line, int line_number, bool is_control, std::vector<Command>& out, std::string& error)
{
	std::vector<std::string> t;
	if (!tokenize(line, t, error)) return false;
	if (t.empty()) return true;

	size_t first = out.size();
	size_t i = 0;
	if (t[0][0] == '@' || t[0][0] == '+')
	{
		uint64_t n;
		if (!parse_u64(t[0].substr(1), n))
		{
			error = "bad frame prefix '" + t[0] + "'";
			return false;
		}
		Command prefix;
		prefix.op = t[0][0] == '@' ? Op::At : Op::Wait;
		prefix.n = n;
		prefix.line = line_number;
		out.push_back(prefix);
		i = 1;
		if (i >= t.size())
		{
			error = "a frame prefix needs a command after it";
			return false;
		}
	}

	Command c;
	c.line = line_number;
	std::string op = lower(t[i]);
	int64_t v;

	if (op == "set" || op == "hold" || op == "release" || op == "press")
	{
		if (!need_args(t, i, 1, "set|hold|release|press BUTTONS [FRAMES]", error)) return false;
		c.op = op == "set" ? Op::Set : op == "hold" ? Op::Hold : op == "release" ? Op::Release : Op::Press;
		if (c.op == Op::Release && lower(t[i + 1]) == "all")
		{
			c.buttons = Input::ALL_PAD_BUTTONS;
		}
		else if (!parse_buttons(t[i + 1], c.buttons, error))
		{
			return false;
		}
		c.n = DEFAULT_PRESS_FRAMES;
		if (c.op == Op::Press && t.size() > i + 2)
		{
			if (!parse_u64(t[i + 2], c.n) || c.n == 0)
			{
				error = "press FRAMES must be a positive number";
				return false;
			}
		}
	}
	else if (op == "plug")
	{
		if (!need_args(t, i, 1, "plug mouse|pad", error)) return false;
		std::string what = lower(t[i + 1]);
		if (what != "mouse" && what != "pad")
		{
			error = "usage: plug mouse|pad";
			return false;
		}
		c.op = Op::Plug;
		c.n = what == "mouse";
	}
	else if (op == "mouse")
	{
		if (!need_args(t, i, 1, "mouse move|down|up|click|to ...", error)) return false;
		std::string sub = lower(t[i + 1]);
		if (sub == "move" || sub == "to")
		{
			c.op = sub == "move" ? Op::MouseMove : Op::MouseTo;
			if (!need_args(t, i, 3, sub == "move" ? "mouse move DX DY" : "mouse to X Y", error)) return false;
			if (!parse_i64(t[i + 2], c.x) || !parse_i64(t[i + 3], c.y))
			{
				error = "expected two numbers";
				return false;
			}
		}
		else if (sub == "down" || sub == "up" || sub == "click")
		{
			c.op = sub == "down" ? Op::MouseDown : sub == "up" ? Op::MouseUp : Op::MouseClick;
			if (!need_args(t, i, 2, "mouse down|up|click left|right [FRAMES]", error)) return false;
			if (!parse_mouse_button(t[i + 2], c.buttons, error)) return false;
			c.n = DEFAULT_PRESS_FRAMES;
			if (c.op == Op::MouseClick && t.size() > i + 3)
			{
				if (!parse_u64(t[i + 3], c.n) || c.n == 0)
				{
					error = "click FRAMES must be a positive number";
					return false;
				}
			}
		}
		else
		{
			error = "unknown mouse command '" + t[i + 1] + "' (move down up click to)";
			return false;
		}
	}
	else if (op == "wait")
	{
		if (!need_args(t, i, 1, "wait FRAMES", error)) return false;
		if (!parse_u64(t[i + 1], c.n))
		{
			error = "usage: wait FRAMES";
			return false;
		}
		c.op = Op::Wait;
	}
	else if (op == "wait-until")
	{
		c.op = Op::WaitUntil;
		size_t j = i + 1;
		if (!parse_condition(t, j, c.cond, error)) return false;
		if (j < t.size())
		{
			if (lower(t[j]) != "timeout" || j + 1 >= t.size() || !parse_u64(t[j + 1], c.timeout))
			{
				error = "expected 'timeout FRAMES' after the condition";
				return false;
			}
			j += 2;
		}
		if (j != t.size())
		{
			error = "unexpected '" + t[j] + "'";
			return false;
		}
	}
	else if (op == "assert")
	{
		c.op = Op::Assert;
		size_t j = i + 1;
		if (!parse_mem(t, j, c.cond, error)) return false;
		if (j < t.size()) c.text = t[j];
	}
	else if (op == "shot")
	{
		c.op = Op::Shot;
		if (t.size() > i + 1) c.text = t[i + 1];
	}
	else if (op == "reboot")
	{
		c.op = Op::Reboot;
	}
	else if (op == "exit")
	{
		c.op = Op::Exit;
		if (t.size() > i + 1)
		{
			if (!parse_i64(t[i + 1], v) || v < 0 || v > 255)
			{
				error = "exit CODE must be 0-255";
				return false;
			}
			c.n = (uint64_t)v;
		}
	}
	else if (op == "echo")
	{
		c.op = Op::Echo;
		for (size_t j = i + 1; j < t.size(); j++) c.text += (j > i + 1 ? " " : "") + t[j];
	}
	else if (op == "speed")
	{
		if (!need_args(t, i, 1, "speed FACTOR (0 = as fast as possible)", error)) return false;
		if (!parse_double(t[i + 1], c.d) || c.d < 0)
		{
			error = "usage: speed FACTOR (0 = as fast as possible)";
			return false;
		}
		c.op = Op::Speed;
	}
	else if (op == "save-state" || op == "load-state")
	{
		if (!need_args(t, i, 1, "save-state|load-state FILE", error)) return false;
		c.op = op == "save-state" ? Op::SaveState : Op::LoadState;
		c.text = t[i + 1];
	}
	else if (op == "dump")
	{
		if (!need_args(t, i, 2, "dump ram|vram|tiles|oam|palette|layers PATH", error)) return false;
		c.op = Op::Dump;
		c.text = lower(t[i + 1]);
		c.cond.text = t[i + 2];
	}
	else if (op == "pause" || op == "resume" || op == "step" || op == "status")
	{
		if (!is_control)
		{
			error = "'" + op + "' only works on the control channel";
			return false;
		}
		c.op = op == "pause" ? Op::Pause : op == "resume" ? Op::Resume : op == "step" ? Op::Step : Op::Status;
		c.n = 1;
		if (c.op == Op::Step && t.size() > i + 1 && (!parse_u64(t[i + 1], c.n) || c.n == 0))
		{
			error = "usage: step [FRAMES]";
			return false;
		}
	}
	else
	{
		error = "unknown command '" + t[i] + "'";
		return false;
	}

	c.source = line;
	c.last_of_line = true;
	out.push_back(c);
	for (size_t k = first; k < out.size(); k++)
	{
		if (out[k].source.empty()) out[k].source = line;
	}
	return true;
}

// ---------------------------------------------------------------- running

void reply(const std::string& text)
{
	if (!have_control) return;
	fputs(text.c_str(), stdout);
	fputc('\n', stdout);
	fflush(stdout);
}

std::string trim_source(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

void fail(Program& p, const Command& c, uint64_t frame, int code, const std::string& message)
{
	std::string where = p.is_control ? std::string("control") : p.name + ":" + std::to_string(c.line);
	Log::error("[Automation] %s: %s", where.c_str(), message.c_str());
	if (p.is_control) reply("error " + std::to_string(frame) + " " + message);
	exiting = true;
	if (hooks.exit) hooks.exit(code, message);
}

uint32_t read_mem(uint32_t addr, int size)
{
	switch (size)
	{
	case 1:
		return SH2::Bus::read8(addr);
	case 2:
		return SH2::Bus::read16(addr);
	default:
		return SH2::Bus::read32(addr);
	}
}

bool compare(uint32_t a, Cmp cmp, uint32_t b)
{
	switch (cmp)
	{
	case Cmp::EQ:
		return a == b;
	case Cmp::NE:
		return a != b;
	case Cmp::LT:
		return a < b;
	case Cmp::GT:
		return a > b;
	case Cmp::LE:
		return a <= b;
	case Cmp::GE:
		return a >= b;
	case Cmp::AND:
		return (a & b) != 0;
	}
	return false;
}

void disarm(Program& p)
{
	if (p.watch_id)
	{
		Watch::remove(p.watch_id);
		p.watch_id = 0;
	}
	p.serial_seen.clear();
}

void arm(Program& p, const Condition& cond)
{
	p.cond_hit = false;
	p.serial_seen.clear();
	if (cond.kind == CondKind::Write)
	{
		// Any write touching ADDR; with a VALUE, a write that starts at ADDR
		// and stores exactly VALUE.
		Program* prog = &p;
		bool has_value = cond.has_value;
		uint32_t want = cond.value;
		uint32_t target = Watch::normalize(cond.addr);
		p.watch_id = Watch::add_write(cond.addr, 1, [prog, has_value, want, target](uint32_t addr, int, uint32_t value, uint32_t) {
			if (!has_value || (addr == target && value == want))
			{
				prog->cond_hit = true;
			}
		});
	}
}

bool condition_met(Program& p, const Condition& cond)
{
	switch (cond.kind)
	{
	case CondKind::Write:
		return p.cond_hit;
	case CondKind::Serial:
		return p.serial_seen.find(cond.text) != std::string::npos;
	case CondKind::Mem:
		return compare(read_mem(cond.addr, cond.size), cond.cmp, cond.value);
	}
	return false;
}

void apply_mouse_mask(int desired)
{
	int pressed = desired & ~applied_mouse;
	int released = applied_mouse & ~desired;
	if (pressed) Input::inject_mouse_buttons(pressed, true);
	if (released) Input::inject_mouse_buttons(released, false);
	applied_mouse = desired;
}

// Run the command at p.pc; true when it is done.
bool execute(Program& p, Command& c, uint64_t frame)
{
	std::string err;
	switch (c.op)
	{
	case Op::At:
		return frame >= c.n;
	case Op::Wait:
		if (!p.started)
		{
			p.started = true;
			p.until = frame + c.n;
		}
		return frame >= p.until;
	case Op::WaitUntil:
		if (!p.started)
		{
			p.started = true;
			p.deadline = c.timeout ? frame + c.timeout : 0;
			arm(p, c.cond);
		}
		if (condition_met(p, c.cond))
		{
			disarm(p);
			return true;
		}
		if (p.deadline && frame >= p.deadline)
		{
			disarm(p);
			fail(p, c, frame, EXIT_TIMEOUT, "timed out: " + trim_source(c.source));
			return false;
		}
		return false;
	case Op::Set:
		held_pad = c.buttons;
		timed_pad.clear();
		return true;
	case Op::Hold:
		held_pad |= c.buttons;
		return true;
	case Op::Release:
		held_pad &= ~c.buttons;
		for (auto& t : timed_pad) t.mask &= ~c.buttons;
		return true;
	case Op::Press:
		timed_pad.push_back({c.buttons, frame + c.n});
		return true;
	case Op::Plug:
		if (hooks.plug) hooks.plug(c.n != 0);
		return true;
	case Op::MouseMove:
		Input::inject_mouse_move((int)c.x, (int)c.y);
		return true;
	case Op::MouseDown:
		held_mouse |= c.buttons;
		return true;
	case Op::MouseUp:
		held_mouse &= ~c.buttons;
		for (auto& t : timed_mouse) t.mask &= ~c.buttons;
		return true;
	case Op::MouseClick:
		timed_mouse.push_back({c.buttons, frame + c.n});
		return true;
	case Op::MouseTo:
		if (!hooks.mouse_to || !hooks.mouse_to((int)c.x, (int)c.y))
		{
			Log::warn("[Automation] mouse to: no cartridge is publishing its cursor (needs input.autodrive)");
		}
		return true;
	case Op::Shot:
	{
		std::string path = hooks.shot ? hooks.shot(c.text) : "";
		if (path.empty())
		{
			Log::error("[Automation] could not save screenshot");
		}
		else if (p.is_control)
		{
			reply("shot " + std::to_string(frame) + " " + path);
		}
		return true;
	}
	case Op::Reboot:
		if (hooks.reboot) hooks.reboot();
		return true;
	case Op::Exit:
		exiting = true;
		if (hooks.exit) hooks.exit((int)c.n, p.is_control ? "control: exit" : p.name + ": exit");
		return true;
	case Op::Echo:
		if (p.is_control) reply("echo " + std::to_string(frame) + " " + c.text);
		else Log::info("[Script] %s", c.text.c_str());
		return true;
	case Op::Speed:
		if (hooks.set_speed) hooks.set_speed(c.d);
		return true;
	case Op::Assert:
	{
		uint32_t value = read_mem(c.cond.addr, c.cond.size);
		if (!compare(value, c.cond.cmp, c.cond.value))
		{
			char buf[96];
			snprintf(buf, sizeof buf, " (read %X)", value);
			fail(p, c, frame, EXIT_ASSERT, "assert failed: " + (c.text.empty() ? trim_source(c.source) : c.text) + buf);
			return false;
		}
		return true;
	}
	case Op::Pause:
		is_paused = true;
		return true;
	case Op::Resume:
		is_paused = false;
		return true;
	case Op::Step:
		if (!p.started)
		{
			p.started = true;
			p.until = frame + c.n;
			is_paused = false;
		}
		if (frame >= p.until)
		{
			is_paused = true;
			return true;
		}
		return false;
	case Op::Status:
	{
		std::string s = "status frame=" + std::to_string(frame) + " paused=" + (is_paused ? "1" : "0");
		if (hooks.status) s += " " + hooks.status();
		reply(s);
		return true;
	}
	case Op::SaveState:
	case Op::LoadState:
	{
		auto& fn = c.op == Op::SaveState ? hooks.save_state : hooks.load_state;
		if (!fn)
		{
			err = "savestates are not available";
		}
		else if (fn(c.text, err))
		{
			return true;
		}
		fail(p, c, frame, EXIT_USAGE, err);
		return false;
	}
	case Op::Dump:
		if (!hooks.dump)
		{
			err = "dumps are not available";
		}
		else if (hooks.dump(c.text, c.cond.text, err))
		{
			return true;
		}
		fail(p, c, frame, EXIT_USAGE, err);
		return false;
	}
	return true;
}

void run_program(Program& p, uint64_t frame)
{
	while (!exiting && p.pc < p.cmds.size())
	{
		Command& c = p.cmds[p.pc];
		if (!execute(p, c, frame)) return;
		if (p.is_control && c.last_of_line && c.op != Op::Status)
		{
			reply("ok " + std::to_string(frame) + " " + trim_source(c.source));
		}
		p.pc++;
		p.started = false;
	}
	if (!p.is_control && p.pc >= p.cmds.size() && !p.finished_logged && !p.cmds.empty())
	{
		p.finished_logged = true;
		Log::info("[Automation] %s finished at frame %llu", p.name.c_str(), (unsigned long long)frame);
	}
}

void drain_control(uint64_t frame)
{
	std::deque<std::string> lines;
	{
		std::lock_guard<std::mutex> lock(incoming_mutex);
		lines.swap(incoming);
	}
	for (const auto& line : lines)
	{
		std::vector<Command> cmds;
		std::string error;
		if (!parse_line(line, 0, true, cmds, error))
		{
			reply("error " + std::to_string(frame) + " " + error + ": " + trim_source(line));
			continue;
		}
		if (cmds.empty()) continue;
		for (auto& c : cmds) control.cmds.push_back(c);
	}
}

void apply_input(uint64_t frame)
{
	int pad = held_pad;
	for (const auto& t : timed_pad)
	{
		if (frame < t.until) pad |= t.mask;
	}
	timed_pad.erase(
		std::remove_if(timed_pad.begin(), timed_pad.end(), [frame](const Timed& t) { return frame >= t.until; }),
		timed_pad.end()
	);
	Input::inject_pad(pad);

	int mouse = held_mouse;
	for (const auto& t : timed_mouse)
	{
		if (frame < t.until) mouse |= t.mask;
	}
	timed_mouse.erase(
		std::remove_if(timed_mouse.begin(), timed_mouse.end(), [frame](const Timed& t) { return frame >= t.until; }),
		timed_mouse.end()
	);
	apply_mouse_mask(mouse);
}

void on_serial_byte(int port, uint8_t byte)
{
	for (Program* p : {&script, &control})
	{
		if (p->pc >= p->cmds.size() || !p->started) continue;
		const Command& c = p->cmds[p->pc];
		if (c.op == Op::WaitUntil && c.cond.kind == CondKind::Serial && c.cond.port == port)
		{
			p->serial_seen += (char)byte;
			if (p->serial_seen.size() > SERIAL_KEEP) p->serial_seen.erase(0, p->serial_seen.size() - SERIAL_KEEP / 2);
		}
	}
}

}  // namespace

void initialize(const Hooks& h)
{
	hooks = h;
	script = {};
	control = {};
	script.name = "script";
	control.name = "control";
	control.is_control = true;
	have_script = have_control = exiting = is_paused = false;
	held_pad = held_mouse = applied_mouse = 0;
	timed_pad.clear();
	timed_mouse.clear();
	serial_watch = Watch::add_serial(on_serial_byte);
}

void shutdown()
{
	disarm(script);
	disarm(control);
	if (serial_watch)
	{
		Watch::remove(serial_watch);
		serial_watch = 0;
	}
}

bool load_script(const std::string& path, std::string& error)
{
	std::ifstream file(path);
	if (!file.is_open())
	{
		error = "cannot open script " + path;
		return false;
	}
	script.name = path;
	std::string line;
	int number = 0;
	while (std::getline(file, line))
	{
		number++;
		std::string err;
		if (!parse_line(line, number, false, script.cmds, err))
		{
			error = path + ":" + std::to_string(number) + ": " + err;
			return false;
		}
	}
	have_script = true;
	Input::set_user_input_blocked(true);
	return true;
}

void start_control_stdin()
{
	have_control = true;
	Input::set_user_input_blocked(true);
	std::thread([] {
		std::string line;
		while (std::getline(std::cin, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			std::lock_guard<std::mutex> lock(incoming_mutex);
			incoming.push_back(line);
		}
		stdin_closed = true;
	}).detach();
}

bool control_enabled()
{
	return have_control;
}

void set_scheduled_shots(uint64_t every, const std::vector<uint64_t>& at)
{
	shot_every = every;
	shot_at = std::set<uint64_t>(at.begin(), at.end());
}

void update(uint64_t frame)
{
	if (exiting) return;

	if (have_control) drain_control(frame);
	if (have_script) run_program(script, frame);
	if (have_control) run_program(control, frame);
	if (exiting) return;

	if (have_script || have_control) apply_input(frame);

	if ((int64_t)frame != last_scheduled_shot && ((shot_every && frame && frame % shot_every == 0) || shot_at.count(frame)))
	{
		last_scheduled_shot = (int64_t)frame;
		if (hooks.shot) hooks.shot("");
	}

	if (have_control && stdin_closed && control.pc >= control.cmds.size())
	{
		bool empty;
		{
			std::lock_guard<std::mutex> lock(incoming_mutex);
			empty = incoming.empty();
		}
		if (empty)
		{
			exiting = true;
			if (hooks.exit) hooks.exit(EXIT_OK, "control input closed");
		}
	}
}

bool paused()
{
	return is_paused && !exiting;
}

void set_paused(bool p)
{
	is_paused = p;
}

void run_line(const std::string& line)
{
	std::lock_guard<std::mutex> lock(incoming_mutex);
	incoming.push_back(line);
}

}  // namespace Automation
