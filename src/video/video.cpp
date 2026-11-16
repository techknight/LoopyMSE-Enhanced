
#include <common/bswp.h>
#include <common/wordops.h>
#include <core/loopy_io.h>
#include <core/memory.h>
#include <core/sh2/peripherals/sh2_dmac.h>
#include <core/sh2/peripherals/sh2_intc.h>
#include <core/sh2/sh2_local.h>
#include <core/timing.h>
#include <log/log.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "video/render.h"
#include "video/vdp_local.h"

namespace Video
{

static Timing::FuncHandle vcount_func, hsync_func, irq0_func;
static Timing::EventHandle vcount_ev, hsync_ev, irq0_ev;

VDP vdp;

//NTSC raster timing, measured on hardware: the VDP runs at 21.477272 MHz, a
//line is 1365 VDP cycles and a frame 263 lines (59.826 frames a second). The
//active part of a line, HCOUNT 0 to 0x101 at 4 VDP cycles a step, takes 1029
//cycles; then HCOUNT jumps to 0x1AC for the blanking, and VCOUNT counts up.
constexpr static int LINES_PER_FRAME = 263;
constexpr static int64_t VDP_CLOCK = 21477272;
constexpr static int VDP_CYCLES_PER_LINE = 1365;
constexpr static int VDP_ACTIVE_CYCLES = 1029;
//2684659 VDP cycles are exactly 2000000 CPU cycles (16 MHz / 21.477272 MHz)
constexpr static int64_t VDP_REBASE = 2684659;
constexpr static int64_t CPU_REBASE = 2000000;

static int64_t vdp_to_cpu(int64_t vdp_cycles)
{
	return vdp_cycles * CPU_REBASE / VDP_REBASE;
}

//VDP cycles since the current line started
static int64_t cycle_in_line()
{
	int64_t cpu = Timing::get_timestamp(Timing::CPU_TIMER) - vdp.base_cpu;
	return cpu * VDP_REBASE / CPU_REBASE - vdp.line_start;
}

static Timing::EventHandle schedule_vdp(Timing::FuncHandle func, int64_t vdp_cycle, uint64_t param = 0)
{
	int64_t delay = vdp.base_cpu + vdp_to_cpu(vdp_cycle) - Timing::get_timestamp(Timing::CPU_TIMER);
	if (delay < 0) delay = 0;
	return Timing::add_event(func, Timing::convert_cpu(delay), param, Timing::CPU_TIMER);
}

//The line position where HCOUNT shows a value, or -1 if it never does
static int hcount_to_cycle(int hcount)
{
	if (hcount <= 0x101) return hcount * 4;
	if (hcount >= 0x1AC && hcount <= 0x1FF) return VDP_ACTIVE_CYCLES + (hcount - 0x1AC) * 4;
	return -1;
}

static uint16_t current_hcount()
{
	int64_t v = cycle_in_line();
	if (v < VDP_ACTIVE_CYCLES) return (uint16_t)std::min<int64_t>(v / 4, 0x101);
	return (uint16_t)std::min<int64_t>(0x1AC + (v - VDP_ACTIVE_CYCLES) / 4, 0x1FF);
}

// Palette and OAM writes while a line is being drawn put dots on the picture
// of a real console; the emulator draws them clean. With raster warnings on,
// such writes are counted and reported once a frame.
static bool raster_warn = false;
struct RasterReport
{
	int palette = 0;
	int oam = 0;
	int first_line = -1;
	uint32_t first_pc = 0;
	const char* first_kind = "";
};
static RasterReport raster;

static inline void note_raster_write(bool is_palette)
{
	if (!raster_warn) return;
	if (vdp.vcount >= vdp.visible_scanlines || cycle_in_line() >= VDP_ACTIVE_CYCLES) return;
	if (raster.first_line < 0)
	{
		raster.first_line = vdp.vcount;
		raster.first_pc = SH2::OCPM::DMAC::is_dma_access() ? 0 : SH2::sh2.executing_addr;
		raster.first_kind = is_palette ? "palette" : "OAM";
	}
	(is_palette ? raster.palette : raster.oam)++;
}

static void report_raster_writes()
{
	if (raster.first_line >= 0)
	{
		Log::warn(
			"[Raster] %d palette and %d OAM writes during active display; first: %s on line %d by %s %08X",
			raster.palette, raster.oam, raster.first_kind, raster.first_line,
			raster.first_pc ? "pc" : "DMA", raster.first_pc
		);
	}
	raster = {};
}

void set_raster_warnings(bool enabled)
{
	raster_warn = enabled;
	raster = {};
}

int get_vcount()
{
	return vdp.vcount;
}

//Everything in the VDP after its output buffers (VRAM, OAM, palette,
//registers, counters), the last finished picture, and the pending events.
//The other buffers are rebuilt during every frame.
static size_t vdp_plain_offset()
{
	return (const char*)&vdp.frame_ended - (const char*)&vdp;
}

void save_state(State::Writer& w)
{
	w.section("vdp");
	size_t offset = vdp_plain_offset();
	w.put<uint32_t>((uint32_t)(sizeof(VDP) - offset));
	w.bytes((const char*)&vdp + offset, sizeof(VDP) - offset);
	w.bytes(vdp.display_output.get(), sizeof(uint16_t) * DISPLAY_WIDTH * DISPLAY_HEIGHT);
	w.put(vcount_ev);
	w.put(hsync_ev);
	w.put(irq0_ev);
}

void load_state(State::Reader& r)
{
	r.section("vdp");
	size_t offset = vdp_plain_offset();
	if (r.get<uint32_t>() != sizeof(VDP) - offset)
	{
		throw std::runtime_error("savestate does not match this build (VDP layout)");
	}
	r.bytes((char*)&vdp + offset, sizeof(VDP) - offset);
	r.bytes(vdp.display_output.get(), sizeof(uint16_t) * DISPLAY_WIDTH * DISPLAY_HEIGHT);
	r.get(vcount_ev);
	r.get(hsync_ev);
	r.get(irq0_ev);
	//The controller scan mode lives in LoopyIO too; keep them in step
	LoopyIO::set_controller_scan_mode(vdp.mode.pad_scan, vdp.mode.mouse_scan);
}

double get_frame_rate()
{
	return (double)VDP_CLOCK / (VDP_CYCLES_PER_LINE * LINES_PER_FRAME);
}

struct DumpHeader
{
	uint32_t addr;
	uint32_t length;
	uint32_t data_width;
};

// Internal simple BMP writer for video debugging, not for real screenshots
static void dump_bmp(std::string name, std::unique_ptr<uint16_t[]>& data)
{
	std::ofstream bmp_file(name.c_str(), std::ios::binary);

	const char* SIGNATURE = "BM";
	bmp_file.write(SIGNATURE, 2);

	constexpr static int DATA_SIZE = (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2);
	uint32_t file_size = DATA_SIZE + 0x36;
	bmp_file.write((char*)&file_size, 4);

	uint32_t reserved = 0;
	bmp_file.write((char*)&reserved, 4);

	uint32_t data_offs = 0x36;
	bmp_file.write((char*)&data_offs, 4);

	uint32_t info_size = 0x28;
	bmp_file.write((char*)&info_size, 4);

	bmp_file.write((char*)&DISPLAY_WIDTH, 4);
	bmp_file.write((char*)&DISPLAY_HEIGHT, 4);

	uint16_t planes = 1;
	bmp_file.write((char*)&planes, 2);

	uint16_t bpp = 16;
	bmp_file.write((char*)&bpp, 2);

	uint32_t compression = 0;
	bmp_file.write((char*)&compression, 4);
	bmp_file.write((char*)&compression, 4);
	bmp_file.write((char*)&compression, 4);
	bmp_file.write((char*)&compression, 4);
	bmp_file.write((char*)&compression, 4);
	bmp_file.write((char*)&compression, 4);

	for (int y = 0; y < DISPLAY_HEIGHT; y++)
	{
		int flipped_y = DISPLAY_HEIGHT - y - 1;
		bmp_file.write((char*)(data.get() + flipped_y * DISPLAY_WIDTH), DISPLAY_WIDTH * 2);
	}
}

void dump_all_bmps()
{
	for (int i = 0; i < 4; i++)
	{
		std::string bitmap_name = "output_bitmap" + std::to_string(i) + ".bmp";
		dump_bmp(bitmap_name, vdp.bitmap_output[i]);
	}

	for (int i = 0; i < 2; i++)
	{
		std::string bg_name = "output_bg" + std::to_string(i) + ".bmp";
		dump_bmp(bg_name, vdp.bg_output[i]);

		std::string screen_name = "output_screen_" + std::string((i == 1) ? "B" : "A") + ".bmp";
		dump_bmp(screen_name, vdp.screen_output[i]);

		std::string obj_name = "output_obj" + std::to_string(i) + ".bmp";
		dump_bmp(obj_name, vdp.obj_output[i]);
	}

	std::string display_name = "output_display.bmp";
	dump_bmp(display_name, vdp.display_output);
}

static void pulse_irq(SH2::OCPM::INTC::IRQ irq_id)
{
	SH2::OCPM::INTC::assert_irq(irq_id, 0);
	SH2::OCPM::INTC::deassert_irq(irq_id);
}

//IRQ0: the position compare interrupt, when HCOUNT reaches HCMP (and VCOUNT
//equals VCMP, if that is enabled too)
static void irq0_event(uint64_t param, int cycles_late)
{
	if (vdp.cmp_irq_ctrl.irq0_enable && vdp.cmp_irq_ctrl.irq0_enable2)
	{
		if (!vdp.cmp_irq_ctrl.use_vcmp || vdp.vcount == vdp.irq0_vcmp)
		{
			pulse_irq(SH2::OCPM::INTC::IRQ::IRQ0);
		}
	}
}

//IRQ2: a scanline capture is ready (source 0) or an ADC conversion is done
//(source 1), when INTERRUPT_CTRL enables it (bits 0, 3 and 6) for that source
//(bit 4)
static void irq2_source_done(int source)
{
	uint16_t ctrl = vdp.irq_ctrl_raw;
	if ((ctrl & 0x49) == 0x49 && ((ctrl >> 4) & 1) == source)
	{
		pulse_irq(SH2::OCPM::INTC::IRQ::IRQ2);
	}
}

//The raster DMA signal (on the CPU's PA13/IRQ1/DREQ0 pin) falls when the
//active picture ends: once a frame, or after every visible line
static void raster_signal()
{
	pulse_irq(SH2::OCPM::INTC::IRQ::IRQ1);
}

static void vsync_start()
{
	Log::debug("[Video] VSYNC start");

	//This is kinda weird, but when the VDP enters VSYNC, the total number of scanlines is subtracted from VCOUNT
	//Think of the VSYNC lines as being negative
	vdp.vcount = (vdp.vcount - LINES_PER_FRAME) & 0x1FF;
	vdp.frame_ended = true;

	//NMI is triggered on VSYNC
	if (vdp.cmp_irq_ctrl.nmi_enable)
	{
		pulse_irq(SH2::OCPM::INTC::IRQ::NMI);
	}

	//The raster DMA signal, frame mode
	if (vdp.sync_irq_ctrl.irq1_enable && (vdp.sync_irq_ctrl.irq1_source == 0))
	{
		raster_signal();
	}
}

//The end of a line's active pixels (HCOUNT jumps from 0x101 to 0x1AC): the
//line is drawn, VCOUNT counts up, and the blanking interrupts happen. Register
//writes made in the blanking that follows land on the next line.
static void active_end(uint64_t param, int cycles_late)
{
	bool visible = vdp.vcount < vdp.visible_scanlines;
	if (visible)
	{
		bool capture_pending = vdp.capture_enable;
		Renderer::draw_scanline(vdp.vcount);
		if (capture_pending && !vdp.capture_enable)
		{
			irq2_source_done(0);
		}
	}

	vdp.vcount++;

	//The raster DMA signal, line mode: after every visible line
	if (visible && vdp.sync_irq_ctrl.irq1_enable && vdp.sync_irq_ctrl.irq1_source == 1)
	{
		raster_signal();
	}

	//Once we go past the visible region, enter VSYNC
	if (vdp.vcount == vdp.visible_scanlines)
	{
		vsync_start();
	}

	//At the end of VSYNC, wrap around to the start of the visible region
	constexpr static int VSYNC_END = 0x200;

	if (vdp.vcount == VSYNC_END)
	{
		Log::debug("[Video] VSYNC end");
		vdp.vcount = 0;

		//Draw the background color outside the active area
		if (!vdp.mode.extra_scanlines)
		{
			//SDL will be sampling only the active area rect, but during blending, AA will sample from the very next row,
			//so fill it with the background instead of 0x0.
			//The top, left, and right are not subject to this because the source rect touches the edges of the texture so it's not sampled beyond that
			Renderer::draw_border_scanline(0xE0);
		}
	}

}

//The start of a line: schedule its events on the VDP clock
static void line_begin(uint64_t param, int cycles_late)
{
	if (param)
	{
		//Called by the event: this line starts where the last one ended
		vdp.line_start += VDP_CYCLES_PER_LINE;
		if (vdp.line_start >= VDP_REBASE)
		{
			//Keep the numbers small; VDP_REBASE VDP cycles are exactly
			//CPU_REBASE CPU cycles
			vdp.line_start -= VDP_REBASE;
			vdp.base_cpu += CPU_REBASE;
		}
	}

	vcount_ev = schedule_vdp(vcount_func, vdp.line_start + VDP_ACTIVE_CYCLES);
	hsync_ev = schedule_vdp(hsync_func, vdp.line_start + VDP_CYCLES_PER_LINE, 1);

	if (vdp.cmp_irq_ctrl.irq0_enable && vdp.cmp_irq_ctrl.irq0_enable2)
	{
		int offset = hcount_to_cycle(vdp.irq0_hcmp);
		if (offset >= 0)
		{
			irq0_ev = schedule_vdp(irq0_func, vdp.line_start + offset);
		}
	}
}

static void dump_serial_region(std::ofstream& dump, uint8_t* mem, uint32_t addr, uint32_t length)
{
	DumpHeader header;
	header.addr = Common::bswp32(addr | (1 << 27));	 //Make sure the address is 16-bit for the CPU
	header.length = Common::bswp32(length);
	header.data_width = Common::bswp32(2);

	dump.write((char*)&header, sizeof(header));
	dump.write((char*)mem, length);
}

void initialize()
{
	vdp = {};

	vdp.visible_scanlines = 0xE0;

	//Initialize output buffers
	for (int i = 0; i < 2; i++)
	{
		vdp.bg_output[i] = std::make_unique<uint16_t[]>(DISPLAY_WIDTH * DISPLAY_HEIGHT);
		vdp.obj_output[i] = std::make_unique<uint16_t[]>(DISPLAY_WIDTH * DISPLAY_HEIGHT);
		vdp.screen_output[i] = std::make_unique<uint16_t[]>(DISPLAY_WIDTH * DISPLAY_HEIGHT);
	}

	//Set all OBJs to invisible
	for (int i = 0; i < OAM_SIZE; i += 4)
	{
		oam_write32(i, 0x200);
	}
	raster = {};

	for (int i = 0; i < 4; i++)
	{
		vdp.bitmap_output[i] = std::make_unique<uint16_t[]>(DISPLAY_WIDTH * DISPLAY_HEIGHT);
	}

	vdp.display_output = std::make_unique<uint16_t[]>(DISPLAY_WIDTH * DISPLAY_HEIGHT);

	//Map VRAM to the CPU
	//Bitmap VRAM is mirrored
	Memory::map_sh2_pagetable(vdp.bitmap, BITMAP_VRAM_START, BITMAP_VRAM_SIZE);
	Memory::map_sh2_pagetable(vdp.bitmap, BITMAP_VRAM_START + BITMAP_VRAM_SIZE, BITMAP_VRAM_SIZE);
	Memory::map_sh2_pagetable(vdp.tile, TILE_VRAM_START, TILE_VRAM_SIZE);

	vcount_func = Timing::register_func("Video::active_end", active_end);
	hsync_func = Timing::register_func("Video::line_begin", line_begin);
	irq0_func = Timing::register_func("Video::irq0", irq0_event);

	//Start the raster at the beginning of line 0
	vdp.base_cpu = Timing::get_timestamp(Timing::CPU_TIMER);
	vdp.line_start = 0;
	line_begin(0, 0);
}

void shutdown()
{
	// nop
}

void start_frame()
{
	vdp.frame_ended = false;
	if (raster_warn) report_raster_writes();

	constexpr static int BUFFER_SIZE = DISPLAY_WIDTH * DISPLAY_HEIGHT;

	//Clear the output buffers
	for (int i = 0; i < 2; i++)
	{
		memset(vdp.bg_output[i].get(), 0, BUFFER_SIZE);
		memset(vdp.obj_output[i].get(), 0, BUFFER_SIZE);
		memset(vdp.bitmap_output[i].get(), 0, BUFFER_SIZE);
		memset(vdp.bitmap_output[i + 2].get(), 0, BUFFER_SIZE);
		memset(vdp.screen_output[i].get(), 0, BUFFER_SIZE);
	}

	memset(vdp.display_output.get(), 0, BUFFER_SIZE);
}

bool check_frame_end()
{
	return vdp.frame_ended;
}

uint16_t get_background_color()
{
	return vdp.backdrops[0];
}

int get_display_scanlines()
{
	return vdp.visible_scanlines;
}

uint16_t* get_display_output()
{
	return vdp.display_output.get();
}

//Private internal use only! Screenshots should use get_display_output()
void dump_current_frame(std::string bmp_path)
{
	// Do we need to wait for vsync? This happens on input processing loop
	dump_bmp(bmp_path, vdp.display_output);
}

void dump_for_serial()
{
	std::ofstream dump("emudump.bin", std::ios::binary);
	const char* MAGIC = "LPSTATE\0";

	dump.write(MAGIC, 8);

	dump_serial_region(dump, vdp.bitmap, BITMAP_VRAM_START, BITMAP_VRAM_SIZE);
	dump_serial_region(dump, vdp.tile, TILE_VRAM_START, TILE_VRAM_SIZE);
	dump_serial_region(dump, vdp.palette, PALETTE_START, PALETTE_SIZE);
	dump_serial_region(dump, vdp.oam, OAM_START, OAM_SIZE);

	//TODO: dump MMIO
}

uint8_t palette_read8(uint32_t addr)
{
	return vdp.palette[addr & 0x1FF];
}

uint16_t palette_read16(uint32_t addr)
{
	uint16_t value;
	memcpy(&value, &vdp.palette[addr & 0x1FE], 2);
	return Common::bswp16(value);
}

uint32_t palette_read32(uint32_t addr)
{
	uint32_t value;
	memcpy(&value, &vdp.palette[addr & 0x1FE], 4);
	return Common::bswp32(value);
}

void palette_write8(uint32_t addr, uint8_t value)
{
	note_raster_write(true);
	vdp.palette[addr & 0x1FF] = value;
}

void palette_write16(uint32_t addr, uint16_t value)
{
	note_raster_write(true);
	value = Common::bswp16(value);
	memcpy(&vdp.palette[addr & 0x1FE], &value, 2);
}

void palette_write32(uint32_t addr, uint32_t value)
{
	note_raster_write(true);
	value = Common::bswp32(value);
	memcpy(&vdp.palette[addr & 0x1FE], &value, 4);
}

uint8_t oam_read8(uint32_t addr)
{
	return vdp.oam[addr & 0x1FF];
}

uint16_t oam_read16(uint32_t addr)
{
	uint16_t value;
	memcpy(&value, &vdp.oam[addr & 0x1FE], 2);
	return Common::bswp16(value);
}

uint32_t oam_read32(uint32_t addr)
{
	uint32_t value;
	memcpy(&value, &vdp.oam[addr & 0x1FE], 4);
	return Common::bswp32(value);
}

void oam_write8(uint32_t addr, uint8_t value)
{
	note_raster_write(false);
	vdp.oam[addr & 0x1FF] = value;
}

void oam_write16(uint32_t addr, uint16_t value)
{
	note_raster_write(false);
	value = Common::bswp16(value);
	memcpy(&vdp.oam[addr & 0x1FE], &value, 2);
}

void oam_write32(uint32_t addr, uint32_t value)
{
	note_raster_write(false);
	value = Common::bswp32(value);
	memcpy(&vdp.oam[addr & 0x1FE], &value, 4);
}

uint8_t capture_read8(uint32_t addr)
{
	return vdp.capture_buffer[addr & 0x1FF];
}

uint16_t capture_read16(uint32_t addr)
{
	uint16_t value;
	memcpy(&value, &vdp.capture_buffer[addr & 0x1FE], 2);
	return Common::bswp16(value);
}

uint32_t capture_read32(uint32_t addr)
{
	uint32_t value;
	memcpy(&value, &vdp.capture_buffer[addr & 0x1FE], 4);
	return Common::bswp32(value);
}

void capture_write8(uint32_t addr, uint8_t value)
{
	Log::unimplemented("[Video] write8 to the capture buffer %08X: %02X", addr, value);
}

void capture_write16(uint32_t addr, uint16_t value)
{
	Log::unimplemented("[Video] write16 to the capture buffer %08X: %04X", addr, value);
}

void capture_write32(uint32_t addr, uint32_t value)
{
	Log::unimplemented("[Video] write32 to the capture buffer %08X: %08X", addr, value);
}

uint8_t bitmap_reg_read8(uint32_t addr)
{
	READ_HALFWORD(bitmap_reg, addr);
}

uint16_t bitmap_reg_read16(uint32_t addr)
{
	addr &= 0xFFE;

	int index = (addr >> 1) & 0x3;
	auto layer = &vdp.bitmap_regs[index];
	int reg = addr & ~0x7;

	switch (reg)
	{
	case 0x000:
		return layer->scrollx;
	case 0x008:
		return layer->scrolly;
	case 0x010:
		return layer->screenx;
	case 0x018:
		return layer->screeny;
	case 0x020:
		return layer->w | (layer->clipx << 8);
	case 0x028:
		return layer->h;
	case 0x030:
		return vdp.bitmap_ctrl;
	case 0x040:
		return vdp.bitmap_palsel;
	case 0x050:
		return layer->buffer_ctrl;
	default:
		Log::unimplemented("[Video] read16 bitmap register %03X", addr & 0xFFF);
		return 0;
	}
}

uint32_t bitmap_reg_read32(uint32_t addr)
{
	READ_DOUBLEWORD(bitmap_reg, addr);
}

void bitmap_reg_write8(uint32_t addr, uint8_t value)
{
	WRITE_HALFWORD(bitmap_reg, addr, value);
}

void bitmap_reg_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFE;

