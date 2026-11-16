#pragma once
#include <cstdint>

#include "core/state.h"

namespace SH2::OCPM::PFC
{

void initialize();
void save_state(State::Writer& w);
void load_state(State::Reader& r);

uint16_t read16(uint32_t addr);

void write16(uint32_t addr, uint16_t value);

}