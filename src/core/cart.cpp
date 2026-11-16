#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include "core/cart.h"
#include "core/memory.h"

namespace Cart
{

struct State
{
	std::vector<uint8_t> rom;
	std::vector<uint8_t> sram;
	std::vector<uint8_t> sram_on_disk;  // what the .sav file holds, to write only changes
	std::string sram_file_path;
};

static State state;
static bool sram_write_enabled = true;

static void commit_sram()
{
	if (!sram_write_enabled || state.sram_file_path.empty()) return;
	if (state.sram == state.sram_on_disk) return;
	std::ofstream file(state.sram_file_path, std::ios::binary);
	file.write((char*)state.sram.data(), state.sram.size());
	if (file.good())
	{
		state.sram_on_disk = state.sram;
	}
}

void initialize(Config::CartInfo& info)
{
	state = {};

	state.rom = info.rom;
	state.sram = info.sram;
	state.sram_file_path = info.sram_file_path;

	//Ensure that the ROM and SRAM are aligned to a 4 KB boundary
	if (state.rom.size() & 0xFFF)
	{
		size_t new_size = (state.rom.size() + 0xFFF) & ~0xFFF;
		state.rom.resize(new_size, 0xFF);
	}

	if (state.sram.size() & 0xFFF)
	{
		size_t new_size = (state.sram.size() + 0xFFF) & ~0xFFF;
		state.sram.resize(new_size, 0xFF);
	}
	//Treat the loaded contents as already saved; a missing file is written
	//on the first commit
	state.sram_on_disk = info.sram_from_file ? state.sram : std::vector<uint8_t>();

	Memory::map_sh2_pagetable(state.rom.data(), ROM_START, state.rom.size());
	Memory::map_sh2_pagetable(state.sram.data(), SRAM_START, state.sram.size());
}

void shutdown(Config::CartInfo& info)
{
	commit_sram();
	info.sram = state.sram;
}

void sram_commit_check()
{
	//Force a save every 60 frames
	//TODO: is there a better way of doing this?
	static int frame_count = 0;
	frame_count++;

	if (frame_count < 60)
	{
		return;
	}

	frame_count = 0;
	commit_sram();
}

void set_sram_write_enabled(bool enabled)
{
	sram_write_enabled = enabled;
}

void save_state(::State::Writer& w)
{
	w.section("cart");
	w.put_vector(state.sram);
}

void load_state(::State::Reader& r)
{
	r.section("cart");
	std::vector<uint8_t> sram;
	r.get_vector(sram);
	if (sram.size() != state.sram.size())
	{
		throw std::runtime_error("savestate is for a cartridge with a different save size");
	}
	//In place: the SRAM buffer is mapped into the CPU's address space
	memcpy(state.sram.data(), sram.data(), sram.size());
}

}
