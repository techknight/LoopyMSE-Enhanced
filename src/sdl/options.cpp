#include "options.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>

#include "config.h"
#include "log/log.h"

namespace Options
{

namespace
{

struct Setting
{
	std::string key;
	std::string help;
	bool is_bool = false;
	std::function<bool(const std::string& value, std::string& error)> parse;
	std::function<std::string()> current;
};

std::string trim(const std::string& s)
{
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos) return "";
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

bool parse_bool(const std::string& text, bool& out)
{
	std::string v = lower(trim(text));
	if (v == "true" || v == "1" || v == "yes" || v == "on")
	{
		out = true;
		return true;
	}
	if (v == "false" || v == "0" || v == "no" || v == "off")
	{
		out = false;
		return true;
	}
	return false;
}

bool parse_u64(const std::string& text, uint64_t& out)
{
	std::string v = trim(text);
	if (v.empty()) return false;
	try
	{
		size_t used = 0;
		out = std::stoull(v, &used, 0);
		return used == v.size();
	}
	catch (std::exception&)
	{
		return false;
	}
}

bool parse_double(const std::string& text, double& out)
{
	std::string v = trim(text);
	if (v.empty()) return false;
	try
	{
		size_t used = 0;
		out = std::stod(v, &used);
		return used == v.size();
	}
	catch (std::exception&)
	{
		return false;
	}
}

std::string fmt_double(double d)
{
	char buf[32];
	snprintf(buf, sizeof buf, "%g", d);
	return buf;
}

class Registry
{
public:
	std::vector<Setting> settings;

	void add_bool(const char* key, bool& field, const char* help)
	{
		Setting s;
		s.key = key;
		s.help = help;
		s.is_bool = true;
		s.parse = [&field](const std::string& v, std::string& err) {
			if (parse_bool(v, field)) return true;
			err = "expected true or false";
			return false;
		};
		s.current = [&field]() { return std::string(field ? "true" : "false"); };
		settings.push_back(s);
	}

	void add_int(const char* key, int& field, int min, int max, const char* help)
	{
		Setting s;
		s.key = key;
		s.help = help;
		s.parse = [&field, min, max](const std::string& v, std::string& err) {
			uint64_t n;
			if (!parse_u64(v, n) || (int64_t)n < min || (int64_t)n > max)
			{
				err = "expected a whole number from " + std::to_string(min) + " to " + std::to_string(max);
				return false;
			}
			field = (int)n;
			return true;
		};
		s.current = [&field]() { return std::to_string(field); };
		settings.push_back(s);
	}

	void add_u64(const char* key, uint64_t& field, const char* help)
	{
		Setting s;
		s.key = key;
		s.help = help;
		s.parse = [&field](const std::string& v, std::string& err) {
			if (parse_u64(v, field)) return true;
			err = "expected a whole number";
			return false;
		};
		s.current = [&field]() { return std::to_string(field); };
		settings.push_back(s);
	}

	void add_hex32(const char* key, uint32_t& field, const char* help)
	{
		Setting s;
		s.key = key;
		s.help = help;
		s.parse = [&field](const std::string& v, std::string& err) {
			std::string t = trim(v);
			if (t.empty())
			{
				field = 0;
				return true;
			}
			uint64_t n;
			if (t.rfind("0x", 0) != 0 && t.rfind("0X", 0) != 0) t = "0x" + t;
			if (!parse_u64(t, n) || n > 0xFFFFFFFFull)
			{
				err = "expected a hexadecimal address";
				return false;
			}
			field = (uint32_t)n;
			return true;
		};
		s.current = [&field]() {
			if (!field) return std::string();
			char buf[16];
			snprintf(buf, sizeof buf, "%08X", field);
			return std::string(buf);
		};
		settings.push_back(s);
	}

	void add_double(const char* key, double& field, double min, const char* help)
	{
		Setting s;
		s.key = key;
		s.help = help;
		s.parse = [&field, min](const std::string& v, std::string& err) {
			double d;
			if (!parse_double(v, d) || d < min)
			{
				err = "expected a number of at least " + fmt_double(min);
				return false;
			}
			field = d;
			return true;
		};
		s.current = [&field]() { return fmt_double(field); };
		settings.push_back(s);
	}

	void add_string(const char* key, std::string& field, const char* help)
	{
		Setting s;
		s.key = key;
		s.help = help;
		s.parse = [&field](const std::string& v, std::string&) {
			field = trim(v);
			return true;
		};
		s.current = [&field]() { return field; };
		settings.push_back(s);
	}

