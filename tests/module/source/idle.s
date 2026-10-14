@ void halt_as_idle(void (*halt)(void)): call the module's halt entry the way
@ NitroSDK's idle thread calls OS_Halt, so it does its work (the module only
@ works for that caller: the instruction after the call branches back to it).
@ Here that branch leads to a return instead.
	.arm
	.section .text
	.global halt_as_idle
	.type halt_as_idle, %function
halt_as_idle:
	push	{r4, lr}
	adr	lr, 1f
	bx	r0
0:	pop	{r4, pc}
1:	b	0b		@ "b .-4": the shape the module looks for

@ void mpu_regions(u32 out[8]): the protection regions (CP15 c6, 0-7).
	.global mpu_regions
	.type mpu_regions, %function
mpu_regions:
	mrc	p15, 0, r1, c6, c0, 0
	str	r1, [r0, #0]
	mrc	p15, 0, r1, c6, c1, 0
	str	r1, [r0, #4]
	mrc	p15, 0, r1, c6, c2, 0
	str	r1, [r0, #8]
	mrc	p15, 0, r1, c6, c3, 0
	str	r1, [r0, #12]
	mrc	p15, 0, r1, c6, c4, 0
	str	r1, [r0, #16]
	mrc	p15, 0, r1, c6, c5, 0
	str	r1, [r0, #20]
	mrc	p15, 0, r1, c6, c6, 0
	str	r1, [r0, #24]
	mrc	p15, 0, r1, c6, c7, 0
	str	r1, [r0, #28]
	bx	lr

@ u32 mpu_dperm(void) / void mpu_set_dperm(u32): the data access permissions
@ (CP15 c5, 4 bits a region). The test plays a game calling NitroSDK's
@ CTRDG_Enable(FALSE): its own MPU, set by itself.
	.global mpu_dperm
	.type mpu_dperm, %function
mpu_dperm:
	mrc	p15, 0, r0, c5, c0, 2
	bx	lr
	.global mpu_set_dperm
	.type mpu_set_dperm, %function
mpu_set_dperm:
	mcr	p15, 0, r0, c5, c0, 2
	bx	lr