	int index = (addr >> 1) & 0x3;
	auto layer = &vdp.bitmap_regs[index];
	int reg = addr & ~0x7;

	switch (reg)
	{
	case 0x000:
		Log::debug("[Video] write BM%d_SCROLLX: %04X", index, value);
		layer->scrollx = value & 0x1FF;
		break;
	case 0x008:
		Log::debug("[Video] write BM%d_SCROLLY: %04X", index, value);
		layer->scrolly = value & 0x1FF;
		break;
	case 0x010:
		Log::debug("[Video] write BM%d_SCREENX: %04X", index, value);
		layer->screenx = value & 0x1FF;
		break;
	case 0x018:
		Log::debug("[Video] write BM%d_SCREENY: %04X", index, value);
		layer->screeny = value & 0x1FF;
		break;
	case 0x020:
		Log::debug("[Video] write BM%d_CLIPWIDTH: %04X", index, value);
		layer->w = value & 0xFF;
		layer->clipx = value >> 8;
		break;
	case 0x028:
		Log::debug("[Video] write BM%d_HEIGHT: %04X", index, value);
		layer->h = value & 0xFF;
		break;
	case 0x030:
		Log::debug("[Video] write BM_CTRL: %04X", value);
		vdp.bitmap_ctrl = value;
		break;
	case 0x040:
		Log::debug("[Video] write BM_PALSEL: %04X", value);
		vdp.bitmap_palsel = value;
		break;
	case 0x050:
		Log::debug("[Video] write BM%d_BUFFER_CTRL: %04X", index, value);
		layer->buffer_ctrl = value;
		break;
	default:
		Log::unimplemented("[Video] write16 bitmap register %03X: %04X", addr & 0xFFF, value);
	}
}

