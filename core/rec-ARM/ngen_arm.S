@@

#include "build.h"

.arm
.align 8

.equ SH4_TIMESLICE, 448
.equ BM_BLOCKLIST_MASK, 65532 @FFFC
.equ CPU_RATIO, 5

#ifdef __MACH__
#define CSYM(n) _##n
#define HIDDEN(n)
#else
#define CSYM(n) n
#define HIDDEN(n) .hidden CSYM(n)
#endif

@@@@@@@@@@ some helpers @@@@@@@@@@

.global CSYM(do_sqw_nommu_area_3)
HIDDEN(do_sqw_nommu_area_3)
@r0: addr
@r1: sq_both
CSYM(do_sqw_nommu_area_3):
add r3,r1,#0x0C000000	@ get ram ptr from r1, part 1
and r2,r0,#0x20			@ SQ# selection, isolate
ubfx r0,r0,#5,#20		@ get ram offset
add r1,r2				@ SQ# selection, add to SQ ptr
add r3,#512				@ get ram ptr from r1, part 2
add r3,r0,lsl #5		@ ram + offset
vldm r1,{d0-d3}
vstm r3,{d0-d3}
bx lr

.global CSYM(TAWriteSQ)
HIDDEN(TAWriteSQ)
@r0: addr
@r1: sq_both
CSYM(TAWriteSQ):
MOV     R2, R0					@the address as it came, for a write to video memory
BIC     R3, R0, #0xFE000000		@clear unused bits
AND     R0, R0, #0x20			@SQ#, isolate
CMP     R3, #0x800000			@TA write?
ADD     R0, R1, R0				@SQ#, add to SQ ptr
BCC     CSYM(_Z13ta_vtx_data32Pv)	@TA write?

CSYM(TAWriteSQ_yuv):
CMP     R3, #0x1000000	@Yuv write ?
BCS     CSYM(TAWriteSQ_vram)
MOV     R1, #1
B       CSYM(_Z8YUV_dataPjj)

@ To video memory, which is what Windows CE games do. There was a
@ breakpoint here, and after it a copy that took the first queue whichever
@ was meant, straight into the 64-bit view of video memory whichever was
@ asked for, at where that is when the host maps the SH4's memory: the
@ first write ended the core. The C routine the other hosts have does it.
CSYM(TAWriteSQ_vram):
MOV     R1, R0					@the queue
MOV     R0, R2					@the address
B       CSYM(TAWriteSQ_to_vram)


#if FEAT_SHREC != DYNAREC_NONE

@@@@@@@@@@ ngen_LinkBlock_*****_stub @@@@@@@@@@

.global CSYM(ngen_LinkBlock_Generic_stub)
HIDDEN(ngen_LinkBlock_Generic_stub)
CSYM(ngen_LinkBlock_Generic_stub):

    mov r1,r4	@ djump/pc -> in case we need it ..
	b CSYM(ngen_LinkBlock_Shared_stub)


.global CSYM(ngen_LinkBlock_cond_Branch_stub)
HIDDEN(ngen_LinkBlock_cond_Branch_stub)
CSYM(ngen_LinkBlock_cond_Branch_stub):

	mov r1,#1
	b CSYM(ngen_LinkBlock_Shared_stub)
    
.global CSYM(ngen_LinkBlock_cond_Next_stub)
HIDDEN(ngen_LinkBlock_cond_Next_stub)
CSYM(ngen_LinkBlock_cond_Next_stub):

	mov r1,#0
	b CSYM(ngen_LinkBlock_Shared_stub)


