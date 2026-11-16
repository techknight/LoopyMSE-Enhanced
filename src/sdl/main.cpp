#include <SDL.h>
#include <automation/automation.h>
#include <automation/debugtools.h>
#include <common/bswp.h>
#include <core/cart.h>
#include <core/config.h>
#include <core/loopy_io.h>
#include <core/sh2/sh2.h>
#include <core/sh2/sh2_bus.h>
#include <core/sh2/sh2_cost.h>
#include <core/system.h>
#include <input/input.h>
#include <log/log.h>
#include <sdl/imgwriter.h>
#include <sound/sound.h>
#include <video/video.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "autodrive.h"
#include "config.h"
#include "options.h"
#include "recorder.h"

#define PRESCALE_FACTOR 4
#define MAX_WINDOW_INT_SCALE 10

namespace imagew = SDL::ImageWriter;

namespace SDL
{

using Video::DISPLAY_HEIGHT;
using Video::DISPLAY_WIDTH;
// Logical size includes border to allow for both 224 line and 240 line modes, and show some of the background effects.
// In reality the Loopy active area is drawn inside this and borders are visible, in 240 line mode more of this is used.
static constexpr int FRAME_WIDTH = 280;
static constexpr int FRAME_HEIGHT = 240;
// Scales the frame size up to 4:3 320x240
static constexpr float ASPECT_CORRECT_SCALE_X = (320.0f / FRAME_WIDTH);

struct Screen
{
	SDL_Renderer* renderer;
	SDL_Window* window;
	SDL_Texture* framebuffer;
	SDL_Texture* prescaled;
	int visible_scanlines = DISPLAY_HEIGHT;
	int window_int_scale = 1;
	int prescale = 1;
	bool correct_aspect_ratio;
	bool crop_overscan;
	bool antialias;
	bool hidden = false;

	bool is_fullscreen()
	{
		return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) > 0;
	}
};

static Screen screen;
static SDL_GameController* controller;
static bool mouse_captured;

void capture_mouse(bool cap)
{
	SDL_SetRelativeMouseMode(cap ? SDL_TRUE : SDL_FALSE);
	mouse_captured = cap;
}

bool is_mouse_captured()
{
	return mouse_captured;
}

void shutdown()
{
	capture_mouse(false);
	SDL_ShowCursor(SDL_ENABLE);

	//Destroy window, then kill SDL2
	if (screen.prescaled) SDL_DestroyTexture(screen.prescaled);
	SDL_DestroyTexture(screen.framebuffer);
	SDL_DestroyRenderer(screen.renderer);
	SDL_DestroyWindow(screen.window);

	SDL_Quit();
}

void open_first_controller()
{
	// Gets the first controller available
	for (int i = 0; i < SDL_NumJoysticks(); i++)
	{
		if (SDL_IsGameController(i))
		{
			Log::info("Connected to game controller %s", SDL_GameControllerNameForIndex(i));
			controller = SDL_GameControllerOpen(i);
			if (screen.is_fullscreen())
			{
				SDL_ShowCursor(SDL_DISABLE);
			}
			return;
		}
	}

	controller = nullptr;
}

SDL_Point resize_window(bool apply = true, int scale = 0)
{
	if (scale <= 0) scale = screen.window_int_scale;
	float frame_w = scale * (screen.crop_overscan ? DISPLAY_WIDTH : FRAME_WIDTH);
	float frame_h = scale * (screen.crop_overscan ? screen.visible_scanlines : FRAME_HEIGHT);
	if (screen.correct_aspect_ratio)
	{
		frame_w *= ASPECT_CORRECT_SCALE_X;
	}

	SDL_Point frame = {(int)SDL_roundf(frame_w), (int)SDL_roundf(frame_h)};
	if (apply)
	{
		SDL_SetWindowSize(screen.window, frame.x, frame.y);
	}
	Log::debug("[SCREEN] size width=%d height=%d", frame.x, frame.y);
	return frame;
}

void change_window_scale(int delta)
{
	float logical_w = (screen.crop_overscan ? DISPLAY_WIDTH : FRAME_WIDTH);
	float logical_h = (screen.crop_overscan ? screen.visible_scanlines : FRAME_HEIGHT);
	if (screen.correct_aspect_ratio)
	{
		logical_w *= ASPECT_CORRECT_SCALE_X;
	}
	int window_w, window_h;
	SDL_GetWindowSize(screen.window, &window_w, &window_h);
	float scale = SDL_min(window_w / logical_w, window_h / logical_h);
	int new_scale = (int)SDL_roundf(scale + delta);
	screen.window_int_scale = std::clamp(new_scale, 1, MAX_WINDOW_INT_SCALE);
	resize_window();
}

void toggle_fullscreen()
{
	bool to_fullscreen = !screen.is_fullscreen();
	if (SDL_SetWindowFullscreen(screen.window, to_fullscreen * SDL_WINDOW_FULLSCREEN_DESKTOP) < 0)
	{
		Log::error("Error fullscreening: %s", SDL_GetError());
		return;
	}

	if (controller)
	{
		SDL_ShowCursor(to_fullscreen ? SDL_DISABLE : SDL_ENABLE);
	}
}