	void add_choice(const char* key, std::string& field, std::vector<std::string> choices, const char* help)
	{
		Setting s;
		s.key = key;
		std::string all;
		for (const auto& c : choices) all += (all.empty() ? "" : " | ") + c;
		s.help = std::string(help) + " (" + all + ")";
		s.parse = [&field, choices, all](const std::string& v, std::string& err) {
			std::string t = lower(trim(v));
			if (std::find(choices.begin(), choices.end(), t) == choices.end())
			{
				err = "expected one of: " + all;
				return false;
			}
			field = t;
			return true;
		};
		s.current = [&field]() { return field; };
		settings.push_back(s);
	}

	void add_frame_list(const char* key, std::vector<uint64_t>& field, const char* help)
	{
		Setting s;
		s.key = key;
		s.help = help;
		s.parse = [&field](const std::string& v, std::string& err) {
			std::vector<uint64_t> frames;
			std::stringstream list(v);
			std::string item;
			while (std::getline(list, item, ','))
			{
				if (trim(item).empty()) continue;
				uint64_t n;
				if (!parse_u64(item, n))
				{
					err = "expected comma-separated frame numbers";
					return false;
				}
				frames.push_back(n);
			}
			field = frames;
			return true;
		};
		s.current = [&field]() {
			std::string out;
			for (uint64_t f : field) out += (out.empty() ? "" : ",") + std::to_string(f);
			return out;
		};
		settings.push_back(s);
	}

	void add_map_entry(const char* key, std::map<std::string, std::string>& map, const char* entry, const char* help)
	{
		Setting s;
		s.key = key;
		s.help = help;
		std::string name = entry;
		s.parse = [&map, name](const std::string& v, std::string&) {
			map[name] = trim(v);
			return true;
		};
		s.current = [&map, name]() {
			auto it = map.find(name);
			return it == map.end() ? std::string() : it->second;
		};
		settings.push_back(s);
	}

