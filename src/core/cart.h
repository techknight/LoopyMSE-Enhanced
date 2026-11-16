#pragma once
#include "core/config.h"
#include "core/state.h"

namespace Cart
{

// The cartridge's SRAM (the ROM is not part of a savestate)
void save_state(State::Writer& w);
void load_state(State::Reader& r);

constexpr static int SRAM_START = 0x02000000;
constexpr static int ROM_START = 0x06000000;

void initialize(Config::CartInfo& info);
void shutdown(Config::CartInfo& info);

void sram_commit_check();

//Dev automation: when disabled, SRAM is never written back to the .sav file (default: enabled)
void set_sram_write_enabled(bool enabled);

}