void screenshot(int image_type, fs::path path)
{
	int w, h;
	SDL_GetRendererOutputSize(screen.renderer, &w, &h);
	SDL_Surface* screenshot = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGB24);
	SDL_RenderReadPixels(screen.renderer, nullptr, screenshot->format->format, screenshot->pixels, screenshot->pitch);
	imagew::save_surface(image_type, path, screenshot);
	SDL_FreeSurface(screenshot);
}

static inline void set_draw_color_16bpp(uint16_t c)
{
	uint8_t r = ((c >> 10) & 31) * 255 / 31;
	uint8_t g = ((c >> 5) & 31) * 255 / 31;
	uint8_t b = (c & 31) * 255 / 31;
	// uint8_t a = (c >> 15) * 255;
	SDL_SetRenderDrawColor(screen.renderer, r, g, b, SDL_ALPHA_OPAQUE);
}

void update(uint16_t* display_output, int visible_scanlines, uint16_t background_color)
{
	if (screen.hidden) return;

	if (visible_scanlines != screen.visible_scanlines)
	{
		screen.visible_scanlines = visible_scanlines;
		if (!screen.is_fullscreen() && screen.crop_overscan)
		{
			resize_window();
		}
	}

	void* pixels;
	int pitch;

	// More efficient alternative to SDL_UpdateTexture(screen.texture, NULL, display_output, sizeof(uint16_t) * DISPLAY_WIDTH);
	if (SDL_LockTexture(screen.framebuffer, nullptr, &pixels, &pitch) == 0)
	{
		memcpy(pixels, display_output, sizeof(uint16_t) * DISPLAY_WIDTH * DISPLAY_HEIGHT);
		SDL_UnlockTexture(screen.framebuffer);
	}

	// Prescale
	if (screen.prescaled)
	{
		SDL_SetRenderTarget(screen.renderer, screen.prescaled);
		SDL_RenderClear(screen.renderer);
		SDL_RenderCopy(screen.renderer, screen.framebuffer, nullptr, nullptr);
	}

	// Change target back to screen (must be done before querying renderer output size!)
	SDL_SetRenderTarget(screen.renderer, nullptr);
	set_draw_color_16bpp(background_color);
	SDL_RenderClear(screen.renderer);

	SDL_Rect src = {0, 0, DISPLAY_WIDTH * screen.prescale, visible_scanlines * screen.prescale};
	SDL_Rect frame = {0, 0, FRAME_WIDTH * screen.prescale, FRAME_HEIGHT * screen.prescale};
	if (screen.crop_overscan) frame = src;
	SDL_Rect dest = {0};
	SDL_GetRendererOutputSize(screen.renderer, &dest.w, &dest.h);

	float scale_x = (float)dest.w / frame.w;
	float scale_y = (float)dest.h / frame.h;
	float scale = SDL_min(scale_x, scale_y);
	if (!screen.antialias && !screen.correct_aspect_ratio)
	{
		scale = SDL_floorf(scale);
	}
	scale_x = scale_y = scale;
	if (screen.correct_aspect_ratio)
	{
		scale_x *= ASPECT_CORRECT_SCALE_X;
	}
	float w = scale_x * src.w;
	float h = scale_y * src.h;
	dest.x = (dest.w - w) / 2;
	dest.y = (dest.h - h) / 2;
	dest.w = w;
	dest.h = h;

	SDL_RenderCopy(screen.renderer, (screen.prescaled ? screen.prescaled : screen.framebuffer), &src, &dest);
	SDL_RenderPresent(screen.renderer);
}

void initialize(Options::Args& args, const std::string& title_suffix)
{
	//Allow use of our own main()
	SDL_SetMainReady();

	screen.hidden = args.window == "hidden";
	bool vsync = args.speed == 1.0 && !screen.hidden;

	//Try synchronizing window drawing to VBLANK (only at normal speed)
	SDL_SetHint(SDL_HINT_RENDER_VSYNC, vsync ? "1" : "0");
	SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "1");
	//Workaround for bug in SDL2 2.32 where window doesn't activate, not needed in 2.30
	SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "0");
	if (args.unattended())
	{
		//Never take the focus from whatever the user is doing
		SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0)
	{
		Log::error("Failed to initialize SDL2: %s", SDL_GetError());
		exit(Automation::EXIT_USAGE);
	}

	//Set up SDL screen
	screen.correct_aspect_ratio = args.correct_aspect_ratio;
	screen.crop_overscan = args.crop_overscan;
	screen.antialias = args.antialias;
	screen.window_int_scale = std::clamp(args.scale, 1, MAX_WINDOW_INT_SCALE);
	screen.prescale = args.antialias ? PRESCALE_FACTOR : 1;

	std::string title = std::string(PROJECT_DESCRIPTION) + " " + PROJECT_VERSION + title_suffix;
	SDL_Point window_size = resize_window(false);
	Uint32 window_opts = 0;
	window_opts |= args.crop_overscan ? 0 : SDL_WINDOW_RESIZABLE;
	window_opts |= args.fullscreen && !screen.hidden ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
	window_opts |= screen.hidden ? SDL_WINDOW_HIDDEN : 0;
	window_opts |= args.window == "minimized" ? SDL_WINDOW_MINIMIZED : 0;
	screen.window = SDL_CreateWindow(
		title.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_size.x, window_size.y, window_opts
	);
	window_size = resize_window(false, 1);
	SDL_SetWindowMinimumSize(screen.window, window_size.x, window_size.y);

	screen.renderer =
		SDL_CreateRenderer(screen.window, -1, SDL_RENDERER_ACCELERATED | (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));

	screen.framebuffer = SDL_CreateTexture(
		screen.renderer, SDL_PIXELFORMAT_ARGB1555, SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH, DISPLAY_HEIGHT
	);
	SDL_SetTextureBlendMode(screen.framebuffer, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(screen.framebuffer, SDL_ScaleModeNearest);

	if (screen.prescale > 1)
	{
		screen.prescaled = SDL_CreateTexture(
			screen.renderer, SDL_PIXELFORMAT_ARGB1555, SDL_TEXTUREACCESS_TARGET, DISPLAY_WIDTH * screen.prescale,
			DISPLAY_HEIGHT * screen.prescale
		);
		SDL_SetTextureBlendMode(screen.prescaled, SDL_BLENDMODE_BLEND);
		SDL_SetTextureScaleMode(screen.prescaled, SDL_ScaleModeBest);
	}

	// Allow dropping a ROM onto the window
	SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

	if (SDL_GameControllerAddMappingsFromFile((RESOURCE_PATH / CONTROLLER_DB_PATH).string().c_str()) < 0)
	{
		Log::debug("Could not load game controller database: %s", SDL_GetError());
		// Nonfatal: continue without the mappings
	}
	open_first_controller();

	// Mouse mappings don't really need configuration
	Input::add_mouse_binding(SDL_BUTTON_LEFT, Input::MouseButton::MOUSE_L);
	Input::add_mouse_binding(SDL_BUTTON_RIGHT, Input::MouseButton::MOUSE_R);
}

}  // namespace SDL