void bitmap_reg_write32(uint32_t addr, uint32_t value)
{
	WRITE_DOUBLEWORD(bitmap_reg, addr, value);
}

uint8_t ctrl_read8(uint32_t addr)
{
	READ_HALFWORD(ctrl, addr);
}

uint16_t ctrl_read16(uint32_t addr)
{
	addr &= 0xFFE;
	switch (addr)
	{
	case 0x000:
	{
		uint16_t result = vdp.mode.use_pal;
		result |= vdp.mode.extra_scanlines << 1;
		result |= vdp.mode.unk << 2;
		result |= vdp.mode.mouse_scan << 3;
		result |= vdp.mode.pad_scan << 4;
		result |= vdp.mode.unk2 << 5;
		return result;
	}
	case 0x002:
		return current_hcount();
	case 0x004:
		return vdp.vcount;
	default:
		Log::unimplemented("[Video] read16 control register %03X", addr & 0xFFF);
		return 0;
	}
}

uint32_t ctrl_read32(uint32_t addr)
{
	READ_DOUBLEWORD(ctrl, addr);
}

void ctrl_write8(uint32_t addr, uint8_t value)
{
	WRITE_HALFWORD(ctrl, addr, value);
}

void ctrl_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFE;
	switch (addr)
	{
	case 0x000:
		Log::debug("[Video] write MODE: %04X", value);
		vdp.mode.use_pal = value & 0x1;
		vdp.mode.extra_scanlines = (value >> 1) & 0x1;
		vdp.mode.unk = (value >> 2) & 0x1;
		vdp.mode.mouse_scan = (value >> 3) & 0x1;
		vdp.mode.pad_scan = (value >> 4) & 0x1;
		vdp.mode.unk2 = (value >> 5) & 0x1;
		if (vdp.mode.use_pal) Log::unimplemented("[Video] PAL mode (MODE %04X)", value);

		vdp.visible_scanlines = (vdp.mode.extra_scanlines) ? 0xF0 : 0xE0;
		LoopyIO::set_controller_scan_mode(vdp.mode.pad_scan, vdp.mode.mouse_scan);
		break;
	case 0x006:
		if (value & 0x01)
		{
			vdp.capture_enable = true;
		}

		if (value & 0x02)
		{
			LoopyIO::update_print_temp();
			irq2_source_done(1);
		}

		if (value & 0x04)
		{
			LoopyIO::update_sensors();
		}

		//Only log writes to unimplemented bits
		if ((value & ~0x0007) != 0x01)
		{
			Log::debug("[Video] write ctrl 006: %04X", value);
		}
		break;
	case 0x008:
		Log::debug("[Video] write SYNC_IRQ_CTRL: %04X", value);
		vdp.sync_irq_ctrl.irq1_enable = value & 0x1;
		vdp.sync_irq_ctrl.irq1_source = (value >> 1) & 0x1;
		break;
	default:
		Log::unimplemented("[Video] write16 control register %03X: %04X", addr & 0xFFF, value);
	}
}

