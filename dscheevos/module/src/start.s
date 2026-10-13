@ DScheevos module header and the ARM entry points the hooks call.
	.syntax unified
	.arch armv5te

	.section .header, "ax"
	.global dscm_header
	.align 2
dscm_header:
	.word 0x56435344            @ DSCM_MAGIC "DSCV"
	.word 6                     @ DSCM_VERSION (the module test checks it matches)
	.word __image_size
	.word init_entry
	.word vblank_entry
	.word halt_entry
	.word module_shutdown
	.word dscm_g_status
	.word dscm_g_net
dscm_vblank_count:
	.word 0                     @ 0x24: bumped by the ARM7 VBlank hook
	.word reset_entry           @ 0x28
	.global dscm_list_ptr
dscm_list_ptr:
	.word 0                     @ 0x2C: published achievement list (module.c swaps it)
	.word dscm_g_notices        @ 0x30
	.word dscm_g_journal        @ 0x34: written through the cache, cleaned at each step
	.space 8
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

@ 0x60: what the VBlank IRQ reads and writes (struct dscm_irq_t in module.c,
@ which checks these offsets): every VBlank touches the first cache line
@ only. The IRQ code follows, and the core's part of it (.text.irq) right
@ after: an IRQ that finds everything cold touches few lines.
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
	.balign	32
	.global dscm_irq
dscm_irq:
	.space	64

	.arm
@ VBlank: called from the ARM9 IPC sync IRQ handler, on the game's IRQ stack
@ (DTCM): the three words pushed here are all it uses, unless the frame
@ needs a copy (dsc_vblank_copy), which runs on the module's IRQ stack.
@ Acts once per ARM7 VBlank.
	.global vblank_entry
	.type vblank_entry, %function
vblank_entry:
	push	{r4, r5, lr}
	adr	r4, dscm_irq
	ldmia	r4, {r0-r2}                 @ last_vblank, frames, flag bytes
	tst	r2, #0x100                  @ in_vblank: the copy below is running
	bne	9f
	adr	r12, dscm_vblank_count
	@ The ARM7 writes the count behind the data cache. Don't rely on the
	@ 0x0C000000 mirror being uncached: SDK5 games map it cached write-back.
	@ Drop the line and read through the cache; the ARM9 never leaves that
	@ line dirty (dscm_publish_list cleans its one store at once).
	mcr	p15, 0, r12, c7, c6, 1
	ldr	r12, [r12]
	subs	lr, r12, r0                 @ VBlanks since the last call
	beq	9f                          @ another IPC message
	mov	r0, r12
	add	r1, r1, lr
	stmia	r4, {r0, r1}                @ last_vblank, frames
	tst	r2, #0xFF                   @ on
	beq	9f
	mov	r0, #0x04000000
	ldrh	r5, [r0, #6]                @ REG_VCOUNT: start
	subs	lr, lr, #1
	ldrne	r0, [r4, #IRQ_MISSED]
	addne	r0, r0, lr
	strne	r0, [r4, #IRQ_MISSED]
	bl	dsc_vblank_queue
	cmp	r0, #0
	bne	8f
1:	ldr	r0, [r4, #IRQ_IDLE_OPEN]
	cmp	r0, #0
	bne	7f
	@ this IRQ's own lines
2:	mov	r0, #0x04000000
	ldrh	r0, [r0, #6]
	subs	r0, r0, r5
	addmi	r0, r0, #256                @ + LINES_PER_FRAME (263)
	addmi	r0, r0, #7
	add	r12, r4, #IRQ_TIMED
	ldmia	r12, {r1-r3}                @ timed_frames, irq_lines_total, irq_lines_last | max << 16
	add	r1, r1, #1
	add	r2, r2, r0
	cmp	r0, r3, lsr #16
	bichi	r3, r3, #0xFF000000
	bichi	r3, r3, #0x00FF0000
	orrhi	r3, r3, r0, lsl #16
	mov	r3, r3, lsr #16
	orr	r3, r0, r3, lsl #16
	stmia	r12, {r1-r3}
9:	pop	{r4, r5, lr}
	bx	lr
	@ An idle pass was running: this IRQ preempted it, the game runs next.
	@ Its lines up to here count (module_idle adds them to its frame).
7:	mov	r0, #0
	str	r0, [r4, #IRQ_IDLE_OPEN]
	ldr	r1, [r4, #IRQ_IDLE_START]
	subs	r1, r5, r1
	addmi	r1, r1, #256
	addmi	r1, r1, #7
	add	r12, r4, #IRQ_IDLE_CUT_LINES
	ldmia	r12, {r2, r3}               @ idle_cut_lines, idle_cut
	add	r2, r2, r1
	add	r3, r3, #1
	stmia	r12, {r2, r3}
	b	2b
	@ A copy in the IRQ (nothing captured yet), on the module's IRQ stack:
	@ the game's is small.
8:	mov	r0, #1
	strb	r0, [r4, #IRQ_IN_VBLANK]
	mov	r1, sp
	ldr	sp, =__irq_stack_top
	push	{r1, r2}
	ldr	r0, =module_vblank_copy
	blx	r0
	pop	{r1, r2}
	mov	sp, r1
	mov	r0, #0
	strb	r0, [r4, #IRQ_IN_VBLANK]
	b	1b
	.ltorg

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
	push	{r4, r5, lr}
	mrs	r0, cpsr
	tst	r0, #0x80
	bne	2f
	mov	r0, #0x04000000
	ldr	r0, [r0, #0x208]            @ REG_IME
	tst	r0, #1
	beq	2f
	ldr	r5, =dscm_irq               @ in_halt: a byte in the line every wake uses
	ldrb	r0, [r5, #IRQ_IN_HALT]
	cmp	r0, #0
	bne	2f
	mov	r0, #1
	strb	r0, [r5, #IRQ_IN_HALT]
	@ r0 = 1 if the caller is NitroSDK's idle loop: the instruction after
	@ its call is a branch back to the call (ARM "b .-4" or Thumb "b .-4").
	@ Read through the uncached mirror when it is in main RAM: a data
	@ cache line fill here, every wake, would evict a line of the game's.
	bic	r1, lr, #1
	mov	r2, r1, lsr #24
	cmp	r2, #2
	addeq	r1, r1, #0x0A000000         @ 0x02xxxxxx -> 0x0Cxxxxxx
	mov	r0, #0
	tst	lr, #1
	bne	4f
	ldr	r1, [r1]
	ldr	r2, =0xEAFFFFFD
	b	5f
4:	ldrh	r1, [r1]
	ldr	r2, =0xE7FC
5:	cmp	r1, r2
	moveq	r0, #1
	mov	r4, sp
	ldr	sp, =__stack_top
	push	{r4, r5}
	ldr	r1, =module_idle
	blx	r1
	pop	{r4, r5}
	mov	sp, r4
	mov	r1, #0
	strb	r1, [r5, #IRQ_IN_HALT]
	cmp	r0, #0
	bne	3f
2:	mov	r0, #0
	mcr	p15, 0, r0, c7, c0, 4       @ wait for interrupt
3:	pop	{r4, r5, lr}
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

