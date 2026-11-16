#include "core/system.h"

#include <cstring>
#include <stdexcept>

#include <expansion/expansion.h>
#include <input/input.h>
#include <sound/sound.h>
#include <video/video.h>
#include <printer/printer.h>

#include "core/cart.h"
#include "core/loopy_io.h"
#include "core/memory.h"
#include "core/sh2/peripherals/sh2_serial.h"
#include "core/sh2/sh2.h"
#include "core/timing.h"

namespace System
{

void initialize(Config::SystemInfo& config)
{
	//Memory must initialize first
	Memory::initialize(config.bios_rom);

	//Ensure that timing initializes before any CPUs
	Timing::initialize();

	//Initialize CPUs
	SH2::initialize();

	//Initialize core hardware
	Cart::initialize(config.cart);
	LoopyIO::initialize();

	//Initialize subprojects after everything else
	Input::initialize();
	Video::initialize();
	Sound::initialize(config.sound_rom);
	Expansion::initialize(config.cart);
	Printer::initialize(config);

	//Hook up connections between modules
	SH2::OCPM::Serial::set_tx_callback(1, &Sound::midi_byte_in);
}

void shutdown(Config::SystemInfo& config)
{
	//Shutdown all components in the reverse order they were initialized
	Printer::shutdown();
	Expansion::shutdown();
	Sound::shutdown();
	Video::shutdown();
	Input::shutdown();

	LoopyIO::shutdown();
	Cart::shutdown(config.cart);

	SH2::shutdown();

	Timing::shutdown();
	Memory::shutdown();
}

void run()
{
	//Run an entire frame of emulation, stopping when the VDP reaches VSYNC
	Video::start_frame();

	while (!Video::check_frame_end())
	{
		//TODO: if multiple cores are added, ensure that they are relatively synced

		//Calculate the smallest timeslice between all cores
		int64_t slice_length = (std::numeric_limits<int64_t>::max)();
		for (int i = 0; i < Timing::NUM_TIMERS; i++)
		{
			slice_length = std::min(slice_length, Timing::calc_slice_length(i));
		}

		//Run all cores, processing any scheduler events that happen for them
		for (int i = 0; i < Timing::NUM_TIMERS; i++)
		{
			Timing::process_slice(i, slice_length);
		}
	}

	Cart::sram_commit_check();
}

uint16_t* get_display_output()
{
	return Video::get_display_output();
}

namespace
{

const char STATE_MAGIC[16] = "LOOPYMSE-STATE";
constexpr uint32_t STATE_FORMAT = 2;

uint32_t cart_id(const Config::CartInfo& cart)
{
	if (cart.rom.size() < 12) return 0;
	return (uint32_t)cart.rom[8] << 24 | (uint32_t)cart.rom[9] << 16 | (uint32_t)cart.rom[10] << 8 | cart.rom[11];
}

void write_all(State::Writer& w)
{
	Timing::save_state(w);
	SH2::save_state(w);
	Memory::save_state(w);
	Cart::save_state(w);
	LoopyIO::save_state(w);
	Video::save_state(w);
	Sound::save_state(w);
	Expansion::save_state(w);
	w.section("end");
}

void read_all(State::Reader& r)
{
	Timing::load_state(r);
	SH2::load_state(r);
	Memory::load_state(r);
	Cart::load_state(r);
	LoopyIO::load_state(r);
	Video::load_state(r);
	Sound::load_state(r);
	Expansion::load_state(r);
	r.section("end");
	if (!r.at_end()) throw std::runtime_error("savestate has unexpected data at the end");
}

}  // namespace

std::vector<uint8_t> save_state(const Config::SystemInfo& config, uint64_t frame)
{
	State::Writer w;
	w.bytes(STATE_MAGIC, sizeof(STATE_MAGIC));
	w.put(STATE_FORMAT);
	w.put(cart_id(config.cart));
	w.put((uint32_t)config.cart.rom.size());
	w.put(frame);
	write_all(w);
	return w.data;
}

bool load_state(Config::SystemInfo& config, const std::vector<uint8_t>& data, uint64_t& frame, std::string& error)
{
	try
	{
		State::Reader r(data.data(), data.size());
		char magic[sizeof(STATE_MAGIC)];
		r.bytes(magic, sizeof(magic));
		if (memcmp(magic, STATE_MAGIC, sizeof(magic)) != 0) throw std::runtime_error("not a LoopyMSE savestate");
		if (r.get<uint32_t>() != STATE_FORMAT) throw std::runtime_error("savestate is from another version");
		if (r.get<uint32_t>() != cart_id(config.cart) || r.get<uint32_t>() != (uint32_t)config.cart.rom.size())
		{
			throw std::runtime_error("savestate is for a different cartridge");
		}
		uint64_t saved_frame = r.get<uint64_t>();

		// Keep the machine as it is if the state turns out to be unusable
		State::Writer backup;
		write_all(backup);
		try
		{
			read_all(r);
		}
		catch (std::exception&)
		{
			State::Reader restore(backup.data.data(), backup.data.size());
			read_all(restore);
			throw;
		}
		frame = saved_frame;
		return true;
	}
	catch (std::exception& e)
	{
		error = e.what();
		return false;
	}
}

}  // namespace System