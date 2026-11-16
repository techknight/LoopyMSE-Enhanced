#pragma once
#include <cstdint>

#include "core/state.h"

namespace SH2::OCPM::PFC
{

void initialize();
void save_state(State::Writer& w);
void load_state(State::Reader& r);

// The cartridge board's wiring: DET (PA8) tied to PB1 instead of high, and
// which port B inputs it pulls up (a bit mask)
void set_cart_wiring(bool det_on_pb1, uint16_t pb_pullups);

uint16_t read16(uint32_t addr);

void write16(uint32_t addr, uint16_t value);

}