namespace
{

Config::SystemInfo config;
Options::Args args;

// Frames emulated since power-on (continues across reboots)
uint64_t frame = 0;
bool has_quit = false;
int exit_code = Automation::EXIT_OK;
double speed = 1.0;
bool focus_paused = false;
uint64_t last_frame_ticks = 0;
uint32_t prof_watch_start = 0;

fs::path output_dir;
fs::path shot_dir;
fs::path print_dir;

// Games known to use the XN-110 mouse, by the checksum in their header
// (the list from LoopyMSE's mouse-detection branch).
const uint32_t MOUSE_CARTS[] = {
	0xA5341F72,	 // Chakrakun no Omajinai Paradise
	0xF294F931,	 // Pasokon Collection
	0x6A410BB2,	 // Little Romance
	0x8373E9DD,	 // Loopy Town no Oheya ga Hoshii!
	0x6E00CE71,	 // Lupiton no Wonder Palette
};

uint32_t header_checksum(const std::vector<uint8_t>& rom)
{
	if (rom.size() < 12) return 0;
	return (uint32_t)rom[8] << 24 | (uint32_t)rom[9] << 16 | (uint32_t)rom[10] << 8 | rom[11];
}

bool is_mouse_cart(const std::vector<uint8_t>& rom)
{
	uint32_t sum = header_checksum(rom);
	return std::find(std::begin(MOUSE_CARTS), std::end(MOUSE_CARTS), sum) != std::end(MOUSE_CARTS);
}

// Relative output files go into paths.output when it is set.
fs::path output_file(const std::string& name)
{
	fs::path p(name);
	if (p.is_relative() && !output_dir.empty()) return output_dir / p;
	return p;
}

fs::path sram_path_for(const fs::path& rom_path)
{
	if (!args.sram_path.empty()) return output_file(args.sram_path);
	fs::path name = rom_path.filename();
	name.replace_extension(".sav");
	if (!output_dir.empty()) return output_dir / name;
	fs::path p = rom_path;
	return p.replace_extension(".sav");
}

void set_output_dirs_for(const fs::path& rom_path)
{
	fs::path rom_dir = fs::absolute(rom_path).parent_path();
	fs::path base = output_dir.empty() ? rom_dir : output_dir;
	shot_dir = args.screenshot_dir.empty() ? base : fs::absolute(args.screenshot_dir);
	print_dir = args.print_dir.empty() ? base : fs::absolute(args.print_dir);
	std::error_code ec;
	fs::create_directories(shot_dir, ec);
	fs::create_directories(print_dir, ec);
	config.emulator.image_save_directory = shot_dir;
	config.emulator.print_directory = print_dir;
}

bool load_cart(const std::string& path)
{
	config.cart = {};

	std::ifstream cart_file(path, std::ios::binary);
	if (!cart_file.is_open())
	{
		Log::error("Couldn't load cartridge at %s", path.c_str());
		return false;
	}

	config.cart.rom_path = path;
	config.cart.rom.assign(std::istreambuf_iterator<char>(cart_file), {});
	cart_file.close();
	if (config.cart.rom.size() < 0x20)
	{
		Log::error("%s is too small to be a Loopy ROM", path.c_str());
		config.cart = {};
		return false;
	}

	//Determine the size of SRAM from the cartridge header
	uint32_t sram_start, sram_end;
	memcpy(&sram_start, config.cart.rom.data() + 0x10, 4);
	memcpy(&sram_end, config.cart.rom.data() + 0x14, 4);
	uint32_t sram_size = Common::bswp32(sram_end) - Common::bswp32(sram_start) + 1;

	//Attempt to load SRAM from a file
	config.cart.sram_file_path = sram_path_for(path).string();
	std::ifstream sram_file(config.cart.sram_file_path, std::ios::binary);
	if (!sram_file.is_open())
	{
		Log::debug("No save file yet at %s", config.cart.sram_file_path.c_str());
	}
	else
	{
		config.cart.sram.assign(std::istreambuf_iterator<char>(sram_file), {});
		config.cart.sram_from_file = true;
		sram_file.close();
	}

	//Ensure SRAM is at the proper size. If no file is loaded, it will be filled with 0xFF.
	//If a file was loaded but was smaller than the SRAM size, the uninitialized bytes will be 0xFF.
	//If the file was larger, then the vector size is clamped
	config.cart.sram.resize(sram_size, 0xFF);
	set_output_dirs_for(path);
	return true;
}

bool load_file(std::vector<uint8_t>& out, const fs::path& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) return false;
	out.assign(std::istreambuf_iterator<char>(file), {});
	return true;
}

