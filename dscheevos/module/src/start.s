@ DScheevos module header and the ARM entry points the hooks call.
	.syntax unified
	.arch armv5te

	.section .header, "ax"
	.global dscm_header
	.align 2
dscm_header:
	.word 0x56435344            @ DSCM_MAGIC "DSCV"
	.word 7                     @ DSCM_VERSION (the module test checks it matches)
	.word __image_size
	.word init_entry
	.word vblank_entry
	.word halt_entry
	.word module_shutdown
	.word dscm_g_status          @ 0x1C: the module's view (0x02 or 0x0C), for the ARM9 menu
	.word dscm_g_net
dscm_vblank_count:
	.word 0                     @ 0x24: bumped by the ARM7 VBlank hook
	.word reset_entry           @ 0x28
	.global dscm_list_ptr
dscm_list_ptr:
	.word 0                     @ 0x2C: published achievement list (module.c swaps it)
	.word dscm_g_notices        @ 0x30
	.word dscm_g_journal_phys   @ 0x34: physical (the ARM7 reads it); cleaned at each step
	.word flush_entry           @ 0x38: evaluate what is queued, before a journal copy
	.space 4
	.global dscm_notify_count
dscm_notify_count:
	.word 0                     @ 0x40: unlock count for the ARM7 (own cache line)
	.global dscm_net_ring
dscm_net_ring:
	.word 0                     @ 0x44: network ring for the ARM7 SDIO shim
	.global dscm_cues
dscm_cues:
	.word 0                     @ 0x48: LED cues for the ARM7 (same cache line)
	.space 20

