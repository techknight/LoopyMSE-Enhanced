#pragma once
#include <cstdint>
#include <string>

// Observability for program development: what the program sends out of its
// serial ports, and its writes to chosen addresses, written to files without
// a verbose log.
namespace DebugTools
{

struct Settings
{
	std::string serial_log[2];  // per port; empty = off
	bool serial_timed = false;  // one "FRAME CYCLE BYTE" line per byte instead of raw bytes
	std::string watch_writes;   // "ADDR[:LENGTH][,ADDR[:LENGTH]...]"
	std::string watch_log;      // empty = the main log
};

bool configure(const Settings& settings, std::string& error);

// Called by the frontend before each frame is emulated.
void begin_frame(uint64_t frame);

void shutdown();

}  // namespace DebugTools