std::vector<fs::path> search_paths(fs::path file_path, fs::path cart_path)
{
	std::vector<fs::path> vec;
	if (file_path.is_relative())
	{
		if (!cart_path.empty() && cart_path.has_parent_path())
		{
			vec.push_back(cart_path.parent_path() / file_path);
		}
		vec.push_back(PREFS_PATH / file_path);
		vec.push_back(RESOURCE_PATH / file_path);
#ifdef __APPLE__
		// On MacOS, look in the folder containing the .app as well as Resources/ in the bundle
		// RESOURCE_PATH = ./LoopyMSE.app/Contents/Resources
		vec.push_back(RESOURCE_PATH / ".." / ".." / ".." / file_path);
#endif
	}
	vec.push_back(file_path);
	return vec;
}

void request_exit(int code, const std::string& why)
{
	if (has_quit) return;
	has_quit = true;
	exit_code = code;
	if (code == Automation::EXIT_OK) Log::info("Exiting at frame %llu: %s", (unsigned long long)frame, why.c_str());
	else Log::error("Exiting with code %d at frame %llu: %s", code, (unsigned long long)frame, why.c_str());
	if (Automation::control_enabled())
	{
		printf("exit %llu %d\n", (unsigned long long)frame, code);
		fflush(stdout);
	}
}

void plug(bool mouse)
{
	if (mouse == Input::is_mouse_plugged()) return;
	if (!mouse && SDL::is_mouse_captured())
	{
		SDL::capture_mouse(false);
	}
	Input::inject_mouse_buttons(Input::MOUSE_L | Input::MOUSE_R, false);
	Input::set_mouse_plugged(mouse);
	Log::info(mouse ? "Loopy mouse plugged in" : "Gamepad plugged in");
}

void reboot()
{
	if (!config.cart.is_loaded()) return;
	Log::info("Rebooting Loopy...");
	Autodrive::reset();
	System::shutdown(config);
	System::initialize(config);
	last_frame_ticks = SDL_GetPerformanceCounter();
}

std::string sanitize(const std::string& name)
{
	std::string out;
	for (char c : name)
	{
		out += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') ? c : '_';
	}
	return out;
}

// Save the raw picture of the last emulated frame.
std::string take_shot(const std::string& name)
{
	int type = imagew::parse_image_type(args.screenshot_image_type, imagew::IMAGE_TYPE_DEFAULT);
	fs::path file;
	if (name.empty())
	{
		char buf[40];
		snprintf(buf, sizeof buf, "frame_%06llu", (unsigned long long)frame);
		file = buf;
	}
	else
	{
		file = sanitize(name);
	}
	if (!file.has_extension()) file += imagew::image_extension(type);
	fs::path path = shot_dir / file;
	bool ok = imagew::save_image_16bpp(
		type, path, Video::DISPLAY_WIDTH, Video::get_display_scanlines(), Video::get_display_output()
	);
	if (!ok) return "";
	Log::info("[Automation] screenshot %s", path.string().c_str());
	return path.string();
}

fs::path state_slot_path(int slot)
{
	fs::path rom = config.cart.rom_path;
	fs::path dir = !args.state_dir.empty()		? fs::path(args.state_dir)
				   : !output_dir.empty()		? output_dir
				   : rom.has_parent_path()		? fs::absolute(rom).parent_path()
												: PREFS_PATH;
	fs::path name = rom.filename();
	name.replace_extension(".state" + std::to_string(slot));
	return dir / name;
}

bool save_state_file(const fs::path& path, std::string& error)
{
	if (!config.cart.is_loaded())
	{
		error = "no cartridge is running";
		return false;
	}
	std::vector<uint8_t> data = System::save_state(config, frame);
	std::error_code ec;
	if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
	std::ofstream file(path, std::ios::binary);
	if (!file.is_open() || !file.write((const char*)data.data(), data.size()))
	{
		error = "cannot write " + path.string();
		return false;
	}
	Log::info("Saved state at frame %llu to %s", (unsigned long long)frame, path.string().c_str());
	return true;
}

bool load_state_file(const fs::path& path, std::string& error)
{
	if (!config.cart.is_loaded())
	{
		error = "no cartridge is running";
		return false;
	}
	std::vector<uint8_t> data;
	if (!load_file(data, path))
	{
		error = "cannot read " + path.string();
		return false;
	}
	uint64_t loaded_frame = 0;
	if (!System::load_state(config, data, loaded_frame, error))
	{
		error = path.string() + ": " + error;
		return false;
	}
	frame = loaded_frame;
	Log::set_frame(frame);
	Autodrive::reset();
	last_frame_ticks = SDL_GetPerformanceCounter();
	Log::info("Loaded state of frame %llu from %s", (unsigned long long)frame, path.string().c_str());
	return true;
}

