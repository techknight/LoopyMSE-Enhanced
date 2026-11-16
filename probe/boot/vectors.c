// Vector table, from Kasami's loopy-homebrew-template (zlib;
// LICENSE-loopy-template.txt).
//
// ALTERED from the original: the probe takes no interrupts at all (SR stays
// at level 15 from boot/startup.s), so every interrupt vector is a stub, and
// the CPU exceptions report themselves into the SRAM log's header
// (PB_Exception, src/log.c) instead of halting in silence. The exception
// stubs are from loopy-attorney.

#include <stdint.h>

/*
All vector functions MUST be declared with __attribute__((interrupt_handler)) to
return properly. Not strictly necessary for halting functions but done for best
practice. Small functions may also have __attribute__((section(".smallfunc")))
to place them in the small space between vector table and the main entry point.
*/

__attribute__((interrupt_handler))
__attribute__((section(".smallfunc")))
void doNothing(void) { }

__attribute__((noreturn))
__attribute__((interrupt_handler))
__attribute__((section(".smallfunc")))
void halt(void) { while(1); }
#define RESERVED halt

// A CPU exception -- an illegal instruction, an illegal slot, a CPU or DMA
// address error. Each stub hands the reporter the vector and the frame the
// CPU pushed (at entry [r15] is the PC and [r15 + 4] the SR).
void PB_Exception(unsigned kind, const unsigned long *frame)
	__attribute__((noreturn));
void exc_illegal(void);
void exc_slot(void);
void exc_cpu_address(void);
void exc_dma_address(void);
__asm__(
"	.text\n"
"	.align 2\n"
"	.global _exc_illegal\n"
"_exc_illegal:\n"
"	bra 1f\n"
"	mov #4, r4\n"
"	.global _exc_slot\n"
"_exc_slot:\n"
"	bra 1f\n"
"	mov #6, r4\n"
"	.global _exc_cpu_address\n"
"_exc_cpu_address:\n"
"	bra 1f\n"
"	mov #9, r4\n"
"	.global _exc_dma_address\n"
"_exc_dma_address:\n"
"	mov #10, r4\n"
"1:	mov r15, r5\n"
"	mov.l 2f, r0\n"
"	jmp @r0\n"
"	nop\n"
"	.align 2\n"
"2:	.long _PB_Exception\n"
);

// Vector table laid out according to SH7021 datasheet tables 4.2, 5.3.
__attribute__((section (".vectors"), used))
void (* const vector_table[])(void) = {
/* Reset vectors */
/* Never used, BIOS has its own at address 0 */
	// Power-on reset
	RESERVED, RESERVED,
	// Manual reset
	RESERVED, RESERVED,
/* CPU interrupts */
/* Always active, except User Break masked at level 15 */
	// General illegal instruction
	exc_illegal,
	// (Reserved for system use)
	RESERVED,
	// Illegal slot instruction
	exc_slot,
	// (Reserved for system use)
	RESERVED, RESERVED,
	// CPU address error
	exc_cpu_address,
	// DMA address error
	exc_dma_address,
	// NMI
	doNothing,
	// User Break
	doNothing,
	// (Reserved for system use)
	RESERVED, RESERVED, RESERVED, RESERVED,
	RESERVED, RESERVED, RESERVED, RESERVED,
	RESERVED, RESERVED, RESERVED, RESERVED,
	RESERVED, RESERVED, RESERVED, RESERVED,
	RESERVED, RESERVED, RESERVED,
	// Trap instruction (user vectors)
	doNothing, doNothing, doNothing, doNothing,
	doNothing, doNothing, doNothing, doNothing,
	doNothing, doNothing, doNothing, doNothing,
	doNothing, doNothing, doNothing, doNothing,
	doNothing, doNothing, doNothing, doNothing,
	doNothing, doNothing, doNothing, doNothing,
	doNothing, doNothing, doNothing, doNothing,
	doNothing, doNothing, doNothing, doNothing,
/* IRQ and on-chip module interrupts: all masked, never taken */
	// IRQ0..IRQ7
	doNothing, doNothing, doNothing, doNothing,
	doNothing, doNothing, doNothing, doNothing,
	// DMAC0..DMAC3 (DEI, Reserved)
	doNothing, RESERVED,
	doNothing, RESERVED,
	doNothing, RESERVED,
	doNothing, RESERVED,
	// ITU0..ITU4 (IMIA, IMIB, OVI, Reserved)
	doNothing, doNothing, doNothing, RESERVED,
	doNothing, doNothing, doNothing, RESERVED,
	doNothing, doNothing, doNothing, RESERVED,
	doNothing, doNothing, doNothing, RESERVED,
	doNothing, doNothing, doNothing, RESERVED,
	// SCI0..SCI1 (ERI, RxI, TxI, TEI)
	doNothing, doNothing, doNothing, doNothing,
	doNothing, doNothing, doNothing, doNothing,
	// PRT PEI
	doNothing,
	// Reserved
	RESERVED, RESERVED, RESERVED,
	// WDT
	halt,
	// REF CMI
	halt,
	// Reserved
	RESERVED, RESERVED
};
