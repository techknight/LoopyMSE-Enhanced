#pragma once
#include <cstdint>
#include <functional>

// Observers for the emulated machine, for automation and debugging: writes to
// chosen addresses and bytes sent out of the serial ports. Nothing here
// changes emulation; with no observers registered the cost is one flag test
// per bus write.
namespace Watch
{

// Called for every CPU or DMA write that touches [addr, addr + length).
// The address passed in is the one written (normalized: mirrors folded),
// size is 1, 2 or 4 and pc the address of the instruction doing the write
// (0 for DMA).
using WriteFunc = std::function<void(uint32_t addr, int size, uint32_t value, uint32_t pc)>;

// Called for every byte a serial port finishes sending.
using SerialFunc = std::function<void(int port, uint8_t byte)>;

int add_write(uint32_t addr, uint32_t length, WriteFunc func);
int add_serial(SerialFunc func);
void remove(int id);

// Fold the address mirrors the way the bus does, so 0x0C059000 and
// 0x04059000 name the same register.
uint32_t normalize(uint32_t addr);

// Hooks for the core.
extern bool writes_active;
void on_write(uint32_t normalized_addr, int size, uint32_t value);
void on_serial(int port, uint8_t byte);

}  // namespace Watch
