#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace Log
{

static Level level = INFO;
static FILE* file = nullptr;
static bool to_stderr = false;
static bool frame_prefix = false;
static uint64_t frame = 0;
static uint64_t unimplemented_hits = 0;

static FILE* out()
{
	if (file) return file;
	return to_stderr ? stderr : stdout;
}

void set_level(Level l)
{
	level = l;
}

Level get_level()
{
	return level;
}

bool parse_level(const std::string& name, Level& result)
{
	static const struct
	{
		const char* name;
		Level level;
	} NAMES[] = {
		{"verbose", VERBOSE}, {"trace", TRACE}, {"debug", DEBUG}, {"info", INFO}, {"warn", WARN}, {"error", ERROR},
	};
	for (const auto& entry : NAMES)
	{
		if (name == entry.name)
		{
			result = entry.level;
			return true;
		}
	}
	return false;
}

void use_stderr(bool enable)
{
	to_stderr = enable;
}

bool open_file(const std::string& path)
{
	close_file();
	file = fopen(path.c_str(), "w");
	return file != nullptr;
}

void close_file()
{
	if (file)
	{
		fclose(file);
		file = nullptr;
	}
}

void flush()
{
	fflush(out());
}

void set_frame_prefix(bool enable)
{
	frame_prefix = enable;
}

void set_frame(uint64_t f)
{
	frame = f;
}

static const char* level_tag(Level l)
{
	switch (l)
	{
	case TRACE:
		return "[TRACE] ";
	case DEBUG:
		return "[DEBUG] ";
	case INFO:
		return "[INFO] ";
	case WARN:
		return "[WARN] ";
	case ERROR:
		return "[ERROR] ";
	default:
		return "";
	}
}

static void write_line(Level l, const char* fmt, va_list args)
{
	FILE* f = out();
	if (frame_prefix)
	{
		fprintf(f, "[f%llu] ", (unsigned long long)frame);
	}
	fputs(level_tag(l), f);
	vfprintf(f, fmt, args);
	fputc('\n', f);
	// Problems are flushed at once, so they survive a crash or a kill; the
	// rest is flushed once a frame by the frontend.
	if (l >= WARN)
	{
		fflush(f);
	}
}

#define LOG_AT(x)                \
	if (level > x) return;       \
	va_list args;                \
	va_start(args, fmt);         \
	write_line(x, fmt, args);    \
	va_end(args);

void log(Level l, const char* fmt, ...)
{
	if (level > l) return;
	va_list args;
	va_start(args, fmt);
	write_line(l, fmt, args);
	va_end(args);
}

void trace(const char* fmt, ...)
{
	LOG_AT(TRACE);
}

void debug(const char* fmt, ...)
{
	LOG_AT(DEBUG);
}

void info(const char* fmt, ...)
{
	LOG_AT(INFO);
}

void warn(const char* fmt, ...)
{
	LOG_AT(WARN);
}

void error(const char* fmt, ...)
{
	LOG_AT(ERROR);
}

// Every occurrence counts; each distinct message is printed once, with the
// frame it first happened at, so a register poked in a loop does not flood
// the log.
static void report_once(const char* tag, const char* fmt, va_list args)
{
	static std::set<std::string> reported;
	constexpr size_t MAX_REPORTED = 256;

	unimplemented_hits++;
	char message[512];
	vsnprintf(message, sizeof message, fmt, args);
	if (reported.size() >= MAX_REPORTED || !reported.insert(std::string(tag) + message).second) return;

	FILE* f = out();
	if (frame_prefix)
	{
		fprintf(f, "[f%llu] ", (unsigned long long)frame);
	}
	fprintf(f, "[ERROR] [%s] %s (first at frame %llu)\n", tag, message, (unsigned long long)frame);
	if (reported.size() == MAX_REPORTED)
	{
		fputs("[ERROR] further distinct reports suppressed\n", f);
	}
	fflush(f);
}

void unimplemented(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	report_once("UNIMPLEMENTED", fmt, args);
	va_end(args);
}

void fault(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	report_once("FAULT", fmt, args);
	va_end(args);
}

uint64_t unimplemented_count()
{
	return unimplemented_hits;
}

}  // namespace Log
