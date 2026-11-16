	.section .text.startup
	.globl start
start:
	! Mask every interrupt and point VBR at our own table before anything
	! else. The BIOS sets neither (it leaves SR as reset left it). The
	! probe never unmasks them: everything it times, it times by polling,
	! so no interrupt can land inside a measurement.
	mov #-16, r0		! 0xFFFFFFF0
	extu.b r0, r0		! 0x000000F0: I3-I0 = 15
	ldc r0, sr

	! Setup VBR
	mov.l vbraddr, r1
	ldc	r1, vbr

	! Set up initial stack pointer
	mov.l stackaddr, r15
	mov #0, r0

	! Initialize bss, data, and ctors
	mov.l crtinitaddr, r0
	jsr @r0
	nop

	! Execute main program
	mov.l mainaddr, r0
	jsr @r0
	nop

	! IF main program ever exits, halt
terminated:
	bra terminated
	nop

	.align 4
stackaddr:
	.long __stack_end
vbraddr:
	.long __vbr_link_start
crtinitaddr:
	.long _crt_init
mainaddr:
	.long _main

	.end