void ctrl_write32(uint32_t addr, uint32_t value)
{
	WRITE_DOUBLEWORD(ctrl, addr, value);
}

uint8_t bgobj_read8(uint32_t addr)
{
	READ_HALFWORD(bgobj, addr);
}

uint16_t bgobj_read16(uint32_t addr)
{
	addr &= 0xFFE;
	switch (addr)
	{
	case 0x000:
	{
		uint16_t result = vdp.bg_ctrl.shared_maps;
		result |= vdp.bg_ctrl.map_size << 1;
		result |= vdp.bg_ctrl.bg0_8bit << 3;
		result |= vdp.bg_ctrl.tile_size1 << 4;
		result |= vdp.bg_ctrl.tile_size0 << 6;
		return result;
	}
	case 0x002:
		return vdp.bg_scrollx[0];
	case 0x004:
		return vdp.bg_scrolly[0];
	case 0x006:
		return vdp.bg_scrollx[1];
	case 0x008:
		return vdp.bg_scrolly[1];
	case 0x00A:
		return vdp.bg_palsel[0];
	case 0x00C:
		return vdp.bg_palsel[1];
	case 0x010:
	{
		uint16_t result = vdp.obj_ctrl.id_offs;
		result |= vdp.obj_ctrl.tile_index_offs[1] << 8;
		result |= vdp.obj_ctrl.tile_index_offs[0] << 11;
		result |= vdp.obj_ctrl.is_8bit << 14;
		return result;
	}
	case 0x012:
		return vdp.obj_palsel[0];
	case 0x014:
		return vdp.obj_palsel[1];
	case 0x020:
		return vdp.tilebase;
	default:
		Log::unimplemented("[Video] read16 BG/OBJ register %03X", addr & 0xFFF);
		return 0;
	}
}