.global CSYM(ngen_LinkBlock_Shared_stub)
HIDDEN(ngen_LinkBlock_Shared_stub)
CSYM(ngen_LinkBlock_Shared_stub):

	mov r0,lr
    sub r0,#4                  @go before the call
    bl CSYM(rdv_LinkBlock)
	@ What comes back may be the routine that compiles what is at r4: with
	@ the MMU on, when there was an exception to raise instead of a block
	@ to find. The context has the address either way.
	ldr r4,[r8,#-184]          @ pc
    bx r0

@@@@@@@@@@ ngen_FailedToFindBlock_ @@@@@@@@@@


.global CSYM(ngen_FailedToFindBlock_)
HIDDEN(ngen_FailedToFindBlock_)
CSYM(ngen_FailedToFindBlock_):
	mov r0,r4
    bl CSYM(rdv_FailedToFindBlock)
    bx r0

@@@@@@@@@@ ngen_blockcheckfail @@@@@@@@@@

.global CSYM(ngen_blockcheckfail)
HIDDEN(ngen_blockcheckfail)
CSYM(ngen_blockcheckfail):
    bl CSYM(rdv_BlockCheckFail)
    bx r0


@@@@@@@@@@ ngen_mainloop @@@@@@@@@@

@ you can load the address of the sh4 reg struct on the mainloop init
@ using (u8*)regptr-(u8*)Sh4cntx
@ all registers are < 1024 bytes from that
@ so you can use reg+imm forms for it



.global CSYM(ngen_mainloop)
HIDDEN(ngen_mainloop)
CSYM(ngen_mainloop):

push { r4-r12,lr }


#ifdef __MACH__
	mov r11, #SH4_TIMESLICE @ load cycle counter
#else
	mov r9, #SH4_TIMESLICE @ load cycle counter
#endif

    mov r8, r0			@Load context
#ifndef NO_MMU
	@ With the MMU on, an exception raised under a block comes back to
	@ here by longjmp(), with the registers as they are now and the SH4's
	@ PC at the handler. Once a call of this loop: it costs nothing to run.
	bl CSYM(rec_arm_jmp_env)
	bl CSYM(_setjmp)
	ldr r4, [r8,#-184]	@load pc
	bl CSYM(rec_arm_mmu_on)
	cmp r0,#0
	bne CSYM(no_update_mmu)
#else
	ldr r4, [r8,#-184]	@load pc
#endif
	b CSYM(no_update)			@Go to mainloop !


	@this code is here for fall-through behavior of do_iter
	.global CSYM(intc_sched)
	HIDDEN(intc_sched)
CSYM(intc_sched):        @ next_pc _MUST_ be on ram
#ifdef __MACH__
    add r11,r11,#SH4_TIMESLICE
#else
    add r9,r9,#SH4_TIMESLICE
#endif
	mov r4,lr
    bl CSYM(UpdateSystem)
	mov lr,r4
	cmp r0,#0
	bne do_iter		@ an interrupt to take
	@ None. But the frontend's frame may be over: the video code says so by
	@ clearing CpuRunning, and the only place that was looked at is
	@ no_update, which a block reached only through here, with an interrupt
	@ pending. A program running with its interrupts masked therefore never
	@ gave the frontend its frame back: retro_run() did not return.
	ldr r0,[r8,#-156]	@ CpuRunning
	cmp r0,#0
	bxne lr			@ still running: back into the block

do_iter:
	mov r0,r4
	bl CSYM(rdv_DoInterrupts)
	mov r4,r0

.global CSYM(no_update)
HIDDEN(no_update)
CSYM(no_update):              @ next_pc _MUST_ be on r4 *R4 NOT R0 anymore*

	ldr r0,[r8,#-156]         @load CpuRunning
	cmp r0,#0
	beq CSYM(cleanup)

	sub r2,r8,#0x4100000
	ubfx r1,r4,#1,#24
	ldr pc,[r2,r1,lsl #2]

	@bic r1,r4,#0xFF000000
	@ldr pc,[r2,r1,lsl #1]

#ifndef NO_MMU
@ The same two entry points for blocks compiled with the MMU on. Their
@ next block is not found through the table, which goes by address alone
@ and knows nothing of address spaces, but looked up, and the looking up
@ can itself raise an exception: the PC is put where both find it.
.global CSYM(intc_sched_mmu)
HIDDEN(intc_sched_mmu)
CSYM(intc_sched_mmu):
#ifdef __MACH__
    add r11,r11,#SH4_TIMESLICE
#else
    add r9,r9,#SH4_TIMESLICE
#endif
	mov r4,lr
    bl CSYM(UpdateSystem)
	mov lr,r4
	cmp r0,#0
	bne 1f
	ldr r0,[r8,#-156]	@ CpuRunning
	cmp r0,#0
	bxne lr
1:
	mov r0,r4
	bl CSYM(rdv_DoInterrupts)
	mov r4,r0

.global CSYM(no_update_mmu)
HIDDEN(no_update_mmu)
CSYM(no_update_mmu):          @ next_pc on r4
	ldr r0,[r8,#-156]         @load CpuRunning
	cmp r0,#0
	beq CSYM(cleanup)
	str r4,[r8,#-184]         @ pc
	mov r0,r4
	bl CSYM(rec_arm_code_mmu)
	ldr r4,[r8,#-184]         @ the handler's, if getting there raised an exception
	bx r0
#endif

HIDDEN(cleanup)
CSYM(cleanup):

pop {r4-r12,lr}
bx lr

end_ngen_mainloop:
@@@@@@@@@@ ngen_mainloop @@@@@@@@@@

#if FEAT_AREC == DYNAREC_JIT
.global CSYM(arm_compilecode)
HIDDEN(arm_compilecode)
CSYM(arm_compilecode):
bl CSYM(CompileCode)
b CSYM(arm_dispatch)
#endif

#ifdef TARGET_IPHONE
Xarm_Reg: .word CSYM(arm_Reg)
XEntryPoints: .word CSYM(EntryPoints)
#endif

.global CSYM(arm_mainloop)
HIDDEN(arm_mainloop)
CSYM(arm_mainloop): @(cntx,lookup_base,cycles)

#ifdef __MACH__
push {r4,r5,r8,r11,lr}
#else
push {r4,r5,r8,r9,lr}
#endif

	#ifdef TARGET_IPHONE
	ldr r8,Xarm_Reg			@load cntx
	ldr r4,XEntryPoints		@load lookup base
	#else
	mov r8,r1			@load cntx
	mov r4,r2		@load lookup base
	#endif

	ldr r5,[r8,#192]	@load cycle count
	add r5,r0			@add cycles for this timeslice

	b CSYM(arm_dispatch)

.global CSYM(arm_dispatch)
HIDDEN(arm_dispatch)
CSYM(arm_dispatch):
	#ifdef TARGET_IPHONE
	ldrd r0,r1,[r8,#184]		@load: Next PC, interrupt
	#else
	ldrd r0,[r8,#184]		@load: Next PC, interrupt
	#endif

	ubfx r2,r0,#2,#21
	cmp r1,#0
	bne arm_dofiq

	ldr pc,[r4,r2,lsl #2]
	
arm_dofiq:
	bl CSYM(CPUFiq)
	b CSYM(arm_dispatch)

.global CSYM(arm_exit)
HIDDEN(arm_exit)
CSYM(arm_exit):
	str r5,[r8,#192]		@if timeslice is over, save remaining cycles
#ifdef __MACH__
	pop {r4,r5,r8,r11,pc}
#else
	pop {r4,r5,r8,r9,pc}
#endif

@@@@@@
@matrix mul
#ifndef _ANDROID
.global CSYM(ftrv_asm)
HIDDEN(ftrv_asm)
CSYM(ftrv_asm):

@r0=dst,r1=vec,r2=mtx

@3x vld1.32 might be faster
vldm r2,{d16-d24}
vldm r1, {d0-d1} 

VMUL.F32 Q2,Q8,d0[0]
VMLA.F32 Q2,Q9,d0[1]
VMLA.F32 Q2,Q10,d1[0]
VMLA.F32 Q2,Q11,d1[1]

vstm r0,{d4,d5}

bx lr

.global CSYM(fipr_asm)
HIDDEN(fipr_asm)
CSYM(fipr_asm):

@ vdot
@		idp=fr[n+0]*fr[m+0];
@		idp+=fr[n+1]*fr[m+1];
@		idp+=fr[n+2]*fr[m+2];
@		idp+=fr[n+3]*fr[m+3];


vldm r0, {d0,d1}
vldm r1, {d2,d3}

vmul.f32 q0,q1
@NEON is quite nice actually ! if only its performance was good enough ...
vpadd.f32 d0,d0,d1 @d0={d0[0]+d0[1], d1[0]+d1[1]}
vpadd.f32 d0,d0,d0 @d0={d0[0]+d0[1]+d1[0]+d1[1], d0[0]+d0[1]+d1[0]+d1[1]}

@store to ret ..
vmov r0,s0
bx lr

#endif

#endif
