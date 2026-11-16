#pragma once
#include "core/config.h"

namespace Cart
{

constexpr static int SRAM_START = 0x02000000;
constexpr static int ROM_START = 0x06000000;

void initialize(Config::CartInfo& info);
void shutdown(Config::CartInfo& info);

void sram_commit_check();

//Dev automation: when disabled, SRAM is never written back to the .sav file (default: enabled)
void set_sram_write_enabled(bool enabled);

}