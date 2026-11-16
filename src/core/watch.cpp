#include "core/watch.h"

#include <vector>

#include "core/sh2/peripherals/sh2_dmac.h"
#include "core/sh2/sh2_local.h"

namespace Watch
{

bool writes_active = false;

struct WriteWatch
{
	int id;
	uint32_t addr;
	uint32_t length;
	WriteFunc func;
};

struct SerialWatch
{
	int id;
	SerialFunc func;
};

static std::vector<WriteWatch> write_watches;
static std::vector<SerialWatch> serial_watches;
static int next_id = 1;

uint32_t normalize(uint32_t addr)
{
	//Same folding as the SH2 bus: bits 28-31 are ignored, and everything
	//except the on-chip region is mirrored at bit 27.
	if ((addr & 0x0F000000) != 0x0F000000)
	{
		return addr & ~0xF8000000;
	}
	return addr & ~0xF0000000;
}

int add_write(uint32_t addr, uint32_t length, WriteFunc func)
{
	int id = next_id++;
	write_watches.push_back({id, normalize(addr), length ? length : 1, func});
	writes_active = true;
	return id;
}

int add_serial(SerialFunc func)
{
	int id = next_id++;
	serial_watches.push_back({id, func});
	return id;
}

void remove(int id)
{
	for (auto it = write_watches.begin(); it != write_watches.end(); ++it)
	{
		if (it->id == id)
		{
			write_watches.erase(it);
			break;
		}
	}
	for (auto it = serial_watches.begin(); it != serial_watches.end(); ++it)
	{
		if (it->id == id)
		{
			serial_watches.erase(it);
			break;
		}
	}
	writes_active = !write_watches.empty();
}

void on_write(uint32_t addr, int size, uint32_t value)
{
	uint32_t pc = SH2::OCPM::DMAC::is_dma_access() ? 0 : SH2::sh2.executing_addr;
	//Copy: a callback may add or remove watches
	std::vector<WriteWatch> watches = write_watches;
	for (const auto& w : watches)
	{
		if (addr < w.addr + w.length && addr + size > w.addr)
		{
			w.func(addr, size, value, pc);
		}
	}
}

void on_serial(int port, uint8_t byte)
{
	std::vector<SerialWatch> watches = serial_watches;
	for (const auto& w : watches)
	{
		w.func(port, byte);
	}
}

}  // namespace Watch
