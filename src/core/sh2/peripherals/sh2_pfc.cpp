#include <log/log.h>

#include <cassert>
#include "core/sh2/peripherals/sh2_pfc.h"
#include "core/sh2/sh2_bus.h"

namespace SH2::OCPM::PFC
{

struct GpioState
{
	uint16_t output[2];
	uint16_t direction[2];
	//Pin function controls (PACR1/2, PBCR1/2) and CASCR: kept as written so
	//read-modify-writes work; their pin functions are not emulated
	uint16_t ctrl1[2];
	uint16_t ctrl2[2];
	uint16_t cascr;
};

static GpioState gpio;

//How the cartridge is wired to the ports (a property of the board, not state):
//its detect line DET (PA8) is tied high, or on some boards to PB1 (the "Magical
//Shop unlock"), and its pull-ups hold some port B inputs high.
static bool det_on_pb1 = false;
static uint16_t pb_pullups = (1 << 3) | (1 << 1);

void set_cart_wiring(bool det_pb1, uint16_t pullups)
{
	det_on_pb1 = det_pb1;
	pb_pullups = pullups;
}

void save_state(State::Writer& w)
{
	w.section("pfc");
	w.put(gpio);
}

void load_state(State::Reader& r)
{
	r.section("pfc");
	r.get(gpio);
}

uint16_t read_gpio_inputs(int port)
{
	if (port == 0)
	{
		// Port A
		//PA8 is the cartridge's DET line: high, or the level of PB1 on boards
		//that tie it there (the program driving PB1 low then sees PA8 low too)
		int pa8_det = 1;
		if (det_on_pb1)
		{
			bool pb1_output = gpio.direction[1] & (1 << 1);
			pa8_det = pb1_output ? (gpio.output[1] >> 1) & 1 : (pb_pullups >> 1) & 1;
		}
		constexpr int pa11_unk = 0;  //Tied hard low on all known boards, BIOS copies to an unknown VDP option
		return (pa11_unk << 11) | (pa8_det << 8);
	}
	else if (port == 1)
	{
		// Port B: the inputs the cartridge pulls up
		return pb_pullups;
	}
	return 0;
}

uint16_t read16(uint32_t addr)
{
	addr &= 0x3F;

	int gpio_port;

	switch(addr)
	{
	case 0x00:
	case 0x02:
		gpio_port = (addr >> 1) & 1;
		{
			uint16_t input = read_gpio_inputs(gpio_port);
			uint16_t output = gpio.output[gpio_port];
			uint16_t direction = gpio.direction[gpio_port];
			return (output & direction) | (input & ~direction);
		}
	case 0x04:
	case 0x06:
		gpio_port = (addr >> 1) & 1;
		return gpio.direction[gpio_port];
	case 0x08:
	case 0x0C:
		return gpio.ctrl1[(addr >> 2) & 1];
	case 0x0A:
	case 0x0E:
		return gpio.ctrl2[(addr >> 2) & 1];
	case 0x2E:
		return gpio.cascr;
	default:
		Log::warn("[PFC] unmapped read %08X", addr);
		return 0;
	}
}

void write16(uint32_t addr, uint16_t value)
{
	addr &= 0x3F;

	int gpio_port;

	switch(addr)
	{
	case 0x00:
	case 0x02:
		gpio_port = (addr >> 1) & 1;
		gpio.output[gpio_port] = value;
		Log::debug("[PFC] GPIO write P%sDR: %04X", gpio_port ? "B" : "A", value);
		break;
	case 0x04:
	case 0x06:
		gpio_port = (addr >> 1) & 1;
		gpio.direction[gpio_port] = value;
		Log::debug("[PFC] GPIO write P%sIOR: %04X", gpio_port ? "B" : "A", value);
		break;
	case 0x08:
	case 0x0C:
		gpio_port = (addr >> 2) & 1;
		gpio.ctrl1[gpio_port] = value;
		Log::debug("[PFC] GPIO write P%sCR1: %04X", gpio_port ? "B" : "A", value);
		break;
	case 0x0A:
	case 0x0E:
		gpio_port = (addr >> 2) & 1;
		gpio.ctrl2[gpio_port] = value;
		Log::debug("[PFC] GPIO write P%sCR2: %04X", gpio_port ? "B" : "A", value);
		break;
	case 0x2E:
		//CASCR: the DRAM /CASH and /CASL pin functions (the BIOS sets 0xAFFF)
		gpio.cascr = value;
		break;
	default:
		Log::warn("[PFC] unmapped write %08X: %04X", addr, value);
	}
}

void initialize()
{
	gpio = {};
}

}  // namespace SH2::OCPM::PFC