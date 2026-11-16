#include "automation/debugtools.h"

#include <core/timing.h>
#include <core/watch.h>
#include <log/log.h>
#include <video/video.h>

#include <cstdio>
#include <sstream>
#include <vector>

namespace DebugTools
{

namespace
{

FILE* serial_files[2] = {nullptr, nullptr};
bool serial_timed = false;
FILE* watch_file = nullptr;
std::vector<int> watch_ids;
int serial_watch = 0;
uint64_t frame = 0;
int64_t frame_start = 0;

int64_t cycle_in_frame()
{
	return Timing::get_timestamp(Timing::CPU_TIMER) - frame_start;
}

void on_serial(int port, uint8_t byte)
{
	FILE* f = (port == 0 || port == 1) ? serial_files[port] : nullptr;
	if (!f) return;
	if (serial_timed)
	{
		fprintf(f, "%llu %lld %02X\n", (unsigned long long)frame, (long long)cycle_in_frame(), byte);
	}
	else
	{
		fputc(byte, f);
	}
}

void on_write(uint32_t addr, int size, uint32_t value, uint32_t pc)
{
	char line[160];
	snprintf(
		line, sizeof line, "[Watch] frame %llu line %3d %s %08X write%d %08X = %0*X", (unsigned long long)frame,
		Video::get_vcount(), pc ? "pc" : "DMA", pc, size * 8, addr, size * 2, value
	);
	if (watch_file)
	{
		fputs(line, watch_file);
		fputc('\n', watch_file);
	}
	else
	{
		Log::info("%s", line);
	}
}

bool parse_number(const std::string& s, uint64_t& out)
{
	try
	{
		size_t used = 0;
		out = std::stoull(s, &used, 0);
		return used == s.size();
	}
	catch (std::exception&)
	{
		return false;
	}
}

}  // namespace

bool configure(const Settings& s, std::string& error)
{
	shutdown();
	serial_timed = s.serial_timed;
	bool any_serial = false;
	for (int port = 0; port < 2; port++)
	{
		if (s.serial_log[port].empty()) continue;
		serial_files[port] = fopen(s.serial_log[port].c_str(), "wb");
		if (!serial_files[port])
		{
			error = "cannot write serial log " + s.serial_log[port];
			return false;
		}
		any_serial = true;
	}
	if (any_serial) serial_watch = Watch::add_serial(on_serial);

	if (!s.watch_writes.empty())
	{
		if (!s.watch_log.empty())
		{
			watch_file = fopen(s.watch_log.c_str(), "wb");
			if (!watch_file)
			{
				error = "cannot write watch log " + s.watch_log;
				return false;
			}
		}
		std::stringstream list(s.watch_writes);
		std::string item;
		while (std::getline(list, item, ','))
		{
			size_t a = item.find_first_not_of(" \t");
			if (a == std::string::npos) continue;
			item = item.substr(a, item.find_last_not_of(" \t") - a + 1);
			uint64_t addr, length = 1;
			size_t colon = item.find(':');
			bool ok = parse_number(item.substr(0, colon), addr);
			if (ok && colon != std::string::npos) ok = parse_number(item.substr(colon + 1), length);
			if (!ok || addr > 0xFFFFFFFFull || length == 0)
			{
				error = "debug.watch_writes: expected ADDR[:LENGTH], got '" + item + "'";
				return false;
			}
			watch_ids.push_back(Watch::add_write((uint32_t)addr, (uint32_t)length, on_write));
		}
	}
	return true;
}

void begin_frame(uint64_t f)
{
	frame = f;
	frame_start = Timing::get_timestamp(Timing::CPU_TIMER);
	for (FILE* file : serial_files)
	{
		if (file) fflush(file);
	}
	if (watch_file) fflush(watch_file);
}

void shutdown()
{
	for (FILE*& file : serial_files)
	{
		if (file) fclose(file);
		file = nullptr;
	}
	if (serial_watch)
	{
		Watch::remove(serial_watch);
		serial_watch = 0;
	}
	for (int id : watch_ids) Watch::remove(id);
	watch_ids.clear();
	if (watch_file) fclose(watch_file);
	watch_file = nullptr;
}

}  // namespace DebugTools
