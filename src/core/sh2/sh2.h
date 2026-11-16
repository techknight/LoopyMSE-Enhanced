#pragma once

#include <string>

#include "core/state.h"

namespace SH2
{

void initialize();
void shutdown();
void run();

// The CPU and its on-chip peripherals
void save_state(State::Writer& w);
void load_state(State::Reader& r);

// CPU profile: while enabled, every emulated CPU cycle is charged to the
// address of the instruction being executed. Cartridge ROM (0x0E000000,
// 4 MB) and main RAM (0x09000000, 512 KB) are kept per address; everything
// else is one total.
void profile_enable(bool on);
bool profile_write(const std::string& path, const std::string& note = "");

// Console timing model for every instruction (sh2_cost.h); off = stock timing.
void console_cost_enable(bool on);

}