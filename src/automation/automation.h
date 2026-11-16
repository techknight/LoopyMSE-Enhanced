#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Automation: drives the emulated machine from a command script and/or a live
// control channel (one command per line on stdin), frame-exactly and without
// any window, focus or host input involved. The command language is
// documented in docs/automation.md.
//
// Frames are counted from power-on: "frame F" is the moment F frames have
// been emulated. Commands due at frame F run before frame F+1 is emulated, so
// input set at F is seen by the game from that frame on, and a screenshot
// taken at F shows the picture of the F-th frame.
namespace Automation
{

enum ExitCode
{
	EXIT_OK = 0,
	EXIT_USAGE = 1,    // bad command line, settings or script
	EXIT_STRICT = 2,   // the program used unimplemented hardware (debug.strict)
	EXIT_TIMEOUT = 3,  // a wait-until ran out of time
	EXIT_ASSERT = 4,   // an assert failed
};

// What automation asks of the frontend.
struct Hooks
{
	// Save the current picture; name is empty for the default name. Returns
	// the path written, or an empty string on failure.
	std::function<std::string(const std::string& name)> shot;
	std::function<void()> reboot;
	std::function<void(int code, const std::string& why)> exit;
	std::function<void(bool mouse)> plug;
	std::function<void(double speed)> set_speed;
	// Steer the mouse to a game pixel through the cartridge interface;
	// returns false when no cooperating cartridge is running.
	std::function<bool(int x, int y)> mouse_to;
	std::function<bool(const std::string& path, std::string& error)> save_state;
	std::function<bool(const std::string& path, std::string& error)> load_state;
	std::function<bool(const std::string& what, const std::string& path, std::string& error)> dump;
	// A one-line status for the control channel's "status" command.
	std::function<std::string()> status;
};

void initialize(const Hooks& hooks);
void shutdown();

// Parse a script file. All lines are checked up front; on an error the
// message names the file and line.
bool load_script(const std::string& path, std::string& error);

// Read commands from stdin on a background thread. End of input exits.
void start_control_stdin();
bool control_enabled();

// Scheduled screenshots (automation.shot_every / shot_at).
void set_scheduled_shots(uint64_t every, const std::vector<uint64_t>& at);

// Called by the frontend at every frame boundary (and repeatedly while
// paused): runs whatever is due at frame `frame` and applies the scripted
// pad and mouse. Safe to call more than once for the same frame.
void update(uint64_t frame);

// True while the control channel holds emulation (pause, or a finished step).
bool paused();
void set_paused(bool paused);

// Run one command line now, as the control channel would (used by tests).
void run_line(const std::string& line);

}  // namespace Automation
