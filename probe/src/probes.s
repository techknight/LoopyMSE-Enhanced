! LoopyProbe: the timing loops.
! Copyright (C) 2026 Derek Quenneville
! SPDX-License-Identifier: GPL-2.0-or-later
!
! The memory probes are loopy-attorney's (platform/lp_bench_probes.s): one
! kind of instruction repeated in a loop of `iterations` (r5), with the
! addresses in r4 and r6 and a value to store in r7:
!
!     void probe(volatile void *a, uint32_t iterations, volatile void *b,
!                uint32_t value);
!
! No literal pool and only relative branches, so each can be copied
! anywhere and called there (src/memcost.c copies them to on-chip RAM and
! work RAM; they also run in place in the cartridge). `_end` marks each
! one's last byte + 1. The SH-1 has no `dt`, so a loop is add/tst/bf.

	.section .text.pb_probes,"ax"

	.macro PROBE name, count, body:vararg
	.balign 4
	.globl _pb_\name
_pb_\name:
1:
	.rept \count
	\body
	.endr
	add	#-1, r5
	tst	r5, r5
	bf	1b
	rts
	nop
	.globl _pb_\name\()_end
_pb_\name\()_end:
	.endm

	! Two addresses a DRAM row apart, alternately: every access opens a
	! row. Eight pairs, sixteen accesses, as the others.
	.macro PROBE2 name, first, second
	.balign 4
	.globl _pb_\name
_pb_\name:
1:
	.rept 8
	\first
	\second
	.endr
	add	#-1, r5
	tst	r5, r5
	bf	1b
	rts
	nop
	.globl _pb_\name\()_end
_pb_\name\()_end:
	.endm

	PROBE	nop16, 16, nop
	PROBE	nop32, 32, nop
	PROBE	rd16, 16, mov.w @r4, r1
	PROBE	wr16, 16, mov.w r7, @r4
	PROBE2	rd16x2, "mov.w @r4, r1", "mov.w @r6, r1"
	PROBE2	wr16x2, "mov.w r7, @r4", "mov.w r7, @r6"

	! Eight calls a pass to the routine at r4 (a `rts; nop`): what a fetch
	! costs where it lives -- the BIOS, which has no straight-line code to
	! borrow, against the same two instructions in on-chip RAM, work RAM
	! and the cartridge.
	.balign 4
	.globl _pb_jsr8
_pb_jsr8:
	sts.l	pr, @-r15
1:
	.rept 8
	jsr	@r4
	nop
	.endr
	add	#-1, r5
	tst	r5, r5
	bf	1b
	lds.l	@r15+, pr
	rts
	nop
	.globl _pb_jsr8_end
_pb_jsr8_end:

	.balign 4
	.globl _pb_stub
_pb_stub:
	rts
	nop
	.globl _pb_stub_end
_pb_stub_end:

	! The raster test's accesses (src/raster.c), from on-chip RAM:
	!
	!     uint32_t acc(volatile void *target, volatile uint16_t *vcount,
	!                  uint32_t value);
	!
	! Eight accesses to r4, then a VCOUNT read, until VCOUNT's bit 8 (the
	! vertical blank) is set; returns the passes. Called at the start of
	! the active picture, it hammers the whole of it.
	.macro ACCESS name, body:vararg
	.balign 4
	.globl _pb_acc_\name
_pb_acc_\name:
	mov	#1, r3
	shll8	r3			! 0x100: the vertical-blank bit
	mov	#0, r0
1:
	.rept 8
	\body
	.endr
	add	#1, r0
	mov.w	@r5, r1
	tst	r3, r1
	bt	1b
	rts
	nop
	.globl _pb_acc_\name\()_end
_pb_acc_\name\()_end:
	.endm

	ACCESS	none, nop
	ACCESS	wr, mov.w r6, @r4
	ACCESS	rd, mov.w @r4, r1

	! The HCOUNT/VCOUNT trace (src/timing.c), kept in on-chip RAM:
	!
	!     void pb_hvtrace(uint16_t *buf, uint32_t n,
	!                     volatile uint16_t *hcount, volatile uint16_t *tcnt);
	!
	! n samples of VCOUNT (hcount + 2), HCOUNT and ITU1's counter, three
	! halfwords each, as fast as the reads go.
	.section .onchip,"ax"
	.balign 4
	.globl _pb_hvtrace
_pb_hvtrace:
1:
	mov.w	@(2,r6), r0		! VCOUNT
	mov.w	@r6, r1			! HCOUNT
	mov.w	@r7, r2			! TCNT1
	mov.w	r0, @r4
	mov	r1, r0
	mov.w	r0, @(2,r4)
	mov	r2, r0
	mov.w	r0, @(4,r4)
	add	#6, r4
	add	#-1, r5
	tst	r5, r5
	bf	1b
	rts
	nop

	.end