// Memory dumps for the "dump" command: raw bytes as the CPU sees them.
bool dump_memory(const std::string& what, const std::string& name, std::string& error)
{
	struct Region
	{
		const char* name;
		uint32_t addr;
		uint32_t size;
	};
	static const Region REGIONS[] = {
		{"ram", 0x09000000, 0x80000},		 {"bitmap", Video::BITMAP_VRAM_START, Video::BITMAP_VRAM_SIZE},
		{"tiles", Video::TILE_VRAM_START, Video::TILE_VRAM_SIZE}, {"oam", Video::OAM_START, Video::OAM_SIZE},
		{"palette", Video::PALETTE_START, Video::PALETTE_SIZE},   {"onchip", 0x0F000000, 0x400},
	};
	std::vector<uint8_t> data;
	if (what == "sram")
	{
		data = config.cart.sram;
		uint32_t sram_start;
		memcpy(&sram_start, config.cart.rom.data() + 0x10, 4);
		sram_start = Common::bswp32(sram_start);
		for (size_t i = 0; i < data.size(); i++) data[i] = SH2::Bus::read8(sram_start + (uint32_t)i);
	}
	else
	{
		const Region* region = nullptr;
		for (const auto& r : REGIONS)
		{
			if (what == r.name) region = &r;
		}
		if (!region)
		{
			error = "unknown dump '" + what + "' (ram sram bitmap tiles oam palette onchip)";
			return false;
		}
		data.resize(region->size);
		for (uint32_t i = 0; i < region->size; i++) data[i] = SH2::Bus::read8(region->addr + i);
	}
	fs::path path = output_file(name);
	std::ofstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		error = "cannot write " + path.string();
		return false;
	}
	file.write((const char*)data.data(), data.size());
	Log::info("[Automation] dumped %s (%zu bytes) to %s", what.c_str(), data.size(), path.string().c_str());
	return true;
}

void apply_bindings()
{
	static const std::pair<const char*, Input::PadButton> PAD[] = {
		{"up", Input::PAD_UP},	   {"down", Input::PAD_DOWN}, {"left", Input::PAD_LEFT}, {"right", Input::PAD_RIGHT},
		{"start", Input::PAD_START}, {"a", Input::PAD_A},		  {"b", Input::PAD_B},		 {"c", Input::PAD_C},
		{"d", Input::PAD_D},		 {"l", Input::PAD_L1},		  {"r", Input::PAD_R1},
	};
	for (const auto& [name, button] : PAD)
	{
		const std::string& key = args.keyboard[name];
		SDL_Keycode code = SDL_GetKeyFromName(key.c_str());
		if (code == SDLK_UNKNOWN) Log::error("keyboard.%s: unknown key '%s'", name, key.c_str());
		else Input::add_key_binding(code, button);

		const std::string& pad = args.controller[name];
		SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(pad.c_str());
		if (b == SDL_CONTROLLER_BUTTON_INVALID) Log::error("controller.%s: unknown button '%s'", name, pad.c_str());
		else Input::add_controller_binding(b, button);
	}
}

// Read emulated memory from outside the CPU, a byte at a time (any address,
// no address errors).
uint32_t peek32(uint32_t addr)
{
	uint32_t value = 0;
	for (int i = 0; i < 4; i++) value = (value << 8) | SH2::Bus::read8(addr + i);
	return value;
}

void run_frame()
{
	if (!args.profile_file.empty() && frame == args.profile_from)
	{
		SH2::profile_enable(true);
		if (args.profile_watch) prof_watch_start = peek32(args.profile_watch);
	}

	DebugTools::begin_frame(frame);
	System::run();
	Autodrive::run_frame();
	Input::end_of_frame();
	frame++;
	Log::set_frame(frame);
	Recorder::video_frame(frame, Video::get_display_output(), Video::get_display_scanlines());

	if (!args.profile_file.empty() && frame == args.profile_to)
	{
		SH2::profile_enable(false);
		std::string note;
		if (args.profile_watch)
		{
			char buf[96];
			snprintf(
				buf, sizeof buf, "watch %08X start %u end %u fields %llu", (unsigned)args.profile_watch,
				(unsigned)prof_watch_start, (unsigned)peek32(args.profile_watch),
				(unsigned long long)(args.profile_to - args.profile_from)
			);
			note = buf;
		}
		fs::path path = output_file(args.profile_file);
		if (SH2::profile_write(path.string(), note)) Log::info("Wrote CPU profile %s", path.string().c_str());
		else Log::error("Could not write CPU profile %s", path.string().c_str());
	}

	if (args.strict && Log::unimplemented_count())
	{
		request_exit(Automation::EXIT_STRICT, "the program used unimplemented hardware (debug.strict)");
	}
}

