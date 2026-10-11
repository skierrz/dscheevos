@ DScheevos module header and the idle trampoline.
	.syntax unified
	.arch armv5te

	.section .header, "ax"
	.global dscm_header
	.align 2
dscm_header:
	.word 0x56435344            @ DSCM_MAGIC "DSCV"
	.word 1                     @ DSCM_VERSION
	.word __image_size
	.word module_init
	.word vblank_entry
	.word idle_entry
	.word module_shutdown
	.word dscm_g_status
	.word dscm_g_net

	.section .text.entries, "ax"
	.arm

@ VBlank: called from the ARM9 IPC sync IRQ handler, on the IRQ stack.
	.global vblank_entry
vblank_entry:
	push	{r4, lr}
	ldr	r4, =module_vblank
	blx	r4
	pop	{r4, lr}
	bx	lr

@ Idle: called from the game's OS_Halt hook on the game's (or idle thread's)
@ stack. Switch to the module stack so rcheevos never touches game stack
@ space, run, switch back.
	.global idle_entry
idle_entry:
	push	{r4, lr}
	mov	r4, sp
	ldr	sp, =__stack_top
	push	{r4}
	ldr	r0, =module_idle
	blx	r0
	pop	{r4}
	mov	sp, r4
	pop	{r4, lr}
	bx	lr

	.pool