uint32_t bgobj_read32(uint32_t addr)
{
	READ_DOUBLEWORD(bgobj, addr);
}

void bgobj_write8(uint32_t addr, uint8_t value)
{
	WRITE_HALFWORD(bgobj, addr, value);
}

void bgobj_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFE;
	switch (addr)
	{
	case 0x000:
		Log::debug("[Video] write BG_CTRL: %04X", value);
		vdp.bg_ctrl.shared_maps = value & 0x1;
		vdp.bg_ctrl.map_size = (value >> 1) & 0x3;
		vdp.bg_ctrl.bg0_8bit = (value >> 3) & 0x1;

		//Note the reversed order!
		vdp.bg_ctrl.tile_size1 = (value >> 4) & 0x3;
		vdp.bg_ctrl.tile_size0 = (value >> 6) & 0x3;
		break;
	case 0x002:
	case 0x006:
	{
		int index = (addr - 0x002) >> 2;
		Log::debug("[Video] write BG%d_SCROLLX: %04X", index, value);
		vdp.bg_scrollx[index] = value & 0xFFF;
		break;
	}
	case 0x004:
	case 0x008:
	{
		int index = (addr - 0x004) >> 2;
		Log::debug("[Video] write BG%d_SCROLLY: %04X", index, value);
		vdp.bg_scrolly[index] = value & 0xFFF;
		break;
	}
	case 0x00A:
	case 0x00C:
	{
		int index = (addr - 0x00A) >> 1;
		Log::debug("[Video] write BG%d_PALSEL: %04X", index, value);
		vdp.bg_palsel[index] = value;
		break;
	}
	case 0x010:
		Log::debug("[Video] write OBJ_CTRL: %04X", value);
		vdp.obj_ctrl.id_offs = value & 0xFF;

		//Note the reversed order!
		vdp.obj_ctrl.tile_index_offs[1] = (value >> 8) & 0x7;
		vdp.obj_ctrl.tile_index_offs[0] = (value >> 11) & 0x7;
		vdp.obj_ctrl.is_8bit = (value >> 14) & 0x1;
		break;
	case 0x012:
	case 0x014:
	{
		int index = (addr - 0x012) >> 1;
		Log::debug("[Video] write OBJ%d_PALSEL: %04X", index, value);
		vdp.obj_palsel[index] = value;
		break;
	}
	case 0x020:
		Log::debug("[Video] write TILEBASE: %04X", value);
		vdp.tilebase = value & 0xFF;
		break;
	default:
		Log::unimplemented("[Video] write16 BG/OBJ register %03X: %04X", addr & 0xFFF, value);
	}
}

