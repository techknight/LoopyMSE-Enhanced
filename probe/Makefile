# LoopyProbe for the Casio Loopy: a hardware probe for LoopyMSE-Enhanced.
#
# Produces LoopyProbe.bin (big-endian, as LoopyMSE and the Floopy Drive take
# it) and LoopyProbe.elf. Build with build.sh (WSL), which finds the
# Wonderful toolchain.
#
# Layout follows Kasami's loopy-homebrew-template (boot/, include/,
# tools/*.ld, tools/fixrom.py; zlib, LICENSE-loopy-template.txt), by way of
# the author's loopy-computer-fun Paint.

ROMBASE  ?= 0x0E000000
SRAMBASE ?= 0x02000000
# The log needs about 13 KB; 32 KB leaves room.
SRAMSIZE ?= 32K
STACKSIZE = 8K

WONDERFUL_TOOLCHAIN ?= /opt/wonderful
TOOLBIN ?= $(WONDERFUL_TOOLCHAIN)/toolchain/gcc-sh-elf/bin/
PREFIX  ?= sh-elf-
CC  = $(TOOLBIN)$(PREFIX)gcc
OBJ = $(TOOLBIN)$(PREFIX)objcopy
SIZE = $(TOOLBIN)$(PREFIX)size

FIXROM = /usr/bin/env python3 ./tools/fixrom.py

OBJDIR = ./obj
ROM    = ./LoopyProbe.bin
ELF    = ./LoopyProbe.elf
LDSCRIPT = ./tools/loopy.ld

SRCS_S = $(wildcard boot/*.s) $(wildcard src/*.s)
SRCS_C = $(wildcard boot/*.c) $(wildcard src/*.c)

OBJS = $(patsubst %.s,$(OBJDIR)/%.o,$(SRCS_S)) \
       $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS_C))

INCLUDES = -Iinclude -Isrc

CFLAGS  = -O2 -g -gdwarf-4
# -m1: the SH-1; -mrenesas: interrupt handlers and the BIOS's calling
# convention.
CFLAGS += -m1 -mrenesas
CFLAGS += -MMD -MP
CFLAGS += -ffreestanding
CFLAGS += -falign-functions=4 -ffunction-sections -fdata-sections
CFLAGS += -fomit-frame-pointer -fno-asynchronous-unwind-tables -fno-unwind-tables
CFLAGS += -Wall -Wextra -Wno-unused-parameter
CFLAGS += $(INCLUDES)
CFLAGS += -DCASIO_LOOPY -DPB_SRAM_SIZE='($(subst K,*1024,$(SRAMSIZE))u)'

SIZEDEFS  = -Wl,--defsym=ROMBASE=$(ROMBASE)
SIZEDEFS += -Wl,--defsym=SRAMBASE=$(SRAMBASE)
SIZEDEFS += -Wl,--defsym=SRAMSIZE=$(SRAMSIZE)
SIZEDEFS += -Wl,--defsym=STACKSIZE=$(STACKSIZE)

LDFLAGS  = -nostartfiles -Wl,--gc-sections -Wl,--no-warn-rwx-segment
LDFLAGS += -Wl,--orphan-handling=error -Wl,--print-memory-usage
LDFLAGS += $(SIZEDEFS) -Wl,-T $(LDSCRIPT)
# picolibc: memcpy, memset, vsnprintf.
LIBS = -lc

.PHONY: all clean

all: $(ROM)

$(ELF): $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(ROM): $(ELF)
	$(OBJ) -O binary $< $@
	$(FIXROM) $@
	$(SIZE) $<

$(OBJDIR)/%.o: %.s Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(ELF) $(ROM)

-include $(OBJS:.o=.d)
