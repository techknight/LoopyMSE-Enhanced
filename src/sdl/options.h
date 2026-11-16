#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Options
{

// Every setting lives in one namespace, "section.key". The ini file sets
// them in [section] blocks and the command line overrides any of them as
// --section.key=value (see print_settings() or --help-config for the list).
struct Args
{
	std::string cart;

	// [emulator]
	std::string bios = "bios.bin";
	std::string sound_bios = "soundbios.bin";
	bool run_in_background = false;
	double speed = 1.0;  // 1 = real time, 2 = twice as fast, 0 = as fast as possible

	// [video]
	bool fullscreen = false;
	int scale = 3;
	bool correct_aspect_ratio = true;
	bool crop_overscan = true;
	bool antialias = true;

	// [input]
	std::string device = "pad";  // pad | mouse | auto
	bool autodrive = false;
	std::map<std::string, std::string> keyboard;    // pad button -> SDL key name
	std::map<std::string, std::string> controller;  // pad button -> SDL controller button name

	// [sound]
	std::string sound_output = "device";  // device | none

	// [cart]
	bool sram_write = true;
	std::string sram_path;  // empty = <rom name>.sav beside the ROM, or in paths.output

	// [paths]
	std::string output_dir;
	std::string screenshot_dir;
	std::string print_dir;
	std::string state_dir;

	// [screenshot]
	std::string screenshot_image_type = "png";

	// [printer]
	std::string printer_image_type = "png";
	bool printer_view = true;
	std::string printer_view_command = "(OPEN)";
	bool printer_correct_aspect_ratio = true;
	int printer_cassette = 1;

	// [debug]
	std::string log_level = "info";
	std::string log_file;
	bool log_frames = false;
	bool strict = false;
	std::string serial_log[2];
	bool serial_log_timed = false;
	std::string watch_writes;
	std::string watch_log;
	bool trace_io = false;
	bool raster_warnings = false;

	// [automation]
	std::string script;
	std::string control = "none";  // none | stdin
	std::string window = "normal";  // normal | minimized | hidden
	bool start_paused = false;
	std::string load_state;
	uint64_t exit_after_frames = 0;
	uint64_t shot_every = 0;
	std::vector<uint64_t> shot_at;

	// [record]
	std::string record_video;
	std::string record_audio;
	std::string record_frames;
	int record_scale = 3;
	int record_quality = 16;
	std::string record_ffmpeg = "ffmpeg";

	// [profile]
	std::string profile_file;
	uint64_t profile_from = 0;
	uint64_t profile_to = 0;
	uint32_t profile_watch = 0;

	// [timing]
	std::string timing_model = "stock";  // stock | console
	double cart_rom_cycles = 0.0;

	// Keys given on the command line (they win over everything).
	std::set<std::string> cli_keys;

	// Nobody is expected to be watching or playing: a script, the control
	// channel, a hidden or minimised window, scheduled screenshots, a frame
	// limit or a profile.
	bool unattended() const;
	// Automation owns the pad and mouse; real input is ignored.
	bool automation_owns_input() const;
};

enum ParseResult
{
	PARSE_OK,
	PARSE_EXIT_OK,     // --help, --version etc. printed what was asked
	PARSE_EXIT_ERROR,  // bad option or value, already reported
};

// Reads the command line, then the ini file it names (or the default one at
// default_ini), and fills args. Command-line values override the ini file.
ParseResult parse(int argc, char** argv, const fs::path& default_ini, const fs::path& default_ini_source, Args& args);

void print_usage();

}  // namespace Options