void bgobj_write32(uint32_t addr, uint32_t value)
{
	WRITE_DOUBLEWORD(bgobj, addr, value);
}

uint8_t display_read8(uint32_t addr)
{
	READ_HALFWORD(display, addr);
}

uint16_t display_read16(uint32_t addr)
{
	addr &= 0xFFE;
	switch (addr)
	{
	case 0x000:
		return vdp.dispmode;
	case 0x002:
	{
		uint16_t result = 0;

		for (int i = 0; i < 2; i++)
		{
			result |= vdp.layer_ctrl.bg_enable[i] << i;
			result |= vdp.layer_ctrl.obj_enable[i] << (i + 6);
		}

		for (int i = 0; i < 4; i++)
		{
			result |= vdp.layer_ctrl.bitmap_enable[i] << (i + 2);
		}

		result |= vdp.layer_ctrl.bitmap_screen_mode[0] << 8;
		result |= vdp.layer_ctrl.bitmap_screen_mode[1] << 10;
		result |= vdp.layer_ctrl.obj_screen_mode[0] << 12;
		result |= vdp.layer_ctrl.obj_screen_mode[1] << 14;
		return result;
	}
	case 0x004:
	{
		uint16_t result = vdp.color_prio.prio_mode;
		result |= vdp.color_prio.screen_b_backdrop_only << 4;
		result |= vdp.color_prio.output_screen_b << 5;
		result |= vdp.color_prio.output_screen_a << 6;
		result |= vdp.color_prio.blend_mode << 7;
		return result;
	}
	case 0x006:
		//Note the reversed order!
		return vdp.backdrops[1];
	case 0x008:
		return vdp.backdrops[0];
	case 0x00A:
		return vdp.capture_ctrl_raw;
	default:
		Log::unimplemented("[Video] read16 display register %03X", addr & 0xFFF);
		return 0;
	}
}

