#pragma once
#include <string>
#include <vector>

#include "core/config.h"

namespace System
{

void initialize(Config::SystemInfo& config);
void shutdown(Config::SystemInfo& config);

void run();

uint16_t* get_display_output();

// Savestates, between two run() calls. A state names its cartridge and the
// frame it was taken at, and only loads into the same build of the emulator
// with the same cartridge. The sound synthesizer restarts silent on load.
std::vector<uint8_t> save_state(const Config::SystemInfo& config, uint64_t frame);
bool load_state(Config::SystemInfo& config, const std::vector<uint8_t>& data, uint64_t& frame, std::string& error);

}