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