//Sync calibration and the raster debug controls at 0x0C060000-0x0C060005: the
//BIOS writes 2 to SYNC_CALIBRATE at boot (it moves the picture against sync,
//which an emulator can ignore); games do not use the debug controls.
uint8_t sysctl_read8(uint32_t addr)
{
	READ_HALFWORD(sysctl, addr);
}

uint16_t sysctl_read16(uint32_t addr)
{
	addr &= 0xE;
	if (addr == 0x2) return vdp.video_debug[0];
	if (addr == 0x4) return vdp.video_debug[1];
	return 0;
}

uint32_t sysctl_read32(uint32_t addr)
{
	READ_DOUBLEWORD(sysctl, addr);
}

void sysctl_write8(uint32_t addr, uint8_t value)
{
	WRITE_HALFWORD(sysctl, addr, value);
}

void sysctl_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xE;
	if (addr == 0x0) vdp.sync_calibrate = value;
	else if (addr == 0x2) vdp.video_debug[0] = value;
	else if (addr == 0x4) vdp.video_debug[1] = value;
	if (addr != 0x0 && value) Log::unimplemented("[Video] raster debug register %X: %04X", addr, value);
}

void sysctl_write32(uint32_t addr, uint32_t value)
{
	WRITE_DOUBLEWORD(sysctl, addr, value);
}

uint32_t display_read32(uint32_t addr)
{
	READ_DOUBLEWORD(display, addr);
}

void display_write8(uint32_t addr, uint8_t value)
{
	WRITE_HALFWORD(display, addr, value);
}

void display_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFE;
	switch (addr)
	{
	case 0x000:
		vdp.dispmode = value & 0x7;
		Log::debug("[Video] write DISPMODE: %04X", value);
		break;
	case 0x002:
		for (int i = 0; i < 2; i++)
		{
			vdp.layer_ctrl.bg_enable[i] = (value >> i) & 0x1;
			vdp.layer_ctrl.obj_enable[i] = (value >> (i + 6)) & 0x1;
		}

		for (int i = 0; i < 4; i++)
		{
			vdp.layer_ctrl.bitmap_enable[i] = (value >> (i + 2)) & 0x1;
		}

		vdp.layer_ctrl.bitmap_screen_mode[0] = (value >> 8) & 0x3;
		vdp.layer_ctrl.bitmap_screen_mode[1] = (value >> 10) & 0x3;
		vdp.layer_ctrl.obj_screen_mode[0] = (value >> 12) & 0x3;
		vdp.layer_ctrl.obj_screen_mode[1] = value >> 14;
		Log::debug("[Video] write LAYER_CTRL: %04X", value);
		break;
	case 0x004:
		vdp.color_prio.prio_mode = value & 0xF;
		vdp.color_prio.screen_b_backdrop_only = (value >> 4) & 0x1;
		vdp.color_prio.output_screen_b = (value >> 5) & 0x1;
		vdp.color_prio.output_screen_a = (value >> 6) & 0x1;
		vdp.color_prio.blend_mode = (value >> 7) & 0x1;
		Log::debug("[Video] write COLORPRIO: %04X", value);
		break;
	case 0x006:
		//Note the reversed order!
		vdp.backdrops[1] = value;
		break;
	case 0x008:
		vdp.backdrops[0] = value;
		break;
	case 0x00A:
		vdp.capture_ctrl_raw = value;
		vdp.capture_ctrl.scanline = value & 0xFF;
		vdp.capture_ctrl.format = (value >> 8) & 0x3;
		break;
	default:
		Log::unimplemented("[Video] write16 display register %03X: %04X", addr & 0xFFF, value);
	}
}

