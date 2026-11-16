#include <common/wordops.h>
#include <core/loopy_io.h>
#include <core/sh2/sh2_local.h>
#include <log/log.h>
#include <video/video.h>

#include <algorithm>
#include <cassert>
#include <cstdio>

namespace LoopyIO
{

struct PadState
{
	bool plugged;
	uint16_t buttons;
};

struct MouseState
{
	bool plugged;
	uint16_t buttons;
	uint16_t counter_x;
	uint16_t counter_y;
	uint8_t encoder_x;	//Position of each rotary encoder in its 4-step gray-code cycle
	uint8_t encoder_y;
};

struct State
{
	uint16_t latched_sensors;
	uint16_t print_temp;  //ADC result of the last conversion, in bits 15-6
	uint16_t adc_ctrl;	  //ADC channel select and sampling bits (5-0)
	uint16_t exp_timing;  //Expansion strobe timing (0x020)
	uint16_t print_motor;  //Stepper phases (0x042)
	uint16_t head_ctrl;	   //Print head control (0x044)
	uint16_t control_out;  //Controller output pins (0x054)
	PadState pad;
	MouseState mouse;
	bool scan_pad;
	bool scan_mouse;
};

static State state;

void initialize()
{
	state = {};
}

void shutdown()
{
	//nop
}

//The mouse's input pins, as every controller byte reads them (measured with
//LoopyProbe): bit 7 detect (always 1), bit 6 right button and bit 4 left button
//(active-low), bits 3-0 the raw encoders YB YA XB XA. Each encoder steps through
//the gray code 00, 01, 11, 10 (B A) per count in the + direction, so the bits
//stay still while the mouse does and change with every count of motion; software
//that reads the pad while a mouse is on the port sees phantom buttons only then.
static uint8_t mouse_pins()
{
	static const uint8_t gray[4] = {0b00, 0b01, 0b11, 0b10};
	uint8_t pins = 0x80 | gray[state.mouse.encoder_x & 3] | (gray[state.mouse.encoder_y & 3] << 2);
	if (!(state.mouse.buttons & 0x1000)) pins |= 0x10;
	if (!(state.mouse.buttons & 0x4000)) pins |= 0x40;
	return pins;
}

//The delta counters are signed 12-bit and saturate at +2047 and -2048
static uint16_t add_saturated(uint16_t counter, int delta)
{
	int value = (counter & 0x800) ? (int)(counter & 0xFFF) - 0x1000 : (int)(counter & 0xFFF);
	value = std::clamp(value + delta, -2048, 2047);
	return (uint16_t)(value & 0xFFF);
}

uint8_t reg_read8(uint32_t addr)
{
	READ_HALFWORD(reg, addr);
}

static uint16_t read16_impl(uint32_t addr);
static void write16_impl(uint32_t addr, uint16_t value);

//Log every register access (debug.trace_io): which scan mode a game arms,
//what it reads from the pad and mouse registers, and from where.
static bool trace = false;

void set_trace(bool enabled)
{
	trace = enabled;
}

void save_state(::State::Writer& w)
{
	w.section("loopyio");
	w.put(state);
}

void load_state(::State::Reader& r)
{
	r.section("loopyio");
	r.get(state);
}

uint16_t reg_read16(uint32_t addr)
{
	uint16_t value = read16_impl(addr);
	if (trace)
	{
		Log::info(
			"[IO trace] read  %03X -> %04X  line %3d pc %08X", addr & 0xFFE, value, Video::get_vcount(),
			SH2::sh2.executing_addr
		);
	}
	return value;
}

void reg_write16(uint32_t addr, uint16_t value)
{
	if (trace)
	{
		Log::info(
			"[IO trace] write %03X <- %04X  line %3d pc %08X", addr & 0xFFE, value, Video::get_vcount(),
			SH2::sh2.executing_addr
		);
	}
	write16_impl(addr, value);
}

static uint16_t read16_impl(uint32_t addr)
{
	addr &= 0xFFE;
	switch (addr)
	{
	case 0x000:
		//ANALOG_IN: the 10-bit result of the last conversion and the control bits
		return state.print_temp | (state.adc_ctrl & 0x3F);
	case 0x020:
		return state.exp_timing;
	case 0x042:
		return state.print_motor & 0xF;
	case 0x044:
		return state.head_ctrl & 0xF;
	case 0x054:
		return state.control_out;
	case 0x010:
		if (state.scan_pad && state.pad.plugged)
		{
			return ((state.pad.buttons << 4) & 0x0F00) | (state.pad.buttons & 0x000E) | 0x0001;
		}
		else if (state.mouse.plugged)
		{
			//Both bytes of all three registers read the mouse's pins; bit 15 is
			//how games detect it
			return mouse_pins() * 0x0101;
		}
		return 0;
	case 0x012:
		if (state.scan_pad && state.pad.plugged)
		{
			return (state.pad.buttons >> 8) & 0x000F;
		}
		else if (state.mouse.plugged)
		{
			return mouse_pins() * 0x0101;
		}
		return 0;
	case 0x014:
		return state.mouse.plugged ? mouse_pins() * 0x0101 : 0;
	case 0x030:
		return state.latched_sensors;
	case 0x050:
		if (state.mouse.plugged)
		{
			//12-bit delta counter, L active-low in bit 12, R active-low in bit 14.
			//Reading clears the counter. Bits 13 and 15 read low on hardware.
			uint16_t mouse_xreg = (state.mouse.counter_x & 0xFFF) | ((~state.mouse.buttons) & 0x5000);
			state.mouse.counter_x = 0;
			return mouse_xreg;
		}
		return 0;
	case 0x052:
		if (state.mouse.plugged)
		{
			//12-bit delta counter, read-to-clear. No buttons mirrored here.
			uint16_t mouse_yreg = state.mouse.counter_y & 0xFFF;
			state.mouse.counter_y = 0;
			return mouse_yreg;
		}
		return 0;
	default:
		Log::warn("[IO] unmapped read16 %08X", addr);
		return 0;
	}
}

uint32_t reg_read32(uint32_t addr)
{
	READ_DOUBLEWORD(reg, addr);
}

void reg_write8(uint32_t addr, uint8_t value)
{
	WRITE_HALFWORD(reg, addr, value);
}

static void write16_impl(uint32_t addr, uint16_t value)
{
	addr &= 0xFFE;
	//The printer's motor and head registers only take writes while the printer
	//is enabled (ENP, sensor register bit 8) and carry a fixed key in bits 15-4
	bool printer_enabled = (state.latched_sensors & 0x0100) != 0;
	switch (addr)
	{
	case 0x000:
		//ANALOG_IN: bits 5-3 select the channel, bits 2-0 the sampling
		state.adc_ctrl = value & 0x3F;
		break;
	case 0x020:
		//EXP_TIMING: strobe speed of the sound and expansion areas
		state.exp_timing = value & 0xF;
		break;
	case 0x030:
		state.latched_sensors = (state.latched_sensors & ~0x0100) | (value & 0x0100);
		break;
	case 0x040:
		//PRINT_HEAD_DATA: the dot data the print DMA streams (printing is
		//emulated at a high level, so the data is not used)
		break;
	case 0x042:
		if (printer_enabled && (value >> 4) == 0x5A5) state.print_motor = value;
		break;
	case 0x044:
		if (printer_enabled && (value >> 4) == 0xA5A) state.head_ctrl = value;
		break;
	case 0x054:
		//CONTROL_OUT: controller output pins in direct mode
		state.control_out = value & 0x3F;
		break;
	default:
		Log::warn("[IO] unmapped write16 %08X: %04X", addr, value);
	}
}

void reg_write32(uint32_t addr, uint32_t value)
{
	WRITE_DOUBLEWORD(reg, addr, value);
}

void update_pad(int btn_info, bool pressed)
{
	if (state.pad.plugged)
	{
		if (pressed)
		{
			state.pad.buttons |= btn_info;
		}
		else
		{
			state.pad.buttons &= ~btn_info;
		}
	}
}

void update_mouse_buttons(int btn_info, bool pressed)
{
	if (state.mouse.plugged)
	{
		if (pressed)
		{
			state.mouse.buttons |= btn_info;
		}
		else
		{
			state.mouse.buttons &= ~btn_info;
		}
	}
}

void update_mouse_position(int delta_x, int delta_y)
{
	if (!state.mouse.plugged)
	{
		return;
	}
	//The encoder pins follow the motion whatever the scan mode
	state.mouse.encoder_x = (uint8_t)((state.mouse.encoder_x + delta_x) & 3);
	state.mouse.encoder_y = (uint8_t)((state.mouse.encoder_y + delta_y) & 3);
	//Measured on hardware: the delta counters only accumulate under mouse-only
	//scan. With the pad scan bit also set the same motion produces no counts,
	//so software that leaves both scan bits armed sees a frozen cursor.
	if (state.scan_mouse && !state.scan_pad)
	{
		state.mouse.counter_x = add_saturated(state.mouse.counter_x, delta_x);
		state.mouse.counter_y = add_saturated(state.mouse.counter_y, delta_y);
	}
}

void set_controller_scan_mode(bool scan_pad, bool scan_mouse)
{
	state.scan_pad = scan_pad;
	state.scan_mouse = scan_mouse;
}

void set_controller_plugged(bool plugged_pad, bool plugged_mouse)
{
	state.pad.plugged = plugged_pad && !plugged_mouse;
	state.mouse.plugged = plugged_mouse;

	if (!plugged_mouse)
	{
		state.mouse.buttons = 0;
		state.mouse.counter_x = 0;
		state.mouse.counter_y = 0;
		state.mouse.encoder_x = 0;
		state.mouse.encoder_y = 0;
	}
}

//An ADC conversion of the channel ANALOG_IN selects (bits 5-3)
void update_print_temp()
{
	int value;
	switch ((state.adc_ctrl >> 3) & 7)
	{
	case 0:
	{
		//Print head thermistor; the BIOS prints below 580 and warns below 670
		float temp = 22.f;
		value = std::clamp((int)(temp * 16), 0, 0x3FF);
		break;
	}
	case 1:
		//Head calibration resistor (a typical head)
		value = 0x1AF;
		break;
	case 2:
		//Contrast knob, centred
		value = 0x200;
		break;
	default:
		//Cartridge analog inputs, not connected
		value = 0x3FF;
		break;
	}
	state.print_temp = value << 6;
}

//The sticker cassette in the printer: 0 = none, otherwise its type (1 = XS-11,
//the type the BIOS print hooks emulate)
static int seal_cassette = 1;

void set_seal_cassette(int type)
{
	seal_cassette = type & 7;
}

int get_seal_cassette()
{
	return seal_cassette;
}

void update_sensors()
{
	//Stock mainboard is always configured for NTSC
	constexpr int region_jumper = 1;

	const bool seal_cartridge_present = seal_cassette != 0;
	const int seal_cartridge_type = seal_cassette;

	//Set sensors for appropriate idle state
	int print_mech_sensors = seal_cartridge_present ? 0b100 : 0b011;

	state.latched_sensors = (state.latched_sensors & 0x0100) | ((seal_cartridge_type & 7) << 4) | ((print_mech_sensors & 7) << 1) | (region_jumper & 1);
}

}  // namespace LoopyIO