void handle_event(SDL_Event& e)
{
	switch (e.type)
	{
	case SDL_QUIT:
		request_exit(Automation::EXIT_OK, "window closed");
		break;
	case SDL_KEYDOWN:
		Input::set_key_state(e.key.keysym.sym, true);
		break;
	case SDL_KEYUP:
	{
		SDL_Keycode keycode = e.key.keysym.sym;
		switch (keycode)
		{
		case SDLK_F1:
			plug(false);
			break;
		case SDLK_F2:
			if (!Input::is_mouse_plugged())
			{
				plug(true);
				if (!Input::is_user_input_blocked()) Log::info("Click the window to capture the mouse");
			}
			else if (SDL::is_mouse_captured())
			{
				SDL::capture_mouse(false);
				Input::set_mouse_button_state(SDL_BUTTON_LEFT, false);
				Input::set_mouse_button_state(SDL_BUTTON_RIGHT, false);
			}
			break;
		case SDLK_F10:
			if (config.cart.is_loaded())
			{
				int type = imagew::parse_image_type(args.screenshot_image_type, imagew::IMAGE_TYPE_DEFAULT);
				auto path = [type]() {
					return shot_dir / imagew::make_unique_name("loopymse_").replace_extension(imagew::image_extension(type));
				};
				// Raw video buffer
				imagew::save_image_16bpp(
					type, path(), Video::DISPLAY_WIDTH, Video::get_display_scanlines(), Video::get_display_output()
				);
				if (args.correct_aspect_ratio && !SDL::screen.hidden)
				{
					// Export both scaled and original, let user decide which they want
					SDL::screenshot(type, path());
				}
			}
			break;
		case SDLK_F5:
		case SDLK_F6:
		case SDLK_F7:
		case SDLK_F8:
		{
			// F5-F8 load slots 1-4, Shift+F5-F8 save them
			int slot = 1 + (int)(keycode - SDLK_F5);
			std::string error;
			bool ok = (e.key.keysym.mod & KMOD_SHIFT) ? save_state_file(state_slot_path(slot), error)
													  : load_state_file(state_slot_path(slot), error);
			if (!ok) Log::error("Savestate slot %d: %s", slot, error.c_str());
			break;
		}
		case SDLK_F11:
			SDL::toggle_fullscreen();
			break;
		case SDLK_F12:
			reboot();
			break;
		case SDLK_MINUS:
			if (!SDL::screen.is_fullscreen())
			{
				SDL::change_window_scale(-1);
			}
			break;
		case SDLK_EQUALS:
			if (!SDL::screen.is_fullscreen())
			{
				SDL::change_window_scale(1);
			}
			break;
		case SDLK_ESCAPE:
			if (SDL::screen.is_fullscreen())
			{
				SDL::toggle_fullscreen();
			}
			else
			{
				request_exit(Automation::EXIT_OK, "escape pressed");
			}
			break;
		default:
			Input::set_key_state(keycode, false);
			break;
		}
		break;
	}
	case SDL_CONTROLLERBUTTONDOWN:
		// Quit on select+start
		if (SDL::controller && SDL_GameControllerGetButton(SDL::controller, SDL_CONTROLLER_BUTTON_START) &&
			SDL_GameControllerGetButton(SDL::controller, SDL_CONTROLLER_BUTTON_BACK))
		{
			request_exit(Automation::EXIT_OK, "select+start pressed");
			break;
		}
		Input::set_controller_state(e.cbutton.button, true);
		break;
	case SDL_CONTROLLERBUTTONUP:
		Input::set_controller_state(e.cbutton.button, false);
		break;
	case SDL_WINDOWEVENT:
		switch (e.window.event)
		{
		case SDL_WINDOWEVENT_FOCUS_GAINED:
			if (!args.run_in_background)
			{
				Sound::set_mute(false);
				focus_paused = false;
			}
			break;
		case SDL_WINDOWEVENT_FOCUS_LOST:
			if (!args.run_in_background)
			{
				Sound::set_mute(true);
				focus_paused = true;
			}
			if (SDL::is_mouse_captured())
			{
				SDL::capture_mouse(false);
				Input::set_mouse_button_state(SDL_BUTTON_LEFT, false);
				Input::set_mouse_button_state(SDL_BUTTON_RIGHT, false);
			}
			break;
		}
		break;
	case SDL_MOUSEBUTTONDOWN:
		if (SDL::is_mouse_captured())
		{
			Input::set_mouse_button_state(e.button.button, true);
		}
		else if (Input::is_mouse_plugged() && !Input::is_user_input_blocked())
		{
			SDL::capture_mouse(true);
		}
		break;
	case SDL_MOUSEBUTTONUP:
		if (SDL::is_mouse_captured())
		{
			Input::set_mouse_button_state(e.button.button, false);
		}
		break;
	case SDL_MOUSEMOTION:
		if (SDL::is_mouse_captured())
		{
			Input::move_mouse(e.motion.xrel, -e.motion.yrel);
		}
		break;
	case SDL_CONTROLLERDEVICEADDED:
		if (!SDL::controller)
		{
			Log::info("New controller added.");
			SDL::open_first_controller();
		}
		break;
	case SDL_CONTROLLERDEVICEREMOVED:
		// Only react to current device being removed
		if (SDL::controller &&
			e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(SDL::controller)))
		{
			Log::info("Controller removed, using next available one.");
			SDL_GameControllerClose(SDL::controller);
			SDL::open_first_controller();
		}
		break;
	case SDL_DROPFILE:
	{
		std::string path = e.drop.file;
		SDL_free(e.drop.file);
		if (config.cart.is_loaded()) System::shutdown(config);
		if (load_cart(path))
		{
			Log::info("Loaded %s...", path.c_str());
			Autodrive::reset();
			System::initialize(config);
			// So you can tell that MSE is running even before you click into it
			focus_paused = false;
			last_frame_ticks = SDL_GetPerformanceCounter();
		}
		break;
	}
	}
}

}  // namespace