	Setting* find(const std::string& key)
	{
		for (auto& s : settings)
		{
			if (s.key == key) return &s;
		}
		return nullptr;
	}
};

void register_settings(Registry& r, Args& a)
{
	r.add_string("emulator.bios", a.bios, "BIOS file");
	r.add_string("emulator.sound_bios", a.sound_bios, "Sound BIOS file");
	r.add_bool("emulator.run_in_background", a.run_in_background, "Keep running while the window is not focused");
	r.add_double("emulator.speed", a.speed, 0.0, "Emulation speed: 1 = real time, 2 = double, 0 = as fast as possible");

	r.add_bool("video.fullscreen", a.fullscreen, "Start in fullscreen");
	r.add_int("video.scale", a.scale, 1, 10, "Integer window scale");
	r.add_bool("video.correct_aspect_ratio", a.correct_aspect_ratio, "Stretch display pixels to 4:3");
	r.add_bool("video.crop_overscan", a.crop_overscan, "Crop border and overscan areas");
	r.add_bool("video.antialias", a.antialias, "Smooth scaling (recommended with aspect ratio correction)");

	r.add_choice(
		"input.device", a.device, {"pad", "mouse", "auto"},
		"Controller plugged in at startup; auto plugs the mouse for known mouse games (F1 pad, F2 mouse at runtime)"
	);
	r.add_bool(
		"input.autodrive", a.autodrive,
		"Let a cooperating cartridge steer the emulated mouse (see docs/homebrew-debug-interface.md)"
	);

	static const char* PAD_NAMES[] = {"up", "down", "left", "right", "start", "a", "b", "c", "d", "l", "r"};
	static const char* KEY_DEFAULTS[] = {"up", "down", "left", "right", "return", "z", "x", "c", "v", "q", "w"};
	static const char* BUTTON_DEFAULTS[] = {"dpup", "dpdown", "dpleft", "dpright",      "start",        "a",
											"b",	"y",	  "x",		"leftshoulder", "rightshoulder"};
	static std::vector<std::string> keyboard_keys, controller_keys;
	if (keyboard_keys.empty())
	{
		for (const char* name : PAD_NAMES)
		{
			keyboard_keys.push_back(std::string("keyboard.") + name);
			controller_keys.push_back(std::string("controller.") + name);
		}
	}
	for (size_t i = 0; i < std::size(PAD_NAMES); i++)
	{
		a.keyboard[PAD_NAMES[i]] = KEY_DEFAULTS[i];
		a.controller[PAD_NAMES[i]] = BUTTON_DEFAULTS[i];
		r.add_map_entry(keyboard_keys[i].c_str(), a.keyboard, PAD_NAMES[i], "Key for this pad button (SDL key name)");
		r.add_map_entry(
			controller_keys[i].c_str(), a.controller, PAD_NAMES[i], "Controller button for this pad button (SDL name)"
		);
	}

	r.add_choice("sound.output", a.sound_output, {"device", "none"}, "Where sound goes");

	r.add_bool("cart.sram_write", a.sram_write, "Write the cartridge save (SRAM) back to its .sav file");
	r.add_string("cart.sram", a.sram_path, "Save file to use instead of <rom name>.sav");
	r.add_choice(
		"cart.det_wiring", a.det_wiring, {"auto", "vcc", "pb1"},
		"Cartridge detect line: tied high (vcc), or to PB1 like boards with the Magical Shop unlock; auto knows the retail boards"
	);

	r.add_string(
		"paths.output", a.output_dir,
		"Folder for everything a run writes (screenshots, prints, save, log); empty = beside the ROM"
	);
	r.add_string("paths.screenshots", a.screenshot_dir, "Folder for screenshots (overrides paths.output)");
	r.add_string("paths.prints", a.print_dir, "Folder for printed stickers (overrides paths.output)");
	r.add_string("paths.states", a.state_dir, "Folder for savestate slots (F5-F8; overrides paths.output)");

	r.add_choice("screenshot.image_type", a.screenshot_image_type, {"png", "bmp", "jpg"}, "Screenshot file type");

	r.add_choice("printer.image_type", a.printer_image_type, {"png", "bmp", "jpg"}, "Print file type");
	r.add_bool("printer.view", a.printer_view, "Open each print in a viewer");
	r.add_string(
		"printer.view_command", a.printer_view_command,
		"Viewer command; $FILE is replaced by the print's path, (OPEN) uses the system viewer"
	);
	r.add_bool("printer.correct_aspect_ratio", a.printer_correct_aspect_ratio, "Stretch prints to the sticker's shape");
	r.add_int(
		"printer.cassette", a.printer_cassette, 0, 7,
		"Sticker cassette the program sees: 0 = none, 1 = XS-11 (the type prints are made for), 2-7 = other types"
	);

	r.add_choice(
		"debug.log_level", a.log_level, {"verbose", "trace", "debug", "info", "warn", "error"}, "Least important log level shown"
	);
	r.add_string("debug.log_file", a.log_file, "Write the log to this file instead of the console");
	r.add_bool("debug.log_frames", a.log_frames, "Prefix log lines with the emulated frame number");
	r.add_bool(
		"debug.strict", a.strict, "Stop with exit code 2 when the program uses hardware the emulator does not implement"
	);
	r.add_string("debug.serial0_log", a.serial_log[0], "Write what serial port 0 sends (usually printf) to this file");
	r.add_string("debug.serial1_log", a.serial_log[1], "Write what serial port 1 sends (the MIDI stream) to this file");
	r.add_bool(
		"debug.serial_log_timed", a.serial_log_timed, "Serial logs as one 'FRAME CYCLE BYTE' line per byte instead of raw bytes"
	);
	r.add_string(
		"debug.watch_writes", a.watch_writes, "Log writes to these addresses: ADDR[:LENGTH][,ADDR[:LENGTH]...]"
	);
	r.add_string("debug.watch_log", a.watch_log, "File for debug.watch_writes (empty = the log)");
	r.add_bool("debug.trace_io", a.trace_io, "Log every pad, mouse and sensor register access");
	r.add_bool(
		"debug.raster_warnings", a.raster_warnings,
		"Report palette and OAM writes during active display (they show as dots on a console)"
	);

	r.add_string("automation.script", a.script, "Command script to run (see docs/automation.md)");
	r.add_choice("automation.control", a.control, {"none", "stdin"}, "Accept commands while running");
	r.add_choice("automation.window", a.window, {"normal", "minimized", "hidden"}, "Window mode");
	r.add_bool(
		"automation.start_paused", a.start_paused, "Hold emulation at frame 0 until the control channel says resume or step"
	);
	r.add_string("automation.load_state", a.load_state, "Load this savestate before the first frame");
	r.add_u64("automation.exit_after_frames", a.exit_after_frames, "Quit after this many frames (0 = never)");
	r.add_u64("automation.shot_every", a.shot_every, "Save a screenshot every N frames (0 = never)");
	r.add_frame_list("automation.shot_at", a.shot_at, "Save screenshots at these frames (comma-separated)");

	r.add_string("record.video", a.record_video, "Record video with sound to this file (.mp4, .mkv; needs ffmpeg)");
	r.add_string("record.audio", a.record_audio, "Record the sound to this .wav file");
	r.add_string("record.frames", a.record_frames, "Save every frame as a PNG in this folder");
	r.add_int("record.scale", a.record_scale, 1, 10, "Video: integer pixel scale");
	r.add_int("record.quality", a.record_quality, 0, 51, "Video: x264 quality, lower is better (CRF)");
	r.add_string("record.ffmpeg", a.record_ffmpeg, "ffmpeg program to use for video");

	r.add_string("profile.file", a.profile_file, "Write a CPU profile (cycles per program address) to this file");
	r.add_u64("profile.from", a.profile_from, "First frame of the profile");
	r.add_u64("profile.to", a.profile_to, "Frame at which the profile stops and is written");
	r.add_hex32("profile.watch", a.profile_watch, "Address of a 32-bit value recorded at the profile's start and end");

	r.add_choice("timing.model", a.timing_model, {"stock", "console"}, "CPU timing: stock, or measured console costs");
	r.add_double(
		"timing.cart_rom_cycles", a.cart_rom_cycles, 0.0,
		"Console model: cycles a cartridge ROM access takes, overriding WCR3 (0 = follow WCR3)"
	);
}

struct Alias
{
	const char* name;
	const char* key;
	const char* fixed_value;  // nullptr: the alias takes a value
	const char* help;
};

const Alias ALIASES[] = {
	{"cart", nullptr, nullptr, "ROM to load (or give its path as the first argument)"},
	{"bios", "emulator.bios", nullptr, "BIOS file"},
	{"sound-bios", "emulator.sound_bios", nullptr, "Sound BIOS file"},
	{"verbose", "debug.log_level", "debug", "Verbose logging (debug.log_level=debug)"},
	{"v", "debug.log_level", "debug", "Same as --verbose"},
	{"script", "automation.script", nullptr, "Run a command script"},
	{"control", "automation.control", "stdin", "Accept commands on stdin (automation.control=stdin)"},
	{"hidden", "automation.window", "hidden", "Run without a visible window"},
	{"paused", "automation.start_paused", "true", "Start paused (with --control: resume or step to run)"},
	{"fast", "emulator.speed", "0", "Run as fast as possible (emulator.speed=0)"},
	{"frames", "automation.exit_after_frames", nullptr, "Quit after N frames"},
	{"out", "paths.output", nullptr, "Folder for everything the run writes"},
	{"mouse", "input.device", "mouse", "Plug in the mouse at startup"},
	{"autodrive", "input.autodrive", "true", "Let a cooperating cartridge steer the mouse"},
	{"no-sram-write", "cart.sram_write", "false", "Never write the save file"},
	{"strict", "debug.strict", "true", "Fail on unimplemented hardware (exit code 2)"},
	{"log", "debug.log_file", nullptr, "Write the log to a file"},
	{"profile", "profile.file", nullptr, "Write a CPU profile to a file"},
	{"record", "record.video", nullptr, "Record video with sound (needs ffmpeg)"},
	{"load-state", "automation.load_state", nullptr, "Start from a savestate"},
};

const Alias* find_alias(const std::string& name)
{
	for (const auto& alias : ALIASES)
	{
		if (name == alias.name) return &alias;
	}
	return nullptr;
}

void print_aliases()
{
	std::cout << "Usage: LoopyMSE [ROM] [options]\n\n"
			  << "Any setting can be given as --section.key=value (see --help-config).\n"
			  << "Shortcuts:\n";
	for (const auto& alias : ALIASES)
	{
		std::string left = std::string(strlen(alias.name) == 1 ? "-" : "--") + alias.name;
		if (!alias.fixed_value) left += " VALUE";
		std::cout << "  " << left << std::string(left.size() < 24 ? 24 - left.size() : 1, ' ') << alias.help << "\n";
	}
	std::cout << "  --ini FILE              Use this ini file instead of the default one\n"
			  << "  --no-ini                Use built-in defaults only (and the command line)\n"
			  << "  --set KEY=VALUE         Set any setting (same as --KEY=VALUE)\n"
			  << "  --write-ini FILE        Write the effective settings to an ini file and quit\n"
			  << "  --help-config           List every setting with its current value\n"
			  << "  --version               Print the version\n"
			  << "  --help                  This text\n";
}

void print_settings(Registry& r)
{
	std::string section;
	for (const auto& s : r.settings)
	{
		std::string sec = s.key.substr(0, s.key.find('.'));
		if (sec != section)
		{
			section = sec;
			std::cout << "\n[" << section << "]\n";
		}
		std::cout << "  " << s.key << " = " << s.current() << "\n      " << s.help << "\n";
	}
}

bool write_ini(Registry& r, const std::string& path)
{
	std::ofstream file(path, std::ios::binary);  //LF line endings on every platform
	if (!file.is_open()) return false;
	std::string section;
	for (const auto& s : r.settings)
	{
		std::string sec = s.key.substr(0, s.key.find('.'));
		std::string name = s.key.substr(s.key.find('.') + 1);
		if (sec != section)
		{
			if (!section.empty()) file << "\n";
			section = sec;
			file << "[" << section << "]\n";
		}
		file << "# " << s.help << "\n" << name << "=" << s.current() << "\n";
	}
	return true;
}

bool apply(Registry& r, const std::string& key, const std::string& value, const std::string& where)
{
	Setting* s = r.find(key);
	if (!s)
	{
		Log::error("%s: unknown setting '%s'", where.c_str(), key.c_str());
		return false;
	}
	std::string err;
	if (!s->parse(value, err))
	{
		Log::error("%s: bad value '%s' for %s: %s", where.c_str(), value.c_str(), key.c_str(), err.c_str());
		return false;
	}
	return true;
}

bool load_ini(Registry& r, const fs::path& path)
{
	std::ifstream file(path);
	if (!file.is_open()) return false;

	std::string line, section;
	int number = 0;
	while (std::getline(file, line))
	{
		number++;
		std::string t = trim(line);
		if (t.empty() || t[0] == '#' || t[0] == ';') continue;
		std::string where = path.string() + ":" + std::to_string(number);
		if (t.front() == '[' && t.back() == ']')
		{
			section = lower(trim(t.substr(1, t.size() - 2)));
			continue;
		}
		size_t eq = t.find('=');
		if (eq == std::string::npos)
		{
			Log::warn("%s: ignoring line without '='", where.c_str());
			continue;
		}
		std::string key = section + "." + lower(trim(t.substr(0, eq)));
		if (!r.find(key))
		{
			Log::warn("%s: ignoring unknown setting '%s'", where.c_str(), key.c_str());
			continue;
		}
		// A bad value in the ini file is reported and the default kept.
		apply(r, key, t.substr(eq + 1), where);
	}
	return true;
}

}  // namespace

bool Args::unattended() const
{
	return !script.empty() || control != "none" || window != "normal" || exit_after_frames || shot_every ||
		   !shot_at.empty() || !profile_file.empty();
}

bool Args::automation_owns_input() const
{
	return !script.empty() || control != "none";
}

void print_usage()
{
	print_aliases();
}

ParseResult parse(int argc, char** argv, const fs::path& default_ini, const fs::path& default_ini_source, Args& args)
{
	Registry r;
	register_settings(r, args);

	// First pass: collect the command line, so --ini can pick the file and
	// command-line values can be applied after it.
	std::vector<std::pair<std::string, std::string>> cli;
	fs::path ini_path = default_ini;
	bool use_ini = true;
	bool want_help_config = false;
	std::string write_ini_path;
	bool positional_only = false;

	for (int i = 1; i < argc; i++)
	{
		std::string arg = argv[i];
		if (positional_only || arg.empty() || arg[0] != '-' || arg == "-")
		{
			if (!args.cart.empty())
			{
				Log::error("Only one ROM can be given (got '%s' and '%s')", args.cart.c_str(), arg.c_str());
				return PARSE_EXIT_ERROR;
			}
			args.cart = arg;
			continue;
		}
		if (arg == "--")
		{
			positional_only = true;
			continue;
		}

		std::string name = arg.substr(arg[1] == '-' ? 2 : 1);
		std::string value;
		bool has_value = false;
		size_t eq = name.find('=');
		if (eq != std::string::npos)
		{
			value = name.substr(eq + 1);
			name = name.substr(0, eq);
			has_value = true;
		}
		auto take_value = [&](const char* what) -> bool {
			if (has_value) return true;
			if (i + 1 >= argc)
			{
				Log::error("--%s needs %s", name.c_str(), what);
				return false;
			}
			value = argv[++i];
			has_value = true;
			return true;
		};

		if (name == "help" || name == "h")
		{
			print_aliases();
			return PARSE_EXIT_OK;
		}
		if (name == "version")
		{
			std::cout << PROJECT_DESCRIPTION << " " << PROJECT_VERSION << "\n";
			return PARSE_EXIT_OK;
		}
		if (name == "help-config")
		{
			want_help_config = true;
			continue;
		}
		if (name == "ini")
		{
			if (!take_value("a file")) return PARSE_EXIT_ERROR;
			ini_path = value;
			continue;
		}
		if (name == "no-ini")
		{
			use_ini = false;
			continue;
		}
		if (name == "write-ini")
		{
			if (!take_value("a file")) return PARSE_EXIT_ERROR;
			write_ini_path = value;
			continue;
		}
		if (name == "set")
		{
			if (!take_value("KEY=VALUE")) return PARSE_EXIT_ERROR;
			size_t e = value.find('=');
			if (e == std::string::npos)
			{
				Log::error("--set needs KEY=VALUE, got '%s'", value.c_str());
				return PARSE_EXIT_ERROR;
			}
			cli.emplace_back(lower(trim(value.substr(0, e))), value.substr(e + 1));
			continue;
		}

		if (const Alias* alias = find_alias(name))
		{
			if (!alias->key)
			{
				// --cart
				if (!take_value("a ROM path")) return PARSE_EXIT_ERROR;
				args.cart = value;
				continue;
			}
			if (alias->fixed_value)
			{
				if (has_value)
				{
					Log::error("--%s takes no value", name.c_str());
					return PARSE_EXIT_ERROR;
				}
				cli.emplace_back(alias->key, alias->fixed_value);
				continue;
			}
			if (!take_value("a value")) return PARSE_EXIT_ERROR;
			cli.emplace_back(alias->key, value);
			continue;
		}

		std::string key = lower(name);
		Setting* s = r.find(key);
		if (!s)
		{
			Log::error("Unknown option --%s (see --help and --help-config)", name.c_str());
			return PARSE_EXIT_ERROR;
		}
		if (!has_value)
		{
			if (s->is_bool)
			{
				value = "true";
			}
			else if (!take_value("a value"))
			{
				return PARSE_EXIT_ERROR;
			}
		}
		cli.emplace_back(key, value);
	}

	if (use_ini)
	{
		std::error_code ec;
		if (ini_path == default_ini && !fs::exists(default_ini, ec) && default_ini_source != default_ini &&
			fs::exists(default_ini_source, ec))
		{
			Log::info("Creating default ini file at %s", default_ini.string().c_str());
			fs::copy_file(default_ini_source, default_ini, ec);
		}
		if (!load_ini(r, ini_path))
		{
			if (ini_path != default_ini)
			{
				Log::error("Could not read ini file %s", ini_path.string().c_str());
				return PARSE_EXIT_ERROR;
			}
			Log::info("No ini file at %s, using defaults", ini_path.string().c_str());
		}
	}

	for (const auto& [key, value] : cli)
	{
		if (!apply(r, key, value, "command line")) return PARSE_EXIT_ERROR;
		args.cli_keys.insert(key);
	}

	if (want_help_config)
	{
		print_settings(r);
		return PARSE_EXIT_OK;
	}
	if (!write_ini_path.empty())
	{
		if (!write_ini(r, write_ini_path))
		{
			Log::error("Could not write %s", write_ini_path.c_str());
			return PARSE_EXIT_ERROR;
		}
		Log::info("Wrote settings to %s", write_ini_path.c_str());
		return PARSE_EXIT_OK;
	}

	// Unattended runs: nobody is watching or listening, so do not pause when
	// the window loses focus, keep prints out of the viewer and sound off the
	// speakers. Values given on the command line still win.
	if (args.unattended())
	{
		auto set_default = [&](const char* key, const char* value) {
			if (!args.cli_keys.count(key)) apply(r, key, value, "unattended run");
		};
		set_default("emulator.run_in_background", "true");
		set_default("printer.view", "false");
		if (args.window != "normal" || args.speed == 0.0)
		{
			set_default("sound.output", "none");
		}
	}
	return PARSE_OK;
}

}  // namespace Options