void display_write32(uint32_t addr, uint32_t value)
{
	WRITE_DOUBLEWORD(display, addr, value);
}

uint8_t irq_read8(uint32_t addr)
{
	READ_HALFWORD(irq, addr);
}

uint16_t irq_read16(uint32_t addr)
{
	addr &= 0xFFE;

	switch (addr)
	{
	case 0x000:
		return vdp.irq_ctrl_raw;
	case 0x002:
		return vdp.irq0_hcmp;
	case 0x004:
		return vdp.irq0_vcmp;
	default:
		Log::unimplemented("[Video] read16 IRQ register %03X", addr & 0xFFF);
		return 0;
	}
}

uint32_t irq_read32(uint32_t addr)
{
	READ_DOUBLEWORD(irq, addr);
}

void irq_write8(uint32_t addr, uint8_t value)
{
	WRITE_HALFWORD(irq, addr, value);
}

void irq_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFE;

	switch (addr)
	{
	case 0x000:
		//TODO: emulate IRQ0, a screen position compare interrupt (no game uses it, but homebrew might)
		vdp.irq_ctrl_raw = value;
		vdp.cmp_irq_ctrl.irq0_enable = (value >> 1) & 0x1;
		vdp.cmp_irq_ctrl.nmi_enable = (value >> 2) & 0x1;
		vdp.cmp_irq_ctrl.use_vcmp = (value >> 5) & 0x1;
		vdp.cmp_irq_ctrl.irq0_enable2 = (value >> 7) & 0x1;
		Log::debug("[VDP] write CMP_IRQ_CTRL: %04X", value);
		break;
	case 0x002:
		vdp.irq0_hcmp = value & 0x1FF;
		break;
	case 0x004:
		vdp.irq0_vcmp = value & 0x1FF;
		break;
	default:
		Log::unimplemented("[Video] write16 IRQ register %03X: %04X", addr & 0xFFF, value);
	}
}

void irq_write32(uint32_t addr, uint32_t value)
{
	WRITE_DOUBLEWORD(irq, addr, value);
}

uint8_t dma_ctrl_read8(uint32_t addr)
{
	READ_HALFWORD(dma_ctrl, addr);
}

uint16_t dma_ctrl_read16(uint32_t addr)
{
	addr &= 0xFFE;
	switch (addr)
	{
	case 0x000:
		return vdp.bm_mem_ctrl;
	case 0x002:
		return vdp.dma_mask;
	case 0x004:
		return vdp.dma_value;
	default:
		Log::unimplemented("[Video] read16 DMA control register %03X", addr & 0xFFF);
		return 0;
	}
}

uint32_t dma_ctrl_read32(uint32_t addr)
{
	READ_DOUBLEWORD(dma_ctrl, addr);
}

void dma_ctrl_write8(uint32_t addr, uint8_t value)
{
	WRITE_HALFWORD(dma_ctrl, addr, value);
}

void dma_ctrl_write16(uint32_t addr, uint16_t value)
{
	addr &= 0xFFE;
	switch (addr)
	{
	case 0x000:
		//BM_MEM_CTRL: bit 0 speeds up bitmap memory access (the BIOS sets 3);
		//only the console timing model uses it
		Log::debug("[Video] write BM_MEM_CTRL: %04X", value);
		vdp.bm_mem_ctrl = value;
		break;
	case 0x002:
		//TODO: what does bit 8 do? Seems to have no effect in HW tests at this time
		vdp.dma_mask = value & 0x1FF;
		break;
	case 0x004:
		vdp.dma_value = value & 0xFF;
		break;
	default:
		Log::unimplemented("[Video] write16 DMA control register %03X: %04X", addr & 0xFFF, value);
	}
}

void dma_ctrl_write32(uint32_t addr, uint32_t value)
{
	WRITE_DOUBLEWORD(dma_ctrl, addr, value);
}

uint8_t dma_read8(uint32_t addr)
{
	Log::unimplemented("[Video] read8 from the DMA fill port %08X", addr);
	return 0;
}

uint16_t dma_read16(uint32_t addr)
{
	Log::unimplemented("[Video] read16 from the DMA fill port %08X", addr);
	return 0;
}

uint32_t dma_read32(uint32_t addr)
{
	Log::unimplemented("[Video] read32 from the DMA fill port %08X", addr);
	return 0;
}

void dma_write8(uint32_t addr, uint8_t value)
{
	WRITE_HALFWORD(dma, addr, value);
}

void dma_write16(uint32_t addr, uint16_t value)
{
	//Value written doesn't matter, it always triggers this
	//TODO: how long does this take? Is the CPU stalled?
	addr &= 0x3FE;

	int y = addr >> 1;
	for (int x = 0; x < DISPLAY_WIDTH; x++)
	{
		uint32_t addr = x + (y * DISPLAY_WIDTH);
		uint8_t data = vdp.bitmap[addr];
		data &= ~vdp.dma_mask;
		data |= vdp.dma_value & vdp.dma_mask;
		vdp.bitmap[addr] = data;
	}
}

void dma_write32(uint32_t addr, uint32_t value)
{
	WRITE_DOUBLEWORD(dma, addr, value);
}

}  // namespace Video