int main(int argc, char** argv)
{
	switch (Options::parse(argc, argv, PREFS_PATH / INI_PATH, RESOURCE_PATH / INI_PATH, args))
	{
	case Options::PARSE_EXIT_OK:
		return Automation::EXIT_OK;
	case Options::PARSE_EXIT_ERROR:
		return Automation::EXIT_USAGE;
	default:
		break;
	}

	// Logging first, so everything after it lands in the right place
	Log::Level level = Log::INFO;
	Log::parse_level(args.log_level, level);
	Log::set_level(level);
	Log::set_frame_prefix(args.log_frames);
	if (!args.output_dir.empty())
	{
		output_dir = fs::absolute(args.output_dir);
		std::error_code ec;
		fs::create_directories(output_dir, ec);
	}
	if (args.control == "stdin")
	{
		// stdout carries the control channel's replies only
		Log::use_stderr(true);
	}
	if (!args.log_file.empty())
	{
		fs::path log_path = output_file(args.log_file);
		if (!Log::open_file(log_path.string()))
		{
			Log::error("Could not open log file %s", log_path.string().c_str());
			return Automation::EXIT_USAGE;
		}
	}

	speed = args.speed;
	config.emulator.image_save_directory = PREFS_PATH;
	config.emulator.screenshot_image_type = imagew::parse_image_type(args.screenshot_image_type, imagew::IMAGE_TYPE_DEFAULT);
	config.emulator.printer_image_type = imagew::parse_image_type(args.printer_image_type, imagew::IMAGE_TYPE_DEFAULT);
	config.emulator.printer_view_command = args.printer_view ? args.printer_view_command : "";
	config.emulator.printer_correct_aspect_ratio = args.printer_correct_aspect_ratio;

	Cart::set_sram_write_enabled(args.sram_write);
	Sound::set_output_enabled(args.sound_output == "device");
	SH2::console_cost_enable(args.timing_model == "console");
	SH2::Cost::set_cart_rom_cycles(args.cart_rom_cycles);
	apply_bindings();

	bool result = false;
	for (const auto& path : search_paths(args.bios, args.cart))
	{
		if (load_file(config.bios_rom, path))
		{
			Log::info("Located BIOS at %s", path.string().c_str());
			result = true;
			break;
		}
	}
	if (!result)
	{
		Log::error(
			"Missing BIOS file. Provide it with --bios, or place it at %s.",
			(PREFS_PATH / DEFAULT_BIOS_PATH).string().c_str()
		);
		Options::print_usage();
		return Automation::EXIT_USAGE;
	}

	result = false;
	for (const auto& path : search_paths(args.sound_bios, args.cart))
	{
		if (load_file(config.sound_rom, path))
		{
			Log::info("Located Sound BIOS at %s", path.string().c_str());
			result = true;
			break;
		}
	}
	if (!result)
	{
		Log::warn(
			"Missing sound BIOS file. Provide it with --sound-bios, or place it at %s.\n"
			"Emulation will continue without sound.",
			DEFAULT_SOUND_BIOS_PATH.string().c_str()
		);
	}

	if (args.cart.empty())
	{
		if (args.unattended())
		{
			Log::error("An unattended run needs a ROM");
			return Automation::EXIT_USAGE;
		}
		Log::info("No ROM provided. Drop a Loopy ROM onto the window to play.");
	}
	else if (!load_cart(args.cart))
	{
		Log::error("Could not load ROM file.");
		return Automation::EXIT_USAGE;
	}

	// Plug in the controller (the plugged state survives reboots, like a real one)
	bool mouse = args.device == "mouse" || args.autodrive ||
				 (args.device == "auto" && config.cart.is_loaded() && is_mouse_cart(config.cart.rom));
	Input::set_mouse_plugged(mouse);
	if (mouse && args.device == "auto") Log::info("Known mouse game: plugging in the mouse");
	Autodrive::set_enabled(args.autodrive);

	Automation::Hooks hooks;
	hooks.shot = take_shot;
	hooks.reboot = reboot;
	hooks.exit = request_exit;
	hooks.plug = plug;
	hooks.set_speed = [](double s) {
		speed = s;
		last_frame_ticks = SDL_GetPerformanceCounter();
	};
	hooks.mouse_to = [](int x, int y) { return Autodrive::set_target(x, y); };
	hooks.status = []() {
		char buf[128];
		snprintf(
			buf, sizeof buf, "device=%s speed=%g unimplemented=%llu", Input::is_mouse_plugged() ? "mouse" : "pad", speed,
			(unsigned long long)Log::unimplemented_count()
		);
		return std::string(buf);
	};
	hooks.dump = dump_memory;
	hooks.save_state = [](const std::string& name, std::string& error) {
		return save_state_file(output_file(name), error);
	};
	hooks.load_state = [](const std::string& name, std::string& error) {
		// A relative name is looked up where save-state writes it (paths.output) first
		std::error_code ec;
		fs::path in_output = output_file(name);
		return load_state_file(fs::exists(in_output, ec) ? in_output : fs::path(name), error);
	};
	Automation::initialize(hooks);
	Automation::set_scheduled_shots(args.shot_every, args.shot_at);

	DebugTools::Settings debug;
	for (int port = 0; port < 2; port++)
	{
		if (!args.serial_log[port].empty()) debug.serial_log[port] = output_file(args.serial_log[port]).string();
	}
	debug.serial_timed = args.serial_log_timed;
	debug.watch_writes = args.watch_writes;
	if (!args.watch_log.empty()) debug.watch_log = output_file(args.watch_log).string();
	std::string debug_error;
	if (!DebugTools::configure(debug, debug_error))
	{
		Log::error("%s", debug_error.c_str());
		return Automation::EXIT_USAGE;
	}
	LoopyIO::set_trace(args.trace_io);
	LoopyIO::set_seal_cassette(args.printer_cassette);
	Video::set_raster_warnings(args.raster_warnings);

	Recorder::Settings rec;
	if (!args.record_video.empty()) rec.video = output_file(args.record_video).string();
	if (!args.record_audio.empty()) rec.audio = output_file(args.record_audio).string();
	if (!args.record_frames.empty()) rec.frames_dir = output_file(args.record_frames).string();
	rec.scale = args.record_scale;
	rec.quality = args.record_quality;
	rec.ffmpeg = args.record_ffmpeg;
	std::string record_error;
	if (!Recorder::start(rec, record_error))
	{
		Log::error("%s", record_error.c_str());
		return Automation::EXIT_USAGE;
	}
	if (!args.script.empty())
	{
		std::string error;
		if (!Automation::load_script(args.script, error))
		{
			Log::error("%s", error.c_str());
			return Automation::EXIT_USAGE;
		}
	}
	if (args.control == "stdin")
	{
		Automation::start_control_stdin();
		Automation::set_paused(args.start_paused);
	}
	else if (args.start_paused)
	{
		Log::warn("automation.start_paused only applies with the control channel (--control)");
	}

	if (config.cart.is_loaded())
	{
		System::initialize(config);
		if (!args.load_state.empty())
		{
			std::string error;
			if (!load_state_file(args.load_state, error))
			{
				Log::error("%s", error.c_str());
				return Automation::EXIT_USAGE;
			}
		}
	}

	std::string title_suffix;
	if (config.cart.is_loaded()) title_suffix = " - " + fs::path(config.cart.rom_path).filename().string();
	SDL::initialize(args, title_suffix);

	const uint64_t freq = SDL_GetPerformanceFrequency();
	constexpr int FRAMERATE = 60;  //TODO: get this from Video if it can be changed (e.g. for PAL mode)
	constexpr uint64_t MAX_LAG_FRAMES = 5;
	last_frame_ticks = SDL_GetPerformanceCounter();
	uint64_t last_present = 0;

	while (!has_quit)
	{
		uint64_t now = SDL_GetPerformanceCounter();
		bool running = config.cart.is_loaded() && !focus_paused;
		uint64_t ticks_per_frame = speed > 0 ? (uint64_t)(freq / (FRAMERATE * speed)) : 0;

		// How many frames are due now
		uint64_t due = 0;
		if (running && speed > 0)
		{
			due = (now - last_frame_ticks) / ticks_per_frame;
			last_frame_ticks += due * ticks_per_frame;
			if (due > MAX_LAG_FRAMES)
			{
				Log::debug("%llu frames behind, skipping ahead", (unsigned long long)due);
				last_frame_ticks = now;
				due = 1;
			}
		}
		else if (running)
		{
			// As fast as possible: run for about one display frame of wall time
			due = UINT64_MAX;
			last_frame_ticks = now;
		}
		else
		{
			last_frame_ticks = now;
		}

		bool emulated = false;
		for (uint64_t i = 0; i < due && !has_quit; i++)
		{
			// Commands due at this frame boundary, then the frame itself
			Automation::update(frame);
			if (has_quit || Automation::paused()) break;
			if (args.exit_after_frames && frame >= args.exit_after_frames)
			{
				request_exit(Automation::EXIT_OK, "frame limit reached");
				break;
			}
			run_frame();
			emulated = true;
			if (speed == 0 && SDL_GetPerformanceCounter() - now > freq / FRAMERATE) break;
		}
		if (!emulated && !has_quit)
		{
			// Paused or between frames: still take control commands
			Automation::update(frame);
		}

		if (emulated && config.cart.is_loaded() && !SDL::screen.hidden)
		{
			uint64_t t = SDL_GetPerformanceCounter();
			if (speed == 1.0 || t - last_present >= freq / FRAMERATE)
			{
				SDL::update(System::get_display_output(), Video::get_display_scanlines(), Video::get_background_color());
				last_present = t;
			}
		}
		Log::flush();

		// Sleep until the next frame is due, or any event arrives
		if (!has_quit)
		{
			uint64_t wait_ms = 0;
			if (!running || Automation::paused())
			{
				wait_ms = 5;
			}
			else if (speed > 0)
			{
				uint64_t since = SDL_GetPerformanceCounter() - last_frame_ticks;
				if (since < ticks_per_frame) wait_ms = (ticks_per_frame - since) * 1000 / freq;
			}
			if (wait_ms > 0) SDL_WaitEventTimeout(nullptr, (int)wait_ms);
		}

		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			handle_event(e);
		}
	}

	Automation::shutdown();
	DebugTools::shutdown();
	if (config.cart.is_loaded()) System::shutdown(config);
	Recorder::stop();
	SDL::shutdown();
	Log::flush();
	Log::close_file();
	return exit_code;
}
