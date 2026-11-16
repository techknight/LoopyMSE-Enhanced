#include "core/sh2/sh2_cost.h"

#include <algorithm>
#include <cstdio>

namespace SH2::Cost
{

bool enabled = false;

// Measured costs, in cycles, from the console benchmark.
// Cartridge ROM is a variable for the what-if of fewer wait states (WCR3);
// the BIOS's setting is two, so three cycles an access.
static double ROM = 3.0;
static bool rom_forced = false;
constexpr double SRAM = 3.0, BIOS = 3.0;

void set_cart_rom_cycles(double cycles)
{
	rom_forced = cycles > 0.0;
	ROM = rom_forced ? cycles : 3.0;
}

void wcr3_written(double cycles)
{
	if (!rom_forced) ROM = cycles;
}
constexpr double DRAM_HIT = 1.0, DRAM_MISS = 3.0, DRAM_WHIT = 2.0;
constexpr double ONCHIP = 1.0, PERIPH = 3.0, SOUND = 3.0;
constexpr double VDP_REG_R = 4.0, VDP_REG_W = 3.5;
constexpr double BM_R = 5.2, BM_R_FAST = 4.3, BM_W = 3.7, BM_W_FAST = 2.75;
constexpr double TILE_R = 7.1, TILE_W = 5.5;
constexpr double PAL_W = 3.55, OAM_W = 3.55, PAL_R = 5.0, OAM_R = 5.0;
// A 16-bit DMA unit, by source and destination.
constexpr double DMA_DRAM_DRAM = 7.45, DMA_ROM_DRAM = 5.2, DMA_DRAM_BM = 4.4, DMA_DRAM_BM_FAST = 3.4;

enum Kind
{
	K_BIOS,
	K_DRAM,
	K_SRAM,
	K_SOUND,
	K_PERIPH,
	K_VDP,
	K_ROM,
	K_ONCHIP,
	K_OTHER
};

static Kind kind_of(uint32_t a)
{
	switch ((a >> 24) & 0xF)
	{
	case 0x0:
	case 0x8:
		return K_BIOS;
	case 0x1:
	case 0x9:
		return K_DRAM;
	case 0x2:
	case 0xA:
		return K_SRAM;
	case 0x4:
		return K_SOUND;
	case 0x5:
	case 0xD:
		return K_PERIPH;
	case 0xC:
		return K_VDP;
	case 0x6:
	case 0xE:
		return K_ROM;
	case 0xF:
		return K_ONCHIP;
	default:
		return K_OTHER;
	}
}

// What the cycles went on, for the profile's summary.
enum Split
{
	S_FETCH_ROM,
	S_FETCH_DRAM_HIT,
	S_FETCH_DRAM_MISS,
	S_FETCH_BIOS,
	S_FETCH_OTHER,
	S_LIT_ROM,
	S_LIT_OTHER,
	S_RD_DRAM_HIT,
	S_RD_DRAM_MISS,
	S_WR_DRAM_HIT,
	S_WR_DRAM_MISS,
	S_RD_ROM,
	S_RD_SRAM,
	S_WR_SRAM,
	S_ONCHIP,
	S_VDP_BITMAP,
	S_VDP_TILE,
	S_VDP_OAM_PAL,
	S_VDP_REG,
	S_PERIPH,
	S_SOUND,
	S_BIOS_DATA,
	S_EXEC,
	S_DMA,
	S_NUM
};
static const char* SPLIT_NAMES[S_NUM] = {
	"fetch cart ROM", "fetch work RAM (open row)", "fetch work RAM (other row)", "fetch BIOS", "fetch other",
	"literal pool, cart ROM", "literal pool, elsewhere", "read work RAM (open row)", "read work RAM (other row)",
	"write work RAM (open row)", "write work RAM (other row)", "read cart ROM", "read SRAM", "write SRAM",
	"on-chip RAM data", "VDP bitmap", "VDP tile", "VDP OAM / palette", "VDP registers", "CPU peripherals",
	"sound", "BIOS data", "execute (states, stalls, branches)", "DMA (CPU controller)",
};

static int row = -1;
static bool fast_bm = false;
static bool collect = false;
static bool cur_pcrel = false;
static double data_sum = 0.0;
static double pending = 0.0;
static int last_load = -1;
static bool split_on = false;
static double split[S_NUM];

void reset()
{
	row = -1;
	fast_bm = false;
	collect = false;
	cur_pcrel = false;
	data_sum = 0.0;
	pending = 0.0;
	last_load = -1;
	if (!rom_forced) ROM = 3.0;
}

static inline void add_split(int s, double c)
{
	if (split_on) split[s] += c;
}

static double dram(uint32_t a, bool write, int units, bool* hit_out)
{
	int r = (int)((a & 0x7FFFF) >> 10);
	bool hit = r == row;
	row = r;
	if (hit_out) *hit_out = hit;
	if (write)
	{
		return (hit ? DRAM_WHIT : DRAM_MISS) + (units - 1) * DRAM_WHIT;
	}
	return (hit ? DRAM_HIT : DRAM_MISS) + (units - 1) * DRAM_HIT;
}

double fetch(uint32_t pc, bool* onchip)
{
	*onchip = false;
	switch (kind_of(pc))
	{
	case K_ONCHIP:
		*onchip = true;
		return 0.0;
	case K_DRAM:
	{
		bool hit;
		double c = dram(pc, false, 1, &hit);
		add_split(hit ? S_FETCH_DRAM_HIT : S_FETCH_DRAM_MISS, c);
		return c;
	}
	case K_ROM:
		add_split(S_FETCH_ROM, ROM);
		return ROM;
	case K_BIOS:
		add_split(S_FETCH_BIOS, BIOS);
		return BIOS;
	default:
		add_split(S_FETCH_OTHER, 3.0);
		return 3.0;
	}
}

void begin(uint16_t op)
{
	collect = true;
	data_sum = 0.0;
	// MOV.W / MOV.L @(disp,PC),Rn: a literal-pool load
	cur_pcrel = (op >> 12) == 0x9 || (op >> 12) == 0xD;
}

double end()
{
	collect = false;
	return data_sum;
}

bool collecting()
{
	return collect;
}

void access(uint32_t a, int size, bool write, uint32_t value)
{
	int units = size == 4 ? 2 : 1;
	double c;
	int s;
	Kind k = kind_of(a);
	switch (k)
	{
	case K_DRAM:
	{
		bool hit;
		c = dram(a, write, units, &hit);
		s = cur_pcrel ? S_LIT_OTHER : write ? (hit ? S_WR_DRAM_HIT : S_WR_DRAM_MISS) : (hit ? S_RD_DRAM_HIT : S_RD_DRAM_MISS);
		break;
	}
	case K_ONCHIP:
		c = ONCHIP;
		s = cur_pcrel ? S_LIT_OTHER : S_ONCHIP;
		break;
	case K_VDP:
	{
		uint32_t off = a & 0xFFFFF;
		if (off < 0x40000)
		{
			c = write ? (fast_bm ? BM_W_FAST : BM_W) : (fast_bm ? BM_R_FAST : BM_R);
			s = S_VDP_BITMAP;
		}
		else if (off < 0x50000)
		{
			c = write ? TILE_W : TILE_R;
			s = S_VDP_TILE;
		}
		else if (off < 0x51000)
		{
			c = write ? OAM_W : OAM_R;
			s = S_VDP_OAM_PAL;
		}
		else if (off < 0x52000)
		{
			c = write ? PAL_W : PAL_R;
			s = S_VDP_OAM_PAL;
		}
		else
		{
			c = write ? VDP_REG_W : VDP_REG_R;
			s = S_VDP_REG;
			// The bitmap's fast mode: 0x0C05E000 bit 0 (big-endian halfword).
			if (write && (off & ~1u) == 0x5E000)
			{
				if (size == 2) fast_bm = (value & 1) != 0;
				else if (size == 1 && off == 0x5E001) fast_bm = (value & 1) != 0;
				else if (size == 4) fast_bm = ((value >> 16) & 1) != 0;
			}
		}
		c *= units;
		break;
	}
	case K_ROM:
		c = ROM * units;
		s = cur_pcrel ? S_LIT_ROM : S_RD_ROM;
		break;
	case K_SRAM:
		c = SRAM * units;
		s = write ? S_WR_SRAM : S_RD_SRAM;
		break;
	case K_PERIPH:
		c = PERIPH * units;
		s = S_PERIPH;
		break;
	case K_SOUND:
		c = SOUND * units;
		s = S_SOUND;
		break;
	case K_BIOS:
		c = BIOS * units;
		s = cur_pcrel ? S_LIT_OTHER : S_BIOS_DATA;
		break;
	default:
		c = 3.0 * units;
		s = S_PERIPH;
		break;
	}
	data_sum += c;
	add_split(s, c);
}

struct Dec
{
	int issue;
	uint32_t srcs;  // registers read, a bit each
	int ldst;       // register a load wrote, or -1
};

// Issue states and register use per instruction.
static Dec decode(uint16_t op)
{
	int n = (op >> 8) & 15, m = (op >> 4) & 15, lo = op & 15, hi = op >> 12, low8 = op & 0xFF;
	uint32_t N = 1u << n, M = 1u << m, R0 = 1u;
	switch (hi)
	{
	case 0x0:
		switch (op)
		{
		case 0x000B:  // RTS
			return {2, 0, -1};
		case 0x002B:  // RTE
			return {4, 0, -1};
		case 0x001B:  // SLEEP
			return {3, 0, -1};
		default:
			break;
		}
		if (lo >= 4 && lo <= 6) return {1, R0 | N | M, -1};
		if (lo >= 0xC && lo <= 0xE) return {1, R0 | M, n};
		if (lo == 7) return {2, N | M, -1};               // MUL.L (SH-2)
		if (low8 == 0x03 || low8 == 0x23) return {2, N, -1};  // BSRF / BRAF (SH-2)
		return {1, 0, -1};
	case 0x1:
		return {1, N | M, -1};
	case 0x2:
		if (lo == 0xE || lo == 0xF) return {3, N | M, -1};  // MULU / MULS
		return {1, N | M, -1};
	case 0x3:
		return {1, N | M, -1};
	case 0x4:
		if (low8 == 0x0B || low8 == 0x2B) return {2, N, -1};  // JSR / JMP
		if (low8 == 0x1B) return {4, N, -1};                  // TAS.B
		if (low8 == 0x03 || low8 == 0x13 || low8 == 0x23) return {2, N, -1};  // STC.L
		if (low8 == 0x07 || low8 == 0x17 || low8 == 0x27) return {3, N, -1};  // LDC.L
		if (lo == 0xF) return {3, N | M, -1};                 // MAC.W
		return {1, N, -1};
	case 0x5:
		return {1, M, n};
	case 0x6:
		if (lo <= 2 || (lo >= 4 && lo <= 6)) return {1, M, n};
		return {1, M, -1};
	case 0x7:
		return {1, N, -1};
	case 0x8:
		switch (n)
		{
		case 0x0:
		case 0x1:
			return {1, R0 | M, -1};
		case 0x4:
		case 0x5:
			return {1, M, 0};
		case 0x8:
			return {1, R0, -1};
		default:
			return {1, 0, -1};  // BT / BF: price() makes a taken one 3
		}
	case 0x9:
	case 0xD:
		return {1, 0, n};
	case 0xA:
	case 0xB:
		return {2, 0, -1};
	case 0xC:
		switch (n)
		{
		case 0x0:
		case 0x1:
		case 0x2:
			return {1, R0, -1};
		case 0x3:  // TRAPA
			return {8, 0, -1};
		case 0x4:
		case 0x5:
		case 0x6:
			return {1, 0, 0};
		case 0x7:
			return {1, 0, -1};
		case 0xC:
		case 0xD:
		case 0xE:
		case 0xF:
			return {3, R0, -1};
		default:
			return {1, R0, -1};
		}
	default:
		return {1, 0, -1};
	}
}

double price(uint16_t op, double fcost, bool onchip, double data, bool taken, bool delayed)
{
	Dec d = decode(op);
	int issue = d.issue;
	if (taken && (op >> 12) == 0x8) issue = 3;  // BT / BF taken
	int stall = (last_load >= 0 && ((d.srcs >> last_load) & 1)) ? 1 : 0;
	last_load = d.ldst;
	double t;
	if (onchip)
	{
		t = std::max((double)issue, data > 0.0 ? 0.5 + data : 0.0) + stall;
	}
	else if (taken || delayed)
	{
		// A taken branch throws the prefetch away: each extra state is
		// another fetch.
		t = fcost + data + (issue - 1) * fcost;
		add_split(S_EXEC, (issue - 1) * fcost);
		return t;
	}
	else
	{
		t = std::max((double)issue, fcost + data);
		if (stall && fcost <= 1.0) t += stall;
	}
	add_split(S_EXEC, t - fcost - data);
	return t;
}

void dma(uint32_t src, uint32_t dst, int size, uint32_t units)
{
	if (!units) return;
	Kind sk = kind_of(src), dk = kind_of(dst);
	bool dst_bm = dk == K_VDP && (dst & 0xFFFFF) < 0x40000;
	double per;
	if (sk == K_DRAM && dk == K_DRAM) per = DMA_DRAM_DRAM;
	else if (sk == K_ROM && dk == K_DRAM) per = DMA_ROM_DRAM;
	else if (sk == K_DRAM && dst_bm) per = fast_bm ? DMA_DRAM_BM_FAST : DMA_DRAM_BM;
	else
	{
		// Not measured: the two accesses' own prices, the row left open.
		auto one = [](Kind k, uint32_t a, bool w) {
			switch (k)
			{
			case K_DRAM: return w ? DRAM_WHIT : DRAM_HIT;
			case K_ONCHIP: return ONCHIP;
			case K_VDP: return (a & 0xFFFFF) < 0x40000 ? (w ? BM_W : BM_R) : (w ? TILE_W : TILE_R);
			default: return 3.0;
			}
		};
		per = one(sk, src, false) + one(dk, dst, true);
	}
	(void)size;
	if (dk == K_DRAM) row = (int)((dst & 0x7FFFF) >> 10);
	else if (sk == K_DRAM) row = (int)((src & 0x7FFFF) >> 10);
	double c = per * units;
	add_split(S_DMA, c);
	if (collect) data_sum += c;
	else pending += c;
}

double take_pending()
{
	double p = pending;
	pending = 0.0;
	return p;
}

void split_enable(bool on)
{
	split_on = on;
}

void split_add_exec(double cycles)
{
	add_split(S_EXEC, cycles);
}

std::string split_text()
{
	std::string out;
	double total = 0.0;
	for (double v : split) total += v;
	char buf[160];
	for (int i = 0; i < S_NUM; i++)
	{
		if (split[i] == 0.0) continue;
		snprintf(buf, sizeof buf, "# split %-36s %14.0f %5.1f%%\n", SPLIT_NAMES[i], split[i],
			total > 0 ? 100.0 * split[i] / total : 0.0);
		out += buf;
	}
	snprintf(buf, sizeof buf, "# split %-36s %14.0f\n", "total", total);
	out += buf;
	return out;
}

}  // namespace SH2::Cost
