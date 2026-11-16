#pragma once
#include <cstdint>

#include "core/state.h"

namespace SH2::OCPM::Timer
{

void initialize();
void save_state(State::Writer& w);
void load_state(State::Reader& r);

uint8_t read8(uint32_t addr);
uint16_t read16(uint32_t addr);

void write8(uint32_t addr, uint8_t value);
void write16(uint32_t addr, uint16_t value);

}