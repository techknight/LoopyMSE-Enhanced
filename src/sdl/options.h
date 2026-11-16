#pragma once
#include <boost/program_options.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Options
{

struct Args
{
	std::string cart;
	std::string bios;
	std::string sound_bios;
	bool run_in_background;
	bool start_in_fullscreen;
	bool correct_aspect_ratio;
	bool crop_overscan;
	bool antialias;
	bool mouse = false;
	bool autodrive = false;
	bool verbose;
	int int_scale = 2;
	int screenshot_image_type;

	int printer_image_type;
	bool printer_correct_aspect_ratio;
	std::string printer_view_command;

	// Dev automation (command line only); defaults leave behaviour unchanged
	uint64_t screenshot_every = 0;
	std::vector<uint64_t> screenshot_at;
	std::string screenshot_dir;
	uint64_t exit_after_frames = 0;
	std::string input_script;
	bool headless_fast = false;
	bool no_sram_write = false;
	// CPU profile: cycles per program address over emulated frames [from, to)
	std::string pc_profile;
	uint64_t pc_profile_from = 0;
	uint64_t pc_profile_to = 0;
	// Optional address whose 32-bit value is recorded at the start and end of
	// the window (e.g. a game frame counter), 0 = none
	uint32_t pc_profile_watch = 0;
	// Console timing model for every instruction (core/sh2/sh2_cost.h)
	bool console_cost = false;
	double cost_cart_rom = 0.0;  // 0 = follow WCR3

	bool automation_enabled() const
	{
		return screenshot_every || !screenshot_at.empty() || exit_after_frames || !input_script.empty() || headless_fast ||
			   !pc_profile.empty();
	}
};

void parse_commandline(int argc, char** argv, Args& args);
bool parse_config(fs::path config_path, Args& args);
void print_usage();

}  // namespace Options