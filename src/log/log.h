#pragma once

#include <cstdint>
#include <string>

namespace Log
{

enum Level
{
	VERBOSE,
	TRACE,
	DEBUG,
	INFO,
	WARN,
	ERROR,
};

void set_level(Level level);
Level get_level();
bool parse_level(const std::string& name, Level& out);

// Where log lines go: stdout by default. use_stderr moves them to stderr
// (the control channel keeps stdout for its own replies); a log file, when
// opened, receives every line instead of either stream.
void use_stderr(bool enable);
bool open_file(const std::string& path);
void close_file();
void flush();

// Prefix every line with the emulated frame number ("[f123] ").
void set_frame_prefix(bool enable);
void set_frame(uint64_t frame);

void log(Level level, const char* fmt, ...);
void trace(const char* fmt, ...);
void debug(const char* fmt, ...);
void info(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);

// The emulated program used hardware the emulator does not implement. Always
// logged as an error and counted, so a strict run can stop with a failure
// instead of carrying on with wrong behaviour.
void unimplemented(const char* fmt, ...);
uint64_t unimplemented_count();

}  // namespace Log
