#pragma once
#include <cstdint>
#include <functional>

#include "core/state.h"

namespace SH2::OCPM::Serial
{

void save_state(State::Writer& w);
void load_state(State::Reader& r);

void initialize();

uint8_t read8(uint32_t addr);

void write8(uint32_t addr, uint8_t value);

void set_tx_callback(int port, std::function<void(uint8_t)> callback);

}