@ dscm_irq (struct dscm_irq_t in module.c, which checks these offsets):
@ frame counting and the wake's guard bytes, read and written by the idle
@ thread only (nothing of ours runs in the game's interrupts).
	.equ	IRQ_LAST, 0
	.equ	IRQ_FRAMES, 4
	.equ	IRQ_ON, 8
	.equ	IRQ_IN_VBLANK, 9
	.equ	IRQ_IN_HALT, 10
	.equ	IRQ_ALIAS_CACHED, 11
	.equ	IRQ_IDLE_OPEN, 12
	.equ	IRQ_IDLE_START, 16
	.equ	IRQ_TIMED, 20
	.equ	IRQ_LINES_TOTAL, 24
	.equ	IRQ_LINES_LAST, 28
	.equ	IRQ_LINES_MAX, 30
	.equ	IRQ_IDLE_CUT_LINES, 32
	.equ	IRQ_IDLE_CUT, 36
	.equ	IRQ_MISSED, 40
	@ (its own section: module.ld puts it with the uncached data,
	@ module-cached.ld next to the code)
	.section .data.wake, "aw"
	.balign	32
	.global dscm_irq
dscm_irq:
	.space	64

	.section .header, "ax"

	.arm
@ The VBlank entry is no longer called: the ARM7 bumps dscm_vblank_count and
@ the idle thread reads it (module.c, vblank_poll), so DScheevos runs nothing
@ in the game's interrupts and uses none of its stacks. Kept as a stub so the
@ header layout (and old callers) stay valid.
	.global vblank_entry
	.type vblank_entry, %function
vblank_entry:
	bx	lr

	.section .text.entries, "ax"
	.arm

@ Init: called once by nds-bootstrap's ARM9 before the game starts, with
@ the session file in r0. Its stack is tiny, so run on the module stack.
	.global init_entry
	.type init_entry, %function
init_entry:
	push	{r4, lr}
	mov	r4, sp
	ldr	sp, =__stack_top
	push	{r4, r5}
	ldr	r1, =module_init
	blx	r1
	pop	{r4, r5}
	mov	sp, r4
	pop	{r4, lr}
	bx	lr

@ Halt: the game's OS_Halt jumps here. Run idle work on the module stack,
@ then wait for an interrupt as OS_Halt does. A game thread woken by an IRQ
@ preempts the idle thread (and this work) as usual. If the work is already
@ running (preempted, then OS_Halt called again), just halt.
@ With IRQs masked (CPSR.I or IME 0) nothing could preempt the work, so it
@ is skipped. If the work took a scanline or more, an IRQ may already have
@ come, so return without waiting: the caller checks and calls OS_Halt again.
	.global halt_entry
	.type halt_entry, %function
halt_entry:
	@ Nothing goes on the caller's (the game's) stack: until the switch to
	@ the module stack only r0-r3 and r12 are used, which the caller's
	@ OS_Halt call leaves free.
	mrs	r0, cpsr
	tst	r0, #0x80
	bne	2f
	mov	r0, #0x04000000
	ldr	r0, [r0, #0x208]            @ REG_IME
	tst	r0, #1
	beq	2f
	@ in_halt: a byte in the line every wake uses. Test and set in one step:
	@ a thread that preempts us between a read and a write would otherwise
	@ also take the module stack.
	ldr	r12, =dscm_irq + IRQ_IN_HALT
	mov	r0, #1
	swpb	r1, r0, [r12]
	cmp	r1, #0
	bne	2f
	@ r0 = 1 if the caller is NitroSDK's idle loop: the instruction after
	@ its call is a branch back to the call (ARM "b .-4" or Thumb "b .-4").
	@ Read through the 0x0C000000 mirror when it is in main RAM: a data
	@ cache line fill here, every wake, would evict a line of the game's.
	@ SDK5 games map the mirror cached: then the read does fill a line, and
	@ a line left there could go stale (an overlay loaded at that address
	@ through 0x02xxxxxx), so clean and drop it after the read. Clean, not
	@ just drop: nothing of the game's in that line is ever lost.
	bic	r3, lr, #1
	mov	r12, r3, lsr #24
	cmp	r12, #2
	addeq	r3, r3, #0x0A000000         @ 0x02xxxxxx -> 0x0Cxxxxxx
	mov	r0, #0
	tst	lr, #1
	bne	4f
	ldr	r1, [r3]
	ldr	r2, =0xEAFFFFFD
	b	5f
4:	ldrh	r1, [r3]
	ldr	r2, =0xE7FC
5:	cmp	r12, #2
	mcreq	p15, 0, r3, c7, c14, 1      @ clean and invalidate the mirror's line
	cmp	r1, r2
	moveq	r0, #1
	mov	r12, sp
	ldr	sp, =__stack_top
	push	{r12, lr}
	ldr	r1, =module_idle
	blx	r1
	pop	{r12, lr}
	mov	sp, r12
	ldr	r1, =dscm_irq
	mov	r2, #0
	strb	r2, [r1, #IRQ_IN_HALT]
	cmp	r0, #0
	bxne	lr
2:	mov	r0, #0
	mcr	p15, 0, r0, c7, c0, 4       @ wait for interrupt
	bx	lr

@ Reset: called from nds-bootstrap's reset path with IRQs off. The game's
@ threads are gone, so work that was preempted will never resume: clear
@ the re-entry guards. The achievement reset itself happens in idle time.
	.global reset_entry
	.type reset_entry, %function
reset_entry:
	ldr	r0, =dscm_irq
	mov	r1, #0
	strb	r1, [r0, #IRQ_IN_VBLANK]
	strb	r1, [r0, #IRQ_IN_HALT]
	ldr	r0, =module_reset
	bx	r0

@ void dscm_publish_notify(uint32_t count): store the unlock count and clean
@ its cache line to RAM so the ARM7 sees it.
	.global dscm_publish_notify
	.type dscm_publish_notify, %function
dscm_publish_notify:
	ldr	r1, =dscm_notify_count
	str	r0, [r1]
	mcr	p15, 0, r1, c7, c10, 1      @ clean D-cache line
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4      @ drain write buffer
	bx	lr

@ void dscm_publish_cues(uint32_t cues): as above, for the LED cue word.
	.global dscm_publish_cues
	.type dscm_publish_cues, %function
dscm_publish_cues:
	ldr	r1, =dscm_cues
	str	r0, [r1]
	mcr	p15, 0, r1, c7, c10, 1
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr

@ void dscm_publish_net_ring(void* ring): as above, for the ring pointer.
	.global dscm_publish_net_ring
	.type dscm_publish_net_ring, %function
dscm_publish_net_ring:
	ldr	r1, =dscm_net_ring
	str	r0, [r1]
	mcr	p15, 0, r1, c7, c10, 1
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr

@ void dscm_publish_list(void* list): store the list pointer (header 0x2C)
@ so that it is in RAM and the line is clean.
	.global dscm_publish_list
	.type dscm_publish_list, %function
dscm_publish_list:
	ldr	r1, =dscm_list_ptr
	@ Through the cache, cleaned before an IRQ can drop the line (the VBlank
	@ IRQ invalidates it to read the ARM7's count): IRQs off for three steps.
	mrs	r3, cpsr
	orr	r2, r3, #0xC0
	msr	cpsr_c, r2
	str	r0, [r1]
	mcr	p15, 0, r1, c7, c10, 1      @ clean D-cache line
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4      @ drain write buffer
	msr	cpsr_c, r3
	bx	lr

@ void dscm_dc_clean(const void* addr, uint32_t len): write cached lines to RAM.
	.global dscm_dc_clean
	.type dscm_dc_clean, %function
dscm_dc_clean:
	add	r1, r0, r1
	bic	r0, r0, #31
1:	cmp	r0, r1
	bhs	2f
	mcr	p15, 0, r0, c7, c10, 1
	add	r0, r0, #32
	b	1b
2:	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr

@ void dscm_dc_invalidate(const void* addr, uint32_t len): drop cached lines
@ so the next read comes from RAM. Only for lines the ARM9 never writes.
	.global dscm_dc_invalidate
	.type dscm_dc_invalidate, %function
dscm_dc_invalidate:
	add	r1, r0, r1
	bic	r0, r0, #31
1:	cmp	r0, r1
	bhs	2f
	mcr	p15, 0, r0, c7, c6, 1
	add	r0, r0, #32
	b	1b
2:	bx	lr

@ void dscm_dc_drain(void): wait until the write buffer has reached RAM.
	.global dscm_dc_drain
	.type dscm_dc_drain, %function
dscm_dc_drain:
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr

@ void dscm_mpu_read(uint32_t out[10]): the ARM9 protection unit as the game
@ set it: regions 0-7 (CP15 c6), then the data-cacheable (c2) and bufferable
@ (c3) bits.
	.global dscm_mpu_read
	.type dscm_mpu_read, %function
dscm_mpu_read:
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
	mrc	p15, 0, r1, c2, c0, 0
	str	r1, [r0, #32]
	mrc	p15, 0, r1, c3, c0, 0
	str	r1, [r0, #36]
	bx	lr

@ uint32_t dscm_vblank_count_read(void): the ARM7 writes the count behind
@ the data cache, so drop the cached line first. Nothing on the ARM9 writes
@ that line (header words and code), so the invalidate loses no data.
	.global dscm_vblank_count_read
	.type dscm_vblank_count_read, %function
dscm_vblank_count_read:
	ldr	r1, =dscm_vblank_count
	mcr	p15, 0, r1, c7, c6, 1
	ldr	r0, [r1]
	bx	lr

@ uint32_t dscm_take(volatile uint32_t* p): return *p and clear it in one
@ step, so an IRQ can't come between the read and the write.
	.global dscm_take
	.type dscm_take, %function
dscm_take:
	mov	r1, #0
	swp	r2, r1, [r0]
	mov	r0, r2
	bx	lr

@ uint32_t dscm_lock(volatile uint32_t* p): set *p to 1 in one step and
@ return what it was (0: the caller holds the lock now).
	.global dscm_lock
	.type dscm_lock, %function
dscm_lock:
	mov	r1, #1
	swp	r2, r1, [r0]
	mov	r0, r2
	bx	lr

@ uint32_t dscm_irq_off(void): mask IRQs (CPSR.I), return the old CPSR.
@ void dscm_irq_restore(uint32_t cpsr): put the old IRQ mask back.
	.global dscm_irq_off
	.type dscm_irq_off, %function
dscm_irq_off:
	mrs	r0, cpsr
	orr	r1, r0, #0x80
	msr	cpsr_c, r1
	bx	lr
	.global dscm_irq_restore
	.type dscm_irq_restore, %function
dscm_irq_restore:
	msr	cpsr_c, r0
	bx	lr

@ void dscm_wfi(void): halt until an IRQ is requested (works with IRQs masked).
	.global dscm_wfi
	.type dscm_wfi, %function
dscm_wfi:
	mov	r0, #0
	mcr	p15, 0, r0, c7, c0, 4
	bx	lr

@ uint32_t dscm_dtcm_base(void): the game's DTCM base from CP15.
	.global dscm_dtcm_base
	.type dscm_dtcm_base, %function
dscm_dtcm_base:
	mrc	p15, 0, r0, c9, c1, 0
	lsr	r0, r0, #12
	lsl	r0, r0, #12
	bx	lr

	.pool

@ Flush: nds-bootstrap's ARM9 calls it with the game stopped, before the ARM7
@ copies the journal (in-game menu, reset). The module stack is free unless
@ idle work is running or parked there (in_halt, test and set as halt_entry
@ does): then it does nothing, and a later safe point writes the journal.
	.pushsection .text_cold, "ax"
	.arm
	.global flush_entry
	.type flush_entry, %function
flush_entry:
	push	{r4, lr}
	ldr	r12, =dscm_irq + IRQ_IN_HALT
	mov	r0, #1
	swpb	r1, r0, [r12]
	cmp	r1, #0
	bne	9f
	mov	r4, sp
	ldr	sp, =__stack_top
	push	{r4, r5}
	ldr	r1, =module_flush
	blx	r1
	pop	{r4, r5}
	mov	sp, r4
	ldr	r1, =dscm_irq
	mov	r2, #0
	strb	r2, [r1, #IRQ_IN_HALT]
9:	pop	{r4, lr}
	bx	lr
	.popsection

