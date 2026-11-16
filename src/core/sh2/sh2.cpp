#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <log/log.h>

#include <common/bswp.h>

#include "core/sh2/peripherals/sh2_dmac.h"
#include "core/sh2/peripherals/sh2_intc.h"
#include "core/sh2/peripherals/sh2_ocpm.h"
#include "core/sh2/peripherals/sh2_pfc.h"
#include "core/sh2/peripherals/sh2_serial.h"
#include "core/sh2/peripherals/sh2_timers.h"
#include "core/sh2/sh2.h"
#include "core/sh2/sh2_bus.h"
#include "core/sh2/sh2_cost.h"
#include "core/sh2/sh2_interpreter.h"
#include "core/sh2/sh2_local.h"
#include "core/memory.h"
#include "core/timing.h"

namespace SH2
{

CPU sh2;

// Console timing model (sh2_cost.h): cycles the executed instructions have
// cost and the pipeline has not yet waited out, and the instruction they are
// charged to in the profile.
static double cost_owed = 0.0;
static uint32_t cost_addr = 0;

static bool can_accept_exception(int vector_id, int prio)
{
	int imask = (sh2.sr >> 4) & 0xF;
	if (imask == 0xF)
	{
		return false;
	}
	return prio > imask;
}

static bool can_execute_exception(int vector_id, int prio)
{
	//Some types not accepted after certain instructions (SH7021 datasheet tables 4.9 & 4.2)
	bool is_address_error = (vector_id >= 9 && vector_id <= 10);
	bool is_interrupt = (vector_id >= 11 && vector_id <= 12) || (vector_id >= 64);

	//Our implementation of the pipeline explodes if we allow any exception
	//right after the pipeline became invalid. This fixes it.
	if (!sh2.pipeline_valid)
	{
		return false;
	}
	if (sh2.in_delay_slot && (is_address_error || is_interrupt))
	{
		return false;
	}
	if (sh2.in_nointerrupt_slot && is_interrupt)
	{
		return false;
	}

	return true;
}

void raise_exception(int vector_id);

static bool handle_exception()
{
	if (sh2.pending_exception_vector)
	{
		int vector = sh2.pending_exception_vector;
		int prio = sh2.pending_exception_prio;
		if (can_execute_exception(vector, prio))
		{
			if (Cost::enabled)
			{
				Cost::begin(0x0009);
				raise_exception(vector);
				cost_owed += Cost::end() + Cost::EXCEPTION_STATES;
			}
			else
			{
				raise_exception(vector);
			}

			int new_imask = std::clamp(prio, 0, 15);
		
			//Interrupt mask should only be modified after the above function so that the original value can be pushed onto the stack
			sh2.sr &= ~0xF0;
			sh2.sr |= new_imask << 4;

			sh2.pending_exception_vector = 0;
			return true;
		}
	}
	return false;
}

void initialize()
{
	sh2 = {};

	sh2.pagetable = Memory::get_sh2_pagetable();
	sh2.write_pagetable = Memory::get_sh2_write_pagetable();

	//TODO: make config option to skip BIOS boot?
	bool skip_bios_boot = false;
	if (skip_bios_boot)
	{
		set_pc(0x0E000480);
		sh2.gpr[15] = 0;
	}
	else
	{
		//The initial values of PC and SP are read from the vector table
		int boot_type = 0;
		uint8_t* boot_vectors = sh2.pagetable[0];
		uint32_t reset_pc, reset_sp;
		memcpy(&reset_pc, boot_vectors + boot_type*8 + 0, 4);
		memcpy(&reset_sp, boot_vectors + boot_type*8 + 4, 4);
		set_pc(Common::bswp32(reset_pc));
		sh2.gpr[15] = Common::bswp32(reset_sp);
	}

	//Next, VBR is cleared to zero and interrupt mask bits in SR are set to 1111
	sh2.vbr = 0;
	sh2.sr |= 0xF << 4;

	//Initialize pipeline & execution state
	sh2.pipeline_valid = false;
	sh2.in_delay_slot = false;
	sh2.in_nointerrupt_slot = false;
	sh2.fetch_cycles = 1;

	cost_owed = 0.0;
	cost_addr = 0;
	Cost::reset();

	Timing::register_timer(Timing::CPU_TIMER, &sh2.cycles_left, run);

	//Set up on-chip peripheral modules after CPU is done
	OCPM::DMAC::initialize();
	OCPM::INTC::initialize();
	OCPM::PFC::initialize();
	OCPM::Serial::initialize();
	OCPM::Timer::initialize();
}

void shutdown()
{
	sh2.hooks.clear();
}

static bool prof_on = false;
static std::vector<uint32_t> prof_rom;
static std::vector<uint32_t> prof_ram;
static uint64_t prof_total = 0, prof_other = 0;

void profile_enable(bool on)
{
	if (on && prof_rom.empty())
	{
		prof_rom.assign(0x200000, 0);
		prof_ram.assign(0x40000, 0);
	}
	prof_on = on;
	Cost::split_enable(on);
}

void console_cost_enable(bool on)
{
	Cost::enabled = on;
}

bool profile_write(const std::string& path, const std::string& note)
{
	FILE* f = fopen(path.c_str(), "wb");  //LF line endings on every platform
	if (!f) return false;
	fprintf(f, "# cycles total %llu other %llu\n", (unsigned long long)prof_total, (unsigned long long)prof_other);
	if (!note.empty()) fprintf(f, "# %s\n", note.c_str());
	if (Cost::enabled)
	{
		fprintf(f, "# console pricing (--console-cost)\n");
		fputs(Cost::split_text().c_str(), f);
	}
	for (size_t i = 0; i < prof_rom.size(); i++)
		if (prof_rom[i]) fprintf(f, "%08X %u\n", (unsigned)(0x0E000000 + 2 * i), prof_rom[i]);
	for (size_t i = 0; i < prof_ram.size(); i++)
		if (prof_ram[i]) fprintf(f, "%08X %u\n", (unsigned)(0x09000000 + 2 * i), prof_ram[i]);
	fclose(f);
	return true;
}

static inline void profile_tick(uint32_t a)
{
	prof_total++;
	if ((a & 0xFFC00000u) == 0x0E000000u) prof_rom[(a & 0x3FFFFFu) >> 1]++;
	else if ((a & 0xFFF80000u) == 0x09000000u) prof_ram[(a & 0x7FFFFu) >> 1]++;
	else prof_other++;
}

void run()
{
	while (sh2.cycles_left)
	{
		if (prof_on) profile_tick(Cost::enabled ? cost_addr : sh2.pipeline_src_addr);
		bool last_instruction_done = true; //TODO: wait on longer instructions like multiply

		sh2.fetch_cycles -= 1;
		if (sh2.fetch_cycles <= 0)
		{
			sh2.fetch_cycles = 0;
			sh2.fetch_done = true;
		}

		bool pipeline_ready = sh2.fetch_done && last_instruction_done;
		
		if (pipeline_ready)
		{
			//Handle any pending exceptions first, this may change the following fetch
			handle_exception();

			//Start the next fetch with the current PC
			uint32_t fetch_src_addr = sh2.pc;
			uint16_t fetch_instruction = Bus::read16(fetch_src_addr);
			sh2.fetch_cycles = Bus::read_cycles(fetch_src_addr);
			sh2.fetch_done = false;
			
			//Advance the pipeline
			uint32_t execute_src_addr = sh2.pipeline_src_addr;
			uint16_t execute_instruction = sh2.pipeline_instruction;
			bool execute_valid = sh2.pipeline_valid;
			sh2.pipeline_src_addr = fetch_src_addr;
			sh2.pipeline_instruction = fetch_instruction;
			sh2.pipeline_valid = true;
			sh2.pc += 2;

			//Find and run the hook function at this address
			//TODO: split into smaller paged maps for performance
			if (sh2.hooks.find(execute_src_addr) != sh2.hooks.end())
			{
				SH2::HookFunc hook = sh2.hooks.at(execute_src_addr);
				
				//If hook returns true, the actual instruction is skipped
				if (hook(execute_src_addr))
				{
					execute_valid = false;
				}
			}

			//Execute whatever just came off the pipeline
			bool was_delay_slot = sh2.in_delay_slot;
			bool was_nointerrupt_slot = sh2.in_nointerrupt_slot;
			sh2.executing_addr = execute_src_addr;
			if (execute_valid && Cost::enabled)
			{
				// Console timing: fetch, then the data accesses in order (the
				// open row moves with both), then the instruction's states.
				bool onchip;
				double fcost = Cost::fetch(execute_src_addr, &onchip);
				Cost::begin(execute_instruction);
				SH2::Interpreter::run(execute_instruction, execute_src_addr);
				double data = Cost::end();
				bool taken = !sh2.pipeline_valid;
				bool delayed = sh2.in_delay_slot && !was_delay_slot;
				cost_owed += Cost::price(execute_instruction, fcost, onchip, data, taken, delayed);
				cost_addr = execute_src_addr;
			}
			else if (execute_valid)
			{
				SH2::Interpreter::run(execute_instruction, execute_src_addr);
			}
			if (Cost::enabled)
			{
				// The next issue waits out what this one cost. A pipeline
				// refill still takes the loop's one cycle; the branch that
				// caused it already paid for the refetch, so that cycle is
				// taken back from the debt.
				cost_owed += Cost::take_pending();
				int wait = (int)cost_owed;
				if (wait < 1) wait = 1;
				cost_owed -= wait;
				sh2.fetch_cycles = wait;
			}
			if (sh2.address_error)
			{
				//CPU address error, taken once the faulting instruction is done
				sh2.address_error = false;
				raise_exception(9);
			}
			//This should probably be done more directly in the interpreter
			if (was_delay_slot)
			{
				sh2.in_delay_slot = false;
			}
			if (was_nointerrupt_slot)
			{
				sh2.in_nointerrupt_slot = false;
			}
		}
		sh2.cycles_left -= 1;
	}
}

//The interrupt controller's highest request. An accepted request is latched
//until taken (the VDP's interrupts are pulses). One the mask holds back stays
//pending at the controller while its source stays up (the ITU and SCI
//flags), and is offered again when the mask is lowered (set_sr).
void assert_irq(int vector_id, int prio)
{
	if (!can_accept_exception(vector_id, prio))
	{
		return;
	}
	sh2.pending_exception_vector = vector_id;
	sh2.pending_exception_prio = prio;
}

void raise_exception(int vector_id)
{
	assert(vector_id < 0x100);

	//Push SR and PC onto the stack
	sh2.gpr[15] -= 4;
	Bus::write32(sh2.gpr[15], sh2.sr);
	sh2.gpr[15] -= 4;
	Bus::write32(sh2.gpr[15], sh2.pc - 2);

	uint32_t vector_addr = sh2.vbr + (vector_id * 4);
	uint32_t new_pc = Bus::read32(vector_addr);

	set_pc(new_pc);
	sh2.pipeline_valid = false;
}

void set_pc(uint32_t new_pc)
{
	sh2.pc = new_pc;
}

void set_sr(uint32_t new_sr)
{
	uint32_t old_mask = (sh2.sr >> 4) & 0xF;
	sh2.sr = new_sr & 0x3F3;
	if (((sh2.sr >> 4) & 0xF) < old_mask)
	{
		//A request held back by the old mask may be taken now
		OCPM::INTC::resend();
	}
}

void save_state(State::Writer& w)
{
	w.section("sh2");
	w.put(sh2.gpr);
	w.put(sh2.pc);
	w.put(sh2.pr);
	w.put(sh2.macl);
	w.put(sh2.mach);
	w.put(sh2.gbr);
	w.put(sh2.vbr);
	w.put(sh2.sr);
	w.put(sh2.cycles_left);
	w.put(sh2.pending_exception_prio);
	w.put(sh2.pending_exception_vector);
	w.put(sh2.fetch_done);
	w.put(sh2.fetch_cycles);
	w.put(sh2.pipeline_src_addr);
	w.put(sh2.pipeline_instruction);
	w.put(sh2.pipeline_valid);
	w.put(sh2.in_delay_slot);
	w.put(sh2.in_nointerrupt_slot);
	w.put(sh2.executing_addr);
	w.put(cost_owed);
	w.put(cost_addr);
	Cost::save_state(w);
	OCPM::save_state(w);
}

void load_state(State::Reader& r)
{
	r.section("sh2");
	r.get(sh2.gpr);
	r.get(sh2.pc);
	r.get(sh2.pr);
	r.get(sh2.macl);
	r.get(sh2.mach);
	r.get(sh2.gbr);
	r.get(sh2.vbr);
	r.get(sh2.sr);
	r.get(sh2.cycles_left);
	r.get(sh2.pending_exception_prio);
	r.get(sh2.pending_exception_vector);
	r.get(sh2.fetch_done);
	r.get(sh2.fetch_cycles);
	r.get(sh2.pipeline_src_addr);
	r.get(sh2.pipeline_instruction);
	r.get(sh2.pipeline_valid);
	r.get(sh2.in_delay_slot);
	r.get(sh2.in_nointerrupt_slot);
	r.get(sh2.executing_addr);
	r.get(cost_owed);
	r.get(cost_addr);
	Cost::load_state(r);
	OCPM::load_state(r);
}

void add_hook(uint32_t address, HookFunc hook)
{
	sh2.hooks.emplace(address, hook);
}

void remove_hook(uint32_t address)
{
	sh2.hooks.erase(address);
}

}